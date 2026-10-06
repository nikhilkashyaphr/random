#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "iwfg_user.h"

#define DEVICE_NODE "/dev/iwfg0"
#define OUTPUT_FILE "capture.bin"
#define DATA_WIDTH_BITS 256u
#define DATA_WORD_BYTES (DATA_WIDTH_BITS / 8u)
#define COUNTER_BYTE_OFFSET 0u
#define COUNTER_MASK 0x00FFFFFFu
#define MAX_VERIFY_LOGS 10

#define DEFAULT_NUM_BUFFERS 16
#define MAX_NUM_BUFFERS 128
#define DEFAULT_BUFFER_SIZE (16 * 1024 * 1024)

static size_t g_buffer_size = DEFAULT_BUFFER_SIZE;
static int g_num_buffers = DEFAULT_NUM_BUFFERS;
static int g_no_write = 0;
static int g_verify = 0;
static int g_verify_full = 0;
static uint64_t g_limit_bytes = 0;
static unsigned int g_capture_seconds = 0;

typedef struct {
    void *ptr;
} capture_buffer_t;

typedef struct {
    uint64_t sample;
    uint32_t prev;
    uint32_t curr;
} verify_error_t;

static int g_fd = -1;
static int g_out_fd = -1;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_stop_announced = 0;
static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_capture_done = 0;

static capture_buffer_t g_buf[MAX_NUM_BUFFERS];

static sem_t g_empty_slots;
static sem_t g_full_slots;

static uint64_t g_total_bytes = 0;
static uint64_t g_total_frames = 0;
static uint64_t g_captured_bytes = 0;
static uint64_t g_captured_frames = 0;
static volatile uint64_t g_eagain_count = 0;
static volatile uint64_t g_sched_warn_count = 0;
static uint64_t g_verify_samples = 0;
static uint64_t g_verify_errors = 0;
static uint64_t g_verify_buffers = 0;
static uint32_t g_verify_expected = 0;
static int g_verify_started = 0;
static verify_error_t g_verify_log[MAX_VERIFY_LOGS];
static int g_verify_log_count = 0;

static void free_buffers(void);
static void request_stop(void);
static int parse_buffer_size_from_env(void);
static int parse_buffer_count_from_env(void);
static int parse_limit_from_env(void);
static int parse_capture_seconds_from_env(void);
static int parse_bool_env(const char *name);
static void configure_current_thread_rt(int priority, int cpu_id);
static int parse_cpu_from_env(const char *name);
static int register_buffers(void);
static int write_all(int fd, const void *buf, size_t len);
static uint32_t read_counter_le24(const uint8_t *word);
static void verify_increment_buffer(const void *buf, size_t len);
static void verify_increment_buffer_fast(const void *buf, size_t len);

/* -------------------------------------------------- */
/* SAFE HARD EXIT */
/* -------------------------------------------------- */

static void stop_handler(int sig)
{
    (void)sig;

    if (g_stop_announced) {
        write(STDOUT_FILENO, "\nForce exiting...\n", 18);
        _exit(130);
    }

    g_stop_announced = 1;
    g_stop_requested = 1;
    g_running = 0;
    write(STDOUT_FILENO, "\nStopping safely...\n", 20);
}

/* -------------------------------------------------- */

static void *dma_thread(void *arg)
{
    (void)arg;
    int index = 0;
    int dma_cpu = parse_cpu_from_env("IWFG_DMA_CPU");

    configure_current_thread_rt(80, dma_cpu);

    while (g_running) {

        if (sem_wait(&g_empty_slots) == -1) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (!g_running)
            break;

        /* Combined buffer: kernel reads prearm at arg + sizeof(req) */
        struct {
            struct iwfg_user_dma_req req;
            struct iwfg_c2h_prearm   prearm;
        } iarg;
        int next_index = (index + 1) % g_num_buffers;

        memset(&iarg, 0, sizeof(iarg));
        iarg.req.buf       = NULL;
        iarg.req.size      = g_buffer_size;
        iarg.req.offset    = 0;
        iarg.req.direction = IWFG_DIR_C2H_BUF;
        iarg.req.buf_index = index;
        iarg.req.flags     = 0;

        /* Pre-arm the next ring buffer so the FPGA keeps streaming with
         * zero dead window between consecutive IOCTL calls. */
        iarg.prearm.next_buf  = g_buf[next_index].ptr;
        iarg.prearm.next_size = g_buffer_size;

        if (ioctl(g_fd, IWFG_IOCTL_DMA_DATA, &iarg) < 0) {

            if (!g_running)
                break;

            if (errno == EINTR || errno == EBADF)
                break;

            if (errno == EAGAIN) {
                g_eagain_count++;
                sem_post(&g_empty_slots);
                continue;
            }

            perror("DMA ioctl");
            request_stop();
            break;
        }

        g_captured_bytes += g_buffer_size;
        g_captured_frames++;
        sem_post(&g_full_slots);

        index = (index + 1) % g_num_buffers;
    }

    g_capture_done = 1;
    sem_post(&g_full_slots);

    return NULL;
}

/* -------------------------------------------------- */

static void *writer_thread(void *arg)
{
    (void)arg;
    int index = 0;
    int wr_cpu = parse_cpu_from_env("IWFG_WRITER_CPU");

    configure_current_thread_rt(70, wr_cpu);

    while (g_running || !g_capture_done || g_total_frames < g_captured_frames) {

        if (sem_wait(&g_full_slots) == -1) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (g_capture_done && g_total_frames >= g_captured_frames)
            break;

        if (g_verify) {
            if (g_verify_full)
                verify_increment_buffer(g_buf[index].ptr, g_buffer_size);
			else
                verify_increment_buffer_fast(g_buf[index].ptr, g_buffer_size);
        }

        if (!g_no_write && write_all(g_out_fd, g_buf[index].ptr, g_buffer_size) != 0) {
            perror("write");
            request_stop();
            break;
        }

        g_total_bytes += g_buffer_size;
        g_total_frames++;
        sem_post(&g_empty_slots);

        if (g_limit_bytes && g_total_bytes >= g_limit_bytes) {
            request_stop();
            break;
        }
/*
        if ((g_total_frames % 100) == 0) {
            printf("Frames: %lu  Written: %.2f MB\r",
                   g_total_frames,
                   g_total_bytes / (1024.0 * 1024.0));
            fflush(stdout);
        }
*/
        index = (index + 1) % g_num_buffers;
    }

    return NULL;
}

/* -------------------------------------------------- */

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
        req.size = g_buffer_size;
        req.buf_type = IWFG_BUF_TYPE_USERPTR;
        req.import_fd = -1;
        req.is_chunked = false;
        req.chunk_size = 0;
        req.dmabuf_fd = -1;

        if (ioctl(g_fd, IWFG_IOCTL_ADD_BUF, &req) < 0) {
            perror("IWFG_IOCTL_ADD_BUF");
            return -1;
        }
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
    if (end == env || *end != '\0' || mb == 0) {
        fprintf(stderr, "Invalid IWFG_CAPTURE_BUF_MB='%s' (expected positive integer MB)\n", env);
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
    if (end == env || *end != '\0' || seconds == 0) {
        fprintf(stderr, "Invalid IWFG_CAPTURE_SECONDS='%s' (expected positive integer seconds)\n", env);
        return -1;
    }

    g_capture_seconds = (unsigned int)seconds;
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
        CPU_SET(cpu_id, &set);

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

static uint32_t read_counter_le24(const uint8_t *word)
{
    return (uint32_t)word[COUNTER_BYTE_OFFSET] |
           ((uint32_t)word[COUNTER_BYTE_OFFSET + 1u] << 8) |
           ((uint32_t)word[COUNTER_BYTE_OFFSET + 2u] << 16);
}

static void verify_increment_buffer(const void *buf, size_t len)
{
    const uint8_t *words = (const uint8_t *)buf;
    size_t count = len / DATA_WORD_BYTES;

    for (size_t i = 0; i < count; i++) {
        uint32_t curr = read_counter_le24(words + i * DATA_WORD_BYTES);

        if (!g_verify_started) {
            g_verify_expected = (curr + 1u) & COUNTER_MASK;
            g_verify_started = 1;
            g_verify_samples++;
            continue;
        }

        if (curr != g_verify_expected) {
            if (g_verify_log_count < MAX_VERIFY_LOGS) {
                verify_error_t *err = &g_verify_log[g_verify_log_count++];
                err->sample = g_verify_samples;
                err->prev = (g_verify_expected - 1u) & COUNTER_MASK;
                err->curr = curr;
            }
            g_verify_errors++;
        }

        g_verify_expected = (curr + 1u) & COUNTER_MASK;
        g_verify_samples++;
    }

    g_verify_buffers++;
}

static void verify_increment_buffer_fast(const void *buf, size_t len)
{
    const uint8_t *words = (const uint8_t *)buf;
    size_t count = len / DATA_WORD_BYTES;
    uint32_t first;
    uint32_t last;
    uint32_t expected_last;

    if (count == 0)
        return;

    first = read_counter_le24(words);
    last = read_counter_le24(words + (count - 1u) * DATA_WORD_BYTES);

    if (g_verify_started && first != g_verify_expected) {
        if (g_verify_log_count < MAX_VERIFY_LOGS) {
            verify_error_t *err = &g_verify_log[g_verify_log_count++];
            err->sample = g_verify_samples;
            err->prev = (g_verify_expected - 1u) & COUNTER_MASK;
            err->curr = first;
        }
        g_verify_errors++;
    }

    expected_last = (first + (uint32_t)(count - 1u)) & COUNTER_MASK;
    if (last != expected_last) {
        if (g_verify_log_count < MAX_VERIFY_LOGS) {
            verify_error_t *err = &g_verify_log[g_verify_log_count++];
            err->sample = g_verify_samples + count - 1u;
            err->prev = expected_last;
            err->curr = last;
        }
        g_verify_errors++;
    }

    g_verify_expected = (last + 1u) & COUNTER_MASK;
    g_verify_started = 1;
    g_verify_samples += count;
    g_verify_buffers++;
}

static void free_buffers(void)
{
    for (int i = 0; i < g_num_buffers; i++) {
        free(g_buf[i].ptr);
        g_buf[i].ptr = NULL;
    }
}

static void request_stop(void)
{
    g_running = 0;

    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }

    for (int i = 0; i < g_num_buffers; i++) {
        sem_post(&g_empty_slots);
    }
}

/* -------------------------------------------------- */

int main(void)
{
    pthread_t dma_tid;
    pthread_t writer_tid;

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    if (parse_buffer_count_from_env() != 0)
        return 1;

    if (parse_buffer_size_from_env() != 0)
        return 1;

    if (parse_limit_from_env() != 0)
        return 1;

    if (parse_capture_seconds_from_env() != 0)
        return 1;

    g_no_write = parse_bool_env("IWFG_CAPTURE_NO_WRITE");
    g_verify = parse_bool_env("IWFG_CAPTURE_VERIFY");
    g_verify_full = parse_bool_env("IWFG_CAPTURE_VERIFY_FULL");

    g_fd = open(DEVICE_NODE, O_RDWR);
    if (g_fd < 0) {
        perror("open device");
        return 1;
    }

    if (!g_no_write) {
        g_out_fd = open(OUTPUT_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (g_out_fd < 0) {
            perror("open output");
            close(g_fd);
            return 1;
        }
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        perror("mlockall");

    if (sem_init(&g_empty_slots, 0, g_num_buffers) != 0 ||
        sem_init(&g_full_slots, 0, 0) != 0) {
        perror("sem_init");
        close(g_out_fd);
        g_out_fd = -1;
        close(g_fd);
        return 1;
    }

    if (allocate_buffers() != 0) {
        fprintf(stderr, "buffer allocation failed\n");
        sem_destroy(&g_empty_slots);
        sem_destroy(&g_full_slots);
        close(g_out_fd);
        g_out_fd = -1;
        close(g_fd);
        return 1;
    }

    if (register_buffers() != 0) {
        free_buffers();
        sem_destroy(&g_empty_slots);
        sem_destroy(&g_full_slots);
        if (g_out_fd >= 0) {
            close(g_out_fd);
            g_out_fd = -1;
        }
        close(g_fd);
        return 1;
    }

    printf("Starting capture...\n");
    printf("Device : %s\n", DEVICE_NODE);
    printf("Output : %s\n", g_no_write ? "(disabled)" : OUTPUT_FILE);
        printf("Buffers: %d x %.2f MB\n",
            g_num_buffers,
            g_buffer_size / (1024.0 * 1024.0));
        if (g_limit_bytes)
            printf("Limit  : %.2f MB\n", g_limit_bytes / (1024.0 * 1024.0));
        if (g_capture_seconds)
            printf("Time   : %u seconds\n", g_capture_seconds);
        printf("Hint   : set IWFG_DMA_CPU and IWFG_WRITER_CPU to pin threads\n");
        printf("Hint   : set IWFG_CAPTURE_NUM_BUFS=64 (or up to %d) for deeper buffering\n",
            MAX_NUM_BUFFERS);
        printf("Verify : %u-bit words, 24-bit counter at byte offset %u\n",
            DATA_WIDTH_BITS, COUNTER_BYTE_OFFSET);
        printf("Hint   : set IWFG_CAPTURE_NO_WRITE=1 IWFG_CAPTURE_VERIFY=1 for DMA-only verification\n");
        printf("Hint   : set IWFG_CAPTURE_VERIFY_FULL=1 for slow per-word verification\n");
        printf("Hint   : set IWFG_CAPTURE_LIMIT_MB=19000 to stop automatically\n");
    printf("Press Ctrl+C to stop\n\n");

    pthread_create(&dma_tid, NULL, dma_thread, NULL);
    pthread_create(&writer_tid, NULL, writer_thread, NULL);

    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    while (g_running) {
        if (g_captured_frames || g_total_frames) {
            uint64_t backlog = g_captured_bytes - g_total_bytes;

            printf("Captured: %lu bytes  %s: %lu bytes  Backlog: %lu bytes  Overflows: %lu  SchedWarn: %lu  VerifyErr: %lu\r",
                   g_captured_bytes,
                   g_no_write ? "Processed" : "Written",
                   g_total_bytes,
                   backlog,
                   g_eagain_count,
                   g_sched_warn_count,
                   g_verify_errors);
        } else {
            printf("Waiting for data... (overflows: %lu)\r", g_eagain_count);
        }
        fflush(stdout);

        if (g_capture_seconds) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((unsigned int)(now.tv_sec - start_ts.tv_sec) >= g_capture_seconds)
                request_stop();
        }

        sleep(1);
    }

    if (g_stop_requested)
        request_stop();

    pthread_join(dma_tid, NULL);
    pthread_join(writer_tid, NULL);

    if (g_out_fd >= 0)
        fsync(g_out_fd);

    printf("\nStopped\n");
    printf("Captured frames : %lu\n", g_captured_frames);
    printf("Written frames  : %lu\n", g_total_frames);
    printf("Captured        : %.2f MB\n",
           g_captured_bytes / (1024.0 * 1024.0));
    printf("%s   : %.2f MB\n",
           g_no_write ? "Data" : "Written",
           g_total_bytes / (1024.0 * 1024.0));
    printf("Backlog         : %.2f MB\n",
           (g_captured_bytes - g_total_bytes) / (1024.0 * 1024.0));
    if (g_verify) {
        printf("Verify : %lu 256-bit words, %lu buffers, %lu errors%s\n",
               g_verify_samples, g_verify_buffers, g_verify_errors,
               g_verify_full ? " (full)" : " (fast)");
        for (int i = 0; i < g_verify_log_count; i++) {
            printf("  Error @ word %lu: prev=0x%06X curr=0x%06X\n",
                   g_verify_log[i].sample,
                   g_verify_log[i].prev,
                   g_verify_log[i].curr);
        }
    }

    free_buffers();

    sem_destroy(&g_empty_slots);
    sem_destroy(&g_full_slots);

    if (g_out_fd >= 0) {
        close(g_out_fd);
        g_out_fd = -1;
    }

    if (g_fd >= 0)
        close(g_fd);

    return 0;
}

