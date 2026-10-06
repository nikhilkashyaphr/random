/*
 * rdma_rx_gpu.c  --  GPUDirect RDMA receiver: the ConnectX-5 DMAs IQ
 * payloads STRAIGHT INTO GPU MEMORY. CPU RAM is bypassed for sample data.
 *
 * How it differs from rdma_rx.c:
 *   - The payload ring lives in cudaMalloc'd device memory, registered with
 *     the NIC through the nvidia-peermem kernel module (GPUDirect RDMA).
 *   - Each receive WR has TWO scatter entries: SGE[0] lands the 64-byte
 *     frame header in host shared memory (so this CPU loop can validate
 *     magic/sequence and the Python app can read metadata cheaply), and
 *     SGE[1] lands the 1 MiB payload in the GPU ring. An incoming SEND
 *     fills a receive WR's SGE list in order, so the NIC splits the frame
 *     for us in hardware.
 *   - The visualizer maps the GPU ring zero-copy via CUDA IPC: we export a
 *     cudaIpcMemHandle_t into the host shm control block; holoscan_iq_viz.py
 *     detects RING_MAGIC_GPU and opens the handle with CuPy. End to end
 *     there are ZERO host copies of sample data: NIC -> GPU -> cuFFT ->
 *     Holoviz.
 *
 * Requirements (scripts/05_gpudirect_setup.sh checks all of these):
 *   - nvidia-peermem module loaded (ships with the NVIDIA driver)
 *   - run rdma_rx_gpu AND the visualizer as the SAME user (CUDA IPC is
 *     per-UID). Add yourself to the 'rdma' group instead of using sudo,
 *     and raise the memlock limit.
 *
 * Usage:
 *   ./rdma_rx_gpu [-a 192.168.100.2] [-p 7471] [-d cuda_device]
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

#include <cuda_runtime.h>
#include <rdma/rdma_cma.h>

#include "rdma_common.h"

#define CQ_DEPTH    64
#define POLL_BATCH  16

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

#define DIE(msg) do { \
    fprintf(stderr, "FATAL %s:%d %s: %s\n", __FILE__, __LINE__, (msg), \
            strerror(errno)); exit(1); } while (0)

#define CUDIE(call) do { cudaError_t e_ = (call); if (e_ != cudaSuccess) { \
    fprintf(stderr, "FATAL %s:%d CUDA %s: %s\n", __FILE__, __LINE__, \
            #call, cudaGetErrorString(e_)); exit(1); } } while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint8_t *create_host_block(void)
{
    int fd = iq_shm_open_rw(RING_SHM_NAME);
    if (fd < 0)
        DIE("shm_open " RING_SHM_NAME
            " (stale ring from another user? try: sudo rm /dev/shm/iqring)");
    if (ftruncate(fd, (off_t)GPU_HOST_BYTES))
        DIE("ftruncate (stale ring not writable? sudo rm /dev/shm/iqring)");
    uint8_t *base = mmap(NULL, GPU_HOST_BYTES, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) DIE("mmap");
    close(fd);
    return base;
}

static inline struct frame_hdr *hdr_slot(uint8_t *host, uint32_t s)
{
    return (struct frame_hdr *)(host + GPU_HDR_BASE + (size_t)s * 64u);
}

static void post_recv_slot(struct rdma_cm_id *id,
                           struct ibv_mr *mr_host, struct ibv_mr *mr_gpu,
                           uint8_t *host, uint8_t *gpu, uint32_t slot)
{
    struct ibv_sge sge[2] = {
        { .addr   = (uintptr_t)hdr_slot(host, slot),
          .length = sizeof(struct frame_hdr),
          .lkey   = mr_host->lkey },
        { .addr   = (uintptr_t)(gpu + (size_t)slot * GPU_SLOT_STRIDE),
          .length = FRAME_PAYLOAD,
          .lkey   = mr_gpu->lkey },
    };
    struct ibv_recv_wr wr = { .wr_id = slot, .sg_list = sge, .num_sge = 2 };
    struct ibv_recv_wr *bad = NULL;
    if (ibv_post_recv(id->qp, &wr, &bad))
        DIE("ibv_post_recv");
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [-a bind_ip] [-p port] [-d cuda_dev] [-w send]\n"
        "  -a  local IP of Port B to listen on (default 0.0.0.0)\n"
        "  -p  rdma_cm port                    (default %d)\n"
        "  -d  CUDA device ordinal             (default 0)\n"
        "  -w  transport: send only in GPUDirect mode (default send)\n",
        p, DEFAULT_TCP_PORT);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *bind_ip = "0.0.0.0";
    int port = DEFAULT_TCP_PORT, dev = 0;
    int xport = XPORT_SEND;          /* GPUDirect default: two-sided */

    int opt;
    while ((opt = getopt(argc, argv, "a:p:d:w:h")) != -1) {
        switch (opt) {
        case 'a': bind_ip = optarg;    break;
        case 'p': port = atoi(optarg); break;
        case 'd': dev  = atoi(optarg); break;
        case 'w':
            if      (!strcmp(optarg, "send"))      xport = XPORT_SEND;
            else if (!strcmp(optarg, "write") || !strcmp(optarg, "write_imm")) {
                fprintf(stderr,
                    "GPUDirect receiver supports -w send only.\n"
                    "  One-sided RDMA WRITE into GPU memory needs the frame\n"
                    "  header and payload in two different MRs (host + VRAM),\n"
                    "  i.e. two WRs per frame -- pointless on a board that\n"
                    "  can't route NIC<->GPU P2P anyway. For one-sided RDMA\n"
                    "  use the host-staging receiver:  ./rdma_rx -w %s\n",
                    optarg);
                return 1;
            } else { fprintf(stderr, "bad -w mode: %s\n", optarg);
                     usage(argv[0]); }
            break;
        default:  usage(argv[0]);
        }
    }

    struct sigaction sa = { .sa_handler = on_sig };
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* ---- GPU payload ring + IPC handle -------------------------------- */
    CUDIE(cudaSetDevice(dev));
    uint8_t *gpu_ring = NULL;
    size_t gpu_bytes = (size_t)RING_SLOTS * GPU_SLOT_STRIDE;
    CUDIE(cudaMalloc((void **)&gpu_ring, gpu_bytes));
    CUDIE(cudaMemset(gpu_ring, 0, gpu_bytes));

    cudaIpcMemHandle_t ipc;
    CUDIE(cudaIpcGetMemHandle(&ipc, gpu_ring));
    _Static_assert(sizeof(ipc) == 64, "cudaIpcMemHandle_t expected 64 B");

    /* ---- host control + header block ----------------------------------- */
    uint8_t *host = create_host_block();
    struct ring_ctrl *ctrl = (struct ring_ctrl *)host;
    memset(host, 0, GPU_HOST_BYTES);
    ctrl->version       = 2;
    ctrl->num_slots     = RING_SLOTS;
    ctrl->slot_stride   = GPU_SLOT_STRIDE;      /* stride in the GPU ring */
    ctrl->frame_samples = FRAME_SAMPLES;
    memcpy(host + CTRL_IPC_OFF, &ipc, sizeof(ipc));
    __atomic_store_n(&ctrl->magic, RING_MAGIC_GPU, __ATOMIC_RELEASE);

    printf("gpu  : %zu MiB payload ring on CUDA device %d "
           "(GPUDirect, CUDA-IPC exported)\n", gpu_bytes >> 20, dev);
    printf("gpu  : WARNING: GPUDirect needs PCIe peer-to-peer routing "
           "between NIC and GPU.\n"
           "       On boards where they sit under sibling root ports, "
           "registration and\n"
           "       completions can succeed while payload writes are NOT "
           "delivered.\n"
           "       Verify the spectrum shows the test tone; if it is "
           "empty/garbage,\n"
           "       use the CPU-staging receiver (rdma_rx) instead.\n");
    printf("ring : /dev/shm%s (ctrl + %u headers; payloads live on GPU)\n",
           RING_SHM_NAME, RING_SLOTS);

    /* ---- rdma_cm server -------------------------------------------------*/
    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) DIE("rdma_create_event_channel");
    struct rdma_cm_id *listen_id = NULL;
    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP))
        DIE("rdma_create_id");

    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port   = htons((uint16_t)port) };
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1)
        DIE("bad -a address");
    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr))
        DIE("rdma_bind_addr");
    if (rdma_listen(listen_id, 4)) DIE("rdma_listen");
    printf("rx   : listening on %s:%d  (GPUDirect mode, %s)\n",
           bind_ip, port, xport_name(xport));

    uint64_t total_frames = 0;

    while (!g_stop) {
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

        struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
        if (!pd) DIE("ibv_alloc_pd");
        struct ibv_cq *cq = ibv_create_cq(id->verbs, CQ_DEPTH, NULL, NULL, 0);
        if (!cq) DIE("ibv_create_cq");

        struct ibv_qp_init_attr qpa = {
            .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
            .cap = { .max_send_wr = 4, .max_recv_wr = RING_SLOTS,
                     .max_send_sge = 1, .max_recv_sge = 2 },
        };
        if (rdma_create_qp(id, pd, &qpa)) DIE("rdma_create_qp");

        struct ibv_mr *mr_host = ibv_reg_mr(pd, host, GPU_HOST_BYTES,
                                            IBV_ACCESS_LOCAL_WRITE);
        if (!mr_host) DIE("ibv_reg_mr(host)");

        /* THE GPUDirect moment: register device memory with the NIC. */
        struct ibv_mr *mr_gpu = ibv_reg_mr(pd, gpu_ring, gpu_bytes,
                                           IBV_ACCESS_LOCAL_WRITE);
        if (!mr_gpu) {
            fprintf(stderr,
                "FATAL: ibv_reg_mr on GPU memory failed: %s\n"
                "  GPUDirect RDMA needs the nvidia-peermem module:\n"
                "      sudo modprobe nvidia-peermem\n"
                "  then check:  lsmod | grep peermem\n"
                "  (see scripts/05_gpudirect_setup.sh; if it still fails,\n"
                "   add iommu=pt to the kernel cmdline and reboot)\n",
                strerror(errno));
            exit(1);
        }

        for (uint32_t s = 0; s < RING_SLOTS; s++)
            post_recv_slot(id, mr_host, mr_gpu, host, gpu_ring, s);

        struct rdma_conn_param cp = {
            .responder_resources = 1, .initiator_depth = 1,
            .rnr_retry_count = 7,
        };
        if (rdma_accept(id, &cp)) DIE("rdma_accept");
        if (rdma_get_cm_event(ec, &ev)) DIE("rdma_get_cm_event");
        if (ev->event != RDMA_CM_EVENT_ESTABLISHED) {
            fprintf(stderr, "rx   : connect failed: %s\n",
                    rdma_event_str(ev->event));
            rdma_ack_cm_event(ev);
            goto teardown;
        }
        rdma_ack_cm_event(ev);
        printf("rx   : sender connected (payloads -> GPU).\n");

        int ecflags = fcntl(ec->fd, F_GETFL);
        fcntl(ec->fd, F_SETFL, ecflags | O_NONBLOCK);

        uint64_t expect_seq = UINT64_MAX;
        uint64_t last = now_ns();
        uint64_t bytes_win = 0, frames_win = 0;
        int connected = 1;

        while (!g_stop && connected) {
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
                uint32_t slot = (uint32_t)wc[i].wr_id;
                struct frame_hdr *h = hdr_slot(host, slot);

                if (h->magic == IQ_FRAME_MAGIC) {
                    ctrl->sample_rate_hz = h->sample_rate_hz;
                    ctrl->center_freq_hz = h->center_freq_hz;
                    if (expect_seq != UINT64_MAX && h->seq != expect_seq) {
                        uint64_t miss = (h->seq > expect_seq)
                                      ? h->seq - expect_seq : 1;
                        __atomic_fetch_add(&ctrl->seq_gaps, miss,
                                           __ATOMIC_RELAXED);
                    }
                    expect_seq = h->seq + 1;
                    __atomic_fetch_add(&ctrl->bytes_total, wc[i].byte_len,
                                       __ATOMIC_RELAXED);
                    total_frames++;
                    __atomic_store_n(&ctrl->write_count, total_frames,
                                     __ATOMIC_RELEASE);
                    bytes_win += wc[i].byte_len;
                    frames_win++;
                }
                post_recv_slot(id, mr_host, mr_gpu, host, gpu_ring, slot);
            }

            struct rdma_cm_event *e2 = NULL;
            if (rdma_get_cm_event(ec, &e2) == 0) {
                if (e2->event == RDMA_CM_EVENT_DISCONNECTED ||
                    e2->event == RDMA_CM_EVENT_DEVICE_REMOVAL)
                    connected = 0;
                rdma_ack_cm_event(e2);
            }
            if (n == 0) {
                struct timespec idle = { 0, 20000 };
                nanosleep(&idle, NULL);
            }

            uint64_t t = now_ns();
            if (t - last >= 1000000000ull) {
                double secs = (double)(t - last) / 1e9;
                printf("rx   : %7.2f Gb/s -> GPU  %6.0f fps  total %" PRIu64
                       "  gaps %" PRIu64 "\n",
                       (double)bytes_win * 8.0 / secs / 1e9,
                       (double)frames_win / secs, total_frames,
                       __atomic_load_n(&ctrl->seq_gaps, __ATOMIC_RELAXED));
                bytes_win = frames_win = 0;
                last = t;
            }
        }

        fcntl(ec->fd, F_SETFL, ecflags);
        printf("rx   : sender disconnected; listening again.\n");
teardown:
        rdma_disconnect(id);
        rdma_destroy_qp(id);
        ibv_dereg_mr(mr_gpu);
        ibv_dereg_mr(mr_host);
        ibv_destroy_cq(cq);
        ibv_dealloc_pd(pd);
        rdma_destroy_id(id);
    }

    printf("rx   : exiting (GPU ring freed; visualizer should be stopped "
           "first or restarted).\n");
    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);
    munmap(host, GPU_HOST_BYTES);
    cudaFree(gpu_ring);
    return 0;
}
