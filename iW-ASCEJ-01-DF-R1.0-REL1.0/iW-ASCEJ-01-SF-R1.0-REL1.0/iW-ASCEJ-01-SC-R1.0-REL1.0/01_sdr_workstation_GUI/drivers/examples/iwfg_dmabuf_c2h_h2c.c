/*
 * IWFG DMA-BUF C2H to H2C Loopback Test
 * 
 * This example demonstrates:
 * 1. Driver-allocated DMA buffers using IWFG_BUF_TYPE_DMA_ALLOC
 * 2. DMA-BUF export from kernel driver (without GPU usage)
 * 3. C2H capture -> H2C transmission pipeline
 * 4. Triple buffering with semaphore-based circular buffer synchronization
 * 
 * Architecture:
 * - Driver allocates DMA buffers using dma_alloc_from_contiguous()
 * - Buffers are exported as DMA-BUF file descriptors
 * - C2H thread captures frames into driver-allocated buffers
 * - H2C thread transmits frames from driver-allocated buffers
 * - Zero-copy between C2H and H2C (same physical buffer)
 * 
 * This is similar to iwfg_c2h_h2c_test.c but uses driver DMA allocation
 * instead of userspace malloc'd buffers. This provides:
 * - Guaranteed DMA-capable memory
 * - Option to share buffers with GPU via DMA-BUF fd
 * - Better memory management for high-bandwidth transfers
 * 
 * Note: This example does NOT use the GPU - it just demonstrates
 * the DMA-BUF allocation mechanism for pure DMA transfers.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>

#include "iwfg_user.h"

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

#define NUM_BUFFERS 3       /* Triple buffering for optimal pipeline */
#define WIDTH 1280
#define HEIGHT 1024
#define BYTES_PER_PIXEL 4   /* XRGB8888 format */
#define FRAME_SIZE (WIDTH * HEIGHT * BYTES_PER_PIXEL)

/* ============================================================================
 * DMA-BUF Buffer Descriptor
 * ============================================================================ */

typedef struct {
    int dmabuf_fd;              /* DMA-BUF file descriptor from driver */
    uint32_t buf_index;         /* Buffer index in driver's list */
    void *mapped_addr;          /* mmap'd address for CPU access (optional) */
} dmabuf_desc_t;

/* ============================================================================
 * Global State
 * ============================================================================ */

static int g_iwfg_fd = -1;
static dmabuf_desc_t g_buffers[NUM_BUFFERS];
static volatile bool g_running = true;

/* Circular queue indices (protected by mutex) */
static int g_write_idx = 0;    /* Next buffer for C2H to fill */
static int g_read_idx = 0;     /* Next buffer for H2C to read */

/* Thread handles */
static pthread_t g_c2h_thread;
static pthread_t g_h2c_thread;

/* Semaphore-based synchronization for FIFO triple buffering */
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t g_empty_slots;   /* Counts empty buffers available for C2H (producer) */
static sem_t g_filled_slots;  /* Counts filled buffers ready for H2C (consumer) */

/* Statistics */
static uint32_t g_c2h_frames = 0;
static uint32_t g_h2c_frames = 0;
static uint32_t g_c2h_errors = 0;
static uint32_t g_h2c_errors = 0;

/* Performance monitoring */
static bool g_enable_performance_monitoring = false;
static struct timespec g_c2h_times[100];
static struct timespec g_h2c_times[100];
static int g_c2h_perf_count = 0;
static int g_h2c_perf_count = 0;

/* Optional: mmap buffers for CPU access (e.g., to add markers) */
static bool g_enable_cpu_access = false;

/* ============================================================================
 * Cleanup and Signal Handler
 * ============================================================================ */

static void cleanup_and_exit(int sig __attribute__((unused))) {
    printf("\nCleaning up and exiting...\n");
    g_running = false;
    
    /* Wake up any waiting threads by posting to semaphores */
    sem_post(&g_empty_slots);
    sem_post(&g_filled_slots);
    
    /* Wait for threads to finish */
    pthread_join(g_c2h_thread, NULL);
    pthread_join(g_h2c_thread, NULL);
    
    /* Stop streaming */
    if (g_iwfg_fd >= 0) {
        ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_STOP);
    }
    
    /* Cleanup DMA-BUF resources */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_buffers[i].mapped_addr && g_buffers[i].mapped_addr != MAP_FAILED) {
            munmap(g_buffers[i].mapped_addr, FRAME_SIZE);
        }
        if (g_buffers[i].dmabuf_fd >= 0) {
            close(g_buffers[i].dmabuf_fd);
        }
    }
    
    /* Close device */
    if (g_iwfg_fd >= 0) {
        close(g_iwfg_fd);
    }
    
    /* Print final statistics */
    printf("\n=== Final Statistics ===\n");
    printf("C2H frames received: %u (errors: %u)\n", g_c2h_frames, g_c2h_errors);
    printf("H2C frames transmitted: %u (errors: %u)\n", g_h2c_frames, g_h2c_errors);
    printf("Frames in flight: %u\n", g_c2h_frames - g_h2c_frames);
    
    /* Cleanup synchronization primitives */
    pthread_mutex_destroy(&g_queue_mutex);
    sem_destroy(&g_empty_slots);
    sem_destroy(&g_filled_slots);
    
    exit(0);
}

/* ============================================================================
 * Performance Statistics
 * ============================================================================ */

static void print_queue_state(void) __attribute__((unused));
static void print_queue_state(void) {
    int empty_val, filled_val;
    sem_getvalue(&g_empty_slots, &empty_val);
    sem_getvalue(&g_filled_slots, &filled_val);
    printf("Queue: write_idx=%d, read_idx=%d, empty=%d, filled=%d\n", 
           g_write_idx, g_read_idx, empty_val, filled_val);
}

static void print_performance_stats(void) {
    if (!g_enable_performance_monitoring) return;
    
    printf("\n=== Performance Stats ===\n");
    
    /* C2H stats */
    if (g_c2h_perf_count > 1) {
        uint64_t total_c2h_time = 0;
        for (int i = 0; i < g_c2h_perf_count; i++) {
            total_c2h_time += g_c2h_times[i].tv_sec * 1000000000ULL + g_c2h_times[i].tv_nsec;
        }
        double avg_c2h_ms = (total_c2h_time / g_c2h_perf_count) / 1000000.0;
        printf("C2H avg transfer time: %.2f ms\n", avg_c2h_ms);
    }
    
    /* H2C stats */
    if (g_h2c_perf_count > 1) {
        uint64_t total_h2c_time = 0;
        for (int i = 0; i < g_h2c_perf_count; i++) {
            total_h2c_time += g_h2c_times[i].tv_sec * 1000000000ULL + g_h2c_times[i].tv_nsec;
        }
        double avg_h2c_ms = (total_h2c_time / g_h2c_perf_count) / 1000000.0;
        printf("H2C avg transfer time: %.2f ms\n", avg_h2c_ms);
    }
}

/* ============================================================================
 * Buffer Setup with Driver DMA Allocation
 * ============================================================================ */

static int setup_dmabuf_buffers(void) {
    printf("Setting up driver-allocated DMA buffers...\n");
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        struct iwfg_user_buf user_buf = {
            .buf = NULL,                        /* NULL for driver allocation */
            .size = FRAME_SIZE,
            .buf_type = IWFG_BUF_TYPE_DMA_ALLOC,/* Driver allocates and exports */
            .import_fd = -1,
            .is_chunked = false,
            .chunk_size = 0,
            .dmabuf_fd = -1,                    /* Will be filled by driver */
            .buf_index = 0                      /* Will be filled by driver */
        };
        
        if (ioctl(g_iwfg_fd, IWFG_IOCTL_ADD_BUF, &user_buf) < 0) {
            perror("IWFG_IOCTL_ADD_BUF (DMA_ALLOC) failed");
            return -1;
        }
        
        g_buffers[i].dmabuf_fd = user_buf.dmabuf_fd;
        g_buffers[i].buf_index = user_buf.buf_index;
        g_buffers[i].mapped_addr = NULL;
        
        printf("Buffer %d: driver allocated, DMA-BUF fd=%d, index=%u\n",
               i, g_buffers[i].dmabuf_fd, g_buffers[i].buf_index);
        
        /* Optionally mmap the DMA-BUF for CPU access */
        if (g_enable_cpu_access) {
            g_buffers[i].mapped_addr = mmap(NULL, FRAME_SIZE, 
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, 
                                            g_buffers[i].dmabuf_fd, 0);
            if (g_buffers[i].mapped_addr == MAP_FAILED) {
                fprintf(stderr, "Warning: Failed to mmap buffer %d: %s\n", 
                        i, strerror(errno));
                g_buffers[i].mapped_addr = NULL;
            } else {
                printf("  Buffer %d: mmap'd at %p for CPU access\n", 
                       i, g_buffers[i].mapped_addr);
            }
        }
    }
    
    printf("All %d driver-allocated DMA buffers set up\n", NUM_BUFFERS);
    return 0;
}

/* ============================================================================
 * C2H Thread - Receives Data from Device (PRODUCER)
 * ============================================================================ */

static void *c2h_thread_func(void *arg __attribute__((unused))) {
    printf("C2H thread started\n");
    
    struct iwfg_user_dma_req dma_req;
    struct timespec start_time, end_time;
    
    while (g_running) {
        /* Wait for an empty slot (semaphore blocks if all buffers are filled) */
        if (sem_wait(&g_empty_slots) != 0) {
            if (!g_running) break;
            continue;
        }
        
        /* Check if we should exit after being woken up */
        if (!g_running) {
            sem_post(&g_empty_slots);
            break;
        }
        
        /* Get the next buffer to fill (protected by mutex for index access) */
        pthread_mutex_lock(&g_queue_mutex);
        int buf_idx = g_write_idx;
        pthread_mutex_unlock(&g_queue_mutex);
        
        /* Perform C2H DMA transfer using buffer index */
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &start_time);
        }
        
        /* Use buf_index to reference driver-allocated buffer */
        dma_req.buf = NULL;  /* NULL for DMA_ALLOC - driver looks up by index */
        dma_req.size = FRAME_SIZE;
        dma_req.direction = IWFG_DIR_C2H_BUF;
        dma_req.offset = 0;
        dma_req.buf_index = g_buffers[buf_idx].buf_index;
        dma_req.flags = 0;
        
        int ret = ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req);
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            if (g_c2h_perf_count < 100) {
                g_c2h_times[g_c2h_perf_count].tv_sec = end_time.tv_sec - start_time.tv_sec;
                g_c2h_times[g_c2h_perf_count].tv_nsec = end_time.tv_nsec - start_time.tv_nsec;
                if (g_c2h_times[g_c2h_perf_count].tv_nsec < 0) {
                    g_c2h_times[g_c2h_perf_count].tv_sec--;
                    g_c2h_times[g_c2h_perf_count].tv_nsec += 1000000000;
                }
                g_c2h_perf_count++;
            }
        }
        
        if (ret < 0) {
            if (errno == EAGAIN) {
                /* No frame available yet, return slot and retry */
                sem_post(&g_empty_slots);
                usleep(1000);
                continue;
            }
            /* Other error - return slot and continue */
            g_c2h_errors++;
            fprintf(stderr, "C2H: DMA transfer failed on buffer %d: %s\n", 
                    buf_idx, strerror(errno));
            sem_post(&g_empty_slots);
            continue;
        }
        
        /* Transfer successful - advance write index */
        pthread_mutex_lock(&g_queue_mutex);
        g_write_idx = (g_write_idx + 1) % NUM_BUFFERS;
        g_c2h_frames++;
        pthread_mutex_unlock(&g_queue_mutex);
        
        /* Signal H2C that a filled buffer is available */
        sem_post(&g_filled_slots);
    }
    
    printf("C2H thread exiting\n");
    return NULL;
}

/* ============================================================================
 * H2C Thread - Transmits Data to Device (CONSUMER)
 * ============================================================================ */

static void *h2c_thread_func(void *arg __attribute__((unused))) {
    printf("H2C thread started\n");
    
    struct iwfg_user_dma_req dma_req;
    struct timespec start_time, end_time;
    
    const int marker_size = 32;  /* 32x32 pixel square */
    const uint32_t marker_color = 0x00FF0000;  /* Red in XRGB8888 */
    
    while (g_running) {
        /* Wait for a filled slot (semaphore blocks if no buffers are ready) */
        if (sem_wait(&g_filled_slots) != 0) {
            if (!g_running) break;
            continue;
        }
        
        /* Check if we should exit after being woken up */
        if (!g_running) {
            sem_post(&g_filled_slots);
            break;
        }
        
        /* Get the next buffer to read (protected by mutex for index access) */
        pthread_mutex_lock(&g_queue_mutex);
        int buf_idx = g_read_idx;
        pthread_mutex_unlock(&g_queue_mutex);
        
        /* Optionally overlay a red square marker (if CPU access is enabled) */
        if (g_enable_cpu_access && g_buffers[buf_idx].mapped_addr) {
            uint32_t *pixels = (uint32_t *)g_buffers[buf_idx].mapped_addr;
            for (int y = 0; y < marker_size; y++) {
                for (int x = 0; x < marker_size; x++) {
                    pixels[y * WIDTH + x] = marker_color;
                }
            }
        }
        
        /* Perform H2C DMA transfer using buffer index */
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &start_time);
        }
        
        /* Use buf_index to reference driver-allocated buffer */
        dma_req.buf = NULL;  /* NULL for DMA_ALLOC - driver looks up by index */
        dma_req.size = FRAME_SIZE;
        dma_req.direction = IWFG_DIR_H2C_BUF;
        dma_req.offset = 0;
        dma_req.buf_index = g_buffers[buf_idx].buf_index;
        dma_req.flags = IWFG_FLAG_LAST_PACKET;
        
        int ret = ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req);
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            if (g_h2c_perf_count < 100) {
                g_h2c_times[g_h2c_perf_count].tv_sec = end_time.tv_sec - start_time.tv_sec;
                g_h2c_times[g_h2c_perf_count].tv_nsec = end_time.tv_nsec - start_time.tv_nsec;
                if (g_h2c_times[g_h2c_perf_count].tv_nsec < 0) {
                    g_h2c_times[g_h2c_perf_count].tv_sec--;
                    g_h2c_times[g_h2c_perf_count].tv_nsec += 1000000000;
                }
                g_h2c_perf_count++;
            }
        }
        
        if (ret < 0) {
            g_h2c_errors++;
            fprintf(stderr, "H2C: DMA transfer failed on buffer %d: %s\n", 
                    buf_idx, strerror(errno));
            /* Still advance to prevent deadlock */
        } else {
            g_h2c_frames++;
        }
        
        /* Advance read index */
        pthread_mutex_lock(&g_queue_mutex);
        g_read_idx = (g_read_idx + 1) % NUM_BUFFERS;
        pthread_mutex_unlock(&g_queue_mutex);
        
        /* Signal C2H that an empty slot is available */
        sem_post(&g_empty_slots);
    }
    
    printf("H2C thread exiting\n");
    return NULL;
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -p, --performance    Enable performance monitoring\n");
    printf("  -m, --mmap           Enable CPU access via mmap (for markers)\n");
    printf("  -h, --help           Show this help message\n");
    printf("\n");
    printf("This test demonstrates:\n");
    printf("  - Driver-allocated DMA buffers (IWFG_BUF_TYPE_DMA_ALLOC)\n");
    printf("  - DMA-BUF export from kernel driver\n");
    printf("  - C2H capture -> H2C transmission loopback\n");
    printf("  - Triple buffering with semaphore synchronization\n");
    printf("\n");
    printf("Unlike iwfg_dmabuf_cube.c, this example does NOT use GPU.\n");
    printf("It demonstrates pure DMA transfers with driver-allocated buffers.\n");
    printf("\n");
    printf("The DMA-BUF fd is available for sharing with GPU if needed,\n");
    printf("but this example focuses on the DMA allocation mechanism.\n");
}

int main(int argc, char *argv[]) {
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--performance") == 0) {
            g_enable_performance_monitoring = true;
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mmap") == 0) {
            g_enable_cpu_access = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    printf("IWFG DMA-BUF C2H to H2C Loopback Test\n");
    printf("=====================================\n");
    printf("Resolution: %dx%d\n", WIDTH, HEIGHT);
    printf("Frame size: %d bytes (%.2f MB)\n", FRAME_SIZE, FRAME_SIZE / (1024.0 * 1024.0));
    printf("Number of buffers: %d (triple buffering)\n", NUM_BUFFERS);
    printf("Performance monitoring: %s\n", 
           g_enable_performance_monitoring ? "ENABLED" : "DISABLED");
    printf("CPU access (mmap): %s\n", 
           g_enable_cpu_access ? "ENABLED" : "DISABLED");
    printf("\n");
    
    /* Open IWFG device */
    g_iwfg_fd = open("/dev/iwfg", O_RDWR);
    if (g_iwfg_fd < 0) {
        perror("Failed to open IWFG device");
        return -1;
    }
    printf("IWFG device opened successfully\n");
    
    /* Initialize semaphores for FIFO synchronization */
    if (sem_init(&g_empty_slots, 0, NUM_BUFFERS) != 0) {
        perror("Failed to initialize empty_slots semaphore");
        close(g_iwfg_fd);
        return -1;
    }
    
    if (sem_init(&g_filled_slots, 0, 0) != 0) {
        perror("Failed to initialize filled_slots semaphore");
        sem_destroy(&g_empty_slots);
        close(g_iwfg_fd);
        return -1;
    }
    printf("Semaphores initialized (empty=%d, filled=0)\n", NUM_BUFFERS);
    
    /* Setup driver-allocated DMA buffers */
    printf("\n--- Setting up DMA-BUF buffers ---\n");
    if (setup_dmabuf_buffers() < 0) {
        fprintf(stderr, "Failed to setup DMA-BUF buffers\n");
        fprintf(stderr, "\nNote: Driver DMA allocation requires:\n");
        fprintf(stderr, "  - CMA (Contiguous Memory Allocator) configured\n");
        fprintf(stderr, "  - Kernel boot parameter: cma=64M (or larger)\n");
        fprintf(stderr, "  - Check with: cat /proc/meminfo | grep Cma\n");
        close(g_iwfg_fd);
        return -1;
    }
    
    /* Initialize queue indices */
    g_write_idx = 0;
    g_read_idx = 0;
    
    /* Start streaming */
    printf("\n--- Starting streaming ---\n");
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_START) < 0) {
        perror("IWFG_IOCTL_STREAM_START failed");
        close(g_iwfg_fd);
        return -1;
    }
    printf("Streaming started\n");
    
    /* Create threads */
    printf("\n--- Creating pipeline threads ---\n");
    if (pthread_create(&g_c2h_thread, NULL, c2h_thread_func, NULL) != 0) {
        perror("Failed to create C2H thread");
        return -1;
    }
    
    if (pthread_create(&g_h2c_thread, NULL, h2c_thread_func, NULL) != 0) {
        perror("Failed to create H2C thread");
        return -1;
    }
    
    printf("Pipeline: C2H (capture) -> H2C (transmit)\n");
    printf("Using driver-allocated DMA buffers (DMA-BUF export)\n");
    printf("\nPress Ctrl+C to stop.\n\n");
    
    /* Main thread: periodic status updates */
    while (g_running) {
        sleep(5);
        
        if (!g_running) break;
        
        printf("Status: C2H=%u, H2C=%u, in-flight=%u (errors: C2H=%u, H2C=%u)\n",
               g_c2h_frames, g_h2c_frames, g_c2h_frames - g_h2c_frames,
               g_c2h_errors, g_h2c_errors);
        
        if (g_enable_performance_monitoring) {
            print_performance_stats();
        }
    }
    
    /* Cleanup handled by signal handler */
    return 0;
}
