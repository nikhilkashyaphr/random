/*
 * rdma_rx.c  --  RoCEv2 receiver (runs on Port B, default namespace)
 *
 * Design:
 *   - Creates a POSIX shared-memory ring (/dev/shm/iqring) and registers the
 *     WHOLE ring as a single memory region with the NIC. Each posted receive
 *     points directly at a ring slot, so the ConnectX DMAs every frame
 *     straight into memory the Holoscan Python app can mmap. The CPU never
 *     copies the payload: hardware writes it, Python (CuPy) reads it.
 *   - rdma_cm server with one Reliable Connection (RC) QP. 32 receives are
 *     kept posted at all times; as each completes it is validated, published
 *     (write_count++, release semantics), and immediately re-posted.
 *   - Overwrite policy is drop-oldest: the visualizer always reads the newest
 *     published slot, so a slow reader can never stall the 100 Gb link.
 *   - Survives sender restarts: on DISCONNECT it tears down the QP and goes
 *     back to listening, keeping the ring (and the visualizer) alive.
 *
 * Usage:
 *   sudo ./rdma_rx [-a 192.168.100.2] [-p 7471]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <rdma/rdma_cma.h>

#include "rdma_common.h"

#define CQ_DEPTH    64
#define POLL_BATCH  16

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

#define DIE(msg) do { \
    fprintf(stderr, "FATAL %s:%d %s: %s\n", __FILE__, __LINE__, (msg), \
            strerror(errno)); exit(1); } while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------------- shared-memory ring ---------------- */

static uint8_t *create_ring(void)
{
    /* O_CREAT-or-reopen: tolerates rings left behind by other users (see
     * iq_shm_open_rw) so the visualizer keeps working across restarts. */
    int fd = iq_shm_open_rw(RING_SHM_NAME);
    if (fd < 0)
        DIE("shm_open " RING_SHM_NAME
            " (stale ring from another user? try: sudo rm /dev/shm/iqring)");
    if (ftruncate(fd, (off_t)RING_TOTAL_BYTES))
        DIE("ftruncate ring (stale ring not writable? "
            "sudo rm /dev/shm/iqring)");

    uint8_t *base = mmap(NULL, RING_TOTAL_BYTES, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
        DIE("mmap ring");
    close(fd);

    struct ring_ctrl *ctrl = (struct ring_ctrl *)base;
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->version       = 1;
    ctrl->num_slots     = RING_SLOTS;
    ctrl->slot_stride   = RING_SLOT_STRIDE;
    ctrl->frame_samples = FRAME_SAMPLES;
    /* sample_rate / center_freq are filled in from the first frame header. */
    __atomic_store_n(&ctrl->write_count, 0, __ATOMIC_RELEASE);
    /* Publish the magic LAST so readers never see a half-written header. */
    __atomic_store_n(&ctrl->magic, RING_MAGIC, __ATOMIC_RELEASE);

    printf("ring : %s  (%zu bytes = 4 KiB ctrl + %u slots x %u B)\n",
           "/dev/shm" RING_SHM_NAME, RING_TOTAL_BYTES,
           RING_SLOTS, RING_SLOT_STRIDE);
    return base;
}

static inline uint8_t *slot_ptr(uint8_t *base, uint32_t slot)
{
    return base + RING_CTRL_BYTES + (size_t)slot * RING_SLOT_STRIDE;
}

static void post_recv_slot(struct rdma_cm_id *id, struct ibv_mr *mr,
                           uint8_t *base, uint32_t slot)
{
    struct ibv_sge sge = {
        .addr   = (uintptr_t)slot_ptr(base, slot),
        .length = FRAME_MSG_BYTES,
        .lkey   = mr->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id   = slot,
        .sg_list = &sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad = NULL;
    if (ibv_post_recv(id->qp, &wr, &bad))
        DIE("ibv_post_recv");
}

/* Validate one frame already resident in 'slot', account it, and publish
 * it to the visualizer. Returns the byte length counted, 0 if rejected.
 * Shared by all three transport paths. */
static uint32_t publish_frame(struct ring_ctrl *ctrl, uint8_t *ring,
                              uint32_t slot, uint64_t *expect_seq,
                              uint64_t *total_frames)
{
    struct frame_hdr *h = (struct frame_hdr *)slot_ptr(ring, slot);
    if (h->magic != IQ_FRAME_MAGIC)
        return 0;
    ctrl->sample_rate_hz = h->sample_rate_hz;   /* follow live fs/fc */
    ctrl->center_freq_hz = h->center_freq_hz;
    if (*expect_seq != UINT64_MAX && h->seq != *expect_seq) {
        uint64_t miss = (h->seq > *expect_seq) ? h->seq - *expect_seq : 1;
        __atomic_fetch_add(&ctrl->seq_gaps, miss, __ATOMIC_RELAXED);
    }
    *expect_seq = h->seq + 1;
    __atomic_fetch_add(&ctrl->bytes_total, FRAME_MSG_BYTES, __ATOMIC_RELAXED);
    (*total_frames)++;
    /* RELEASE: header+payload are fully in memory before the count that
     * tells the Python reader the slot is ready becomes visible. */
    __atomic_store_n(&ctrl->write_count, *total_frames, __ATOMIC_RELEASE);
    return FRAME_MSG_BYTES;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: sudo %s [-a bind_ip] [-p port] [-w mode]\n"
        "  -a  local IP of Port B to listen on (default 0.0.0.0)\n"
        "  -p  rdma_cm port                    (default %d)\n"
        "  -w  transport: send | write | write_imm (default write_imm)\n"
        "      must match the sender's -w\n", p,
        DEFAULT_TCP_PORT);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *bind_ip = "0.0.0.0";
    int port = DEFAULT_TCP_PORT;
    int xport = XPORT_WRITE_IMM;

    int opt;
    while ((opt = getopt(argc, argv, "a:p:w:h")) != -1) {
        switch (opt) {
        case 'a': bind_ip = optarg;       break;
        case 'p': port = atoi(optarg);    break;
        case 'w':
            if      (!strcmp(optarg, "send"))      xport = XPORT_SEND;
            else if (!strcmp(optarg, "write"))     xport = XPORT_WRITE;
            else if (!strcmp(optarg, "write_imm")) xport = XPORT_WRITE_IMM;
            else { fprintf(stderr, "bad -w mode: %s\n", optarg);
                   usage(argv[0]); }
            break;
        default:  usage(argv[0]);
        }
    }

    /* No SA_RESTART: Ctrl-C must interrupt blocking rdma_get_cm_event(). */
    struct sigaction sa = { .sa_handler = on_sig };
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uint8_t *ring = create_ring();
    struct ring_ctrl *ctrl = (struct ring_ctrl *)ring;

    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec)
        DIE("rdma_create_event_channel (is the rdma_cm module loaded?)");

    struct rdma_cm_id *listen_id = NULL;
    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP))
        DIE("rdma_create_id");

    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port   = htons((uint16_t)port) };
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1)
        DIE("bad -a address");
    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr))
        DIE("rdma_bind_addr (is the IP configured on Port B?)");
    if (rdma_listen(listen_id, 4))
        DIE("rdma_listen");

    printf("rx   : listening on %s:%d (RoCEv2 via rdma_cm)\n", bind_ip, port);
    printf("rx   : transport: %s\n", xport_name(xport));
    printf("rx   : waiting for sender... start the visualizer any time.\n");

    uint64_t total_frames = 0;          /* persists across reconnects */

    while (!g_stop) {
        /* ---------- wait for a connection request ---------- */
        struct rdma_cm_event *ev = NULL;
        if (rdma_get_cm_event(ec, &ev)) {
            if (errno == EINTR) continue;
            DIE("rdma_get_cm_event");
        }
        if (ev->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
            rdma_ack_cm_event(ev);
            continue;
        }
        struct rdma_cm_id *id = ev->id;
        rdma_ack_cm_event(ev);

        /* ---------- per-connection resources ---------- */
        struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
        if (!pd) DIE("ibv_alloc_pd");

        struct ibv_cq *cq = ibv_create_cq(id->verbs, CQ_DEPTH, NULL, NULL, 0);
        if (!cq) DIE("ibv_create_cq");

        struct ibv_qp_init_attr qpa = {
            .send_cq = cq,
            .recv_cq = cq,
            .qp_type = IBV_QPT_RC,
            .cap = { .max_send_wr = 4, .max_recv_wr = RING_SLOTS,
                     .max_send_sge = 1, .max_recv_sge = 1 },
        };
        if (rdma_create_qp(id, pd, &qpa))
            DIE("rdma_create_qp");

        /* One MR over the whole ring. SEND lands via posted RECVs; the
         * one-sided WRITE modes need the NIC to accept remote writes, so
         * add REMOTE_WRITE there. */
        int access = IBV_ACCESS_LOCAL_WRITE;
        if (xport != XPORT_SEND)
            access |= IBV_ACCESS_REMOTE_WRITE;
        struct ibv_mr *mr = ibv_reg_mr(pd, ring, RING_TOTAL_BYTES, access);
        if (!mr)
            DIE("ibv_reg_mr (check 'ulimit -l' / memlock limits)");

        /* SEND matches each frame to a posted RECV; WRITE_WITH_IMM consumes
         * one RECV per frame to deliver its immediate. Pure WRITE consumes
         * no RECVs at all (the CPU polls memory), so post none. */
        if (xport != XPORT_WRITE)
            for (uint32_t s = 0; s < RING_SLOTS; s++)
                post_recv_slot(id, mr, ring, s);

        /* Advertise the ring to the sender for the WRITE modes. */
        struct rdma_region_info rinfo = {
            .magic = RINFO_MAGIC, .addr = (uint64_t)(uintptr_t)ring,
            .rkey = mr->rkey, .num_slots = RING_SLOTS,
            .slot_stride = RING_SLOT_STRIDE, .ctrl_bytes = RING_CTRL_BYTES,
        };
        struct rdma_conn_param cp = {
            .responder_resources = 1,
            .initiator_depth     = 1,
            .rnr_retry_count     = 7,   /* infinite retry: flow control */
            .private_data        = &rinfo,
            .private_data_len    = sizeof(rinfo),
        };
        if (rdma_accept(id, &cp))
            DIE("rdma_accept");

        if (rdma_get_cm_event(ec, &ev))
            DIE("rdma_get_cm_event (established)");
        if (ev->event != RDMA_CM_EVENT_ESTABLISHED) {
            fprintf(stderr, "rx   : connection failed: %s\n",
                    rdma_event_str(ev->event));
            rdma_ack_cm_event(ev);
            goto teardown;
        }
        rdma_ack_cm_event(ev);
        printf("rx   : sender connected.\n");

        /* Non-blocking CM channel inside the data loop so we can poll the
         * CQ at full speed and still notice DISCONNECTED. */
        int ecflags = fcntl(ec->fd, F_GETFL);
        fcntl(ec->fd, F_SETFL, ecflags | O_NONBLOCK);

        /* ---------- data loop ---------- */
        uint64_t expect_seq = UINT64_MAX;     /* first frame sets it */
        uint64_t last = now_ns();
        uint64_t bytes_win = 0, frames_win = 0;
        uint64_t poll_seq = 0;                /* pure-WRITE: next slot   */
        int connected = 1;

        while (!g_stop && connected) {
            int worked = 0;

            if (xport == XPORT_WRITE) {
                /* Pure one-sided WRITE: no completions arrive. The sender
                 * writes header+payload into slot (seq % RING_SLOTS); we
                 * watch that slot's header for the sequence number we
                 * expect to appear next. The magic is written as part of
                 * the same ordered RDMA WRITE, so seeing the new seq means
                 * the whole frame has landed. */
                uint32_t slot = (uint32_t)(poll_seq % RING_SLOTS);
                struct frame_hdr *h =
                    (struct frame_hdr *)slot_ptr(ring, slot);
                if (h->magic == IQ_FRAME_MAGIC && h->seq == poll_seq) {
                    /* Acquire fence so payload reads aren't hoisted above
                     * the header check. NOTE: pure RDMA_WRITE gives no
                     * hardware completion and no strict last-byte-last
                     * guarantee, so this polled path is best-effort and
                     * meant for demonstrating the opcode; write_imm is the
                     * robust one-sided mode for production. */
                    __atomic_thread_fence(__ATOMIC_ACQUIRE);
                    uint32_t b = publish_frame(ctrl, ring, slot,
                                               &expect_seq, &total_frames);
                    bytes_win += b; frames_win++;
                    poll_seq++;
                    worked = 1;
                }
            } else {
                /* SEND or WRITE_WITH_IMM: both deliver a RECV completion.
                 * For WRITE_WITH_IMM the payload is already in the slot via
                 * the one-sided write; the immediate (slot index) is what
                 * the completion carries. For SEND the data arrived with
                 * the message. Either way we read the slot the same way. */
                struct ibv_wc wc[POLL_BATCH];
                int n = ibv_poll_cq(cq, POLL_BATCH, wc);
                if (n < 0) DIE("ibv_poll_cq");
                for (int i = 0; i < n; i++) {
                    if (wc[i].status != IBV_WC_SUCCESS) {
                        fprintf(stderr, "rx   : WC error: %s\n",
                                ibv_wc_status_str(wc[i].status));
                        connected = 0;
                        break;
                    }
                    uint32_t slot;
                    if (xport == XPORT_WRITE_IMM)
                        slot = ntohl(wc[i].imm_data) % RING_SLOTS;
                    else
                        slot = (uint32_t)wc[i].wr_id;

                    uint32_t b = publish_frame(ctrl, ring, slot,
                                               &expect_seq, &total_frames);
                    if (b) { bytes_win += b; frames_win++; }
                    else fprintf(stderr,
                                 "rx   : bad magic in slot %u, ignored\n",
                                 slot);
                    /* Re-arm a RECV. wr_id carries the slot for SEND; for
                     * WRITE_IMM the slot is irrelevant to the RECV (the
                     * write targets memory directly), so just replenish. */
                    post_recv_slot(id, mr, ring,
                                   (xport == XPORT_WRITE_IMM)
                                       ? (uint32_t)wc[i].wr_id : slot);
                }
                worked = (n > 0);
            }

            /* DISCONNECTED / errors from the CM channel? */
            struct rdma_cm_event *e2 = NULL;
            if (rdma_get_cm_event(ec, &e2) == 0) {
                if (e2->event == RDMA_CM_EVENT_DISCONNECTED ||
                    e2->event == RDMA_CM_EVENT_DEVICE_REMOVAL)
                    connected = 0;
                rdma_ack_cm_event(e2);
            }

            if (!worked) {
                struct timespec idle = { 0, 20000 };   /* 20 us */
                nanosleep(&idle, NULL);
            }

            uint64_t now = now_ns();
            if (now - last >= 1000000000ull) {
                double secs = (double)(now - last) / 1e9;
                printf("rx   : %7.2f Gb/s  %6.0f fps  total %" PRIu64
                       " frames, gaps %" PRIu64 "\n",
                       (double)bytes_win * 8.0 / secs / 1e9,
                       (double)frames_win / secs,
                       total_frames,
                       __atomic_load_n(&ctrl->seq_gaps, __ATOMIC_RELAXED));
                bytes_win = frames_win = 0;
                last = now;
            }
        }

        fcntl(ec->fd, F_SETFL, ecflags);   /* back to blocking accept */
        printf("rx   : sender disconnected; listening again.\n");

teardown:
        rdma_disconnect(id);
        rdma_destroy_qp(id);
        ibv_dereg_mr(mr);
        ibv_destroy_cq(cq);
        ibv_dealloc_pd(pd);
        rdma_destroy_id(id);
    }

    printf("rx   : exiting. Ring left in /dev/shm%s for the visualizer.\n",
           RING_SHM_NAME);
    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);
    munmap(ring, RING_TOTAL_BYTES);
    return 0;
}
