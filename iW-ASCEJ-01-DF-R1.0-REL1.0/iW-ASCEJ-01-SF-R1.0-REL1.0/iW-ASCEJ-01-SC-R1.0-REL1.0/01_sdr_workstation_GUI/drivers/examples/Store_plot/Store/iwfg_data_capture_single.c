#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/aio_abi.h>

#include "iwfg_user.h"

/*
 * Simple lossless C2H store application -- single channel, max rate
 * -----------------------------------------------------------------
 *
 * Reads from one iwfg C2H DMA node and stores everything to a file with no
 * data loss:
 *
 *   - The DMA thread fills a ring of buffers registered with the driver.
 *     Each IWFG_IOCTL_DMA_DATA call blocks in the kernel until the buffer
 *     is complete.
 *   - Pre-arm: the *next* ring buffer is handed to the driver inside the
 *     same ioctl (struct iwfg_c2h_prearm appended after the request), so
 *     the kernel posts its descriptors while the current buffer is still
 *     filling.  The QDMA descriptor ring never starves between ioctls,
 *     which is what makes back-to-back capture gapless.
 *   - The writer thread drains completed buffers to disk with Linux native
 *     AIO + O_DIRECT (multiple writes in flight, page cache bypassed), so
 *     disk latency cannot stall the DMA ring as long as average disk
 *     throughput keeps up.
 *
 * Ownership: g_buffer_empty[i] is a binary semaphore owning buffer i;
 * g_full_slots counts completed buffers.  AIO completions can land out of
 * order, so the writer releases the exact buffer index that completed.
 *
 * The FPGA data source is expected to be running already (or controlled
 * externally); this app performs no test-source control.
 */

/*
 * Optional C2H pre-arm extension.  The driver reads this structure if it is
 * appended directly after struct iwfg_user_dma_req in the ioctl argument and
 * ignores it when next_buf is NULL or next_size does not match the request.
 */
#ifndef IWFG_HAVE_C2H_PREARM
struct iwfg_c2h_prearm {
    void *next_buf;
    uint32_t next_size;
};
#endif

#define DEVICE_NODE_TEMPLATE "/dev/iwfg%d"
#define OUTPUT_FILE_TEMPLATE "capture%d.bin"
#define MAX_CAPTURE_CHANNELS 4

#define DEFAULT_NUM_BUFFERS 16
#define MAX_NUM_BUFFERS 512
#define DEFAULT_BUFFER_SIZE (16 * 1024 * 1024)

typedef struct {
    size_t buffer_size;
    int num_buffers;
    bool no_write;
    bool direct_write;
    bool aio_write;
    unsigned int aio_depth;
    bool drain_on_stop;
    bool prearm;
    uint64_t limit_bytes;
    unsigned int capture_seconds;
    unsigned int channel;
} capture_config_t;

static capture_config_t g_config = {
    .buffer_size = DEFAULT_BUFFER_SIZE,
    .num_buffers = DEFAULT_NUM_BUFFERS,
    .direct_write = true,
    .aio_write = true,
    .aio_depth = 32,
    .drain_on_stop = true,
    .prearm = true,
    .capture_seconds = 0, /* 0 = run until Ctrl+C */
};

#define g_buffer_size     (g_config.buffer_size)
#define g_num_buffers     (g_config.num_buffers)
#define g_no_write        (g_config.no_write)
#define g_direct_write    (g_config.direct_write)
#define g_aio_write       (g_config.aio_write)
#define g_aio_depth       (g_config.aio_depth)
#define g_drain_on_stop   (g_config.drain_on_stop)
#define g_prearm          (g_config.prearm)
#define g_limit_bytes     (g_config.limit_bytes)
#define g_capture_seconds (g_config.capture_seconds)
#define g_channel         (g_config.channel)

typedef struct {
    void *ptr;
    uint32_t index;
} capture_buffer_t;

typedef struct {
    struct iocb cb;
    int index;
    size_t len;
    uint64_t offset;
    int active;
    int submitted;
} aio_write_slot_t;

/* The prearm block is an extension read by the driver right after req. */
typedef struct {
    struct iwfg_user_dma_req req;
    struct iwfg_c2h_prearm prearm;
} c2h_dma_request_t;

typedef struct {
    pthread_t dma_tid;
    pthread_t writer_tid;
    bool dma_started;
    bool writer_started;
} capture_workers_t;

/* ============================ Global state ============================ */

static char g_device_node[32];
static char g_output_file[PATH_MAX];
static atomic_int g_fd = -1;
static int g_out_fd = -1;

static capture_buffer_t g_buf[MAX_NUM_BUFFERS];
static sem_t g_full_slots;
static sem_t g_buffer_empty[MAX_NUM_BUFFERS];
static int g_buffer_empty_initialized;
static bool g_full_slots_initialized;

static atomic_bool g_running;
static atomic_bool g_stop_requested;
static atomic_bool g_capture_done;
static atomic_bool g_capture_deadline_active;
static struct timespec g_capture_deadline_ts;

static atomic_uint_fast64_t g_total_bytes;
static atomic_uint_fast64_t g_total_frames;
static atomic_uint_fast64_t g_captured_bytes;
static atomic_uint_fast64_t g_captured_frames;
static atomic_uint_fast64_t g_eagain_count;
static atomic_uint_fast64_t g_sched_warn_count;

/* Signal handlers only touch this async-signal-safe process-wide flag. */
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_stop_announced = 0;

static void free_buffers(void);
static int init_capture_semaphores(void);
static void destroy_capture_semaphores(void);
static void request_stop(void);
static void interrupt_worker_thread(pthread_t tid);
static void arm_capture_deadline(void);
static int capture_deadline_expired(void);
static int parse_environment(void);
static int parse_args(int argc, char **argv);
static int parse_channel_name(const char *text, unsigned int *channel);
static void update_channel_paths(void);
static int parse_bool_env(const char *name);
static void configure_current_thread_rt(int priority, int cpu_id);
static int parse_cpu_from_env(const char *name);
static int register_buffers(void);
static int write_all(int fd, const void *buf, size_t len);
static int64_t signed_byte_delta(uint64_t captured, uint64_t processed);
static void sleep_ms(unsigned int milliseconds);
static void channel_log(const char *stage);
static void print_progress_line(void);
static void print_capture_configuration(void);
static void print_capture_summary(void);
static void install_channel_signal_handlers(void);
static int open_capture_endpoints(void);
static int prepare_capture_buffers(void);
static void stop_capture_workers(capture_workers_t *workers);
static void cleanup_capture_resources(void);
static void *writer_thread_aio(void *arg);
static int run_capture_channel(void);

/* ===================== Signal and timing control ====================== */

static void signal_write(const char *text, size_t len)
{
    ssize_t ignored = write(STDOUT_FILENO, text, len);
    (void)ignored;
}

static void stop_handler(int sig)
{
    (void)sig;

    if (g_stop_announced) {
        signal_write("\nForce exiting...\n", 18);
        _exit(130);
    }

    g_stop_announced = 1;
    g_shutdown_requested = 1;
    signal_write("\nStopping safely...\n", 20);
}

static void worker_wakeup_handler(int sig)
{
    (void)sig;
}

static int timespec_after_or_equal(const struct timespec *a,
                                   const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec)
        return a->tv_sec > b->tv_sec;

    return a->tv_nsec >= b->tv_nsec;
}

static void arm_capture_deadline(void)
{
    if (!g_capture_seconds)
        return;

    clock_gettime(CLOCK_MONOTONIC, &g_capture_deadline_ts);
    g_capture_deadline_ts.tv_sec += g_capture_seconds;
    g_capture_deadline_active = 1;
}

static int capture_deadline_expired(void)
{
    struct timespec now;

    if (!g_capture_deadline_active)
        return 0;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return timespec_after_or_equal(&now, &g_capture_deadline_ts);
}

/* ======================= DMA producer thread ========================= */

static void *dma_thread(void *arg)
{
    (void)arg;
    int index = 0;
    int dma_cpu = parse_cpu_from_env("IWFG_DMA_CPU");
    /* Default CPU mapping: channel N uses DMA CPU 2N and writer CPU 2N+1. */
    if (dma_cpu < 0)
        dma_cpu = (int)(g_channel * 2);
    int current_reserved = 0;

    configure_current_thread_rt(80, dma_cpu);

    while (g_running) {
        int next_index = (index + 1) % g_num_buffers;
        int next_reserved = 0;

        if (g_shutdown_requested) {
            request_stop();
            break;
        }

        if (capture_deadline_expired()) {
            request_stop();
            break;
        }

        if (!current_reserved) {
            if (sem_wait(&g_buffer_empty[index]) == -1) {
                if (errno == EINTR)
                    continue;
                break;
            }
            current_reserved = 1;
        }

        if (!g_running)
            break;

        if (capture_deadline_expired()) {
            request_stop();
            break;
        }

        if (g_prearm) {
            /*
             * Pre-arm gives ownership of the next buffer to the driver before
             * the next loop iteration. Reserve that empty slot now; otherwise
             * ring wrap can DMA into a buffer still waiting for the writer.
             */
            if (sem_wait(&g_buffer_empty[next_index]) == -1) {
                if (errno == EINTR)
                    continue;
                break;
            }
            next_reserved = 1;
        }

        if (capture_deadline_expired()) {
            if (next_reserved)
                sem_post(&g_buffer_empty[next_index]);
            request_stop();
            break;
        }

        /*
         * Always pass the extended structure.  The driver unconditionally
         * reads the prearm block that follows the request; keeping it inside
         * the ioctl argument (zeroed when pre-arm is disabled) guarantees it
         * never sees stack garbage.
         */
        c2h_dma_request_t iarg;

        memset(&iarg, 0, sizeof(iarg));
        iarg.req.buf       = NULL;
        iarg.req.size      = (uint32_t)g_buffer_size;
        iarg.req.offset    = 0;
        iarg.req.direction = IWFG_DIR_C2H_BUF;
        iarg.req.buf_index = g_buf[index].index;
        iarg.req.flags     = IWFG_FLAG_NORM_PACKET;

        if (g_prearm) {
            iarg.prearm.next_buf  = g_buf[next_index].ptr;
            iarg.prearm.next_size = (uint32_t)g_buffer_size;
        }

        if (ioctl(g_fd, IWFG_IOCTL_DMA_DATA, &iarg) < 0) {

            if (!g_running)
                break;

            if (errno == EINTR || errno == EBADF)
                break;

            if (errno == EAGAIN) {
                g_eagain_count++;
                sem_post(&g_buffer_empty[index]);
                if (next_reserved)
                    sem_post(&g_buffer_empty[next_index]);
                current_reserved = 0;
                continue;
            }

            perror("DMA ioctl");
            if (next_reserved)
                sem_post(&g_buffer_empty[next_index]);
            sem_post(&g_buffer_empty[index]);
            current_reserved = 0;
            request_stop();
            break;
        }

        g_captured_bytes += g_buffer_size;
        g_captured_frames++;
        sem_post(&g_full_slots);

        index = next_index;
        current_reserved = next_reserved;
    }

    g_capture_done = 1;
    sem_post(&g_full_slots);

    return NULL;
}

/* ====================== Buffer consumer thread ======================= */

static void *writer_thread(void *arg)
{
    int index = 0;
    int wr_cpu = parse_cpu_from_env("IWFG_WRITER_CPU");
    /* Keep producer and consumer on separate CPUs unless explicitly pinned. */
    if (wr_cpu < 0)
        wr_cpu = (int)(g_channel * 2 + 1);

    configure_current_thread_rt(70, wr_cpu);

    if (g_aio_write && !g_no_write)
        return writer_thread_aio(arg);

    while (g_running || !g_capture_done ||
           (g_drain_on_stop && g_total_frames < g_captured_frames)) {

        if (sem_wait(&g_full_slots) == -1) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (!g_running && !g_drain_on_stop)
            break;

        if (g_capture_done && g_total_frames >= g_captured_frames)
            break;

        if (!g_no_write && write_all(g_out_fd, g_buf[index].ptr, g_buffer_size) != 0) {
            perror("write");
            request_stop();
            break;
        }

        g_total_bytes += g_buffer_size;
        g_total_frames++;
        sem_post(&g_buffer_empty[index]);

        if (g_limit_bytes && g_total_bytes >= g_limit_bytes) {
            request_stop();
            break;
        }

        index = (index + 1) % g_num_buffers;
    }

    return NULL;
}

static int linux_io_setup(unsigned int nr_events, aio_context_t *ctx)
{
    return (int)syscall(__NR_io_setup, nr_events, ctx);
}

static int linux_io_destroy(aio_context_t ctx)
{
    return (int)syscall(__NR_io_destroy, ctx);
}

static int linux_io_submit(aio_context_t ctx, long nr, struct iocb **iocbpp)
{
    return (int)syscall(__NR_io_submit, ctx, nr, iocbpp);
}

static int linux_io_getevents(aio_context_t ctx, long min_nr, long nr,
                              struct io_event *events, struct timespec *timeout)
{
    return (int)syscall(__NR_io_getevents, ctx, min_nr, nr, events, timeout);
}

static int complete_aio_writes(aio_context_t ctx, struct io_event *events,
                               unsigned int max_events, unsigned int min_events,
                               unsigned int *inflight)
{
    int ret;

    if (*inflight == 0)
        return 0;

    if (min_events > *inflight)
        min_events = *inflight;

    do {
        ret = linux_io_getevents(ctx, min_events, max_events, events, NULL);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        return -1;
    }

    for (int i = 0; i < ret; i++) {
        aio_write_slot_t *slot = (aio_write_slot_t *)(uintptr_t)events[i].data;

        if (!slot || !slot->submitted ||
            slot->index < 0 || slot->index >= g_num_buffers) {
            errno = EIO;
            return -1;
        }

        if (events[i].res < 0) {
            errno = (int)-events[i].res;
            return -1;
        }

        if ((size_t)events[i].res != slot->len) {
            errno = EIO;
            return -1;
        }

        slot->submitted = 0;
        slot->active = 0;
        (*inflight)--;
        g_total_bytes += slot->len;
        g_total_frames++;
        /*
         * AIO completions are allowed to arrive out of order. Release the
         * exact buffer that completed; a count-only semaphore can otherwise
         * let DMA overwrite a different buffer whose write is still active.
         */
        sem_post(&g_buffer_empty[slot->index]);
    }

    return ret;
}

static aio_write_slot_t *find_free_aio_slot(aio_write_slot_t *slots,
                                            unsigned int depth,
                                            unsigned int *cursor)
{
    for (unsigned int tries = 0; tries < depth; tries++) {
        unsigned int idx = (*cursor + tries) % depth;

        if (!slots[idx].active) {
            *cursor = (idx + 1) % depth;
            return &slots[idx];
        }
    }

    return NULL;
}

static void *writer_thread_aio(void *arg)
{
    (void)arg;
    int index = 0;
    aio_context_t ctx = 0;
    aio_write_slot_t *slots = NULL;
    struct io_event *events = NULL;
    aio_write_slot_t *pending_slots[MAX_NUM_BUFFERS];
    struct iocb *submit_list[MAX_NUM_BUFFERS];
    unsigned int inflight = 0;
    unsigned int slot_cursor = 0;
    unsigned int pending_count = 0;
    uint64_t submitted_frames = 0;
    uint64_t next_offset = 0;

    slots = calloc(g_aio_depth, sizeof(*slots));
    events = calloc(g_aio_depth, sizeof(*events));
    if (!slots || !events) {
        perror("calloc aio");
        request_stop();
        goto out;
    }

    if (linux_io_setup(g_aio_depth, &ctx) < 0) {
        perror("io_setup");
        request_stop();
        goto out;
    }

    while (g_running || !g_capture_done ||
           (g_drain_on_stop && submitted_frames < g_captured_frames) ||
           pending_count > 0 || inflight > 0) {

        while ((g_running || g_drain_on_stop) &&
               pending_count < g_aio_depth &&
               (inflight + pending_count) < g_aio_depth) {
            if (sem_trywait(&g_full_slots) != 0) {
                if (errno == EAGAIN)
                    break;
                if (errno == EINTR)
                    continue;
                perror("sem_trywait");
                request_stop();
                goto drain;
            }

            if (g_capture_done && submitted_frames >= g_captured_frames)
                break;

            aio_write_slot_t *slot = find_free_aio_slot(slots, g_aio_depth, &slot_cursor);
            if (!slot) {
                sem_post(&g_buffer_empty[index]);
                errno = EAGAIN;
                perror("aio slot");
                request_stop();
                goto drain;
            }

            memset(&slot->cb, 0, sizeof(slot->cb));
            slot->index = index;
            slot->len = g_buffer_size;
            slot->offset = next_offset;
            slot->active = 1;
            slot->submitted = 0;
            slot->cb.aio_data = (uint64_t)(uintptr_t)slot;
            slot->cb.aio_lio_opcode = IOCB_CMD_PWRITE;
            slot->cb.aio_fildes = (uint32_t)g_out_fd;
            slot->cb.aio_buf = (uint64_t)(uintptr_t)g_buf[index].ptr;
            slot->cb.aio_nbytes = g_buffer_size;
            slot->cb.aio_offset = (int64_t)next_offset;

            pending_slots[pending_count++] = slot;
            submitted_frames++;
            next_offset += g_buffer_size;

            if (g_limit_bytes && next_offset >= g_limit_bytes)
                request_stop();

            index = (index + 1) % g_num_buffers;
        }

        if (pending_count > 0) {
            for (;;) {
                /* pending_slots can move after a partial submission. */
                for (unsigned int i = 0; i < pending_count; i++)
                    submit_list[i] = &pending_slots[i]->cb;

                int ret = linux_io_submit(ctx, (long)pending_count, submit_list);
                if (ret == (int)pending_count) {
                    for (unsigned int i = 0; i < pending_count; i++)
                        pending_slots[i]->submitted = 1;
                    inflight += pending_count;
                    pending_count = 0;
                    break;
                }

                if (ret < 0 && errno == EINTR)
                    continue;

                if (ret < 0 && errno == EAGAIN) {
                    if (complete_aio_writes(ctx, events, g_aio_depth, 1, &inflight) < 0) {
                        perror("io_getevents");
                        request_stop();
                        goto drain;
                    }
                    continue;
                }

                if (ret > 0) {
                    for (int i = 0; i < ret; i++)
                        pending_slots[i]->submitted = 1;
                    inflight += (unsigned int)ret;

                    if ((unsigned int)ret < pending_count) {
                        memmove(pending_slots, pending_slots + ret,
                                (pending_count - (unsigned int)ret) * sizeof(pending_slots[0]));
                        pending_count -= (unsigned int)ret;
                        continue;
                    }

                    pending_count = 0;
                    break;
                }

                if (ret == 0)
                    errno = EIO;
                perror("io_submit");
                request_stop();
                goto drain;
            }
        } else if (inflight > 0) {
            if (complete_aio_writes(ctx, events, g_aio_depth, 1, &inflight) < 0) {
                perror("io_getevents");
                request_stop();
                goto drain;
            }
        } else if (!g_running && (!g_drain_on_stop || g_capture_done) &&
                   pending_count == 0 && inflight == 0) {
            break;
        } else {
            /* A SCHED_FIFO writer must sleep when idle so lower-priority
             * completion work can run on the same CPU. */
            sleep_ms(1);
        }
    }

drain:
    while (inflight > 0) {
        if (complete_aio_writes(ctx, events, g_aio_depth, 1, &inflight) < 0) {
            perror("io_getevents");
            request_stop();
            break;
        }
    }

out:
    if (ctx)
        linux_io_destroy(ctx);
    free(events);
    free(slots);
    return NULL;
}

/* ================= Buffer allocation and configuration =============== */

static int allocate_buffers(void)
{
    for (int i = 0; i < g_num_buffers; i++) {

        if (posix_memalign(&g_buf[i].ptr, 4096, g_buffer_size))
            return -1;
    }

    return 0;
}

static int register_buffers(void)
{
    for (int i = 0; i < g_num_buffers; i++) {
        struct iwfg_user_buf req;

        memset(&req, 0, sizeof(req));
        req.buf = g_buf[i].ptr;
        req.size = (uint32_t)g_buffer_size;
        req.buf_type = IWFG_BUF_TYPE_USERPTR;
        req.import_fd = -1;
        req.is_chunked = false;
        req.chunk_size = 0;
        req.dmabuf_fd = -1;

        if (ioctl(g_fd, IWFG_IOCTL_ADD_BUF, &req) < 0) {
            perror("IWFG_IOCTL_ADD_BUF");
            return -1;
        }

        g_buf[i].index = req.buf_index;
    }

    return 0;
}

static int parse_buffer_count_from_env(void)
{
    const char *env = getenv("IWFG_CAPTURE_NUM_BUFS");
    if (!env || !*env)
        return 0;

    char *end = NULL;
    long count = strtol(env, &end, 10);
    if (end == env || *end != '\0' || count <= 0 || count > MAX_NUM_BUFFERS) {
        fprintf(stderr,
                "Invalid IWFG_CAPTURE_NUM_BUFS='%s' (expected 1..%d)\n",
                env, MAX_NUM_BUFFERS);
        return -1;
    }

    g_num_buffers = (int)count;
    return 0;
}

static int parse_buffer_size_from_env(void)
{
    const char *env = getenv("IWFG_CAPTURE_BUF_MB");
    if (!env || !*env)
        return 0;

    char *end = NULL;
    unsigned long mb = strtoul(env, &end, 10);
    if (end == env || *end != '\0' || mb == 0 ||
        mb > UINT32_MAX / (1024u * 1024u)) {
        fprintf(stderr,
                "Invalid IWFG_CAPTURE_BUF_MB='%s' "
                "(expected 1..%u MB)\n",
                env, UINT32_MAX / (1024u * 1024u));
        return -1;
    }

    g_buffer_size = (size_t)mb * 1024u * 1024u;
    return 0;
}

static int parse_limit_from_env(void)
{
    const char *env = getenv("IWFG_CAPTURE_LIMIT_MB");

    if (!env || !*env)
        return 0;

    char *end = NULL;
    unsigned long mb = strtoul(env, &end, 10);
    if (end == env || *end != '\0' || mb == 0) {
        fprintf(stderr, "Invalid IWFG_CAPTURE_LIMIT_MB='%s' (expected positive integer MB)\n", env);
        return -1;
    }

    g_limit_bytes = (uint64_t)mb * 1024u * 1024u;
    return 0;
}

static int parse_capture_seconds_from_env(void)
{
    const char *env = getenv("IWFG_CAPTURE_SECONDS");

    if (!env || !*env)
        return 0;

    char *end = NULL;
    unsigned long seconds = strtoul(env, &end, 10);
    if (end == env || *end != '\0' || seconds > UINT32_MAX) {
        fprintf(stderr,
                "Invalid IWFG_CAPTURE_SECONDS='%s' "
                "(expected seconds, 0 disables the timer)\n",
                env);
        return -1;
    }

    g_capture_seconds = (unsigned int)seconds;
    return 0;
}

static int parse_aio_depth_from_env(void)
{
    const char *env = getenv("IWFG_CAPTURE_AIO_DEPTH");

    if (!env || !*env)
        return 0;

    char *end = NULL;
    unsigned long depth = strtoul(env, &end, 10);
    if (end == env || *end != '\0' || depth == 0 || depth > (unsigned long)MAX_NUM_BUFFERS) {
        fprintf(stderr, "Invalid IWFG_CAPTURE_AIO_DEPTH='%s' (expected 1..%d)\n",
                env, MAX_NUM_BUFFERS);
        return -1;
    }

    g_aio_depth = (unsigned int)depth;
    return 0;
}

static int parse_channel_name(const char *text, unsigned int *channel)
{
    char *end = NULL;
    unsigned long value;

    if (!text || !*text || !channel)
        return -1;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || end == text || *end != '\0' || value >= MAX_CAPTURE_CHANNELS)
        return -1;

    *channel = (unsigned int)value;
    return 0;
}

static int parse_channel_from_env(void)
{
    const char *env = getenv("IWFG_CAPTURE_CHANNEL");

    if (!env || !*env)
        return 0;

    if (parse_channel_name(env, &g_channel) != 0) {
        fprintf(stderr,
                "Invalid IWFG_CAPTURE_CHANNEL='%s' (expected 0..%d)\n",
                env, MAX_CAPTURE_CHANNELS - 1);
        return -1;
    }

    return 0;
}

/* Parse all value-bearing environment variables before command-line options. */
static int parse_environment(void)
{
    if (parse_buffer_count_from_env() != 0 ||
        parse_buffer_size_from_env() != 0 ||
        parse_limit_from_env() != 0 ||
        parse_capture_seconds_from_env() != 0 ||
        parse_aio_depth_from_env() != 0 ||
        parse_channel_from_env() != 0)
        return -1;

    return 0;
}

static void update_channel_paths(void)
{
    char output_env_name[64];
    const char *configured_output;

    snprintf(g_device_node, sizeof(g_device_node), DEVICE_NODE_TEMPLATE, g_channel * 2u);

    snprintf(output_env_name, sizeof(output_env_name),
             "IWFG_CAPTURE_OUTPUT_CH%u", g_channel);
    configured_output = getenv(output_env_name);

    if (configured_output && *configured_output)
        snprintf(g_output_file, sizeof(g_output_file), "%s", configured_output);
    else
        snprintf(g_output_file, sizeof(g_output_file), OUTPUT_FILE_TEMPLATE, g_channel);
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-c 0..3] [-o output.bin] [-t seconds]\n"
            "\n"
            "Reads from one C2H DMA node and stores everything to a file.\n"
            "The FPGA data source must already be streaming (or be controlled\n"
            "externally); this app performs no test-source control.\n"
            "\n"
            "  -c, --channel N   channel 0..%d (default 0)\n"
            "  -o, --output F    output file (default capture<N>.bin)\n"
            "  -t, --time S      capture duration in seconds (default 0 = until Ctrl+C)\n"
            "\n"
            "Environment overrides:\n"
            "  IWFG_CAPTURE_NUM_BUFS   ring buffers (default %d)\n"
            "  IWFG_CAPTURE_BUF_MB     buffer size MB (default %d)\n"
            "  IWFG_CAPTURE_LIMIT_MB   stop after this many MB written\n"
            "  IWFG_CAPTURE_SECONDS    capture duration (same as -t)\n"
            "  IWFG_CAPTURE_AIO_DEPTH  AIO queue depth (default 32)\n"
            "  IWFG_CAPTURE_AIO=0      synchronous writes instead of AIO\n"
            "  IWFG_CAPTURE_DIRECT=0   disable O_DIRECT\n"
            "  IWFG_CAPTURE_PREARM=0   disable descriptor pre-arm\n"
            "  IWFG_CAPTURE_NO_WRITE=1 discard data (rate test)\n"
            "  IWFG_DMA_CPU / IWFG_WRITER_CPU  pin worker threads\n"
            "\n"
            "Channels map to C2H devices and output files:\n"
            "  channel 0 -> /dev/iwfg0 -> capture0.bin\n"
            "  channel 1 -> /dev/iwfg2 -> capture1.bin\n"
            "  channel 2 -> /dev/iwfg4 -> capture2.bin\n"
            "  channel 3 -> /dev/iwfg6 -> capture3.bin\n",
            prog, MAX_CAPTURE_CHANNELS - 1,
            DEFAULT_NUM_BUFFERS, DEFAULT_BUFFER_SIZE / (1024 * 1024));
}

static int parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }

        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--channel") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                return -1;
            }
            if (parse_channel_name(argv[i], &g_channel) != 0) {
                fprintf(stderr, "Invalid channel '%s' (expected 0..%d)\n",
                        argv[i], MAX_CAPTURE_CHANNELS - 1);
                return -1;
            }
            continue;
        } else if (strncmp(arg, "--channel=", 10) == 0) {
            if (parse_channel_name(arg + 10, &g_channel) != 0) {
                fprintf(stderr, "Invalid channel '%s' (expected 0..%d)\n",
                        arg + 10, MAX_CAPTURE_CHANNELS - 1);
                return -1;
            }
            continue;
        }

        if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                return -1;
            }
            char env_name[64];
            snprintf(env_name, sizeof(env_name),
                     "IWFG_CAPTURE_OUTPUT_CH%u", g_channel);
            setenv(env_name, argv[i], 1);
            continue;
        }

        if (strcmp(arg, "-t") == 0 || strcmp(arg, "--time") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                return -1;
            }
            char *end = NULL;
            unsigned long seconds = strtoul(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || seconds > UINT32_MAX) {
                fprintf(stderr, "Invalid time '%s' (expected seconds)\n", argv[i]);
                return -1;
            }
            g_capture_seconds = (unsigned int)seconds;
            continue;
        }

        fprintf(stderr, "Unknown argument: %s\n", arg);
        print_usage(argv[0]);
        return -1;
    }

    return 0;
}

static int parse_bool_env(const char *name)
{
    const char *env = getenv(name);

    if (!env || !*env)
        return 0;

    return strcmp(env, "0") != 0 &&
           strcmp(env, "false") != 0 &&
           strcmp(env, "FALSE") != 0 &&
           strcmp(env, "no") != 0 &&
           strcmp(env, "NO") != 0;
}

static int parse_cpu_from_env(const char *name)
{
    const char *env = getenv(name);
    if (!env || !*env)
        return -1;

    char *end = NULL;
    long cpu = strtol(env, &end, 10);
    if (end == env || *end != '\0' || cpu < 0)
        return -1;

    return (int)cpu;
}

static void configure_current_thread_rt(int priority, int cpu_id)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = priority;

    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        g_sched_warn_count++;

    if (cpu_id >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((size_t)cpu_id, &set);

        if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
            g_sched_warn_count++;
    }
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        p += (size_t)n;
        left -= (size_t)n;
    }

    return 0;
}

static int64_t signed_byte_delta(uint64_t captured, uint64_t processed)
{
    if (captured >= processed)
        return (int64_t)(captured - processed);

    return -(int64_t)(processed - captured);
}

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec ts;

    ts.tv_sec = milliseconds / 1000u;
    ts.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        if (g_shutdown_requested || atomic_load(&g_stop_requested))
            break;
    }
}

static void channel_log(const char *stage)
{
    fprintf(stderr, "[ch%u] %s\n", g_channel, stage);
}

static void free_buffers(void)
{
    for (int i = 0; i < g_num_buffers; i++) {
        free(g_buf[i].ptr);
        g_buf[i].ptr = NULL;
    }
}

static int init_capture_semaphores(void)
{
    if (sem_init(&g_full_slots, 0, 0) != 0)
        return -1;

    g_full_slots_initialized = 1;
    for (int i = 0; i < g_num_buffers; i++) {
        if (sem_init(&g_buffer_empty[i], 0, 1) != 0) {
            destroy_capture_semaphores();
            return -1;
        }
        g_buffer_empty_initialized++;
    }

    return 0;
}

static void destroy_capture_semaphores(void)
{
    while (g_buffer_empty_initialized > 0) {
        g_buffer_empty_initialized--;
        sem_destroy(&g_buffer_empty[g_buffer_empty_initialized]);
    }

    if (g_full_slots_initialized) {
        sem_destroy(&g_full_slots);
        g_full_slots_initialized = 0;
    }
}

static void request_stop(void)
{
    atomic_store(&g_running, false);
    atomic_store(&g_stop_requested, true);

    for (int i = 0; i < g_buffer_empty_initialized; i++)
        sem_post(&g_buffer_empty[i]);

    if (!g_drain_on_stop && g_full_slots_initialized)
        sem_post(&g_full_slots);
}

static void interrupt_worker_thread(pthread_t tid)
{
    int ret = pthread_kill(tid, SIGUSR1);

    if (ret != 0 && ret != ESRCH)
        g_sched_warn_count++;
}

static void print_progress_line(void)
{
    if (g_captured_frames || g_total_frames) {
        int64_t backlog = signed_byte_delta(g_captured_bytes, g_total_bytes);

        printf("Captured: %" PRIu64 " bytes  %s: %" PRIu64
               " bytes  Backlog: %" PRId64 " bytes  Overflows: %" PRIu64
               "  SchedWarn: %" PRIu64 "\r",
               (uint64_t)g_captured_bytes,
               g_no_write ? "Processed" : "Written",
               (uint64_t)g_total_bytes,
               backlog,
               (uint64_t)g_eagain_count,
               (uint64_t)g_sched_warn_count);
    } else {
        printf("Waiting for data... (overflows: %" PRIu64 ")\r",
               (uint64_t)g_eagain_count);
    }
}

static void install_channel_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    memset(&action, 0, sizeof(action));
    action.sa_handler = worker_wakeup_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);
}

static int open_capture_endpoints(void)
{
    int output_flags = O_CREAT | O_TRUNC | O_WRONLY;

    channel_log("opening device");
    g_fd = open(g_device_node, O_RDWR);
    if (g_fd < 0) {
        perror("open device");
        return -1;
    }
    channel_log("device opened");

    if (g_no_write)
        return 0;

    if (g_direct_write)
        output_flags |= O_DIRECT;

    channel_log("opening output file");
    g_out_fd = open(g_output_file, output_flags, 0644);
    if (g_out_fd < 0) {
        perror("open output");
        return -1;
    }
    channel_log("output file opened");
    return 0;
}

static int prepare_capture_buffers(void)
{
    channel_log("initializing semaphores");
    if (init_capture_semaphores() != 0) {
        perror("sem_init");
        return -1;
    }

    channel_log("allocating buffers");
    if (allocate_buffers() != 0) {
        fprintf(stderr, "buffer allocation failed\n");
        return -1;
    }
    channel_log("buffers allocated");

    channel_log("registering buffers with driver");
    if (register_buffers() != 0)
        return -1;

    channel_log("buffers registered");
    return 0;
}

static void print_capture_configuration(void)
{
    printf("Starting capture...\n");
    printf("Channel: %u\n", g_channel);
    printf("Device : %s\n", g_device_node);
    printf("Output : %s\n", g_no_write ? "(disabled)" : g_output_file);
    printf("Buffers: %d x %.2f MB\n", g_num_buffers,
           (double)g_buffer_size / (1024.0 * 1024.0));
    if (g_limit_bytes)
        printf("Limit  : %.2f MB\n",
               (double)g_limit_bytes / (1024.0 * 1024.0));
    if (g_capture_seconds)
        printf("Time   : %u seconds\n", g_capture_seconds);
    else
        printf("Time   : until Ctrl+C\n");
    printf("Direct : %s\n", g_direct_write ? "enabled" : "disabled");
    printf("AIO    : %s", g_aio_write ? "enabled" : "disabled");
    if (g_aio_write)
        printf(" (depth %u)", g_aio_depth);
    printf("\n");
    printf("Prearm : %s\n", g_prearm ? "enabled" : "disabled");
    printf("Drain  : %s\n", g_drain_on_stop ? "enabled" : "disabled");
    printf("Press Ctrl+C to stop\n\n");
}

static void print_capture_summary(void)
{
    int64_t backlog = signed_byte_delta(g_captured_bytes, g_total_bytes);

    printf("\nStopped channel %u\n", g_channel);
    printf("Captured frames : %" PRIu64 "\n", (uint64_t)g_captured_frames);
    printf("Written frames  : %" PRIu64 "\n", (uint64_t)g_total_frames);
    printf("Captured        : %.2f MB\n",
           (double)atomic_load(&g_captured_bytes) / (1024.0 * 1024.0));
    printf("%-16s: %.2f MB\n", g_no_write ? "Data" : "Written",
           (double)atomic_load(&g_total_bytes) / (1024.0 * 1024.0));
    printf("Backlog         : %.2f MB\n",
           (double)backlog / (1024.0 * 1024.0));

    if (!g_drain_on_stop && backlog > 0)
        printf("Not written     : %.2f MB "
               "(set IWFG_CAPTURE_DRAIN_ON_STOP=1 to drain)\n",
               (double)backlog / (1024.0 * 1024.0));
}

static void stop_capture_workers(capture_workers_t *workers)
{
    request_stop();

    if (workers->dma_started)
        interrupt_worker_thread(workers->dma_tid);
    if (workers->writer_started)
        interrupt_worker_thread(workers->writer_tid);

    if (workers->dma_started) {
        pthread_join(workers->dma_tid, NULL);
        workers->dma_started = false;
    }
    if (workers->writer_started) {
        pthread_join(workers->writer_tid, NULL);
        workers->writer_started = false;
    }
}

static void cleanup_capture_resources(void)
{
    int device_fd;

    if (g_out_fd >= 0) {
        close(g_out_fd);
        g_out_fd = -1;
    }

    /* Closing the device unregisters/unpins buffers before userspace frees them. */
    device_fd = atomic_exchange(&g_fd, -1);
    if (device_fd >= 0)
        close(device_fd);

    free_buffers();
    destroy_capture_semaphores();
}

/* ======================= Capture orchestration ======================= */

static int run_capture_channel(void)
{
    capture_workers_t workers = { 0 };
    bool capture_started = false;
    int exit_code = 0;

    g_running = 1;
    g_stop_requested = 0;
    g_capture_done = 0;

    update_channel_paths();
    channel_log("starting setup");

    if (open_capture_endpoints() != 0) {
        exit_code = 1;
        goto cleanup;
    }

    if (prepare_capture_buffers() != 0) {
        exit_code = 1;
        goto cleanup;
    }

    print_capture_configuration();

    if (pthread_create(&workers.dma_tid, NULL, dma_thread, NULL) != 0) {
        perror("pthread_create dma");
        exit_code = 1;
        goto stop_workers;
    }
    workers.dma_started = true;

    if (pthread_create(&workers.writer_tid, NULL, writer_thread, NULL) != 0) {
        perror("pthread_create writer");
        exit_code = 1;
        goto stop_workers;
    }
    workers.writer_started = true;
    capture_started = true;

    arm_capture_deadline();

    while (g_running) {
        if (g_shutdown_requested) {
            request_stop();
            break;
        }

        print_progress_line();
        fflush(stdout);

        if (capture_deadline_expired()) {
            request_stop();
            break;
        }

        sleep(1);
    }

stop_workers:
    stop_capture_workers(&workers);

    if (g_out_fd >= 0)
        fsync(g_out_fd);

    if (capture_started)
        print_capture_summary();

cleanup:
    cleanup_capture_resources();

    return exit_code;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    install_channel_signal_handlers();
    g_shutdown_requested = 0;
    g_stop_announced = 0;

    if (parse_environment() != 0)
        return 1;

    if (parse_args(argc, argv) != 0)
        return 1;

    g_no_write = parse_bool_env("IWFG_CAPTURE_NO_WRITE");
    if (getenv("IWFG_CAPTURE_DIRECT") != NULL)
        g_direct_write = parse_bool_env("IWFG_CAPTURE_DIRECT");
    if (getenv("IWFG_CAPTURE_AIO") != NULL)
        g_aio_write = parse_bool_env("IWFG_CAPTURE_AIO");
    if (getenv("IWFG_CAPTURE_DRAIN_ON_STOP") != NULL)
        g_drain_on_stop = parse_bool_env("IWFG_CAPTURE_DRAIN_ON_STOP");
    if (getenv("IWFG_CAPTURE_PREARM") != NULL)
        g_prearm = parse_bool_env("IWFG_CAPTURE_PREARM");

    if (g_aio_write)
        g_direct_write = 1;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        perror("mlockall");

    if (g_prearm && g_num_buffers < 2) {
        fprintf(stderr,
                "IWFG_CAPTURE_PREARM requires IWFG_CAPTURE_NUM_BUFS >= 2\n");
        return 1;
    }

    return run_capture_channel();
}
