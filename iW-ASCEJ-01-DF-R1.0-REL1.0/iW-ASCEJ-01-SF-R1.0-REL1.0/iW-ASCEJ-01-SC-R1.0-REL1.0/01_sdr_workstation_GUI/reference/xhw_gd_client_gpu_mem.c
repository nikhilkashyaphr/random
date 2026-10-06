/*
 * RDMA Client Application — GPUDirect, opcode-aware placement, multi-GB buffers
 * Ubuntu 22.04 / CUDA 12.x / MLNX_OFED / ConnectX-5 <-> Xilinx ERNIC
 *
 * MENU:
 *   A) Standard server-driven test (existing behavior): connect, receive
 *      params from the server, serve the requested READ/WRITE/SEND.
 *   B) Client -> FPGA incremental RDMA WRITE: the GPU produces an incrementing
 *      counter stream, which is staged and RDMA-WRITTEN to the FPGA's target
 *      (remote addr + rkey) for N iterations.
 *
 * ---------------------------------------------------------------------------
 * BUFFER PLACEMENT FIX (NAK syndrome 0x63 = Remote Operational Error on READ)
 * ---------------------------------------------------------------------------
 *   When the remote RDMA-READs our buffer (mode A read), OR when WE RDMA-WRITE
 *   (mode B), our NIC must DMA-*read* a local buffer. On this platform GPU and
 *   NIC are under different PCIe root ports with no shared switch, so the RC
 *   cannot route NIC->GPU non-posted reads -> the DMA fails -> NAK 0x63 (mode A)
 *   or a local WQE error (mode B). Posted NIC->GPU writes work, which is why
 *   inbound WRITE/SEND to a GPU buffer is fine.
 *
 *   Fix: any buffer the NIC must READ is placed in host-pinned memory (NIC
 *   reads DRAM, always routable). The GPU stays the producer/consumer via a
 *   device mirror + a staging hop. Placement for mode A keys on
 *   SERVER_READ_OPCODE; mode B forces host-pinned source unless -g 1.
 *
 * ---------------------------------------------------------------------------
 * MULTI-GB BUFFERS (4-5 GB)
 * ---------------------------------------------------------------------------
 *   - data_size is size_t (uint32_t wrapped at exactly 4 GiB).
 *   - Size math is size_t (the old `int * 1024 * 10` overflowed).
 *   - -s <MiB> forces a fixed buffer size locally (server caps transfer_size
 *     at 8192 -> 80 MB, so big buffers must be forced here).
 *   - Producer fill and RDMA WRITE are chunked at MAX_SGE_LEN (the IB SGE
 *     length is 32-bit and a single one-sided op tops out near 2 GiB).
 *
 *   HARD HARDWARE LIMIT for GPU placement at multi-GB:
 *     GPUDirect maps GPU memory through the GPU PCIe BAR1. If BAR1 < buffer,
 *     MR registration FAILS. Check:  nvidia-smi -q | grep -A3 -i bar1
 *     and enable Resizable BAR in BIOS for multi-GB GPU MRs. cudaMalloc itself
 *     succeeds on the 16 GB A4000; registration is the gate. Host-pinned is
 *     unaffected (needs `ulimit -l unlimited`).
 *
 * Placement policy:
 *   -g 0 (auto): mode A READ-served / mode B source -> HOST-PINNED, else GPU
 *   -g 1 (force GPU) / -g 2 (force host-pinned)
 *
 * Licensed under GNU General Public License (GPL) Version 2
 */

#define _GNU_SOURCE
#include <endian.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <semaphore.h>
#include <pthread.h>
#include <sched.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <rdma/rdma_cma.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <rdma/rdma_verbs.h>
#include <arpa/inet.h>

/* GPUDirect RDMA additions */
#include <cuda.h>            /* driver API: cuPointerSetAttribute, cuMemGetHandleForAddressRange */
#include <cuda_runtime.h>    /* runtime API: cudaMalloc, cudaHostAlloc, cudaMemcpy, ... */

/* Constants */
#define MAX_BUFFER_SIZE         (1 * 1024 * 1024)
#define DEFAULT_PORT            52525
#define MAX_IMAGES              10
#define RPING_BUFSIZE           (64 * 1024)
#define RPING_MIN_BUFSIZE       64
#define HH_MAX_QUEUE_DEPTH      16
#define HH_CQ_RQ_DEPTH          (8 * 1024 + 32)
#define HH_RX_REPOST_WQE_CNT    256
#define HH_SQ_DEPTH             1000
#define BYTES_PER_PIXEL         3
#define FILE_HEADER_SIZE        14
#define INFO_HEADER_SIZE        40
#define MAX_RETRY_COUNT         7
#define RESPONDER_RESOURCES     16
#define INITIATOR_DEPTH         16

/* Per-WQE / per-SGE max length. IB SGE length is 32-bit and a single one-sided
 * op is capped near 2 GiB; keep WQE chunks at 1 GiB to stay clear of both. */
#define MAX_SGE_LEN             (1u << 30)   /* 1 GiB */

/* Mode B default buffer size when -s is not given. */
#define MODE_B_DEFAULT_SIZE     (64u << 20)  /* 64 MiB */

/* Opcode value THIS server places in send_format.opcode for a READ-from-client
 * test. Confirmed = 0 from the wire capture (opcode=0 + NAK 0x63). Override at
 * build time with -DSERVER_READ_OPCODE=<n> if your server's encoding differs. */
#ifndef SERVER_READ_OPCODE
#define SERVER_READ_OPCODE 0
#endif

/* RDMA operation codes (client-side enum; may NOT match the server wire
 * values — placement keys on SERVER_READ_OPCODE, not these). */
enum hh_opcode {
    HH_RDMA_WRITE_OP,
    HH_RDMA_READ_OP,
    HH_RDMA_SEND_OP,
};

/* Data buffer placement */
enum data_buf_kind {
    DATA_BUF_GPU = 0,          /* cudaMalloc + GPUDirect MR (NIC P2P)        */
    DATA_BUF_HOST_PINNED = 1,  /* cudaHostAlloc + standard MR (NIC <-> DRAM) */
};

/* Test states */
enum test_state {
    STATE_IDLE = 1,
    STATE_CONNECT_REQUEST,
    STATE_ADDR_RESOLVED,
    STATE_ROUTE_RESOLVED,
    STATE_CONNECTED,
    STATE_RDMA_READ_ADV,
    STATE_RDMA_READ_COMPLETE,
    STATE_RDMA_WRITE_ADV,
    STATE_RDMA_WRITE_COMPLETE,
    STATE_DISCONNECTED,
    STATE_HW_HS_RECV_CMPLT,
    STATE_ERROR
};

/* RDMA info structure */
struct rdma_transfer_info {
    uint64_t buffer_address;
    uint32_t rkey;
    uint32_t size;
    uint32_t iteration_count;
    uint32_t qp_count;
} __attribute__((packed));

/* Send format structure */
struct send_format {
    int transfer_size;
    int opcode;
    int qp_count;
    int wqe_count;
};

/* Forward declarations */
struct rdma_context;
struct test_config;

/* Control block structure */
struct rdma_context {
    pthread_t cq_thread;
    struct ibv_comp_channel *comp_channel;
    struct ibv_cq *cq;
    struct ibv_pd *pd;
    struct ibv_qp *qp;

    struct ibv_recv_wr recv_wr;
    struct ibv_sge recv_sgl;
    struct send_format recv_buffer;
    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;

    struct ibv_send_wr send_wr;
    struct ibv_sge send_sgl;
    struct rdma_transfer_info send_buffer;
    struct ibv_send_wr rdma_send_wr;

    enum test_state state;
    enum test_state disconnect_state;
    sem_t semaphore;

    struct rdma_event_channel *cm_channel;
    struct rdma_cm_id *cm_id;

    int message_count;
    struct rdma_cm_event *event;
    struct ibv_wc *wc;

    volatile int cm_failed;
    volatile int cq_failed;
    volatile int cm_channel_enabled;
    volatile int cm_established;
    volatile int test_completed;
    volatile int disconnect_received;
    volatile int disconnect_check_enabled;
    volatile int poll_packet_count;

    unsigned int qp_number;
    void *data_buffer;
    size_t data_size;         /* widened from uint32_t (overflowed at 4 GiB) */
    uint32_t opcode;
    uint32_t wqe_count;
    struct ibv_mr *data_mr;
    int data_dmabuf_fd;       /* GPUDirect: DMA-BUF fd backing data_buffer (-1 otherwise) */
    int data_buf_kind;        /* DATA_BUF_GPU | DATA_BUF_HOST_PINNED */
    int remote_will_read;     /* 1 if the NIC must locally READ data_buffer */
    void *gpu_mirror;         /* device-side mirror when kind == HOST_PINNED */
    struct send_format hw_info;

    volatile int rx_wqe_ready;
    volatile int rq_depth;
    volatile uint32_t rx_completed;
    volatile uint32_t rx_posted;
    int wr_id;
};

/* Test information structure */
struct test_config {
    struct rdma_context *contexts;
    pthread_t cm_thread;
    pthread_t disconnect_thread;
    pthread_t cq_thread;
    pthread_t data_thread;
    int qp_count;
    int buffer_size;
    int iteration_count;
    int verbose;
    int validate;
    struct sockaddr_storage server_addr;
    struct sockaddr_storage source_addr;
    uint16_t port;
    int cq_initialized;
    volatile int completed_count;
    volatile int transfer_size;
};

/* Global test configuration */
static struct test_config test_config;
static int debug_enabled = 0;
static int g_buf_policy = 0;        /* 0=auto, 1=force GPU P2P, 2=force host  */
static size_t g_force_data_size = 0;/* bytes; 0 = derive size from server     */

/* Menu / mode-B globals */
static char     g_mode = 0;         /* 'A' or 'B'; 0 = ask interactively      */
static uint64_t g_remote_addr = 0;  /* mode B: FPGA target VA  (-R)           */
static uint32_t g_remote_rkey = 0;  /* mode B: FPGA target rkey (-K)          */
static int      g_iterations = 10;  /* mode B: number of write passes (-n)    */
static const char *g_dump_file = NULL; /* mode B: dump GPU buffer to file (-o) */

#define DEBUG_LOG(fmt, ...) \
    do { if (debug_enabled) printf("[DEBUG] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_LOG(fmt, ...) \
    printf("[INFO] " fmt, ##__VA_ARGS__)

/* =========================================================================
 *  GPUDirect RDMA support
 * ========================================================================= */
#ifndef GPUDIRECT_DMABUF
#define GPUDIRECT_DMABUF 0
#endif
#ifndef GPU_DEVICE_ID
#define GPU_DEVICE_ID 0
#endif

#define CUDA_CHECK(call)                                                       \
    do {                                                                       \
        cudaError_t _e = (call);                                               \
        if (_e != cudaSuccess) {                                               \
            fprintf(stderr, "CUDA error %s:%d: %s\n",                          \
                    __FILE__, __LINE__, cudaGetErrorString(_e));               \
            return -EFAULT;                                                    \
        }                                                                      \
    } while (0)

#ifdef WITH_GPU_KERNELS
/* Defined in gpu_validate.cu (compiled with nvcc, linked in). */
extern unsigned long long gpu_checksum(const void *gpu_ptr, size_t n);
extern int gpu_compare(const void *gpu_ptr, const void *host_expected, size_t n);
/* Optional on-GPU incrementing producer for mode B; supply in your .cu:
 *   void gpu_fill_incrementing(void *gpu_ptr, size_t n, unsigned long long base); */
extern void gpu_fill_incrementing(void *gpu_ptr, size_t n, unsigned long long base);
#endif

/* Initialize CUDA, bind the device, and report GPUDirect RDMA capability. */
static int gpu_init(void)
{
    CUresult cu = cuInit(0);
    if (cu != CUDA_SUCCESS) {
        const char *s = NULL; cuGetErrorString(cu, &s);
        fprintf(stderr, "cuInit failed: %s\n", s ? s : "?");
        return -EFAULT;
    }
    CUDA_CHECK(cudaSetDevice(GPU_DEVICE_ID));
    CUDA_CHECK(cudaFree(0));   /* force primary context creation now */

    struct cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, GPU_DEVICE_ID));
    int gdr = 0;
    cudaDeviceGetAttribute(&gdr, cudaDevAttrGPUDirectRDMASupported, GPU_DEVICE_ID);

    size_t free_b = 0, total_b = 0;
    cudaMemGetInfo(&free_b, &total_b);
    INFO_LOG("CUDA device %d = \"%s\" | GPUDirectRDMASupported=%d | gpu_reg_path=%s | "
             "buf_policy=%d | server_read_opcode=%d | gpu_mem free=%.2f/%.2f GiB\n",
             GPU_DEVICE_ID, prop.name, gdr,
             GPUDIRECT_DMABUF ? "dmabuf" : "nvidia-peermem",
             g_buf_policy, SERVER_READ_OPCODE,
             free_b / (1024.0*1024.0*1024.0), total_b / (1024.0*1024.0*1024.0));
    return 0;
}

/* ---- GPU placement: allocate in device memory, register for GPUDirect ---- */
static int alloc_and_register_gpu_p2p(struct rdma_context *ctx, int access)
{
    CUDA_CHECK(cudaMalloc(&ctx->data_buffer, ctx->data_size));
    CUDA_CHECK(cudaMemset(ctx->data_buffer, 0, ctx->data_size));

    unsigned int flag = 1;
    CUresult cu = cuPointerSetAttribute(&flag, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                                        (CUdeviceptr)ctx->data_buffer);
    if (cu != CUDA_SUCCESS) {
        const char *s = NULL; cuGetErrorString(cu, &s);
        fprintf(stderr, "cuPointerSetAttribute(SYNC_MEMOPS) failed: %s\n", s ? s : "?");
        cudaFree(ctx->data_buffer); ctx->data_buffer = NULL;
        return -EFAULT;
    }

#if GPUDIRECT_DMABUF
    CUdeviceptr dptr = (CUdeviceptr)ctx->data_buffer;
    cu = cuMemGetHandleForAddressRange(&ctx->data_dmabuf_fd, dptr, ctx->data_size,
                                       CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
    if (cu != CUDA_SUCCESS) {
        const char *s = NULL; cuGetErrorString(cu, &s);
        fprintf(stderr, "cuMemGetHandleForAddressRange(DMA_BUF_FD) failed: %s\n", s ? s : "?");
        cudaFree(ctx->data_buffer); ctx->data_buffer = NULL;
        return -EFAULT;
    }
    ctx->data_mr = ibv_reg_dmabuf_mr(ctx->cm_id->pd, 0 /*offset*/, ctx->data_size,
                                     (uint64_t)dptr /*iova*/, ctx->data_dmabuf_fd, access);
#else
    ctx->data_mr = ibv_reg_mr(ctx->cm_id->pd, ctx->data_buffer, ctx->data_size, access);
#endif

    if (!ctx->data_mr) {
        fprintf(stderr, "Failed to register GPU data MR (errno=%d: %s).\n", errno, strerror(errno));
        fprintf(stderr, "  Multi-GB GPU MR? Check BAR1 >= buffer:  nvidia-smi -q | grep -A3 -i bar1\n");
        fprintf(stderr, "  Enable Resizable BAR in BIOS, or use -g 2 (host-pinned). "
                "peermem loaded? dmabuf supported?\n");
        if (ctx->data_dmabuf_fd >= 0) { close(ctx->data_dmabuf_fd); ctx->data_dmabuf_fd = -1; }
        cudaFree(ctx->data_buffer); ctx->data_buffer = NULL;
        return -EFAULT;
    }

    CUmemorytype mtype = (CUmemorytype)0;
    cu = cuPointerGetAttribute(&mtype, CU_POINTER_ATTRIBUTE_MEMORY_TYPE,
                               (CUdeviceptr)ctx->data_buffer);
    INFO_LOG("Data buffer [GPU-P2P]: gpu_ptr=%p size=%zu (%.2f GiB) lkey=0x%x rkey=0x%x "
             "mem_type=%s [%s]\n",
             ctx->data_buffer, ctx->data_size,
             ctx->data_size / (1024.0*1024.0*1024.0),
             ctx->data_mr->lkey, ctx->data_mr->rkey,
             (cu == CUDA_SUCCESS && mtype == CU_MEMORYTYPE_DEVICE) ? "DEVICE" : "NON-DEVICE!",
             GPUDIRECT_DMABUF ? "dmabuf" : "peermem");

    if (ctx->remote_will_read)
        INFO_LOG("WARNING: GPU-P2P placement while the NIC must locally READ this buffer "
                 "(mode A read / mode B write source). Expect failure on this PCIe "
                 "topology; use -g 2 unless you have a shared PCIe switch.\n");
    return 0;
}

/* ---- Host placement: pinned bounce buffer + GPU mirror ---- */
static int alloc_and_register_host_pinned(struct rdma_context *ctx, int access)
{
    CUDA_CHECK(cudaHostAlloc(&ctx->data_buffer, ctx->data_size, cudaHostAllocDefault));
    memset(ctx->data_buffer, 0, ctx->data_size);

    CUDA_CHECK(cudaMalloc(&ctx->gpu_mirror, ctx->data_size));
    CUDA_CHECK(cudaMemset(ctx->gpu_mirror, 0, ctx->data_size));

    ctx->data_mr = ibv_reg_mr(ctx->cm_id->pd, ctx->data_buffer, ctx->data_size, access);
    if (!ctx->data_mr) {
        fprintf(stderr, "Failed to register host-pinned data MR (errno=%d: %s)\n",
                errno, strerror(errno));
        cudaFree(ctx->gpu_mirror);      ctx->gpu_mirror = NULL;
        cudaFreeHost(ctx->data_buffer); ctx->data_buffer = NULL;
        return -EFAULT;
    }

    INFO_LOG("Data buffer [HOST-PINNED]: host_ptr=%p mirror=%p size=%zu (%.2f GiB) "
             "lkey=0x%x rkey=0x%x\n",
             ctx->data_buffer, ctx->gpu_mirror, ctx->data_size,
             ctx->data_size / (1024.0*1024.0*1024.0),
             ctx->data_mr->lkey, ctx->data_mr->rkey);
    return 0;
}

/* Allocate + register the bulk RDMA data buffer per ctx->data_buf_kind. */
static int alloc_and_register_data_buffer(struct rdma_context *ctx)
{
    const int access = IBV_ACCESS_LOCAL_WRITE |
                       IBV_ACCESS_REMOTE_READ |
                       IBV_ACCESS_REMOTE_WRITE;

    ctx->data_dmabuf_fd = -1;
    ctx->gpu_mirror = NULL;
    CUDA_CHECK(cudaSetDevice(GPU_DEVICE_ID));

    if (ctx->data_buf_kind == DATA_BUF_HOST_PINNED)
        return alloc_and_register_host_pinned(ctx, access);
    return alloc_and_register_gpu_p2p(ctx, access);
}

/* HOST_PINNED kind: push GPU-produced data into the NIC-visible MR. */
static int stage_gpu_to_host(struct rdma_context *ctx)
{
    if (ctx->data_buf_kind != DATA_BUF_HOST_PINNED || !ctx->gpu_mirror)
        return 0;
    CUDA_CHECK(cudaMemcpy(ctx->data_buffer, ctx->gpu_mirror, ctx->data_size,
                          cudaMemcpyDeviceToHost));
    return 0;
}

/* HOST_PINNED kind: pull NIC-written data up to the GPU mirror. */
static int stage_host_to_gpu(struct rdma_context *ctx)
{
    if (ctx->data_buf_kind != DATA_BUF_HOST_PINNED || !ctx->gpu_mirror)
        return 0;
    CUDA_CHECK(cudaMemcpy(ctx->gpu_mirror, ctx->data_buffer, ctx->data_size,
                          cudaMemcpyHostToDevice));
    return 0;
}

/* The GPU side of whichever placement we chose (producer/consumer target). */
static inline void *gpu_side_ptr(struct rdma_context *ctx)
{
    return (ctx->data_buf_kind == DATA_BUF_HOST_PINNED)
           ? ctx->gpu_mirror : ctx->data_buffer;
}

/* Produce an incrementing pattern on the GPU. `base` continues across calls so
 * successive iterations carry a monotonically increasing counter.
 * Default fills via a chunked host->device copy; replace with your real on-GPU
 * producer kernel (see gpu_fill_incrementing hook) for a true device producer. */
static int produce_incremental_on_gpu(struct rdma_context *ctx, unsigned long long base)
{
    void *gpu_dst = gpu_side_ptr(ctx);

#ifdef WITH_GPU_KERNELS
    /* Real on-GPU generation (no host involvement). */
    gpu_fill_incrementing(gpu_dst, ctx->data_size, base);
    cudaError_t es = cudaDeviceSynchronize();
    if (es != cudaSuccess) {
        fprintf(stderr, "gpu_fill_incrementing sync failed: %s\n", cudaGetErrorString(es));
        return -EFAULT;
    }
    return 0;
#else
    const size_t CHUNK = 64u << 20;   /* 64 MiB host staging window */
    uint8_t *tmp = (uint8_t *)malloc(CHUNK);
    if (!tmp)
        return -ENOMEM;
    size_t off = 0;
    while (off < ctx->data_size) {
        size_t n = ctx->data_size - off;
        if (n > CHUNK) n = CHUNK;
        for (size_t i = 0; i < n; i++)
            tmp[i] = (uint8_t)((base + off + i) & 0xff);
        cudaError_t e = cudaMemcpy((uint8_t *)gpu_dst + off, tmp, n,
                                   cudaMemcpyHostToDevice);
        if (e != cudaSuccess) {
            free(tmp);
            fprintf(stderr, "producer H2D failed at off=%zu: %s\n",
                    off, cudaGetErrorString(e));
            return -EFAULT;
        }
        off += n;
    }
    free(tmp);
    return 0;
#endif
}

/* Hexdump helpers. */
static void hexdump_bytes(const unsigned char *h, size_t n, const char *tag,
                          const void *src)
{
    printf("[BUF-DUMP] %s  ptr=%p  bytes=%zu\n", tag, src, n);
    for (size_t i = 0; i < n; i++) {
        printf("%02x ", h[i]);
        if ((i & 15) == 15) printf("\n");
    }
    if (n & 15) printf("\n");
}

static void dump_gpu_buffer(const void *gpu_ptr, size_t n, const char *tag)
{
    unsigned char *h = (unsigned char *)malloc(n);
    if (!h) return;
    cudaError_t e = cudaMemcpy(h, gpu_ptr, n, cudaMemcpyDeviceToHost);
    if (e != cudaSuccess) {
        fprintf(stderr, "dump_gpu_buffer D2H failed: %s\n", cudaGetErrorString(e));
        free(h);
        return;
    }
    hexdump_bytes(h, n, tag, gpu_ptr);
    free(h);
}

static void dump_data_buffer(struct rdma_context *ctx, size_t n, const char *tag)
{
    if (!ctx->data_buffer || n > ctx->data_size)
        return;
    if (ctx->data_buf_kind == DATA_BUF_HOST_PINNED)
        hexdump_bytes((unsigned char *)ctx->data_buffer, n, tag, ctx->data_buffer);
    else
        dump_gpu_buffer(ctx->data_buffer, n, tag);
}

/* Function forward declarations */
static int handle_client_data_transfer(struct rdma_context *ctx);
static int poll_completion_queue(struct rdma_context *ctx, int max_completions);
static int post_receive_wqes(struct rdma_context *ctx, size_t size, int count);
static int wait_for_completion(struct rdma_context *ctx, int wr_id);
static int handle_cm_event(struct rdma_cm_id *cm_id, struct rdma_cm_event *event);
static void *cq_thread_function(void *arg);
static void *data_thread_function(void *arg);
static void *disconnect_thread_function(void *arg);
static void *cm_thread_function(void *arg);
static int setup_queue_pair(struct rdma_context *ctx, struct rdma_cm_id *cm_id);
static void cleanup_queue_pair(struct rdma_context *ctx);
static int bind_and_resolve(struct rdma_context *ctx);
static int run_mode_a(void);
static int run_mode_b(void);

/* Round value to 0-255 range */
static inline int clamp_value(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

/* Generate bitmap file header */
static void create_bitmap_file_header(unsigned char *header, int height, int stride)
{
    int file_size = FILE_HEADER_SIZE + INFO_HEADER_SIZE + (stride * height);
    memset(header, 0, FILE_HEADER_SIZE);
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (unsigned char)(file_size);
    header[3] = (unsigned char)(file_size >> 8);
    header[4] = (unsigned char)(file_size >> 16);
    header[5] = (unsigned char)(file_size >> 24);
    header[10] = (unsigned char)(FILE_HEADER_SIZE + INFO_HEADER_SIZE);
}

/* Generate bitmap info header */
static void create_bitmap_info_header(unsigned char *header, int height, int width)
{
    memset(header, 0, INFO_HEADER_SIZE);
    header[0] = (unsigned char)(INFO_HEADER_SIZE);
    header[4] = (unsigned char)(width);
    header[5] = (unsigned char)(width >> 8);
    header[6] = (unsigned char)(width >> 16);
    header[7] = (unsigned char)(width >> 24);
    header[8] = (unsigned char)(height);
    header[9] = (unsigned char)(height >> 8);
    header[10] = (unsigned char)(height >> 16);
    header[11] = (unsigned char)(height >> 24);
    header[12] = (unsigned char)(1);
    header[14] = (unsigned char)(BYTES_PER_PIXEL * 8);
}

/* Generate bitmap image from pixel data */
static void generate_bitmap_image(unsigned char ***image, int height, int width,
                                  const char *filename)
{
    int width_in_bytes = width * BYTES_PER_PIXEL;
    int padding_size = (4 - (width_in_bytes % 4)) % 4;
    int stride = width_in_bytes + padding_size;

    FILE *file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Failed to open file: %s\n", filename);
        return;
    }

    unsigned char file_header[FILE_HEADER_SIZE];
    unsigned char info_header[INFO_HEADER_SIZE];
    unsigned char padding[3] = {0, 0, 0};

    create_bitmap_file_header(file_header, height, stride);
    create_bitmap_info_header(info_header, height, width);

    fwrite(file_header, 1, FILE_HEADER_SIZE, file);
    fwrite(info_header, 1, INFO_HEADER_SIZE, file);

    for (int i = 0; i < height; i++) {
        fwrite(image[i][0], BYTES_PER_PIXEL, width, file);
        fwrite(padding, 1, padding_size, file);
    }

    fclose(file);
    INFO_LOG("Bitmap image generated: %s\n", filename);
}

/* Poll completion queue */
static int poll_completion_queue(struct rdma_context *ctx, int max_completions)
{
    int ret;
    while (1) {
        ret = ibv_poll_cq(ctx->cq, max_completions, ctx->wc);
        if (ret < 0) {
            fprintf(stderr, "CQ poll failed: %d\n", ret);
            return -1;
        }
        if (ret > 0) {
            DEBUG_LOG("CQ poll: Retrieved %d completions\n", ret);
            for (int i = 0; i < ret && i < 3; i++) {
                DEBUG_LOG("  WC[%d]: wr_id=%lu, status=%d, opcode=%d, len=%u\n",
                         i, ctx->wc[i].wr_id, ctx->wc[i].status,
                         ctx->wc[i].opcode, ctx->wc[i].byte_len);
            }
            return ret;
        }
    }
}

/* Post receive work queue entries (SEND path). Per-WQE length clamped to
 * MAX_SGE_LEN (ibv_sge.length is 32-bit). */
static int post_receive_wqes(struct rdma_context *ctx, size_t size, int count)
{
    int posted = 0;
    uint32_t available_wqes = ctx->rq_depth - ctx->rx_posted + ctx->rx_completed;

    if (available_wqes < (uint32_t)count) {
        count = available_wqes;
    }

    if (size > MAX_SGE_LEN) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "post_receive_wqes: clamping per-WQE size %zu -> %u "
                    "(SGE length is 32-bit; SEND receives in chunks)\n",
                    size, (unsigned)MAX_SGE_LEN);
        }
        size = MAX_SGE_LEN;
    }

    DEBUG_LOG("Posting %d receive WQEs (buffer=%p, size=%zu, lkey=0x%x)\n",
              count, ctx->data_buffer, size, ctx->data_mr->lkey);

    for (int i = 0; i < count; i++) {
        int ret = rdma_post_recv(ctx->cm_id,
                                 (void *)(uintptr_t)(ctx->rx_posted + i),
                                 ctx->data_buffer, (uint32_t)size, ctx->data_mr);
        if (ret < 0) {
            fprintf(stderr, "Post receive failed at WQE %d\n", i);
            break;
        }
        posted++;
    }

    ctx->rx_posted += posted;
    return posted;
}

/* Wait for specific completion */
static int wait_for_completion(struct rdma_context *ctx, int wr_id)
{
    struct ibv_wc wc;
    int ret;

    while (1) {
        ret = ibv_poll_cq(ctx->cq, 1, &wc);
        if (ret < 0) {
            fprintf(stderr, "CQ poll error\n");
            return -1;
        }
        if (ret == 0) {
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr,
                    "Work completion error: %s (status=%d, opcode=%d, "
                    "vendor_err=0x%x, wr_id=%" PRIu64 ", qp_num=0x%x)\n",
                    ibv_wc_status_str(wc.status), wc.status, wc.opcode,
                    wc.vendor_err, wc.wr_id, wc.qp_num);
            return -1;
        }
        if (wc.wr_id == (uint64_t)wr_id) {
            break;
        }
    }

    return 0;
}

/* Handle RDMA CM events */
static int handle_cm_event(struct rdma_cm_id *cm_id, struct rdma_cm_event *event)
{
    struct rdma_context *ctx = cm_id->context;
    int ret = 0;

    DEBUG_LOG("CM event: %s, cm_id: %p\n", rdma_event_str(event->event), cm_id);

    switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
        ctx->state = STATE_ADDR_RESOLVED;
        ret = rdma_resolve_route(cm_id, 2000);
        if (ret) {
            ctx->state = STATE_ERROR;
            sem_post(&ctx->semaphore);
        }
        break;
    case RDMA_CM_EVENT_ROUTE_RESOLVED:
        ctx->state = STATE_ROUTE_RESOLVED;
        sem_post(&ctx->semaphore);
        break;
    case RDMA_CM_EVENT_CONNECT_REQUEST:
        ctx->state = STATE_CONNECT_REQUEST;
        sem_post(&ctx->semaphore);
        break;
    case RDMA_CM_EVENT_ESTABLISHED:
        DEBUG_LOG("Connection established for QP #%d\n", cm_id->qp->qp_num);
        ctx->state = STATE_CONNECTED;
        ctx->cm_channel_enabled = 0;
        sem_post(&ctx->semaphore);
        break;
    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
        fprintf(stderr, "CM event error: %s, status: %d\n",
                rdma_event_str(event->event), event->status);
        sem_post(&ctx->semaphore);
        ret = -1;
        break;
    case RDMA_CM_EVENT_DISCONNECTED:
        ctx->state = STATE_DISCONNECTED;
        ctx->disconnect_state = STATE_DISCONNECTED;
        test_config.completed_count++;
        sem_post(&ctx->semaphore);
        break;
    case RDMA_CM_EVENT_DEVICE_REMOVAL:
        fprintf(stderr, "Device removal detected\n");
        ctx->state = STATE_ERROR;
        sem_post(&ctx->semaphore);
        ret = -1;
        break;
    default:
        DEBUG_LOG("Unhandled event: %s\n", rdma_event_str(event->event));
        break;
    }

    return ret;
}

/* Setup work requests */
static int setup_work_requests(struct rdma_context *ctx)
{
    INFO_LOG("Setting up work requests\n");

    ctx->recv_mr = ibv_reg_mr(ctx->pd, &ctx->recv_buffer,
                              sizeof(ctx->recv_buffer),
                              IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->recv_mr) {
        fprintf(stderr, "Failed to register receive MR\n");
        return -EFAULT;
    }

    ctx->send_mr = ibv_reg_mr(ctx->pd, &ctx->send_buffer,
                              sizeof(ctx->send_buffer), 0);
    if (!ctx->send_mr) {
        fprintf(stderr, "Failed to register send MR\n");
        ibv_dereg_mr(ctx->recv_mr);
        return -EFAULT;
    }

    ctx->recv_sgl.addr = (uint64_t)(uintptr_t)&ctx->recv_buffer;
    ctx->recv_sgl.length = sizeof(ctx->recv_buffer);
    ctx->recv_sgl.lkey = ctx->recv_mr->lkey;
    ctx->recv_wr.sg_list = &ctx->recv_sgl;
    ctx->recv_wr.num_sge = 1;

    ctx->send_sgl.addr = (uint64_t)(uintptr_t)&ctx->send_buffer;
    ctx->send_sgl.length = sizeof(ctx->send_buffer);
    ctx->send_sgl.lkey = ctx->send_mr->lkey;
    ctx->send_wr.opcode = IBV_WR_SEND;
    ctx->send_wr.send_flags = IBV_SEND_SIGNALED;
    ctx->send_wr.sg_list = &ctx->send_sgl;
    ctx->send_wr.num_sge = 1;

    return 0;
}

/* Setup buffers and post initial receive */
static int setup_buffers(struct rdma_context *ctx)
{
    int ret;
    ret = setup_work_requests(ctx);
    if (ret) {
        return ret;
    }

    ctx->wr_id = rand();
    ret = rdma_post_recv(ctx->cm_id, (void *)(uintptr_t)ctx->wr_id,
                         &ctx->recv_buffer, sizeof(ctx->recv_buffer),
                         ctx->recv_mr);
    if (ret < 0) {
        fprintf(stderr, "Failed to post initial receive\n");
        return ret;
    }

    DEBUG_LOG("Buffers allocated and registered\n");
    return 0;
}

/* Create queue pair */
static int create_queue_pair(struct rdma_context *ctx)
{
    struct ibv_qp_init_attr init_attr;
    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cap.max_send_wr = HH_SQ_DEPTH;
    init_attr.cap.max_recv_wr = HH_CQ_RQ_DEPTH;
    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_send_sge = 1;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.send_cq = ctx->cq;
    init_attr.recv_cq = ctx->cq;

    INFO_LOG("Creating QP (max_send=%d, max_recv=%d)\n",
             init_attr.cap.max_send_wr, init_attr.cap.max_recv_wr);

    int ret = rdma_create_qp(ctx->cm_id, ctx->pd, &init_attr);
    if (!ret) {
        ctx->qp = ctx->cm_id->qp;
        INFO_LOG("QP created: qp_num=%d\n", ctx->qp->qp_num);
    }
    return ret;
}

/* Setup queue pair resources */
static int setup_queue_pair(struct rdma_context *ctx, struct rdma_cm_id *cm_id)
{
    int ret;
    ctx->pd = ibv_alloc_pd(cm_id->verbs);
    if (!ctx->pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        return errno;
    }

    ctx->comp_channel = ibv_create_comp_channel(cm_id->verbs);
    if (!ctx->comp_channel) {
        fprintf(stderr, "Failed to create completion channel\n");
        ret = errno;
        goto cleanup_pd;
    }

    ctx->cq = ibv_create_cq(cm_id->verbs, HH_CQ_RQ_DEPTH, NULL,
                            ctx->comp_channel, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Failed to create CQ\n");
        ret = errno;
        goto cleanup_channel;
    }

    ctx->rq_depth = HH_CQ_RQ_DEPTH;

    ret = ibv_req_notify_cq(ctx->cq, 0);
    if (ret) {
        fprintf(stderr, "Failed to request CQ notification\n");
        goto cleanup_cq;
    }

    ret = create_queue_pair(ctx);
    if (ret) {
        fprintf(stderr, "Failed to create QP\n");
        goto cleanup_cq;
    }

    return 0;

cleanup_cq:
    ibv_destroy_cq(ctx->cq);
cleanup_channel:
    ibv_destroy_comp_channel(ctx->comp_channel);
cleanup_pd:
    ibv_dealloc_pd(ctx->pd);
    return ret;
}

/* Cleanup queue pair resources */
static void cleanup_queue_pair(struct rdma_context *ctx)
{
    if (ctx->qp)            ibv_destroy_qp(ctx->qp);
    if (ctx->cq)            ibv_destroy_cq(ctx->cq);
    if (ctx->comp_channel)  ibv_destroy_comp_channel(ctx->comp_channel);
    if (ctx->pd)            ibv_dealloc_pd(ctx->pd);
}

/* CQ processing thread */
static void *cq_thread_function(void *arg)
{
    struct rdma_context *ctx = (struct rdma_context *)arg;
    while (!ctx->rx_wqe_ready) {
        sched_yield();
    }
    DEBUG_LOG("CQ thread started\n");
    while (1) {
        if (!ctx->rx_posted && !ctx->rx_completed) {
            sched_yield();
        }
        if (!ctx->cq_failed && ctx->cm_established) {
            int ret = poll_completion_queue(ctx, HH_RX_REPOST_WQE_CNT);
            if (ret > 0) {
                ctx->rx_completed += ret;
                DEBUG_LOG("Total completions: %d\n", ctx->rx_completed);
                post_receive_wqes(ctx, ctx->data_size, HH_RX_REPOST_WQE_CNT);
            }
        }
    }
    return NULL;
}

/* Format send buffer with RDMA info */
static void format_send_buffer(struct rdma_context *ctx, void *buffer,
                               struct ibv_mr *mr)
{
    struct rdma_transfer_info *info = &ctx->send_buffer;
    info->buffer_address = htobe64((uint64_t)(uintptr_t)buffer);
    info->rkey = htobe32(mr->rkey);
    info->size = htobe32(test_config.buffer_size);
    info->iteration_count = htobe32(ctx->message_count);
    info->qp_count = htobe32(test_config.qp_count);

    DEBUG_LOG("RDMA info: addr=0x%" PRIx64 ", rkey=0x%x, size=%d\n",
              be64toh(info->buffer_address), be32toh(info->rkey),
              be32toh(info->size));
}

/* Handle client data transfer (mode A) */
static int handle_client_data_transfer(struct rdma_context *ctx)
{
    int ret;
    int wr_id = ctx->wr_id;
    uint32_t data_start_offset = 0;

    INFO_LOG("Starting data transfer sequence\n");

    ret = wait_for_completion(ctx, wr_id);
    if (ret < 0) {
        fprintf(stderr, "Failed waiting for initial completion\n");
        return ret;
    }

    struct send_format *recv_data = &ctx->recv_buffer;
    INFO_LOG("Received parameters: size=%dkB, opcode=%d, QP count=%d\n",
             recv_data->transfer_size, recv_data->opcode, recv_data->qp_count);
 
    //if (!recv_data->transfer_size || recv_data->transfer_size > 8192) {
    if (!recv_data->transfer_size || recv_data->transfer_size > 8388608) {
        fprintf(stderr, "Invalid transfer size: %d\n", recv_data->transfer_size);
        return -EINVAL;
    }

    if (recv_data->transfer_size != 4050) {
        ctx->data_size = (size_t)recv_data->transfer_size * 1024 * 10;
    } else {
        data_start_offset = 64;
        ctx->data_size = ((size_t)recv_data->transfer_size * 1024 + data_start_offset) * 10;
    }

    if (g_force_data_size) {
        ctx->data_size = g_force_data_size;
        INFO_LOG("Override: forcing data_size to %zu bytes (%.2f GiB) via -s\n",
                 ctx->data_size, ctx->data_size / (1024.0*1024.0*1024.0));
    }

    /* Placement: READ-served -> host-pinned (NIC reads DRAM); else GPU P2P. */
    ctx->remote_will_read = (recv_data->opcode == SERVER_READ_OPCODE);
    switch (g_buf_policy) {
    case 1: ctx->data_buf_kind = DATA_BUF_GPU;         break;
    case 2: ctx->data_buf_kind = DATA_BUF_HOST_PINNED; break;
    default:
        ctx->data_buf_kind = ctx->remote_will_read
                             ? DATA_BUF_HOST_PINNED : DATA_BUF_GPU;
        break;
    }

    INFO_LOG("Allocating data buffer: %zu bytes (%.2f GiB), placement=%s, "
             "remote_will_read=%d (opcode=%d vs server_read_opcode=%d)\n",
             ctx->data_size, ctx->data_size / (1024.0*1024.0*1024.0),
             ctx->data_buf_kind == DATA_BUF_GPU ? "GPU-P2P" : "HOST-PINNED",
             ctx->remote_will_read, recv_data->opcode, SERVER_READ_OPCODE);

    ret = alloc_and_register_data_buffer(ctx);
    if (ret) {
        fprintf(stderr, "Failed to allocate/register data buffer\n");
        return ret;
    }

    if (ctx->remote_will_read) {
        /* READ source must be valid before we advertise (addr, rkey). */
        ret = produce_incremental_on_gpu(ctx, 0);
        if (!ret) ret = stage_gpu_to_host(ctx);
        if (ret) {
            fprintf(stderr, "Failed to prepare READ source data\n");
            return ret;
        }
        if (debug_enabled)
            dump_data_buffer(ctx, 64, "first 64B of READ source (pre-advertise)");
    }

    format_send_buffer(ctx, ctx->data_buffer, ctx->data_mr);

    if (recv_data->opcode == HH_RDMA_SEND_OP) {
        ctx->wc = calloc(HH_RX_REPOST_WQE_CNT, sizeof(struct ibv_wc));
        if (!ctx->wc) {
            fprintf(stderr, "Failed to allocate WC array\n");
            return -ENOMEM;
        }
        ret = pthread_create(&ctx->cq_thread, NULL, cq_thread_function, ctx);
        if (ret) {
            fprintf(stderr, "Failed to create CQ thread\n");
            return ret;
        }
        ret = post_receive_wqes(ctx, ctx->data_size, ctx->rq_depth);
        if (ret < 0) {
            fprintf(stderr, "Failed to post receive WQEs\n");
            return ret;
        }
    }

    ctx->opcode = recv_data->opcode;

    INFO_LOG("Sending RDMA info (wr_id=%d)\n", wr_id);
    ret = rdma_post_send(ctx->cm_id, (void *)(uintptr_t)wr_id,
                         &ctx->send_buffer, sizeof(ctx->send_buffer),
                         ctx->send_mr, IBV_SEND_SIGNALED);
    if (ret < 0) {
        fprintf(stderr, "Send failed\n");
        return ret;
    }

    ret = wait_for_completion(ctx, wr_id);
    if (ret < 0) {
        fprintf(stderr, "Send completion failed\n");
        return ret;
    }

    ctx->rx_wqe_ready = 1;
    INFO_LOG("Client ready for data transfer\n");
    return 0;
}

/* Data transfer thread (mode A) */
static void *data_thread_function(void *arg)
{
    struct test_config *config = (struct test_config *)arg;
    while (1) {
        for (int i = 0; i < config->qp_count; i++) {
            struct rdma_context *ctx = &config->contexts[i];
            if (ctx->cm_established && !ctx->test_completed) {
                int ret = handle_client_data_transfer(ctx);
                if (!ret) {
                    INFO_LOG("Data exchange completed for QP #%d\n", i);
                }
                ctx->test_completed = 1;
                ctx->cm_channel_enabled = 0;
            }
        }
        sched_yield();
    }
    return NULL;
}

/* Disconnect monitoring thread (mode A) */
static void *disconnect_thread_function(void *arg)
{
    struct test_config *config = (struct test_config *)arg;
    int connected_count = 0;
    while (1) {
        for (int i = 0; i < config->qp_count; i++) {
            struct rdma_context *ctx = &config->contexts[i];
            if (!ctx->disconnect_received && ctx->disconnect_check_enabled) {
                int ret = rdma_get_cm_event(ctx->cm_channel, &ctx->event);
                if (ret) {
                    fprintf(stderr, "Failed to get CM event\n");
                    ctx->disconnect_received = 1;
                    continue;
                }
                ret = handle_cm_event(ctx->event->id, ctx->event);
                rdma_ack_cm_event(ctx->event);
                if (ret) {
                    ctx->disconnect_received = 1;
                }
                if (ctx->disconnect_state == STATE_DISCONNECTED) {
                    ctx->disconnect_received = 1;
                    connected_count++;
                    if (connected_count >= config->qp_count) {
                        return NULL;
                    }
                }
            }
        }
    }
    return NULL;
}

/* Connection management thread (mode A) */
static void *cm_thread_function(void *arg)
{
    struct test_config *config = (struct test_config *)arg;
    int connected_count = 0;
    while (1) {
        for (int i = 0; i < config->qp_count; i++) {
            struct rdma_context *ctx = &config->contexts[i];
            if (!ctx->cm_failed && ctx->cm_channel_enabled) {
                int ret = rdma_get_cm_event(ctx->cm_channel, &ctx->event);
                if (ret) {
                    fprintf(stderr, "Failed to get CM event\n");
                    ctx->cm_failed = 1;
                    continue;
                }
                ret = handle_cm_event(ctx->event->id, ctx->event);
                rdma_ack_cm_event(ctx->event);
                if (ret) {
                    ctx->cm_failed = 1;
                }
                if (ctx->state == STATE_CONNECTED) {
                    connected_count++;
                    ctx->disconnect_check_enabled = 1;
                    if (connected_count >= config->qp_count) {
                        return NULL;
                    }
                }
            }
        }
    }
    return NULL;
}

/* Connect to server (mode A; relies on cm_thread to post the semaphore) */
static int connect_to_server(struct rdma_context *ctx)
{
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.responder_resources = RESPONDER_RESOURCES;
    conn_param.initiator_depth = INITIATOR_DEPTH;
    conn_param.retry_count = MAX_RETRY_COUNT;

    INFO_LOG("Connecting (QP #%d)\n", ctx->cm_id->qp->qp_num);

    int ret = rdma_connect(ctx->cm_id, &conn_param);
    if (ret) {
        perror("rdma_connect");
        return ret;
    }

    sem_wait(&ctx->semaphore);
    if (ctx->state != STATE_CONNECTED) {
        fprintf(stderr, "Connection failed: state=%d\n", ctx->state);
        return -1;
    }

    ctx->cm_established = 1;
    INFO_LOG("Connection established\n");
    return 0;
}

/* Bind and resolve address (mode A; relies on cm_thread to post the semaphore) */
static int bind_and_resolve(struct rdma_context *ctx)
{
    int ret;
    if (test_config.server_addr.ss_family == AF_INET) {
        ((struct sockaddr_in *)&test_config.server_addr)->sin_port = test_config.port;
    } else {
        ((struct sockaddr_in6 *)&test_config.server_addr)->sin6_port = test_config.port;
    }

    if (test_config.source_addr.ss_family) {
        ret = rdma_resolve_addr(ctx->cm_id,
                                (struct sockaddr *)&test_config.source_addr,
                                (struct sockaddr *)&test_config.server_addr, 2000);
    } else {
        ret = rdma_resolve_addr(ctx->cm_id, NULL,
                                (struct sockaddr *)&test_config.server_addr, 2000);
    }
    if (ret) {
        fprintf(stderr, "Address resolution failed\n");
        return ret;
    }

    sem_wait(&ctx->semaphore);
    if (ctx->state != STATE_ROUTE_RESOLVED) {
        fprintf(stderr, "Route resolution failed: state=%d\n", ctx->state);
        return -1;
    }
    return 0;
}

/* Setup connection (mode A) */
static int setup_connection(struct rdma_context *ctx)
{
    int ret;
    ctx->cm_channel = rdma_create_event_channel();
    if (!ctx->cm_channel) {
        fprintf(stderr, "Failed to create event channel\n");
        return -EFAULT;
    }
    ret = rdma_create_id(ctx->cm_channel, &ctx->cm_id, ctx, RDMA_PS_TCP);
    if (ret) {
        fprintf(stderr, "Failed to create CM ID\n");
        return -EFAULT;
    }
    ctx->cm_channel_enabled = 1;

    ret = bind_and_resolve(ctx);
    if (ret) return ret;

    ret = setup_queue_pair(ctx, ctx->cm_id);
    if (ret) return ret;

    ret = setup_buffers(ctx);
    if (ret) goto cleanup;

    ret = connect_to_server(ctx);
    if (ret) {
        fprintf(stderr, "Connection failed\n");
        goto cleanup;
    }
    return 0;

cleanup:
    cleanup_queue_pair(ctx);
    return ret;
}

/* Resolve hostname/address */
static int resolve_address(const char *address, struct sockaddr *addr)
{
    struct addrinfo *res;
    int ret;
    ret = getaddrinfo(address, NULL, NULL, &res);
    if (ret) {
        fprintf(stderr, "Address resolution failed: %s\n", gai_strerror(ret));
        return ret;
    }
    if (res->ai_family == PF_INET) {
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
    } else if (res->ai_family == PF_INET6) {
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
    } else {
        ret = -1;
    }
    freeaddrinfo(res);
    return ret;
}

/* ============================== MODE B ================================== *
 *  Client -> FPGA incremental RDMA WRITE. Synchronous CM bring-up (no
 *  worker threads), then an N-iteration produce/stage/write loop.
 * ======================================================================= */

/* Pull one CM event and require it to be `expected`; ack it. */
static int wait_for_cm_event(struct rdma_context *ctx,
                             enum rdma_cm_event_type expected)
{
    struct rdma_cm_event *ev = NULL;
    int ret = rdma_get_cm_event(ctx->cm_channel, &ev);
    if (ret) {
        fprintf(stderr, "rdma_get_cm_event failed: %s\n", strerror(errno));
        return -1;
    }
    enum rdma_cm_event_type got = ev->event;
    int status = ev->status;
    rdma_ack_cm_event(ev);
    if (got != expected) {
        fprintf(stderr, "Unexpected CM event: got %s, expected %s (status=%d)\n",
                rdma_event_str(got), rdma_event_str(expected), status);
        return -1;
    }
    DEBUG_LOG("CM event (sync): %s\n", rdma_event_str(got));
    return 0;
}

/* Synchronous connect for mode B (no CM thread, no semaphore). */
static int mode_b_connect(struct rdma_context *ctx)
{
    int ret;

    ctx->cm_channel = rdma_create_event_channel();
    if (!ctx->cm_channel) {
        fprintf(stderr, "Failed to create event channel\n");
        return -EFAULT;
    }
    ret = rdma_create_id(ctx->cm_channel, &ctx->cm_id, ctx, RDMA_PS_TCP);
    if (ret) {
        fprintf(stderr, "Failed to create CM ID\n");
        return -EFAULT;
    }

    if (test_config.server_addr.ss_family == AF_INET)
        ((struct sockaddr_in *)&test_config.server_addr)->sin_port = test_config.port;
    else
        ((struct sockaddr_in6 *)&test_config.server_addr)->sin6_port = test_config.port;

    struct sockaddr *src = test_config.source_addr.ss_family
                           ? (struct sockaddr *)&test_config.source_addr : NULL;

    ret = rdma_resolve_addr(ctx->cm_id, src,
                            (struct sockaddr *)&test_config.server_addr, 2000);
    if (ret) { perror("rdma_resolve_addr"); return -1; }
    if (wait_for_cm_event(ctx, RDMA_CM_EVENT_ADDR_RESOLVED)) return -1;

    ret = rdma_resolve_route(ctx->cm_id, 2000);
    if (ret) { perror("rdma_resolve_route"); return -1; }
    if (wait_for_cm_event(ctx, RDMA_CM_EVENT_ROUTE_RESOLVED)) return -1;

    ret = setup_queue_pair(ctx, ctx->cm_id);
    if (ret) { fprintf(stderr, "QP setup failed\n"); return -1; }

    struct rdma_conn_param cp;
    memset(&cp, 0, sizeof(cp));
    cp.responder_resources = RESPONDER_RESOURCES;
    cp.initiator_depth = INITIATOR_DEPTH;
    cp.retry_count = MAX_RETRY_COUNT;

    INFO_LOG("Connecting (QP #%d) [mode B]\n", ctx->cm_id->qp->qp_num);
    ret = rdma_connect(ctx->cm_id, &cp);
    if (ret) { perror("rdma_connect"); return -1; }
    if (wait_for_cm_event(ctx, RDMA_CM_EVENT_ESTABLISHED)) return -1;

    ctx->cm_established = 1;
    INFO_LOG("Connection established [mode B]\n");
    return 0;
}

/* RDMA-WRITE the whole data_buffer to the remote, walking it in MAX_SGE_LEN
 * chunks (32-bit SGE / ~2 GiB single-op limits). remote_addr advances per
 * chunk. The NIC reads ctx->data_buffer (host-pinned by default). */
static int rdma_write_buffer_chunked(struct rdma_context *ctx,
                                     uint64_t remote_addr, uint32_t rkey)
{
    size_t off = 0;
    uint64_t raddr = remote_addr;
    int chunk_id = 0;

    while (off < ctx->data_size) {
        size_t n = ctx->data_size - off;
        if (n > MAX_SGE_LEN) n = MAX_SGE_LEN;

        int wr = ++chunk_id;
        int ret = rdma_post_write(ctx->cm_id, (void *)(uintptr_t)wr,
                                  (uint8_t *)ctx->data_buffer + off, n,
                                  ctx->data_mr, IBV_SEND_SIGNALED,
                                  raddr, rkey);
        if (ret) {
            fprintf(stderr, "rdma_post_write failed at off=%zu (raddr=0x%" PRIx64 "): %s\n",
                    off, raddr, strerror(errno));
            return -1;
        }

        ret = wait_for_completion(ctx, wr);
        if (ret < 0) {
            fprintf(stderr, "WRITE completion failed at off=%zu\n", off);
            return -1;
        }

        DEBUG_LOG("WRITE chunk %d: local_off=%zu raddr=0x%" PRIx64 " len=%zu\n",
                  wr, off, raddr, n);
        off   += n;
        raddr += n;
    }
    return 0;
}

/* Dump `size` bytes of GPU memory to a text hex file so you can SEE exactly
 * what landed at the GPU address each iteration. The D2H copy here is for
 * inspection only — it is NOT the RDMA data path (the NIC reads the GPU MR
 * directly). Copies in 64 MiB chunks so no multi-GB host buffer is needed.
 * iter==0 truncates the file; later iterations append. */
static int dump_gpu_to_file(const void *gpu_ptr, size_t size, const char *path,
                            int iter, uint64_t remote_addr, uint32_t rkey)
{
    if (!path)
        return 0;

    FILE *f = fopen(path, (iter == 0) ? "w" : "a");
    if (!f) {
        fprintf(stderr, "dump file open failed (%s): %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(f, "=== iteration %d | gpu_addr=%p | remote_addr=0x%" PRIx64
               " | rkey=0x%x | size=%zu bytes ===\n",
            iter, gpu_ptr, remote_addr, rkey, size);

    const size_t CHUNK = 64u << 20;   /* 64 MiB D2H window */
    unsigned char *h = (unsigned char *)malloc(CHUNK);
    if (!h) { fclose(f); return -ENOMEM; }

    size_t off = 0;
    unsigned long col = 0;
    while (off < size) {
        size_t n = size - off;
        if (n > CHUNK) n = CHUNK;
        cudaError_t e = cudaMemcpy(h, (const uint8_t *)gpu_ptr + off, n,
                                   cudaMemcpyDeviceToHost);
        if (e != cudaSuccess) {
            fprintf(stderr, "dump D2H failed at off=%zu: %s\n", off, cudaGetErrorString(e));
            free(h); fclose(f);
            return -EFAULT;
        }
        for (size_t i = 0; i < n; i++) {
            fprintf(f, "%02x ", h[i]);
            if ((++col & 15) == 0) fputc('\n', f);
        }
        off += n;
    }
    if (col & 15) fputc('\n', f);
    fputc('\n', f);
    free(h);
    fclose(f);
    return 0;
}

/* Mode B driver. */
static int run_mode_b(void)
{
    struct rdma_context *ctx = &test_config.contexts[0];
    int ret;

    if (g_remote_addr == 0 || g_remote_rkey == 0) {
        fprintf(stderr,
                "Mode B requires the FPGA target: -R <remote_addr_hex> -K <rkey_hex>\n"
                "  (these are the ERNIC's configured RDMA target VA and rkey; if your\n"
                "   server advertises them in a param exchange, feed them in here.)\n");
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    sem_init(&ctx->semaphore, 0, 0);
    ctx->qp_number = 0;
    ctx->data_dmabuf_fd = -1;

    ret = mode_b_connect(ctx);
    if (ret) {
        fprintf(stderr, "Mode B connect failed\n");
        return ret;
    }

    /* Size: -s override, else a modest default. */
    ctx->data_size = g_force_data_size ? g_force_data_size : MODE_B_DEFAULT_SIZE;

    /* GPU MEMORY ONLY: the WRITE source lives in GPU device memory and the NIC
     * reads it directly via GPUDirect. (-g 2 can still force host-pinned for
     * comparison.) NOTE: on a PCIe topology where GPU and NIC are under
     * separate root ports with no shared switch, the NIC->GPU read needed to
     * satisfy the WRITE may fail — alloc_and_register_gpu_p2p() warns. Run on
     * shared-switch hardware (or check `nvidia-smi topo -m`) for this path. */
    ctx->remote_will_read = 1;
    ctx->data_buf_kind = (g_buf_policy == 2) ? DATA_BUF_HOST_PINNED : DATA_BUF_GPU;

    INFO_LOG("Mode B: %d iteration(s), %zu bytes/iter (%.2f GiB), source=%s -> "
             "remote_addr=0x%" PRIx64 " rkey=0x%x\n",
             g_iterations, ctx->data_size, ctx->data_size / (1024.0*1024.0*1024.0),
             ctx->data_buf_kind == DATA_BUF_GPU ? "GPU-ONLY" : "HOST-PINNED",
             g_remote_addr, g_remote_rkey);

    if (g_dump_file && ctx->data_size > (64u << 20))
        INFO_LOG("WARNING: -o dump of %.2f GiB per iteration produces a very large "
                 "hex file. Use a small -s for readable dumps.\n",
                 ctx->data_size / (1024.0*1024.0*1024.0));

    ret = alloc_and_register_data_buffer(ctx);
    if (ret) {
        fprintf(stderr, "Mode B buffer alloc/register failed\n");
        goto teardown;
    }

    /* The exact GPU device address the NIC reads from / data is stored at. */
    INFO_LOG("Mode B GPU source address = %p (lkey=0x%x rkey=0x%x), this is the "
             "device VA holding the data written to the FPGA.\n",
             ctx->data_buffer, ctx->data_mr->lkey, ctx->data_mr->rkey);

    for (int it = 0; it < g_iterations; it++) {
        /* base continues across iterations -> globally incrementing stream. */
        unsigned long long base = (unsigned long long)it * (unsigned long long)ctx->data_size;

        ret = produce_incremental_on_gpu(ctx, base);   /* GPU produces */
        if (ret) { fprintf(stderr, "producer failed (iter %d)\n", it); goto teardown; }

        ret = stage_gpu_to_host(ctx);                  /* no-op for GPU-only kind */
        if (ret) { fprintf(stderr, "stage failed (iter %d)\n", it); goto teardown; }

        if (debug_enabled)
            dump_data_buffer(ctx, 32, "first 32B of WRITE source");

        /* Show the GPU device address the data sits at this iteration. */
        INFO_LOG("iter %d: GPU addr=%p -> remote 0x%" PRIx64 " (rkey 0x%x), %zu bytes\n",
                 it, gpu_side_ptr(ctx), g_remote_addr, g_remote_rkey, ctx->data_size);

        ret = rdma_write_buffer_chunked(ctx, g_remote_addr, g_remote_rkey);
        if (ret) { fprintf(stderr, "RDMA WRITE failed (iter %d)\n", it); goto teardown; }

        /* Store exactly what was written from GPU memory to a file (D2H read of
         * the GPU MR for inspection; not the data path). gpu_side_ptr() is the
         * device VA = ctx->data_buffer in GPU-only mode. */
        if (g_dump_file) {
            int de = dump_gpu_to_file(gpu_side_ptr(ctx), ctx->data_size, g_dump_file,
                                      it, g_remote_addr, g_remote_rkey);
            if (de)
                fprintf(stderr, "GPU dump to file failed (iter %d)\n", it);
            else
                INFO_LOG("iter %d: dumped GPU buffer @ %p -> %s\n",
                         it, gpu_side_ptr(ctx), g_dump_file);
        }

        INFO_LOG("Mode B iteration %d/%d written (base counter=%llu)\n",
                 it + 1, g_iterations, base);
    }

    INFO_LOG("Mode B: all %d iteration(s) complete\n", g_iterations);
    ret = 0;

teardown:
    if (ctx->data_mr)            ibv_dereg_mr(ctx->data_mr);
    if (ctx->data_dmabuf_fd >= 0) close(ctx->data_dmabuf_fd);
    if (ctx->data_buffer) {
        if (ctx->data_buf_kind == DATA_BUF_HOST_PINNED) cudaFreeHost(ctx->data_buffer);
        else                                            cudaFree(ctx->data_buffer);
    }
    if (ctx->gpu_mirror) cudaFree(ctx->gpu_mirror);
    if (ctx->cm_id) {
        rdma_disconnect(ctx->cm_id);
        cleanup_queue_pair(ctx);
        rdma_destroy_id(ctx->cm_id);
    }
    if (ctx->cm_channel) rdma_destroy_event_channel(ctx->cm_channel);
    sem_destroy(&ctx->semaphore);
    return ret;
}

/* Print usage information */
static void print_usage(const char *program_name)
{
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -a <address>   Server/FPGA IP address (required)\n");
    printf("  -p <port>      Port number (default: 7176)\n");
    printf("  -I <address>   Source address to bind\n");
    printf("  -m <A|B>       Operation mode (skip the interactive menu):\n");
    printf("                   A = standard server-driven test (default flow)\n");
    printf("                   B = client -> FPGA incremental RDMA WRITE\n");
    printf("  -D <mode>      [A] Data integrity mode (0=none, 1=text, 2=image)\n");
    printf("  -g <policy>    Buffer placement: 0=auto, 1=force GPU, 2=force host\n");
    printf("  -s <MiB>       Force data buffer size in MiB (e.g. 5120 = 5 GiB)\n");
    printf("  -R <hex>       [B] Remote (FPGA) target address, e.g. 0x10000000\n");
    printf("  -K <hex>       [B] Remote (FPGA) rkey, e.g. 0x1234\n");
    printf("  -n <count>     [B] Number of write iterations (default 10)\n");
    printf("  -o <file>      [B] Dump the GPU buffer (address + data) to a hex\n");
    printf("                 file each iteration (inspection D2H; not the data path)\n");
    printf("  -d             Enable debug output (flag; no argument)\n");
    printf("  -h             Show this help\n");
}

/* Interactive menu when -m is not given. */
static char select_mode_interactive(void)
{
    printf("\nSelect operation:\n");
    printf("  A) Standard server-driven test (receive params; serve READ/WRITE/SEND)\n");
    printf("  B) Client -> FPGA incremental RDMA WRITE (GPU-produced data)\n");
    printf("Choice [A/B]: ");
    fflush(stdout);

    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin))
        return 'A';
    char c = (char)toupper((unsigned char)buf[0]);
    return (c == 'B') ? 'B' : 'A';
}

/* Process received data and generate output files (mode A; size_t). */
static void process_received_data(uint64_t buffer_address, size_t data_size,
                                  size_t data_start, int mode)
{
    uint8_t *data_ptr = (uint8_t *)buffer_address;

    INFO_LOG("Processing data: addr=0x%lx, size=%zu, start=%zu, mode=%d\n",
             buffer_address, data_size, data_start, mode);

    if (data_size > (256u << 20))
        INFO_LOG("WARNING: data_size is large (%.2f GiB); file output may be huge/slow.\n",
                 data_size / (1024.0*1024.0*1024.0));

    if (mode == 1) {
        const char *filenames[MAX_IMAGES] = {
            "Frame0.txt", "Frame1.txt", "Frame2.txt", "Frame3.txt",
            "Frame4.txt", "Frame5.txt", "Frame6.txt", "Frame7.txt",
            "Frame8.txt", "Frame9.txt"
        };
        for (int img = 0; img < MAX_IMAGES; img++) {
            FILE *file = fopen(filenames[img], "w");
            if (!file) continue;
            for (size_t i = data_start; i < data_size + data_start; i++) {
                fprintf(file, "%02x ", data_ptr[i]);
            }
            fclose(file);
            INFO_LOG("Generated: %s\n", filenames[img]);
            data_ptr += (data_size + data_start);
        }
    } else if (mode == 2) {
        const char *filenames[MAX_IMAGES] = {
            "Frame0.bmp", "Frame1.bmp", "Frame2.bmp", "Frame3.bmp",
            "Frame4.bmp", "Frame5.bmp", "Frame6.bmp", "Frame7.bmp",
            "Frame8.bmp", "Frame9.bmp"
        };
        const int height = 1080;
        const int width = 1920;
        const int ptp_length = 10;
        const int sec_counter = 4;

        uint8_t *rgb_data = malloc((size_t)width * height * 3);
        if (!rgb_data) {
            fprintf(stderr, "Memory allocation failed\n");
            return;
        }

        unsigned char ***image = malloc(height * sizeof(unsigned char **));
        for (int k = 0; k < height; k++) {
            image[k] = malloc(width * sizeof(unsigned char *));
            for (int j = 0; j < width; j++) {
                image[k][j] = malloc(BYTES_PER_PIXEL);
            }
        }

        for (int img = 0; img < MAX_IMAGES; img++) {
            uint64_t seconds = 0;
            uint32_t nanoseconds = 0;
            for (int i = ptp_length - 1; i >= sec_counter; i--) {
                seconds = (seconds << 8) | data_ptr[i];
            }
            for (int i = sec_counter - 1; i >= 0; i--) {
                nanoseconds = (nanoseconds << 8) | data_ptr[i];
            }
            INFO_LOG("Frame %d: PTP seconds=%lu, nanoseconds=%u\n",
                     img, seconds, nanoseconds);

            size_t rgb_idx = 0;
            for (size_t i = data_start; i < data_size + data_start; i += 4) {
                int y1 = data_ptr[i];
                int cb = data_ptr[i + 1];
                int y2 = data_ptr[i + 2];
                int cr = data_ptr[i + 3];

                rgb_data[rgb_idx + 0] = clamp_value((int)(y1 + 1.4075 * (cr - 128)));
                rgb_data[rgb_idx + 1] = clamp_value((int)(y1 - 0.3455 * (cb - 128) - 0.7169 * (cr - 128)));
                rgb_data[rgb_idx + 2] = clamp_value((int)(y1 + 1.7790 * (cb - 128)));

                rgb_data[rgb_idx + 3] = clamp_value((int)(y2 + 1.4075 * (cr - 128)));
                rgb_data[rgb_idx + 4] = clamp_value((int)(y2 - 0.3455 * (cb - 128) - 0.7169 * (cr - 128)));
                rgb_data[rgb_idx + 5] = clamp_value((int)(y2 + 1.7790 * (cb - 128)));

                rgb_idx += 6;
                if (rgb_idx + 6 > (size_t)width * height * 3)
                    break;
            }

            for (int k = 0; k < height; k++) {
                for (int j = 0; j < width; j++) {
                    int offset = (k * width * BYTES_PER_PIXEL) + (j * BYTES_PER_PIXEL);
                    image[k][j][2] = rgb_data[offset + 0];
                    image[k][j][1] = rgb_data[offset + 1];
                    image[k][j][0] = rgb_data[offset + 2];
                }
            }

            generate_bitmap_image(image, height, width, filenames[img]);
            data_ptr += (data_size + data_start);
        }

        for (int k = 0; k < height; k++) {
            for (int j = 0; j < width; j++) {
                free(image[k][j]);
            }
            free(image[k]);
        }
        free(image);
        free(rgb_data);
    }
}

/* Mode A: the original thread-based, server-driven flow. */
static int run_mode_a(void)
{
    int ret;
    int data_integrity_mode = test_config.validate;  /* stashed -D value */

    INFO_LOG("Creating worker threads\n");

    ret = pthread_create(&test_config.cm_thread, NULL,
                        cm_thread_function, &test_config);
    if (ret) { fprintf(stderr, "Failed to create CM thread\n"); return -1; }

    ret = pthread_create(&test_config.disconnect_thread, NULL,
                        disconnect_thread_function, &test_config);
    if (ret) { fprintf(stderr, "Failed to create disconnect thread\n"); return -1; }

    ret = pthread_create(&test_config.data_thread, NULL,
                        data_thread_function, &test_config);
    if (ret) { fprintf(stderr, "Failed to create data thread\n"); return -1; }

    INFO_LOG("\nSetting up %d Queue Pair(s)\n", test_config.qp_count);
    for (int i = 0; i < test_config.qp_count; i++) {
        INFO_LOG("\n--- Setting up QP #%d ---\n", i);
        struct rdma_context *ctx = &test_config.contexts[i];
        memset(ctx, 0, sizeof(struct rdma_context));
        sem_init(&ctx->semaphore, 0, 0);
        ctx->qp_number = i;
        ctx->message_count = test_config.iteration_count;
        ctx->data_dmabuf_fd = -1;

        ret = setup_connection(ctx);
        if (ret) {
            fprintf(stderr, "Setup failed for QP #%d\n", i);
            ctx->cm_channel_enabled = 0;
            test_config.completed_count++;
        } else {
            INFO_LOG("QP #%d setup completed\n", i);
        }
    }

    INFO_LOG("\nWaiting for all QPs to complete...\n");
    while (test_config.completed_count < test_config.qp_count) {
        usleep(100000);
    }

    if (data_integrity_mode > 0) {
        INFO_LOG("\n========================================\n");
        INFO_LOG("Processing received data\n");
        INFO_LOG("========================================\n");

        struct rdma_context *ctx = &test_config.contexts[0];
        size_t total_data = ctx->data_size;
        size_t data_start = 0;

        if (debug_enabled)
            dump_data_buffer(ctx, 256, "first 256B of data buffer");

        if (ctx->data_buf_kind == DATA_BUF_HOST_PINNED) {
            stage_host_to_gpu(ctx);
#ifdef WITH_GPU_KERNELS
            if (ctx->gpu_mirror)
                INFO_LOG("On-GPU checksum of mirrored buffer = 0x%llx\n",
                         gpu_checksum(ctx->gpu_mirror, total_data));
#endif
            process_received_data((uint64_t)(uintptr_t)ctx->data_buffer,
                                  total_data, data_start, data_integrity_mode);
        } else {
#ifdef WITH_GPU_KERNELS
            INFO_LOG("On-GPU checksum of rx buffer = 0x%llx (no D2H copy)\n",
                     gpu_checksum(ctx->data_buffer, total_data));
#endif
            void *host_copy = malloc(total_data);
            if (!host_copy) {
                fprintf(stderr, "Failed to allocate host staging buffer (%zu bytes)\n",
                        total_data);
            } else {
                cudaError_t e = cudaMemcpy(host_copy, ctx->data_buffer, total_data,
                                           cudaMemcpyDeviceToHost);
                if (e != cudaSuccess) {
                    fprintf(stderr, "Staging D2H copy failed: %s\n",
                            cudaGetErrorString(e));
                } else {
                    process_received_data((uint64_t)(uintptr_t)host_copy,
                                          total_data, data_start,
                                          data_integrity_mode);
                }
                free(host_copy);
            }
        }
    }

    /* Per-QP cleanup */
    for (int i = 0; i < test_config.qp_count; i++) {
        struct rdma_context *ctx = &test_config.contexts[i];
        if (ctx->data_mr)            ibv_dereg_mr(ctx->data_mr);
        if (ctx->data_dmabuf_fd >= 0) close(ctx->data_dmabuf_fd);
        if (ctx->data_buffer) {
            if (ctx->data_buf_kind == DATA_BUF_HOST_PINNED) cudaFreeHost(ctx->data_buffer);
            else                                            cudaFree(ctx->data_buffer);
        }
        if (ctx->gpu_mirror)  cudaFree(ctx->gpu_mirror);
        if (ctx->recv_mr)     ibv_dereg_mr(ctx->recv_mr);
        if (ctx->send_mr)     ibv_dereg_mr(ctx->send_mr);
        if (ctx->wc)          free(ctx->wc);
        rdma_disconnect(ctx->cm_id);
        cleanup_queue_pair(ctx);
        rdma_destroy_id(ctx->cm_id);
        rdma_destroy_event_channel(ctx->cm_channel);
        sem_destroy(&ctx->semaphore);
    }
    return 0;
}

/* Main function */
int main(int argc, char *argv[])
{
    int option;
    int ret = 0;
    int data_integrity_mode = 0;

    printf("\n");
    printf("========================================\n");
    printf("RDMA Client Application (Ubuntu 22.04)\n");
    printf("========================================\n\n");

    memset(&test_config, 0, sizeof(test_config));
    test_config.buffer_size = 64;
    test_config.server_addr.ss_family = AF_INET;
    test_config.port = htobe16(7176);
    test_config.qp_count = 1;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    while ((option = getopt(argc, argv, "I:p:da:hD:g:s:m:R:K:n:o:")) != -1) {
        switch (option) {
        case 'a':
            ret = resolve_address(optarg, (struct sockaddr *)&test_config.server_addr);
            if (ret) { fprintf(stderr, "Failed to resolve server address\n"); return -1; }
            INFO_LOG("Server address: %s\n", optarg);
            break;
        case 'I':
            ret = resolve_address(optarg, (struct sockaddr *)&test_config.source_addr);
            if (ret) { fprintf(stderr, "Failed to resolve source address\n"); return -1; }
            INFO_LOG("Source address: %s\n", optarg);
            break;
        case 'p':
            test_config.port = htobe16(atoi(optarg));
            INFO_LOG("Port: %d\n", atoi(optarg));
            break;
        case 'd':
            debug_enabled++;
            INFO_LOG("Debug mode: level %d\n", debug_enabled);
            break;
        case 'D':
            data_integrity_mode = atoi(optarg);
            if (data_integrity_mode < 0 || data_integrity_mode > 2) {
                fprintf(stderr, "Invalid data integrity mode\n");
                print_usage(argv[0]);
                return -EINVAL;
            }
            INFO_LOG("Data integrity mode: %d\n", data_integrity_mode);
            break;
        case 'g':
            g_buf_policy = atoi(optarg);
            if (g_buf_policy < 0 || g_buf_policy > 2) {
                fprintf(stderr, "Invalid buffer placement policy\n");
                print_usage(argv[0]);
                return -EINVAL;
            }
            INFO_LOG("Buffer placement policy: %d (0=auto, 1=GPU, 2=host)\n", g_buf_policy);
            break;
        case 's': {
            unsigned long long mib = strtoull(optarg, NULL, 0);
            if (mib == 0) {
                fprintf(stderr, "Invalid forced size (MiB): %s\n", optarg);
                print_usage(argv[0]);
                return -EINVAL;
            }
            g_force_data_size = (size_t)mib * 1024 * 1024;
            INFO_LOG("Forced data size: %llu MiB (%zu bytes, %.2f GiB)\n",
                     mib, g_force_data_size, g_force_data_size / (1024.0*1024.0*1024.0));
            break;
        }
        case 'm': {
            char c = (char)toupper((unsigned char)optarg[0]);
            if (c != 'A' && c != 'B') {
                fprintf(stderr, "Invalid mode: %s (use A or B)\n", optarg);
                print_usage(argv[0]);
                return -EINVAL;
            }
            g_mode = c;
            INFO_LOG("Mode: %c\n", g_mode);
            break;
        }
        case 'R':
            g_remote_addr = strtoull(optarg, NULL, 0);
            INFO_LOG("Remote target addr: 0x%" PRIx64 "\n", g_remote_addr);
            break;
        case 'K':
            g_remote_rkey = (uint32_t)strtoul(optarg, NULL, 0);
            INFO_LOG("Remote target rkey: 0x%x\n", g_remote_rkey);
            break;
        case 'n':
            g_iterations = atoi(optarg);
            if (g_iterations <= 0) {
                fprintf(stderr, "Invalid iteration count: %s\n", optarg);
                print_usage(argv[0]);
                return -EINVAL;
            }
            INFO_LOG("Iterations: %d\n", g_iterations);
            break;
        case 'o':
            g_dump_file = optarg;
            INFO_LOG("Mode B GPU dump file: %s\n", g_dump_file);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return -EINVAL;
        }
    }

    /* Stash -D for mode A. */
    test_config.validate = data_integrity_mode;

    test_config.contexts = calloc(test_config.qp_count, sizeof(struct rdma_context));
    if (!test_config.contexts) {
        fprintf(stderr, "Failed to allocate contexts\n");
        return -ENOMEM;
    }

    INFO_LOG("Allocated %zu bytes for %d contexts\n",
             test_config.qp_count * sizeof(struct rdma_context),
             test_config.qp_count);

    ret = gpu_init();
    if (ret) {
        fprintf(stderr, "GPU initialization failed\n");
        free(test_config.contexts);
        return -1;
    }

    /* ---- Menu ---- */
    char mode = g_mode ? g_mode : select_mode_interactive();
    INFO_LOG("\nRunning mode %c\n", mode);

    if (mode == 'B')
        ret = run_mode_b();
    else
        ret = run_mode_a();

    INFO_LOG("\n========================================\n");
    INFO_LOG("Application completed (mode %c, ret=%d)\n", mode, ret);
    INFO_LOG("========================================\n\n");

    free(test_config.contexts);
    return ret ? -1 : 0;
}
