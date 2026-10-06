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
#include <sys/stat.h>

#include "iwfg_user.h"

/*
 * fifo_stream.c - iwfg C2H live streaming to a FIFO (e.g. for GNU Radio)
 * ----------------------------------------------------------------------
 *
 * Threaded rework of fifo_pipe.c in the c2h_store style.  The point of the
 * two-thread ring is throughput to the FIFO reader:
 *
 *   - Old fifo_pipe was strictly serial: DMA fill -> FIFO write -> DMA fill.
 *     While the (slow, bursty) GNU Radio reader drained a buffer, the only
 *     DMA overlap was the single kernel-side pre-armed buffer.
 *   - Here the DMA thread fills the ring independently and the writer thread
 *     keeps the FIFO permanently saturated.  GNU Radio's read jitter is
 *     absorbed by the ring instead of stalling DMA immediately.
 *
 * Semantics are unchanged from the old app: everything DMA'd is delivered
 * in order (backpressure model).  When GNU Radio consumes slower than the
 * card produces, the ring fills, the DMA thread blocks on an empty slot,
 * and the excess is dropped upstream in the FPGA - exactly as before, but
 * with a much deeper cushion and no per-buffer serialization.
 *
 * Carried-over fixes from fifo_pipe.c:
 *   - Signal handlers installed WITHOUT SA_RESTART so Ctrl+C interrupts the
 *     blocked IWFG_IOCTL_DMA_DATA / FIFO open / FIFO write with EINTR.
 *   - SIGPIPE ignored: a vanished FIFO reader surfaces as write()==-1/EPIPE
 *     and is handled as a normal shutdown, not process death.
 *   - Teardown order: close(fifo) -> close(device) (driver unpins buffers
 *     in iwfg_release) -> free(buffers).
 *   - EINTR in write_all checks the stop flag instead of spinning.
 *
 * New in this version:
 *   - FIFO inode is REUSED if the path already exists as a FIFO.  The old
 *     unlink+mkfifo dance strands any reader already blocked on the old
 *     inode (it waits forever on a FIFO nobody will ever write to).
 *   - The kernel pipe buffer is enlarged with F_SETPIPE_SZ (default 4 MB,
 *     IWFG_FIFO_PIPE_MB to override).  The stock 64 KB pipe forces a
 *     wakeup/context-switch roughly every 64 KB moved; a multi-MB pipe cuts
 *     that overhead and rides through reader scheduling gaps.
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
#define DEFAULT_FIFO_PATH "/tmp/iwfg_fifo"
#define MAX_CAPTURE_CHANNELS 4

#define DEFAULT_NUM_BUFFERS 8
#define MAX_NUM_BUFFERS 512
#define DEFAULT_BUFFER_SIZE (16 * 1024 * 1024)
#define DEFAULT_PIPE_MB 4u

typedef struct {
    size_t buffer_size;
    int num_buffers;
    bool prearm;
    unsigned int pipe_mb;
    unsigned int channel;
    char device_node[PATH_MAX];
    bool device_node_set;
    char fifo_path[PATH_MAX];
} stream_config_t;

static stream_config_t g_config = {
    .buffer_size = DEFAULT_BUFFER_SIZE,
    .num_buffers = DEFAULT_NUM_BUFFERS,
    .prearm = true,
    .pipe_mb = DEFAULT_PIPE_MB,
    .fifo_path = DEFAULT_FIFO_PATH,
};

#define g_buffer_size  (g_config.buffer_size)
#define g_num_buffers  (g_config.num_buffers)
#define g_prearm       (g_config.prearm)
#define g_pipe_mb      (g_config.pipe_mb)
#define g_channel      (g_config.channel)
#define g_device_node  (g_config.device_node)
#define g_fifo_path    (g_config.fifo_path)

typedef struct {
    void *ptr;
    uint32_t index;
} stream_buffer_t;

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
} stream_workers_t;

/* ============================ Global state ============================ */

static atomic_int g_fd = -1;
static int g_fifo_fd = -1;
static bool g_fifo_created = false;

static stream_buffer_t g_buf[MAX_NUM_BUFFERS];
static sem_t g_full_slots;
static sem_t g_buffer_empty[MAX_NUM_BUFFERS];
static int g_buffer_empty_initialized;
static bool g_full_slots_initialized;

static atomic_bool g_running;
static atomic_bool g_stop_requested;
static atomic_bool g_capture_done;

static atomic_uint_fast64_t g_streamed_bytes;   /* written to FIFO */
static atomic_uint_fast64_t g_streamed_frames;
static atomic_uint_fast64_t g_captured_bytes;   /* DMA'd from card */
static atomic_uint_fast64_t g_captured_frames;
static atomic_uint_fast64_t g_eagain_count;
static atomic_uint_fast64_t g_sched_warn_count;

/* Signal handlers only touch this async-signal-safe process-wide flag. */
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_stop_announced = 0;

static void free_buffers(void);
static int init_stream_semaphores(void);
static void destroy_stream_semaphores(void);
static void request_stop(void);
static void interrupt_worker_thread(pthread_t tid);
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
static void print_stream_configuration(void);
static void print_stream_summary(void);
static void install_signal_handlers(void);
static int open_device(void);
static int prepare_fifo(void);
static int wait_for_fifo_reader(void);
static int prepare_stream_buffers(void);
static void stop_stream_workers(stream_workers_t *workers);
static void cleanup_stream_resources(void);
static int run_stream_channel(void);

/* ===================== Signal handling ====================== */

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

/*
 * SIGINT/SIGTERM installed WITHOUT SA_RESTART so they interrupt the blocked
 * ioctl / FIFO open / FIFO write.  SIGPIPE ignored so a vanished reader
 * surfaces as write()==-1/EPIPE instead of killing the process.
 */
static void install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; /* deliberately NO SA_RESTART */
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    memset(&action, 0, sizeof(action));
    action.sa_handler = worker_wakeup_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPIPE, &action, NULL);
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

        if (!g_running) {
            if (next_reserved)
                sem_post(&g_buffer_empty[next_index]);
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
                sleep_ms(1); /* no data yet, retry */
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

/* ====================== FIFO writer thread ======================= */

static void *writer_thread(void *arg)
{
    (void)arg;
    int index = 0;
    int wr_cpu = parse_cpu_from_env("IWFG_WRITER_CPU");
    /* Keep producer and consumer on separate CPUs unless explicitly pinned. */
    if (wr_cpu < 0)
        wr_cpu = (int)(g_channel * 2 + 1);

    configure_current_thread_rt(70, wr_cpu);

    while (g_running || !g_capture_done) {

        if (sem_wait(&g_full_slots) == -1) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (!g_running)
            break;

        if (g_capture_done && g_streamed_frames >= g_captured_frames)
            break;

        if (write_all(g_fifo_fd, g_buf[index].ptr, g_buffer_size) < 0) {
            if (errno == EPIPE)
                fprintf(stderr, "\nFIFO reader disconnected, shutting down...\n");
            else if (errno == EINTR)
                fprintf(stderr, "\nFIFO write interrupted, shutting down...\n");
            else
                perror("write to fifo");
            request_stop();
            break;
        }

        g_streamed_bytes += g_buffer_size;
        g_streamed_frames++;
        sem_post(&g_buffer_empty[index]);

        index = (index + 1) % g_num_buffers;
    }

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
    const char *env = getenv("IWFG_STREAM_NUM_BUFS");
    if (!env || !*env)
        return 0;

    char *end = NULL;
    long count = strtol(env, &end, 10);
    if (end == env || *end != '\0' || count <= 0 || count > MAX_NUM_BUFFERS) {
        fprintf(stderr,
                "Invalid IWFG_STREAM_NUM_BUFS='%s' (expected 1..%d)\n",
                env, MAX_NUM_BUFFERS);
        return -1;
    }

    g_num_buffers = (int)count;
    return 0;
}

static int parse_buffer_size_from_env(void)
{
    const char *env = getenv("IWFG_STREAM_BUF_MB");
    if (!env || !*env)
        return 0;

    char *end = NULL;
    unsigned long mb = strtoul(env, &end, 10);
    if (end == env || *end != '\0' || mb == 0 ||
        mb > UINT32_MAX / (1024u * 1024u)) {
        fprintf(stderr,
                "Invalid IWFG_STREAM_BUF_MB='%s' "
                "(expected 1..%u MB)\n",
                env, UINT32_MAX / (1024u * 1024u));
        return -1;
    }

    g_buffer_size = (size_t)mb * 1024u * 1024u;
    return 0;
}

static int parse_pipe_mb_from_env(void)
{
    const char *env = getenv("IWFG_FIFO_PIPE_MB");

    if (!env || !*env)
        return 0;

    char *end = NULL;
    unsigned long mb = strtoul(env, &end, 10);
    if (end == env || *end != '\0' || mb > 1024u) {
        fprintf(stderr,
                "Invalid IWFG_FIFO_PIPE_MB='%s' (expected 0..1024, 0 leaves default)\n",
                env);
        return -1;
    }

    g_pipe_mb = (unsigned int)mb;
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
    const char *env = getenv("IWFG_STREAM_CHANNEL");

    if (!env || !*env)
        return 0;

    if (parse_channel_name(env, &g_channel) != 0) {
        fprintf(stderr,
                "Invalid IWFG_STREAM_CHANNEL='%s' (expected 0..%d)\n",
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
        parse_pipe_mb_from_env() != 0 ||
        parse_channel_from_env() != 0)
        return -1;

    return 0;
}

static void update_channel_paths(void)
{
    if (!g_config.device_node_set)
        snprintf(g_device_node, sizeof(g_config.device_node),
                 DEVICE_NODE_TEMPLATE, g_channel * 2u);
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [device] [fifo]\n"
            "       %s [-c 0..3] [-f fifo]\n"
            "\n"
            "Streams one C2H DMA node into a FIFO for GNU Radio.\n"
            "The FPGA data source must already be streaming (or be controlled\n"
            "externally); this app performs no test-source control.\n"
            "\n"
            "  device            device node (default /dev/iwfg0; old fifo_pipe style)\n"
            "  fifo              FIFO path (default %s)\n"
            "  -c, --channel N   channel 0..%d, selects /dev/iwfg<2N>\n"
            "  -f, --fifo PATH   FIFO path\n"
            "\n"
            "Environment overrides:\n"
            "  IWFG_STREAM_NUM_BUFS  ring buffers (default %d)\n"
            "  IWFG_STREAM_BUF_MB    buffer size MB (default %d)\n"
            "  IWFG_FIFO_PIPE_MB     kernel pipe buffer MB (default %u, 0 leaves 64 KB)\n"
            "  IWFG_STREAM_PREARM=0  disable descriptor pre-arm\n"
            "  IWFG_DMA_CPU / IWFG_WRITER_CPU  pin worker threads\n"
            "\n"
            "Channels map to C2H devices:\n"
            "  channel 0 -> /dev/iwfg0\n"
            "  channel 1 -> /dev/iwfg2\n"
            "  channel 2 -> /dev/iwfg4\n"
            "  channel 3 -> /dev/iwfg6\n",
            prog, prog, DEFAULT_FIFO_PATH, MAX_CAPTURE_CHANNELS - 1,
            DEFAULT_NUM_BUFFERS, DEFAULT_BUFFER_SIZE / (1024 * 1024),
            DEFAULT_PIPE_MB);
}

static int parse_args(int argc, char **argv)
{
    int positional = 0;

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
        }

        if (strcmp(arg, "-f") == 0 || strcmp(arg, "--fifo") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                return -1;
            }
            snprintf(g_fifo_path, sizeof(g_config.fifo_path), "%s", argv[i]);
            continue;
        }

        if (arg[0] == '-') {
            fprintf(stderr, "Unknown argument: %s\n", arg);
            print_usage(argv[0]);
            return -1;
        }

        /* Old fifo_pipe positional style: [device] [fifo] */
        if (positional == 0) {
            snprintf(g_device_node, sizeof(g_config.device_node), "%s", arg);
            g_config.device_node_set = true;
        } else if (positional == 1) {
            snprintf(g_fifo_path, sizeof(g_config.fifo_path), "%s", arg);
        } else {
            fprintf(stderr, "Too many positional arguments: %s\n", arg);
            print_usage(argv[0]);
            return -1;
        }
        positional++;
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
            if (errno == EINTR) {
                if (g_shutdown_requested || !g_running)
                    return -1; /* signal means stop, not retry */
                continue;
            }
            return -1;         /* includes EPIPE: reader disconnected */
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

static int init_stream_semaphores(void)
{
    if (sem_init(&g_full_slots, 0, 0) != 0)
        return -1;

    g_full_slots_initialized = 1;
    for (int i = 0; i < g_num_buffers; i++) {
        if (sem_init(&g_buffer_empty[i], 0, 1) != 0) {
            destroy_stream_semaphores();
            return -1;
        }
        g_buffer_empty_initialized++;
    }

    return 0;
}

static void destroy_stream_semaphores(void)
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

    /* Live stream: no drain on stop, just wake the writer so it can exit. */
    if (g_full_slots_initialized)
        sem_post(&g_full_slots);
}

static void interrupt_worker_thread(pthread_t tid)
{
    int ret = pthread_kill(tid, SIGUSR1);

    if (ret != 0 && ret != ESRCH)
        g_sched_warn_count++;
}

static int open_device(void)
{
    channel_log("opening device");
    g_fd = open(g_device_node, O_RDWR | O_CLOEXEC);
    if (g_fd < 0) {
        perror("open device");
        return -1;
    }
    channel_log("device opened");
    return 0;
}

/*
 * Reuse an existing FIFO instead of unlink+mkfifo.  Recreating the inode
 * strands any reader (GNU Radio) already blocked in open() on the old one:
 * it would wait forever on a FIFO nobody will ever write to.
 */
static int prepare_fifo(void)
{
    struct stat st;

    if (stat(g_fifo_path, &st) == 0) {
        if (S_ISFIFO(st.st_mode)) {
            channel_log("reusing existing FIFO");
            return 0;
        }
        fprintf(stderr, "%s exists and is not a FIFO\n", g_fifo_path);
        return -1;
    }

    if (mkfifo(g_fifo_path, 0666) < 0) {
        perror("mkfifo");
        return -1;
    }

    g_fifo_created = true;
    return 0;
}

static int wait_for_fifo_reader(void)
{
    printf("Waiting for FIFO reader: %s\n", g_fifo_path);
    fflush(stdout);

    /*
     * Blocks until a reader opens the other end.  Without SA_RESTART,
     * Ctrl+C interrupts this open with EINTR.
     */
    while (!g_shutdown_requested) {
        g_fifo_fd = open(g_fifo_path, O_WRONLY);
        if (g_fifo_fd >= 0)
            break;
        if (errno == EINTR)
            continue; /* loop re-checks g_shutdown_requested */
        perror("open fifo");
        return -1;
    }

    if (g_shutdown_requested || g_fifo_fd < 0) {
        printf("Interrupted before a FIFO reader connected.\n");
        return -1;
    }

    /*
     * Enlarge the kernel pipe buffer.  The stock 64 KB pipe forces a
     * writer/reader wakeup roughly every 64 KB; a multi-MB pipe both cuts
     * that overhead and absorbs reader scheduling gaps.  Non-fatal if the
     * kernel refuses (pipe-max-size limits for non-root).
     */
    if (g_pipe_mb) {
        long requested = (long)g_pipe_mb * 1024L * 1024L;
        long actual = fcntl(g_fifo_fd, F_SETPIPE_SZ, requested);

        if (actual < 0)
            fprintf(stderr,
                    "F_SETPIPE_SZ %ld failed (%s); continuing with default pipe size\n",
                    requested, strerror(errno));
        else
            printf("Pipe buffer: %.2f MB\n", (double)actual / (1024.0 * 1024.0));
    }

    printf("FIFO connected! Streaming data...\n");
    fflush(stdout);
    return 0;
}

static int prepare_stream_buffers(void)
{
    channel_log("initializing semaphores");
    if (init_stream_semaphores() != 0) {
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

static void print_stream_configuration(void)
{
    printf("Starting stream...\n");
    printf("Channel: %u\n", g_channel);
    printf("Device : %s\n", g_device_node);
    printf("FIFO   : %s\n", g_fifo_path);
    printf("Buffers: %d x %.2f MB\n", g_num_buffers,
           (double)g_buffer_size / (1024.0 * 1024.0));
    printf("Prearm : %s\n", g_prearm ? "enabled" : "disabled");
    printf("Press Ctrl+C to stop\n\n");
}

static void print_stream_summary(void)
{
    int64_t backlog = signed_byte_delta(g_captured_bytes, g_streamed_bytes);

    printf("\nStopped channel %u\n", g_channel);
    printf("Captured frames : %" PRIu64 "\n", (uint64_t)g_captured_frames);
    printf("Streamed frames : %" PRIu64 "\n", (uint64_t)g_streamed_frames);
    printf("Captured        : %.2f MB\n",
           (double)atomic_load(&g_captured_bytes) / (1024.0 * 1024.0));
    printf("Streamed        : %.2f MB\n",
           (double)atomic_load(&g_streamed_bytes) / (1024.0 * 1024.0));
    printf("Backlog dropped : %.2f MB\n",
           (double)backlog / (1024.0 * 1024.0));
}

static void stop_stream_workers(stream_workers_t *workers)
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

static void cleanup_stream_resources(void)
{
    int device_fd;

    /*
     * Teardown in dependency order:
     *  1. FIFO first - unblocks/EOFs the reader.
     *  2. close(fd) - iwfg_release() tears down the RX queue, stops the C2H
     *     stream and unpins every registered buffer.
     *  3. Only then free() the buffers back to the allocator.
     */
    if (g_fifo_fd >= 0) {
        close(g_fifo_fd);
        g_fifo_fd = -1;
    }
    if (g_fifo_created)
        unlink(g_fifo_path);

    device_fd = atomic_exchange(&g_fd, -1);
    if (device_fd >= 0)
        close(device_fd);

    free_buffers();
    destroy_stream_semaphores();
}

/* ======================= Stream orchestration ======================= */

static int run_stream_channel(void)
{
    stream_workers_t workers = { 0 };
    bool stream_started = false;
    int exit_code = 0;
    uint64_t last_streamed = 0;
    uint64_t last_captured = 0;

    g_running = 1;
    g_stop_requested = 0;
    g_capture_done = 0;

    update_channel_paths();
    channel_log("starting setup");

    if (open_device() != 0) {
        exit_code = 1;
        goto cleanup;
    }

    if (prepare_stream_buffers() != 0) {
        exit_code = 1;
        goto cleanup;
    }

    if (prepare_fifo() != 0) {
        exit_code = 1;
        goto cleanup;
    }

    print_stream_configuration();

    /* Do not start DMA until a reader is attached, like the old fifo_pipe:
     * otherwise the ring fills with stale data GNU Radio sees at startup. */
    if (wait_for_fifo_reader() != 0)
        goto cleanup;

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
    stream_started = true;

    while (g_running) {
        if (g_shutdown_requested) {
            request_stop();
            break;
        }

        sleep(1);

        uint64_t streamed = g_streamed_bytes;
        uint64_t captured = g_captured_bytes;
        int64_t backlog = signed_byte_delta(captured, streamed);

        printf("Streaming: %.2f MB/s to FIFO  DMA: %.2f MB/s  "
               "Backlog: %" PRId64 " bytes  Total: %.2f MB  Overflows: %" PRIu64 "\n",
               (double)(streamed - last_streamed) / (1024.0 * 1024.0),
               (double)(captured - last_captured) / (1024.0 * 1024.0),
               backlog,
               (double)streamed / (1024.0 * 1024.0),
               (uint64_t)g_eagain_count);
        fflush(stdout);

        last_streamed = streamed;
        last_captured = captured;
    }

stop_workers:
    stop_stream_workers(&workers);

    if (stream_started)
        print_stream_summary();

cleanup:
    cleanup_stream_resources();

    printf("Cleanup complete.\n");
    return exit_code;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    install_signal_handlers();
    g_shutdown_requested = 0;
    g_stop_announced = 0;

    if (parse_environment() != 0)
        return 1;

    if (parse_args(argc, argv) != 0)
        return 1;

    if (getenv("IWFG_STREAM_PREARM") != NULL)
        g_prearm = parse_bool_env("IWFG_STREAM_PREARM");

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        perror("mlockall");

    if (g_prearm && g_num_buffers < 2) {
        fprintf(stderr,
                "IWFG_STREAM_PREARM requires IWFG_STREAM_NUM_BUFS >= 2\n");
        return 1;
    }

    return run_stream_channel();
}
