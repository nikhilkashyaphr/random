/*
 * IWFG C2H to H2C Loopback Test
 * 
 * This example demonstrates:
 * 1. Receiving frames via C2H DMA
 * 2. Passing received frames to H2C DMA for transmission
 * 3. Strict FIFO circular buffer management with 3 buffers
 * 4. Multi-threaded producer-consumer pattern with semaphores
 * 
 * Features:
 * - 3 circular buffers in strict FIFO order
 * - C2H thread (producer): fills buffers sequentially at write_idx
 * - H2C thread (consumer): reads buffers sequentially at read_idx
 * - Semaphore-based synchronization (empty_slots / filled_slots)
 * - C2H waits on empty_slots when queue is full
 * - H2C waits on filled_slots when queue is empty
 * - No buffer overwriting - strict synchronization
 * - Performance monitoring for both C2H and H2C transfers
 */

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

#define NUM_BUFFERS 3

// Stream parameters (queried from driver at runtime)
static uint32_t g_width = 0;
static uint32_t g_height = 0;
static uint32_t g_bpp = 0;
static uint32_t g_frame_size = 0;

// Buffer descriptor
typedef struct {
    void *data;
} buffer_desc_t;

// Global state
static int g_iwfg_fd_rx = -1;
static int g_iwfg_fd_tx = -1;
static buffer_desc_t g_buffers[NUM_BUFFERS];
static volatile bool g_running = true;

// Circular queue indices (protected by mutex)
static int g_write_idx = 0;    // Next buffer for C2H to fill
static int g_read_idx = 0;     // Next buffer for H2C to read

// Thread handles
static pthread_t g_c2h_thread;
static pthread_t g_h2c_thread;

// Semaphore-based synchronization for FIFO triple buffering
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t g_empty_slots;   // Counts empty buffers available for C2H (producer)
static sem_t g_filled_slots;  // Counts filled buffers ready for H2C (consumer)

// Statistics
static uint32_t g_c2h_frames = 0;
static uint32_t g_h2c_frames = 0;
static uint32_t g_c2h_errors = 0;
static uint32_t g_h2c_errors = 0;

// Performance monitoring
static bool g_enable_performance_monitoring = false;
static struct timespec g_c2h_times[100];
static struct timespec g_h2c_times[100];
static int g_c2h_perf_count = 0;
static int g_h2c_perf_count = 0;

void cleanup_and_exit(int sig __attribute__((unused))) {
    printf("\nCleaning up and exiting...\n");
    g_running = false;
    
    // Wake up any waiting threads by posting to semaphores
    sem_post(&g_empty_slots);
    sem_post(&g_filled_slots);
    
    // Wait for threads to finish
    pthread_join(g_c2h_thread, NULL);
    pthread_join(g_h2c_thread, NULL);
    
    // Stop streaming
    if (g_iwfg_fd_rx >= 0) {
        // ioctl(g_iwfg_fd_rx, IWFG_IOCTL_STREAM_STOP);
        close(g_iwfg_fd_rx);
    }
    if (g_iwfg_fd_tx >= 0) {
        // ioctl(g_iwfg_fd_tx, IWFG_IOCTL_STREAM_STOP);
        close(g_iwfg_fd_tx);
    }
    
    // Free buffers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_buffers[i].data) {
            free(g_buffers[i].data);
        }
    }
    
    // Print final statistics
    printf("\n=== Final Statistics ===\n");
    printf("C2H frames received: %u (errors: %u)\n", g_c2h_frames, g_c2h_errors);
    printf("H2C frames transmitted: %u (errors: %u)\n", g_h2c_frames, g_h2c_errors);
    printf("Frames in flight: %u\n", g_c2h_frames - g_h2c_frames);
    
    // Cleanup synchronization primitives
    pthread_mutex_destroy(&g_queue_mutex);
    sem_destroy(&g_empty_slots);
    sem_destroy(&g_filled_slots);
    
    exit(0);
}

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
    
    // C2H stats
    if (g_c2h_perf_count > 1) {
        uint64_t total_c2h_time = 0;
        for (int i = 0; i < g_c2h_perf_count; i++) {
            total_c2h_time += g_c2h_times[i].tv_sec * 1000000000ULL + g_c2h_times[i].tv_nsec;
        }
        double avg_c2h_ms = (total_c2h_time / g_c2h_perf_count) / 1000000.0;
        printf("C2H avg transfer time: %.2f ms\n", avg_c2h_ms);
    }
    
    // H2C stats
    if (g_h2c_perf_count > 1) {
        uint64_t total_h2c_time = 0;
        for (int i = 0; i < g_h2c_perf_count; i++) {
            total_h2c_time += g_h2c_times[i].tv_sec * 1000000000ULL + g_h2c_times[i].tv_nsec;
        }
        double avg_h2c_ms = (total_h2c_time / g_h2c_perf_count) / 1000000.0;
        printf("H2C avg transfer time: %.2f ms\n", avg_h2c_ms);
    }
}

// C2H thread - receives data from device (PRODUCER)
// Waits on empty_slots semaphore, fills buffer at write_idx, posts to filled_slots
void* c2h_thread_func(void *arg __attribute__((unused))) {
    printf("C2H thread started\n");
    
    struct iwfg_user_dma_req dma_req;
    struct timespec start_time, end_time;
    
    while (g_running) {
        // Wait for an empty slot (semaphore blocks if all buffers are filled)
        if (sem_wait(&g_empty_slots) != 0) {
            if (!g_running) break;
            continue;
        }
        
        // Check if we should exit after being woken up
        if (!g_running) {
            sem_post(&g_empty_slots);  // Put back the semaphore
            break;
        }
        
        // Get the next buffer to fill (protected by mutex for index access)
        pthread_mutex_lock(&g_queue_mutex);
        int buf_idx = g_write_idx;
        pthread_mutex_unlock(&g_queue_mutex);
        
        // Perform C2H DMA transfer (outside mutex to allow H2C to work)
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &start_time);
        }
        
        dma_req.buf = g_buffers[buf_idx].data;
        dma_req.size = g_frame_size;
        dma_req.direction = IWFG_DIR_C2H_BUF;
        dma_req.offset = 0;
        
        int ret = ioctl(g_iwfg_fd_rx, IWFG_IOCTL_DMA_DATA, &dma_req);
		if (ret < 0 && errno == EAGAIN) {
            // This can happen driver encounters fifo overflow in fpga
            g_c2h_errors++;
            printf("C2H: Buffer %d is busy, retrying...\n", buf_idx);
            sem_post(&g_empty_slots);  // Put back the empty slot
            usleep(1000);
            continue;
        }
        
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
            // Other error - return slot and continue
            g_c2h_errors++;
            fprintf(stderr, "C2H: DMA transfer failed on buffer %d: %s\n", buf_idx, strerror(errno));
            sem_post(&g_empty_slots);
            continue;
        }
        
        // Transfer successful - advance write index
        pthread_mutex_lock(&g_queue_mutex);
        g_write_idx = (g_write_idx + 1) % NUM_BUFFERS;
        g_c2h_frames++;
        pthread_mutex_unlock(&g_queue_mutex);
        
        // Signal H2C that a filled buffer is available
        sem_post(&g_filled_slots);
    }
    
    printf("C2H thread exiting\n");
    return NULL;
}

// H2C thread - transmits data to device (CONSUMER)
// Waits on filled_slots semaphore, reads buffer at read_idx, posts to empty_slots
void* h2c_thread_func(void *arg __attribute__((unused))) {
    printf("H2C thread started\n");
    
    struct iwfg_user_dma_req dma_req;
    struct timespec start_time, end_time;

    const int marker_size = 32;  // 32x32 pixel square
    const uint32_t marker_color = 0x00FF0000;
    
    while (g_running) {
        // Wait for a filled slot (semaphore blocks if no buffers are ready)
        if (sem_wait(&g_filled_slots) != 0) {
            if (!g_running) break;
            continue;
        }
        
        // Check if we should exit after being woken up
        if (!g_running) {
            sem_post(&g_filled_slots);  // Put back the semaphore
            break;
        }
        
        // Get the next buffer to read (protected by mutex for index access)
        pthread_mutex_lock(&g_queue_mutex);
        int buf_idx = g_read_idx;
        pthread_mutex_unlock(&g_queue_mutex);

        // Overlay a red square in the top-left corner to identify H2C frames
        // 24-bit pixels: 3 bytes each (R, G, B)
        uint8_t *pixels = (uint8_t *)g_buffers[buf_idx].data;
        for (int y = 0; y < marker_size; y++) {
            for (int x = 0; x < marker_size; x++) {
                uint8_t *px = pixels + (y * g_width + x) * g_bpp;
                px[0] = (marker_color >> 16) & 0xFF;  // R
                px[1] = (marker_color >> 8)  & 0xFF;  // G
                px[2] =  marker_color        & 0xFF;  // B
            }
        }
        
        // Perform H2C DMA transfer (outside mutex to allow C2H to work)
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &start_time);
        }
        
        dma_req.buf = g_buffers[buf_idx].data;
        dma_req.size = g_frame_size;
        dma_req.direction = IWFG_DIR_H2C_BUF;
        dma_req.offset = 0;
        
        int ret = ioctl(g_iwfg_fd_tx, IWFG_IOCTL_DMA_DATA, &dma_req);
		if (ret < 0 && errno == EBUSY) {
            // This can happen driver encounters fifo overflow in fpga
            g_h2c_errors++;
            printf("H2C: Buffer %d is busy, retrying...\n", buf_idx);
            sem_post(&g_filled_slots);  // Put back the filled slot
            usleep(1000);
            continue;
        }
        
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
            fprintf(stderr, "H2C: DMA transfer failed on buffer %d: %s\n", buf_idx, strerror(errno));
            // Still advance to prevent deadlock
        } else {
            g_h2c_frames++;
        }
        
        // Advance read index
        pthread_mutex_lock(&g_queue_mutex);
        g_read_idx = (g_read_idx + 1) % NUM_BUFFERS;
        pthread_mutex_unlock(&g_queue_mutex);
        
        // Signal C2H that an empty slot is available
        sem_post(&g_empty_slots);
    }
    
    printf("H2C thread exiting\n");
    return NULL;
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -p, --performance    Enable performance monitoring\n");
    printf("  -h, --help           Show this help message\n");
    printf("\n");
    printf("This test receives frames via C2H and loops them back via H2C.\n");
    printf("Uses 3 buffers in a circular producer-consumer pattern.\n");
}

int main(int argc, char *argv[]) {
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--performance") == 0) {
            g_enable_performance_monitoring = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    printf("IWFG C2H to H2C Loopback Test\n");
    printf("=============================\n");
    
    // Open IWFG device
    g_iwfg_fd_rx = open("/dev/iwfg0", O_RDWR);
    if (g_iwfg_fd_rx < 0) {
        perror("Failed to open IWFG RX device");
        return -1;
    }
	g_iwfg_fd_tx = open("/dev/iwfg2", O_RDWR);
	if (g_iwfg_fd_tx < 0) {
		perror("Failed to open IWFG TX device");
		close(g_iwfg_fd_rx);
		return -1;
	}
    printf("IWFG RX device opened successfully\n");
    printf("IWFG TX device opened successfully\n");
    
    // Query stream parameters from the driver
    struct iwfg_stream_params stream_params;
    if (ioctl(g_iwfg_fd_rx, IWFG_IOCTL_QUERY_STREAM_PARAMS, &stream_params) < 0) {
        perror("IWFG_IOCTL_QUERY_STREAM_PARAMS failed");
        close(g_iwfg_fd_rx);
		close(g_iwfg_fd_tx);
        return -1;
    }
    g_width = stream_params.width;
    g_height = stream_params.height;
    g_bpp = stream_params.bpp;
    g_frame_size = g_width * g_height * g_bpp;
    
    printf("Stream parameters: %ux%u, %u bytes/pixel\n", g_width, g_height, g_bpp);
    printf("Frame size: %u bytes\n", g_frame_size);
    printf("Number of buffers: %d\n", NUM_BUFFERS);
    printf("Performance monitoring: %s\n", g_enable_performance_monitoring ? "ENABLED" : "DISABLED");
    printf("\n");
    
    // Initialize semaphores for FIFO synchronization
    // empty_slots starts at NUM_BUFFERS (all buffers available for C2H)
    // filled_slots starts at 0 (no buffers ready for H2C)
    if (sem_init(&g_empty_slots, 0, NUM_BUFFERS) != 0) {
        perror("Failed to initialize empty_slots semaphore");
        close(g_iwfg_fd_rx);
		close(g_iwfg_fd_tx);
        return -1;
    }
    
    if (sem_init(&g_filled_slots, 0, 0) != 0) {
        perror("Failed to initialize filled_slots semaphore");
        sem_destroy(&g_empty_slots);
        close(g_iwfg_fd_rx);
		close(g_iwfg_fd_tx);
        return -1;
    }
    printf("Semaphores initialized (empty=%d, filled=0)\n", NUM_BUFFERS);
    
    // Allocate buffers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (posix_memalign(&g_buffers[i].data, sysconf(_SC_PAGESIZE), g_frame_size)) {
            perror("Buffer allocation failed");
            return -1;
        }
        memset(g_buffers[i].data, 0, g_frame_size);

		struct iwfg_user_buf user_buf = {
			.buf = g_buffers[i].data,
			.size = g_frame_size,
			.is_chunked = false,
			.chunk_size = 0
		};

		// if (ioctl(g_iwfg_fd, IWFG_IOCTL_ADD_BUF, &user_buf) < 0) {
		// 	perror("IWFG_IOCTL_ADD_BUF failed");
		// 	return -1;
		// }

        printf("Allocated buffer %d: %p (%u bytes)\n", i, g_buffers[i].data, g_frame_size);
    }
    
    // Initialize queue indices
    g_write_idx = 0;
    g_read_idx = 0;
    
    // Start streaming
    // if (ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_START) < 0) {
    //     perror("IWFG_IOCTL_STREAM_START failed");
    //     return -1;
    // }
    printf("Streaming started\n");
    
    // Create threads
    if (pthread_create(&g_c2h_thread, NULL, c2h_thread_func, NULL) != 0) {
        perror("Failed to create C2H thread");
        return -1;
    }
    
    if (pthread_create(&g_h2c_thread, NULL, h2c_thread_func, NULL) != 0) {
        perror("Failed to create H2C thread");
        return -1;
    }
    
    printf("\nC2H->H2C loopback running. Press Ctrl+C to stop.\n\n");
    
    // Main thread: periodic status updates
    while (g_running) {
        sleep(5);
        
        if (!g_running) break;
        
        // printf("\n=== Status Update ===\n");
        // printf("C2H frames: %u, H2C frames: %u\n", g_c2h_frames, g_h2c_frames);
        // printf("C2H errors: %u, H2C errors: %u\n", g_c2h_errors, g_h2c_errors);
        
        // pthread_mutex_lock(&g_queue_mutex);
        // print_queue_state();
        // pthread_mutex_unlock(&g_queue_mutex);
        
        if (g_enable_performance_monitoring) {
            print_performance_stats();
        }
    }
    
    // Cleanup handled by signal handler
    return 0;
}