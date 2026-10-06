/*
 * rdma_tx.c  --  IQ sample generator + RoCEv2 sender (runs on Port A, in ns_tx)
 *
 * Design:
 *   - At startup, synthesizes a long repeating waveform table (CW tone +
 *     swept chirp + noise floor) as interleaved int16 I/Q.
 *   - The table is registered once with the NIC; every frame is sent
 *     ZERO-COPY with a 2-element scatter/gather list:
 *         SGE[0] = 64 B frame header,  SGE[1] = 1 MiB slice of the table.
 *     The CPU never touches the sample payload again after startup.
 *   - Reliable Connection (RC) QP via rdma_cm. RC gives in-order, lossless
 *     delivery; RNR retries (rnr_retry=7) provide automatic flow control if
 *     the receiver momentarily runs out of posted receive buffers.
 *   - Optional software pacing (-g <Gbit/s>) so you can run a smooth demo;
 *     -g 0 disables pacing and pushes as fast as PCIe Gen3 x8 allows.
 *
 * Usage:
 *   sudo ip netns exec ns_tx ./rdma_tx -s 192.168.100.2 [-g 10] [-f 100e6]
 *                                      [-c 2.45e9] [-t seconds] [-p port]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
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

#define MAX_OUTSTANDING 32u                 /* in-flight RDMA SENDs          */
#define TABLE_FRAMES    64u                 /* waveform table length         */
#define TABLE_SAMPLES   ((uint64_t)TABLE_FRAMES * FRAME_SAMPLES)  /* 16.7 M  */
#define TABLE_BYTES     (TABLE_SAMPLES * BYTES_PER_SAMPLE)        /* 64 MiB  */

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

static uint64_t wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Block until the next rdma_cm event; die loudly if it isn't the one we
 * expect (this turns ADDR_ERROR / ROUTE_ERROR / REJECTED into clear text). */
static void wait_event(struct rdma_event_channel *ec,
                       enum rdma_cm_event_type want)
{
    struct rdma_cm_event *ev = NULL;
    if (rdma_get_cm_event(ec, &ev))
        DIE("rdma_get_cm_event");
    if (ev->event != want) {
        fprintf(stderr, "FATAL: got CM event '%s' (status %d), expected '%s'\n",
                rdma_event_str(ev->event), ev->status, rdma_event_str(want));
        fprintf(stderr, "  Hints: is rdma_rx running? IPs/MTU set? "
                        "ports in the right namespaces?\n");
        exit(1);
    }
    rdma_ack_cm_event(ev);
}

/* Like wait_event, but copies the connection's private_data (the
 * receiver's rdma_region_info) out of the ESTABLISHED event. */
static void wait_established(struct rdma_event_channel *ec,
                             struct rdma_region_info *out)
{
    struct rdma_cm_event *ev = NULL;
    if (rdma_get_cm_event(ec, &ev))
        DIE("rdma_get_cm_event");
    if (ev->event != RDMA_CM_EVENT_ESTABLISHED) {
        fprintf(stderr, "FATAL: got CM event '%s' (status %d), expected "
                "'ESTABLISHED'\n", rdma_event_str(ev->event), ev->status);
        exit(1);
    }
    memset(out, 0, sizeof(*out));
    if (ev->param.conn.private_data &&
        ev->param.conn.private_data_len >= sizeof(*out))
        memcpy(out, ev->param.conn.private_data, sizeof(*out));
    rdma_ack_cm_event(ev);
}

/*
 * Waveform: a clean single complex tone at tone_frac * fs (default -0.25),
 * expressed as a fraction of the sample rate so the spectrum shape is the
 * same for any -f value. The tone frequency is snapped to the nearest exact
 * integer number of cycles across the whole table, so when frames cycle
 * through the table the phase is continuous at the wrap -- no spectral
 * splatter, a single clean spectral line, and a smooth sine in time.
 * Optional light noise (-N) is off by default.
 */
static void gen_table(int16_t *tbl, double tone_frac, double noise_amp)
{
    const double a_tone = 0.90;             /* near full-scale single tone  */

    /* Snap tone to an integer cycle count over the table for seamless loop */
    double cyc = tone_frac * (double)TABLE_SAMPLES;
    long   cyc_i = lrint(cyc);
    if (cyc_i == 0) cyc_i = 1;
    double inc = 2.0 * M_PI * (double)cyc_i / (double)TABLE_SAMPLES;

    unsigned int rs = 0x12345678u;
    double ph = 0.0;
    for (uint64_t n = 0; n < TABLE_SAMPLES; n++) {
        double I = a_tone * cos(ph);
        double Q = a_tone * sin(ph);
        if (noise_amp > 0.0) {
            double ni = ((double)rand_r(&rs) / RAND_MAX - 0.5)
                      + ((double)rand_r(&rs) / RAND_MAX - 0.5);
            double nq = ((double)rand_r(&rs) / RAND_MAX - 0.5)
                      + ((double)rand_r(&rs) / RAND_MAX - 0.5);
            I += noise_amp * ni;
            Q += noise_amp * nq;
        }
        ph += inc;
        if (ph >  M_PI) ph -= 2.0 * M_PI;
        if (I >  0.999) I =  0.999;
        if (I < -0.999) I = -0.999;
        if (Q >  0.999) Q =  0.999;
        if (Q < -0.999) Q = -0.999;
        tbl[2 * n]     = (int16_t)lrint(I * 32767.0);
        tbl[2 * n + 1] = (int16_t)lrint(Q * 32767.0);
    }
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s -s <receiver_ip> [options]\n"
        "  -s ip      receiver (Port B) IP address, e.g. 192.168.100.2\n"
        "  -p port    rdma_cm port                  (default %d)\n"
        "  -g gbps    pacing target in Gbit/s, 0 = unlimited (default 10)\n"
        "  -f hz      nominal sample rate metadata  (default 100e6)\n"
        "  -c hz      RF center frequency metadata  (default 2.45e9)\n"
        "  -T frac    tone freq as fraction of fs   (default -0.25)\n"
        "  -N amp     noise amplitude 0..0.2        (default 0 = clean tone)\n"
        "  -t sec     stop after N seconds, 0 = run forever (default 0)\n"
        "  -w mode    transport: send | write | write_imm (default write_imm)\n"
        "             send=two-sided, write/write_imm=one-sided RDMA WRITE\n",
        p, DEFAULT_TCP_PORT);
}

int main(int argc, char **argv)
{
    const char *server = NULL;
    int port = DEFAULT_TCP_PORT;
    double gbps = 10.0, fs = 100e6, fc = 2.45e9, run_secs = 0.0;
    double tone_frac = -0.25, noise_amp = 0.0;
    int xport = XPORT_WRITE_IMM;

    int opt;
    while ((opt = getopt(argc, argv, "s:p:g:f:c:t:w:T:N:h")) != -1) {
        switch (opt) {
        case 's': server   = optarg;       break;
        case 'p': port     = atoi(optarg); break;
        case 'g': gbps     = atof(optarg); break;
        case 'f': fs       = atof(optarg); break;
        case 'c': fc       = atof(optarg); break;
        case 't': run_secs = atof(optarg); break;
        case 'T': tone_frac = atof(optarg); break;
        case 'N': noise_amp = atof(optarg); break;
        case 'w':
            if      (!strcmp(optarg, "send"))      xport = XPORT_SEND;
            else if (!strcmp(optarg, "write"))     xport = XPORT_WRITE;
            else if (!strcmp(optarg, "write_imm")) xport = XPORT_WRITE_IMM;
            else { fprintf(stderr, "bad -w mode: %s\n", optarg);
                   usage(argv[0]); return 1; }
            break;
        default: usage(argv[0]); return 1;
        }
    }
    if (!server) { usage(argv[0]); return 1; }

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;                 /* no SA_RESTART: interrupt I/O  */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* ---- waveform table ------------------------------------------------ */
    printf("[tx] generating %" PRIu64 " IQ samples (%.0f MiB waveform table)...\n",
           TABLE_SAMPLES, TABLE_BYTES / 1048576.0);
    int16_t *tbl = NULL;
    if (posix_memalign((void **)&tbl, 4096, TABLE_BYTES))
        DIE("posix_memalign(table)");
    gen_table(tbl, tone_frac, noise_amp);

    struct frame_hdr *hdrs = NULL;          /* one header per in-flight WR  */
    if (posix_memalign((void **)&hdrs, 64,
                       MAX_OUTSTANDING * sizeof(struct frame_hdr)))
        DIE("posix_memalign(hdrs)");
    memset(hdrs, 0, MAX_OUTSTANDING * sizeof(struct frame_hdr));

    /* ---- rdma_cm connection setup -------------------------------------- */
    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) DIE("rdma_create_event_channel (is the rdma device visible in "
                 "this namespace? see scripts/02_setup_network.sh)");

    struct rdma_cm_id *id = NULL;
    if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP)) DIE("rdma_create_id");

    struct sockaddr_in dst; memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, server, &dst.sin_addr) != 1) {
        fprintf(stderr, "bad IPv4 address: %s\n", server); return 1;
    }

    if (rdma_resolve_addr(id, NULL, (struct sockaddr *)&dst, 2000))
        DIE("rdma_resolve_addr");
    wait_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED);
    if (rdma_resolve_route(id, 2000)) DIE("rdma_resolve_route");
    wait_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED);

    struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
    if (!pd) DIE("ibv_alloc_pd");
    struct ibv_cq *cq = ibv_create_cq(id->verbs, MAX_OUTSTANDING * 2,
                                      NULL, NULL, 0);
    if (!cq) DIE("ibv_create_cq");

    struct ibv_qp_init_attr qa; memset(&qa, 0, sizeof qa);
    qa.send_cq = cq;
    qa.recv_cq = cq;
    qa.qp_type = IBV_QPT_RC;
    qa.cap.max_send_wr  = MAX_OUTSTANDING;
    qa.cap.max_send_sge = 2;                /* header + payload slice       */
    qa.cap.max_recv_wr  = 1;
    qa.cap.max_recv_sge = 1;
    if (rdma_create_qp(id, pd, &qa)) DIE("rdma_create_qp");

    struct ibv_mr *mr_tbl = ibv_reg_mr(pd, tbl, TABLE_BYTES, 0);
    if (!mr_tbl) DIE("ibv_reg_mr(table) -- check 'ulimit -l' or run as root");
    struct ibv_mr *mr_hdr = ibv_reg_mr(pd, hdrs,
                                MAX_OUTSTANDING * sizeof(struct frame_hdr), 0);
    if (!mr_hdr) DIE("ibv_reg_mr(headers)");

    struct rdma_conn_param cp; memset(&cp, 0, sizeof cp);
    cp.initiator_depth     = 1;
    cp.responder_resources = 1;
    cp.retry_count         = 7;
    cp.rnr_retry_count     = 7;             /* 7 == retry forever            */
    if (rdma_connect(id, &cp)) DIE("rdma_connect");

    struct rdma_region_info rinfo;
    wait_established(ec, &rinfo);

    /* WRITE modes need the receiver's advertised ring (addr+rkey). The
     * receiver sends it in the connection private_data; bail clearly if a
     * SEND-mode receiver answered a WRITE-mode sender. */
    if (xport != XPORT_SEND) {
        if (rinfo.magic != RINFO_MAGIC)
            DIE("receiver did not advertise an RDMA region -- run rdma_rx "
                "with the same -w write/write_imm mode");
        if (rinfo.slot_stride != RING_SLOT_STRIDE ||
            rinfo.ctrl_bytes  != RING_CTRL_BYTES)
            DIE("receiver ring geometry mismatch (rebuild both ends)");
    }
    printf("[tx] connected to %s:%d  (frame=%u B, payload=%u IQ samples)\n",
           server, port, FRAME_MSG_BYTES, FRAME_SAMPLES);
    printf("[tx] transport: %s\n", xport_name(xport));
    if (gbps > 0)
        printf("[tx] pacing to %.2f Gbit/s (use -g 0 for max throughput)\n", gbps);

    /* ---- live runtime control block (/dev/shm/iqctl) ------------------- */
    /* The visualizer writes pace/fs/fc here and bumps 'seq'; we poll at the
     * 1 Hz stats tick. /dev/shm is shared across network namespaces.       */
    struct iq_ctl *ctl = NULL;
    {
        int cfd = iq_shm_open_rw(IQCTL_SHM_NAME);
        if (cfd >= 0 && !ftruncate(cfd, (off_t)IQCTL_BYTES)) {
            ctl = mmap(NULL, IQCTL_BYTES, PROT_READ | PROT_WRITE,
                       MAP_SHARED, cfd, 0);
            if (ctl == MAP_FAILED) ctl = NULL;
        }
        if (cfd >= 0) close(cfd);
        if (ctl) {
            ctl->version        = 1;
            ctl->pace_gbps      = gbps;       /* reflect actual state      */
            ctl->sample_rate_hz = (uint64_t)fs;
            ctl->center_freq_hz = (int64_t)fc;
            __atomic_store_n(&ctl->magic, IQCTL_MAGIC, __ATOMIC_RELEASE);
            printf("[tx] live control ready at /dev/shm%s "
                   "(pace/fs/fc adjustable from the visualizer)\n",
                   IQCTL_SHM_NAME);
        } else {
            fprintf(stderr, "[tx] warning: no live control block (%s)\n",
                    strerror(errno));
        }
    }
    uint32_t ctl_seq_seen = ctl ? ctl->seq : 0;

    /* ---- main streaming loop ------------------------------------------- */
    uint64_t seq = 0, completed = 0;
    uint64_t t0 = now_ns(), last_t = t0, last_completed = 0;
    /* Pacing is rebased whenever the rate changes at runtime so the new
     * rate takes effect smoothly instead of bursting to catch up.          */
    uint64_t pace_t0 = t0, pace_base_bytes = 0;

    while (!g_stop &&
           (run_secs <= 0.0 || (double)(now_ns() - t0) / 1e9 < run_secs)) {

        /* pacing budget: how many bytes are we allowed to have posted?     */
        uint64_t tnow   = now_ns();
        uint64_t budget = (gbps > 0.0)
                        ? pace_base_bytes + (uint64_t)(gbps * 1e9 / 8.0 *
                              ((double)(tnow - pace_t0) / 1e9))
                        : UINT64_MAX;

        int posted_any = 0;
        while (!g_stop &&
               (seq - completed) < MAX_OUTSTANDING &&
               seq * (uint64_t)FRAME_MSG_BYTES < budget) {

            uint32_t k = (uint32_t)(seq % MAX_OUTSTANDING);
            struct frame_hdr *h = &hdrs[k];
            h->magic          = IQ_FRAME_MAGIC;
            h->seq            = seq;
            h->t_ns           = wall_ns();
            h->n_samples      = FRAME_SAMPLES;
            h->flags          = 0;
            h->sample_rate_hz = (uint64_t)fs;
            h->center_freq_hz = (int64_t)fc;

            uint64_t off = (seq % TABLE_FRAMES) * (uint64_t)FRAME_PAYLOAD;
            struct ibv_sge sge[2] = {
                { .addr = (uintptr_t)h,
                  .length = sizeof(*h),     .lkey = mr_hdr->lkey },
                { .addr = (uintptr_t)((uint8_t *)tbl + off),
                  .length = FRAME_PAYLOAD,  .lkey = mr_tbl->lkey },
            };
            struct ibv_send_wr wr; memset(&wr, 0, sizeof wr);
            struct ibv_send_wr *bad = NULL;
            wr.wr_id      = seq;
            wr.sg_list    = sge;
            wr.num_sge    = 2;

            if (xport == XPORT_SEND) {
                wr.opcode = IBV_WR_SEND;
            } else {
                /* One-sided: scatter header+payload into the remote slot.
                 * The two local SGEs land contiguously starting at the
                 * slot base, reproducing the SEND wire layout in remote
                 * memory so the visualizer parser is unchanged. */
                uint32_t slot = (uint32_t)(seq % rinfo.num_slots);
                wr.wr.rdma.remote_addr = rinfo.addr + rinfo.ctrl_bytes +
                                         (uint64_t)slot * rinfo.slot_stride;
                wr.wr.rdma.rkey        = rinfo.rkey;
                if (xport == XPORT_WRITE_IMM) {
                    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
                    wr.imm_data = htonl(slot);   /* signals which slot      */
                } else {
                    wr.opcode = IBV_WR_RDMA_WRITE;
                }
            }
            /* Signal only every 8th WR to cut completion overhead; we still
             * get ordered, reliable delivery on the RC QP. */
            wr.send_flags = ((seq & 7) == 7) ? IBV_SEND_SIGNALED : 0;
            if (ibv_post_send(id->qp, &wr, &bad)) DIE("ibv_post_send");
            seq++;
            posted_any = 1;
        }

        struct ibv_wc wc[16];
        int n = ibv_poll_cq(cq, 16, wc);
        if (n < 0) DIE("ibv_poll_cq");
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[tx] send completion error: %s "
                        "(receiver gone?)\n",
                        ibv_wc_status_str(wc[i].status));
                g_stop = 1;
                break;
            }
            /* RC completes in order and we signal every 8th WR, so a
             * completion for wr_id S means every WR up to and including S
             * is done. Advance 'completed' to S+1. */
            if (wc[i].wr_id + 1 > completed)
                completed = wc[i].wr_id + 1;
        }

        if (!posted_any && n == 0) {        /* idle: don't burn a core      */
            struct timespec ts = { 0, 50000 };  /* 50 us */
            nanosleep(&ts, NULL);
        }

        uint64_t t = now_ns();
        if (t - last_t >= 1000000000ull) {
            double dt = (double)(t - last_t) / 1e9;
            double gb = (double)(completed - last_completed)
                      * FRAME_MSG_BYTES * 8.0 / dt / 1e9;
            printf("[tx] %7.2f Gb/s  %6.0f frames/s  sent=%" PRIu64
                   "  inflight=%" PRIu64 "\n",
                   gb, (double)(completed - last_completed) / dt,
                   completed, seq - completed);
            fflush(stdout);
            last_t = t;
            last_completed = completed;

            /* live runtime control: apply pending changes from the viz   */
            if (ctl && ctl->magic == IQCTL_MAGIC) {
                uint32_t s = __atomic_load_n(&ctl->seq, __ATOMIC_ACQUIRE);
                if (s != ctl_seq_seen) {
                    ctl_seq_seen = s;
                    double ng = ctl->pace_gbps;
                    if (ng != gbps) {       /* rebase pacing at new rate  */
                        pace_base_bytes = seq * (uint64_t)FRAME_MSG_BYTES;
                        pace_t0 = t;
                        gbps = ng;
                    }
                    fs = (double)ctl->sample_rate_hz;
                    fc = (double)ctl->center_freq_hz;
                    printf("[tx] live update: pace=%.2f Gb/s  fs=%.4g Hz  "
                           "fc=%.6g Hz\n", gbps, fs, fc);
                }
            }
        }
    }

    /* drain outstanding completions for up to 0.5 s */
    uint64_t deadline = now_ns() + 500000000ull;
    while (completed < seq && now_ns() < deadline) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(cq, 16, wc);
        for (int i = 0; i < n; i++)
            if (wc[i].status == IBV_WC_SUCCESS && wc[i].wr_id + 1 > completed)
                completed = wc[i].wr_id + 1;
        if (n == 0) { struct timespec ts = {0, 100000}; nanosleep(&ts, NULL); }
    }

    double total_s = (double)(now_ns() - t0) / 1e9;
    printf("[tx] done: %" PRIu64 " frames, %.2f GB in %.1f s (avg %.2f Gb/s)\n",
           completed, completed * (double)FRAME_MSG_BYTES / 1e9, total_s,
           completed * (double)FRAME_MSG_BYTES * 8.0 / total_s / 1e9);

    rdma_disconnect(id);
    rdma_destroy_qp(id);
    ibv_dereg_mr(mr_tbl);
    ibv_dereg_mr(mr_hdr);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(id);
    rdma_destroy_event_channel(ec);
    free(tbl);
    free(hdrs);
    return 0;
}
