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

#define DEVICE_NODE "/dev/iwfg0"
#define OUTPUT_FILE "capture.bin"
#define COUNTER_MASK 0x00FFFFFFu
#define MAX_VERIFY_LOGS 10

#define DEFAULT_NUM_BUFFERS 16
#define MAX_NUM_BUFFERS 16
#define DEFAULT_BUFFER_SIZE (16 * 1024 * 1024)

static size_t g_buffer_size = DEFAULT_BUFFER_SIZE;
static int g_num_buffers = DEFAULT_NUM_BUFFERS;
static int g_no_write = 0;
static int g_verify = 0;
static int g_verify_full = 0;
static uint64_t g_limit_bytes = 0;

typedef struct {
    void *ptr;
} capture_buffer_t;

static int g_fd = -1;
static int g_out_fd = -1;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_stop_announced = 0;
static volatile sig_atomic_t g_stop_requested = 0;

static capture_buffer_t g_buf[MAX_NUM_BUFFERS];

static sem_t g_empty_slots;
static sem_t g_full_slots;

static uint64_t g_total_bytes = 0;
static uint64_t g_total_frames = 0;
static volatile uint64_t g_eagain_count = 0;
static volatile uint64_t g_sched_warn_count = 0;

/* -------------------------------------------------- */
/* Buffer and Device Management Functions */
/* -------------------------------------------------- */

static int allocate_buffers(void) {
    for (int i = 0; i < g_num_buffers; i++) {
        if (posix_memalign(&g_buf[i].ptr, 4096, g_buffer_size)) {
            perror("posix_memalign failed");
            return -1;
        }
    }
    return 0;
}

static int register_buffers(void) {
    for (int i = 0; i < g_num_buffers; i++) {
        struct iwfg_user_buf req;
        memset(&req, 0, sizeof(req));
        req.buf = g_buf[i].ptr;
        req.size = g_buffer_size;
        req.buf_type = IWFG_BUF_TYPE_USERPTR;

        if (ioctl(g_fd, IWFG_IOCTL_ADD_BUF, &req) < 0) {
            perror("IWFG_IOCTL_ADD_BUF failed");
            return -1;
        }
    }
    return 0;
}

static void free_buffers(void) {
    for (int i = 0; i < g_num_buffers; i++) {
        free(g_buf[i].ptr);
        g_buf[i].ptr = NULL;
    }
}

/* -------------------------------------------------- */
/* Thread Functions */
/* -------------------------------------------------- */

static void *dma_thread(void *arg) {
    (void)arg;
    int index = 0;

    while (g_running) {
        if (sem_wait(&g_empty_slots) == -1) {
            if (errno == EINTR) continue;
            break;
        }

        if (!g_running) break;

        int next_index = (index + 1) % g_num_buffers;

        // Simulate data capture (for testing purposes)
        g_total_frames++;
        g_total_bytes += g_buffer_size;

        // Simulate capturing data by writing stats every 100 frames
        if (g_total_frames % 100 == 0) {
            FILE *stats_file = fopen("stats.csv", "a");
            if (stats_file != NULL) {
                fprintf(stats_file, "%lu,%lu\n", g_total_frames, g_total_bytes);
                fclose(stats_file);
            }
        }

        sem_post(&g_full_slots);
        index = (index + 1) % g_num_buffers;
    }

    return NULL;
}

static void *writer_thread(void *arg) {
    (void)arg;

    while (g_running) {
        if (sem_wait(&g_full_slots) == -1) {
            if (errno == EINTR) continue;
            break;
        }

        if (!g_running) break;

        // Simulate writing to output file
        if (!g_no_write) {
            g_total_bytes += g_buffer_size;
        }

        g_total_frames++;
        sem_post(&g_empty_slots);

        if (g_limit_bytes && g_total_bytes >= g_limit_bytes) {
            g_running = 0;  // Request stop
            break;
        }
    }

    return NULL;
}

/* -------------------------------------------------- */
/* Signal Handling and Cleanup */
/* -------------------------------------------------- */

static void stop_handler(int sig) {
    (void)sig;  // To avoid unused variable warning
    g_running = 0;
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }

    for (int i = 0; i < g_num_buffers; i++) {
        sem_post(&g_empty_slots);
        sem_post(&g_full_slots);
    }
}

static void request_stop(int sig) {
    stop_handler(sig);
}

/* -------------------------------------------------- */
/* Main Program */
/* -------------------------------------------------- */

int main(void) {
    pthread_t dma_tid;
    pthread_t writer_tid;

    // Register signal handlers
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    // Open the device node
    g_fd = open(DEVICE_NODE, O_RDWR);
    if (g_fd < 0) {
        perror("open device");
        return 1;
    }

    // Open the output file
    g_out_fd = open(OUTPUT_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (g_out_fd < 0) {
        perror("open output file");
        close(g_fd);
        return 1;
    }

    // Initialize semaphores for buffer management
    if (sem_init(&g_empty_slots, 0, g_num_buffers) != 0 || sem_init(&g_full_slots, 0, 0) != 0) {
        perror("sem_init failed");
        close(g_out_fd);
        g_out_fd = -1;
        close(g_fd);
        return 1;
    }

    // Allocate and register buffers for DMA
    if (allocate_buffers() != 0) {
        fprintf(stderr, "buffer allocation failed\n");
        sem_destroy(&g_empty_slots);
        sem_destroy(&g_full_slots);
        close(g_out_fd);
        close(g_fd);
        return 1;
    }

    if (register_buffers() != 0) {
        free_buffers();
        sem_destroy(&g_empty_slots);
        sem_destroy(&g_full_slots);
        close(g_out_fd);
        close(g_fd);
        return 1;
    }

    printf("Starting capture...\n");
    printf("Device : %s\n", DEVICE_NODE);
    printf("Output : %s\n", g_no_write ? "(disabled)" : OUTPUT_FILE);
    printf("Buffers: %d x %.2f MB\n",
            g_num_buffers,
            g_buffer_size / (1024.0 * 1024.0));

    // Create threads for data capture and writing
    pthread_create(&dma_tid, NULL, dma_thread, NULL);
    pthread_create(&writer_tid, NULL, writer_thread, NULL);

    // Main loop: Wait for threads to complete or stop
    while (g_running) {
        sleep(1);
    }

    // Join the threads
    pthread_join(dma_tid, NULL);
    pthread_join(writer_tid, NULL);

    // Clean up resources
    free_buffers();
    sem_destroy(&g_empty_slots);
    sem_destroy(&g_full_slots);
    close(g_out_fd);
    close(g_fd);

    printf("Capture stopped\n");

    return 0;
}
