/*
 * IWFG C2H to H2C Transfer Example (Multi-threaded)
 * 
 * This example demonstrates:
 * 1. Capturing frames to C2H buffers using IWFG_IOCTL_DMA_DATA in a separate thread
 * 2. Routing captured frames to H2C buffers for H2C transfers using IWFG_IOCTL_DMA_DATA in another thread
 * 
 * Features:
 * - Dual buffer setup: C2H (receive) and H2C (transmit) buffers
 * - Multi-threaded pipeline: C2H capture thread and H2C transmission thread
 * - Frame capture via C2H DMA using new IOCTL in dedicated thread
 * - Frame transmission via H2C DMA using new IOCTL in dedicated thread
 * - Inter-thread communication via lock-free frame queue
 * - Performance monitoring for both directions
 * - Memory-to-memory data movement through DMA engine
 * - Non-blocking pipeline operation
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

#define NUM_C2H_BUFFERS 2  // C2H (receive) buffers
#define NUM_H2C_BUFFERS 2  // H2C (transmit) buffers
#define WIDTH 1920
#define HEIGHT 1080
#define BYTES_PER_PIXEL 4  // XRGB8888 format
#define FRAME_SIZE (WIDTH * HEIGHT * BYTES_PER_PIXEL)

// Frame queue for inter-thread communication
#define FRAME_QUEUE_SIZE 8

typedef struct {
    void *c2h_buffer;       // Source C2H buffer (managed by C2H thread)
    uint32_t c2h_buf_index; // C2H buffer index
    uint32_t frame_number;  // Frame sequence number
    struct timespec capture_time; // When frame was captured
} frame_data_t;

// Chunk processing data for copy->H2C pipeline
typedef struct {
    void *h2c_buffer;       // Destination H2C buffer
    uint32_t h2c_buf_index; // H2C buffer index  
    uint32_t frame_number;  // Frame sequence number
    struct timespec copy_time; // When copying completed
} processed_frame_data_t;

typedef struct {
    frame_data_t frames[FRAME_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    sem_t empty_slots;  // Semaphore for empty slots
    sem_t filled_slots; // Semaphore for filled slots
} frame_queue_t;

typedef struct {
    processed_frame_data_t frames[FRAME_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    sem_t empty_slots;  // Semaphore for empty slots
    sem_t filled_slots; // Semaphore for filled slots
} processed_frame_queue_t;

// Global state for cleanup
static int g_iwfg_fd = -1;
static void *g_c2h_buffers[NUM_C2H_BUFFERS];  // C2H (RX) buffers
static void *g_h2c_buffers[NUM_H2C_BUFFERS];  // H2C (TX) buffers
static volatile bool g_running = true;

// Threading synchronization for 3-stage pipeline
static frame_queue_t g_frame_queue;           // C2H -> Copy thread
static processed_frame_queue_t g_processed_queue; // Copy -> H2C thread
static pthread_t g_c2h_thread;
static pthread_t g_copy_thread;
static pthread_t g_h2c_thread;
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Performance monitoring
static struct timespec g_frame_times[100];
static struct timespec g_c2h_times[100];
static struct timespec g_h2c_times[100];
static int g_frame_count = 0;
static bool g_enable_performance_monitoring = false;
static uint32_t g_total_frames_captured = 0;
static uint32_t g_total_frames_transmitted = 0;
static uint32_t g_frames_dropped = 0;

// Chunked transfer state
static bool g_enable_chunked_mode = false;
static uint32_t g_frame_divisions = 2;
static uint32_t g_chunk_size = 0;
static uint32_t g_chunks_per_frame = 0;
static uint32_t g_total_chunks_captured = 0;
static uint32_t g_total_chunks_transmitted = 0;

// Frame queue functions
int init_frame_queue(frame_queue_t *queue) {
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        perror("Failed to initialize mutex");
        return -1;
    }
    
    if (sem_init(&queue->empty_slots, 0, FRAME_QUEUE_SIZE) != 0) {
        perror("Failed to initialize empty_slots semaphore");
        pthread_mutex_destroy(&queue->mutex);
        return -1;
    }
    
    if (sem_init(&queue->filled_slots, 0, 0) != 0) {
        perror("Failed to initialize filled_slots semaphore");
        sem_destroy(&queue->empty_slots);
        pthread_mutex_destroy(&queue->mutex);
        return -1;
    }
    
    return 0;
}

int init_processed_frame_queue(processed_frame_queue_t *queue) {
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        perror("Failed to initialize processed queue mutex");
        return -1;
    }
    
    if (sem_init(&queue->empty_slots, 0, FRAME_QUEUE_SIZE) != 0) {
        perror("Failed to initialize processed queue empty_slots semaphore");
        pthread_mutex_destroy(&queue->mutex);
        return -1;
    }
    
    if (sem_init(&queue->filled_slots, 0, 0) != 0) {
        perror("Failed to initialize processed queue filled_slots semaphore");
        sem_destroy(&queue->empty_slots);
        pthread_mutex_destroy(&queue->mutex);
        return -1;
    }
    
    return 0;
}

void destroy_processed_frame_queue(processed_frame_queue_t *queue) {
    sem_destroy(&queue->filled_slots);
    sem_destroy(&queue->empty_slots);
    pthread_mutex_destroy(&queue->mutex);
}

int enqueue_processed_frame(processed_frame_queue_t *queue, const processed_frame_data_t *frame) {
    // Wait for an empty slot
    if (sem_wait(&queue->empty_slots) != 0) {
        return -1;
    }
    
    pthread_mutex_lock(&queue->mutex);
    
    // Add frame to queue
    queue->frames[queue->head] = *frame;
    queue->head = (queue->head + 1) % FRAME_QUEUE_SIZE;
    queue->count++;
    
    pthread_mutex_unlock(&queue->mutex);
    
    // Signal that a slot is filled
    sem_post(&queue->filled_slots);
    
    return 0;
}

int dequeue_processed_frame(processed_frame_queue_t *queue, processed_frame_data_t *frame) {
    // Wait for a filled slot
    if (sem_wait(&queue->filled_slots) != 0) {
        return -1;
    }
    
    if (!g_running) {
        // If we're shutting down, put the semaphore back and return
        sem_post(&queue->filled_slots);
        return -1;
    }
    
    pthread_mutex_lock(&queue->mutex);
    
    // Get frame from queue
    *frame = queue->frames[queue->tail];
    queue->tail = (queue->tail + 1) % FRAME_QUEUE_SIZE;
    queue->count--;
    
    pthread_mutex_unlock(&queue->mutex);
    
    // Signal that a slot is empty
    sem_post(&queue->empty_slots);
    
    return 0;
}

void destroy_frame_queue(frame_queue_t *queue) {
    sem_destroy(&queue->filled_slots);
    sem_destroy(&queue->empty_slots);
    pthread_mutex_destroy(&queue->mutex);
}

int enqueue_frame(frame_queue_t *queue, const frame_data_t *frame) {
    // Wait for an empty slot
    if (sem_wait(&queue->empty_slots) != 0) {
        return -1;
    }
    
    pthread_mutex_lock(&queue->mutex);
    
    // Add frame to queue
    queue->frames[queue->head] = *frame;
    queue->head = (queue->head + 1) % FRAME_QUEUE_SIZE;
    queue->count++;
    
    pthread_mutex_unlock(&queue->mutex);
    
    // Signal that a slot is filled
    sem_post(&queue->filled_slots);
    
    return 0;
}

int dequeue_frame(frame_queue_t *queue, frame_data_t *frame) {
    // Wait for a filled slot
    if (sem_wait(&queue->filled_slots) != 0) {
        return -1;
    }
    
    if (!g_running) {
        // If we're shutting down, put the semaphore back and return
        sem_post(&queue->filled_slots);
        return -1;
    }
    
    pthread_mutex_lock(&queue->mutex);
    
    // Get frame from queue
    *frame = queue->frames[queue->tail];
    queue->tail = (queue->tail + 1) % FRAME_QUEUE_SIZE;
    queue->count--;
    
    pthread_mutex_unlock(&queue->mutex);
    
    // Signal that a slot is empty
    sem_post(&queue->empty_slots);
    
    return 0;
}

void cleanup_and_exit(int sig __attribute__((unused))) {
    printf("\nCleaning up and exiting...\n");
    g_running = false;
    
    // Wake up threads by posting to semaphores
    sem_post(&g_frame_queue.filled_slots);
    sem_post(&g_frame_queue.empty_slots);
    sem_post(&g_processed_queue.filled_slots);
    sem_post(&g_processed_queue.empty_slots);

    // Stop streaming
    if (g_iwfg_fd >= 0) {
        ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_STOP);
        close(g_iwfg_fd);
    }
    
    // Wait for threads to finish
    if (pthread_join(g_c2h_thread, NULL) != 0) {
        perror("Failed to join C2H thread");
    }
    if (pthread_join(g_copy_thread, NULL) != 0) {
        perror("Failed to join Copy thread");
    }
    if (pthread_join(g_h2c_thread, NULL) != 0) {
        perror("Failed to join H2C thread");
    }
    
    // Free C2H buffers
    for (int i = 0; i < NUM_C2H_BUFFERS; i++) {
        if (g_c2h_buffers[i]) {
            free(g_c2h_buffers[i]);
        }
    }
    
    // Free H2C buffers
    for (int i = 0; i < NUM_H2C_BUFFERS; i++) {
        if (g_h2c_buffers[i]) {
            free(g_h2c_buffers[i]);
        }
    }
    
    // Cleanup frame queues
    destroy_frame_queue(&g_frame_queue);
    destroy_processed_frame_queue(&g_processed_queue);
    
    printf("Total frames captured: %u\n", g_total_frames_captured);
    printf("Total frames transmitted: %u\n", g_total_frames_transmitted);
    printf("Total frames dropped: %u\n", g_frames_dropped);
    
    exit(0);
}

static void print_performance_stats(void) {
    if (!g_enable_performance_monitoring || g_frame_count < 2) return;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    uint64_t total_frame_time = 0;
    uint64_t total_c2h_time = 0;
    uint64_t total_h2c_time = 0;
    uint64_t min_frame_time = UINT64_MAX;
    uint64_t max_frame_time = 0;
    uint64_t min_c2h_time = UINT64_MAX;
    uint64_t max_c2h_time = 0;
    uint64_t min_h2c_time = UINT64_MAX;
    uint64_t max_h2c_time = 0;
    
    for (int i = 1; i < g_frame_count; i++) {
        uint64_t frame_time = (g_frame_times[i].tv_sec - g_frame_times[i-1].tv_sec) * 1000000000ULL +
                             (g_frame_times[i].tv_nsec - g_frame_times[i-1].tv_nsec);
        uint64_t c2h_time = g_c2h_times[i].tv_sec * 1000000000ULL + g_c2h_times[i].tv_nsec;
        uint64_t h2c_time = g_h2c_times[i].tv_sec * 1000000000ULL + g_h2c_times[i].tv_nsec;
        
        total_frame_time += frame_time;
        total_c2h_time += c2h_time;
        total_h2c_time += h2c_time;
        
        if (frame_time < min_frame_time) min_frame_time = frame_time;
        if (frame_time > max_frame_time) max_frame_time = frame_time;
        if (c2h_time < min_c2h_time) min_c2h_time = c2h_time;
        if (c2h_time > max_c2h_time) max_c2h_time = c2h_time;
        if (h2c_time < min_h2c_time) min_h2c_time = h2c_time;
        if (h2c_time > max_h2c_time) max_h2c_time = h2c_time;
    }
    
    uint64_t avg_frame_time = total_frame_time / (g_frame_count - 1);
    uint64_t avg_c2h_time = total_c2h_time / (g_frame_count - 1);
    uint64_t avg_h2c_time = total_h2c_time / (g_frame_count - 1);
    double fps = 1000000000.0 / avg_frame_time;
    
    printf("Performance Stats (last %d frames):\n", g_frame_count - 1);
    printf("  Average frame time: %.2f ms (%.1f FPS)\n", avg_frame_time / 1000000.0, fps);
    printf("  Average C2H time: %.2f ms (%.1f%% of frame)\n", 
           avg_c2h_time / 1000000.0, (avg_c2h_time * 100.0) / avg_frame_time);
    printf("  Average H2C time: %.2f ms (%.1f%% of frame)\n", 
           avg_h2c_time / 1000000.0, (avg_h2c_time * 100.0) / avg_frame_time);
    printf("  Min/Max frame time: %.2f/%.2f ms\n", 
           min_frame_time / 1000000.0, max_frame_time / 1000000.0);
    printf("  Min/Max C2H time: %.2f/%.2f ms\n", 
           min_c2h_time / 1000000.0, max_c2h_time / 1000000.0);
    printf("  Min/Max H2C time: %.2f/%.2f ms\n", 
           min_h2c_time / 1000000.0, max_h2c_time / 1000000.0);
    printf("  C2H->H2C pipeline efficiency: %.1f%%\n", 
           ((avg_frame_time - avg_c2h_time - avg_h2c_time) * 100.0) / avg_frame_time);
    printf("  Queue status - Captured: %u, Transmitted: %u, Dropped: %u\n",
           g_total_frames_captured, g_total_frames_transmitted, g_frames_dropped);
    
    pthread_mutex_unlock(&g_stats_mutex);
}

// C2H capture thread - handles frame capture from hardware
void *c2h_thread_func(void *arg) {
    (void)arg; // Unused parameter
    uint32_t c2h_buf_index = 0;
    uint32_t frame_number = 0;
    
    printf("C2H capture thread started\n");
    
    while (g_running) {
        struct timespec c2h_start, c2h_end;
        
        // Performance monitoring
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &c2h_start);
        }
        
        // Step 1: Perform C2H data transfer (frame capture)
        struct iwfg_user_dma_req dma_req = {
            .buf = g_c2h_buffers[c2h_buf_index],
            .size = FRAME_SIZE,
            .direction = IWFG_DIR_C2H_BUF  // C2H direction
        };
        
        if (ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req) < 0) {
            if (errno == EINTR || !g_running) {
                printf("C2H thread interrupted\n");
                break;
            }
            perror("IWFG_IOCTL_DMA_DATA failed for C2H transfer");
            break;
        }
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &c2h_end);
        }
        
        // Debug: Show frame capture info for first few frames
        if (frame_number < 5) {
            uint32_t *pixels = (uint32_t*)g_c2h_buffers[c2h_buf_index];
            printf("Frame %d captured to C2H buffer %d: First pixels: 0x%08x 0x%08x 0x%08x 0x%08x\n", 
                   frame_number, c2h_buf_index, pixels[0], pixels[1], pixels[2], pixels[3]);
        }
        
        // Prepare frame data for the queue
        frame_data_t frame_data = {
            .c2h_buffer = g_c2h_buffers[c2h_buf_index],
            .c2h_buf_index = c2h_buf_index,
            .frame_number = frame_number
        };
        
        if (g_enable_performance_monitoring) {
            frame_data.capture_time = c2h_end;
            
            // Record C2H timing
            pthread_mutex_lock(&g_stats_mutex);
            if (g_frame_count < 100) {
                g_c2h_times[g_frame_count].tv_sec = c2h_end.tv_sec - c2h_start.tv_sec;
                g_c2h_times[g_frame_count].tv_nsec = c2h_end.tv_nsec - c2h_start.tv_nsec;
                if (g_c2h_times[g_frame_count].tv_nsec < 0) {
                    g_c2h_times[g_frame_count].tv_sec--;
                    g_c2h_times[g_frame_count].tv_nsec += 1000000000;
                }
                g_frame_times[g_frame_count] = c2h_end;
            }
            pthread_mutex_unlock(&g_stats_mutex);
        }
        
        // Try to enqueue the frame for H2C processing
        if (enqueue_frame(&g_frame_queue, &frame_data) < 0) {
            if (!g_running) break;
            printf("Warning: Frame queue full, dropping frame %d\n", frame_number);
            __sync_fetch_and_add(&g_frames_dropped, 1);
            
            // Continue to next buffer - DMA call signals driver that buffer is available for reuse
        } else {
            __sync_fetch_and_add(&g_total_frames_captured, 1);
        }
        
        frame_number++;
        
        // Cycle to next C2H buffer - DMA call signals driver that current buffer can be reused
        c2h_buf_index = (c2h_buf_index + 1) % NUM_C2H_BUFFERS;
        
        // Show periodic stats
        if (frame_number % 300 == 0) {
            printf("C2H: Captured %d frames\n", frame_number);
        }
    }
    
    printf("C2H capture thread exiting\n");
    return NULL;
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -p, --performance    Enable performance monitoring and statistics\n");
    printf("  -h, --help          Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                  Run with minimal overhead (no performance stats)\n", prog_name);
    printf("  %s -p               Run with performance monitoring enabled\n", prog_name);
}

int setup_c2h_buffers(void) {
    // Allocate page-aligned C2H buffers for receiving data
    for (int i = 0; i < NUM_C2H_BUFFERS; i++) {
        if (posix_memalign(&g_c2h_buffers[i], sysconf(_SC_PAGESIZE), FRAME_SIZE)) {
            perror("C2H buffer allocation failed");
            return -1;
        }
        
        // Initialize with a pattern to help with debugging
        uint32_t *pixels = (uint32_t*)g_c2h_buffers[i];
        for (int j = 0; j < FRAME_SIZE / 4; j++) {
            pixels[j] = 0xDEADBEEF + i;  // Buffer-specific pattern
        }
        
		// Setup C2H buffers with IWFG driver
		struct iwfg_user_buf user_buf = {
			.buf = g_c2h_buffers[i],
			.size = FRAME_SIZE,
			.is_chunked = false,
			.chunk_size = 0
		};
		
		if (ioctl(g_iwfg_fd, IWFG_IOCTL_ADD_BUF, &user_buf) < 0) {
			perror("IWFG_IOCTL_ADD_BUF failed for C2H buffers");
			return -1;
		}

        printf("Allocated C2H buffer %d: %p (%d bytes)\n", i, g_c2h_buffers[i], FRAME_SIZE);
    }
    
    printf("C2H buffers added to IWFG driver\n");
    return 0;
}

int setup_h2c_buffers(void) {
    // Allocate page-aligned H2C buffers for transmitting data
    for (int i = 0; i < NUM_H2C_BUFFERS; i++) {
        if (posix_memalign(&g_h2c_buffers[i], sysconf(_SC_PAGESIZE), FRAME_SIZE)) {
            perror("H2C buffer allocation failed");
            return -1;
        }
        
        // Initialize with different pattern
        uint32_t *pixels = (uint32_t*)g_h2c_buffers[i];
        for (int j = 0; j < FRAME_SIZE / 4; j++) {
            pixels[j] = 0xCAFEBABE + i;  // Buffer-specific pattern
        }

		// Setup H2C buffers with IWFG driver
		struct iwfg_user_buf user_buf = {
			.buf = g_h2c_buffers[i],
			.size = FRAME_SIZE,
			.is_chunked = false,
			.chunk_size = 0
		};
		
		if (ioctl(g_iwfg_fd, IWFG_IOCTL_ADD_BUF, &user_buf) < 0) {
			perror("IWFG_IOCTL_ADD_BUF failed for H2C buffers");
			return -1;
		}
        
        printf("Allocated H2C buffer %d: %p (%d bytes)\n", i, g_h2c_buffers[i], FRAME_SIZE);
    }
    
    printf("H2C buffers added to IWFG driver\n");
    return 0;
}

int copy_frame_data(void *src_buffer, void *dst_buffer, size_t size) {
    // Copy frame data from C2H buffer to H2C buffer
    // In a real application, you might want to process or modify the data here
    memcpy(dst_buffer, src_buffer, size);
    
    // Optional: Add some processing or modification to the frame
    // For demonstration, let's add a simple watermark pattern
    uint32_t *pixels = (uint32_t*)dst_buffer;
    int watermark_size = 100; // 100x100 pixel watermark
    int watermark_x = WIDTH - watermark_size - 50;  // Top-right corner
    int watermark_y = 50;
    
    for (int y = 0; y < watermark_size; y++) {
        for (int x = 0; x < watermark_size; x++) {
            int pixel_idx = (watermark_y + y) * WIDTH + (watermark_x + x);
            if (pixel_idx < (FRAME_SIZE / 4)) {
                // Add a red watermark
                pixels[pixel_idx] = 0xFFFF0000; // ARGB red
            }
        }
    }
    
    return 0;
}

// Chunked frame copying - copy data chunk by chunk from C2H to H2C buffer
int copy_frame_data_chunked(void *src_buffer, void *dst_buffer, uint32_t frame_number) {
    // For now, just do a regular copy since chunked variables may not be properly initialized
    // In the future, this can be enhanced to do chunk-by-chunk copying
    (void)frame_number; // Suppress unused parameter warning
    return copy_frame_data(src_buffer, dst_buffer, FRAME_SIZE);
}

int perform_h2c_transfer(void *h2c_buffer, size_t size) {
    struct timespec start_time, end_time;
    
    if (g_enable_performance_monitoring) {
        clock_gettime(CLOCK_MONOTONIC, &start_time);
    }
    
    // Perform H2C transfer using the new IOCTL
    struct iwfg_user_dma_req dma_req = {
        .buf = h2c_buffer,
        .size = size,
        .direction = IWFG_DIR_H2C_BUF, // H2C direction
	.flags = IWFG_FLAG_LAST_PACKET
    };
    
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req) < 0) {
        perror("IWFG_IOCTL_DMA_DATA failed for H2C transfer");
        return -1;
    }
    
    if (g_enable_performance_monitoring) {
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        
        // Record H2C transfer time (duration)
        pthread_mutex_lock(&g_stats_mutex);
        if (g_frame_count < 100) {
            g_h2c_times[g_frame_count].tv_sec = end_time.tv_sec - start_time.tv_sec;
            g_h2c_times[g_frame_count].tv_nsec = end_time.tv_nsec - start_time.tv_nsec;
            if (g_h2c_times[g_frame_count].tv_nsec < 0) {
                g_h2c_times[g_frame_count].tv_sec--;
                g_h2c_times[g_frame_count].tv_nsec += 1000000000;
            }
            g_frame_count++;
        }
        pthread_mutex_unlock(&g_stats_mutex);
    }
    
    return 0;
}

// Copy thread - handles frame copying from C2H to H2C buffers
void *copy_thread_func(void *arg) {
    (void)arg; // Unused parameter
    uint32_t frames_processed = 0;
    uint32_t h2c_buf_index = 0;  // Copy thread manages H2C buffer allocation
    
    printf("Copy thread started\n");
    
    while (g_running) {
        frame_data_t frame_data;
        
        // Wait for a frame from the C2H thread
        if (dequeue_frame(&g_frame_queue, &frame_data) < 0) {
            if (!g_running) break;
            continue;
        }
        
        struct timespec copy_start, copy_end;
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &copy_start);
        }
        
        // Copy and process frame data from C2H to H2C buffer
        copy_frame_data(frame_data.c2h_buffer, g_h2c_buffers[h2c_buf_index], FRAME_SIZE);
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &copy_end);
        }
        
        // Debug: Show copy info for first few frames
        if (frame_data.frame_number < 5) {
            printf("Frame %d copied from C2H buffer %d to H2C buffer %d\n", 
                   frame_data.frame_number, frame_data.c2h_buf_index, h2c_buf_index);
        }
        
        // Prepare processed frame data for H2C thread
        processed_frame_data_t processed_data = {
            .h2c_buffer = g_h2c_buffers[h2c_buf_index],
            .h2c_buf_index = h2c_buf_index,
            .frame_number = frame_data.frame_number
        };
        
        if (g_enable_performance_monitoring) {
            processed_data.copy_time = copy_end;
        }
        
        // Send processed frame to H2C thread
        if (enqueue_processed_frame(&g_processed_queue, &processed_data) < 0) {
            if (!g_running) break;
            printf("Warning: Processed frame queue full, dropping frame %d\n", frame_data.frame_number);
            __sync_fetch_and_add(&g_frames_dropped, 1);
        }
        
        frames_processed++;
        
        // Cycle to next H2C buffer
        h2c_buf_index = (h2c_buf_index + 1) % NUM_H2C_BUFFERS;
        
        // Show periodic stats
        if (frames_processed % 300 == 0) {
            printf("Copy: Processed %d frames\n", frames_processed);
        }
    }
    
    printf("Copy thread exiting\n");
    return NULL;
}

// H2C transmission thread - handles H2C transfers
void *h2c_thread_func(void *arg) {
    (void)arg; // Unused parameter
    uint32_t frames_transmitted = 0;
    
    printf("H2C transmission thread started\n");
    
    while (g_running) {
        processed_frame_data_t processed_data;
        
        // Wait for a processed frame from the Copy thread
        if (dequeue_processed_frame(&g_processed_queue, &processed_data) < 0) {
            if (!g_running) break;
            continue;
        }
        
        // Step 3: Perform H2C transfer with processed buffer
        if (perform_h2c_transfer(processed_data.h2c_buffer, FRAME_SIZE) < 0) {
            fprintf(stderr, "H2C transfer failed for frame %d\n", processed_data.frame_number);
            // Continue processing, don't exit on single failure
        } else if (processed_data.frame_number < 5) {
            printf("Frame %d transmitted via H2C buffer %d\n", 
                   processed_data.frame_number, processed_data.h2c_buf_index);
        }
        
        frames_transmitted++;
        __sync_fetch_and_add(&g_total_frames_transmitted, 1);
        
        // Show periodic stats
        if (frames_transmitted % 300 == 0) {
            printf("H2C: Transmitted %d frames\\n", frames_transmitted);
        }
        
        // Show performance stats periodically
        if (g_enable_performance_monitoring && frames_transmitted % 60 == 0) {
            printf("H2C: Frame %d - pipeline completed\\n", processed_data.frame_number);
            print_performance_stats();
        }
    }
    
    printf("H2C transmission thread exiting\\n");
    return NULL;
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
    
    printf("IWFG C2H to H2C Transfer Example (Multi-threaded)\n");
    printf("=================================================\n");
    printf("This example demonstrates:\n");
    printf("1. C2H transfers using IWFG_IOCTL_DMA_DATA - in separate thread\n");
    printf("2. H2C transfers using IWFG_IOCTL_DMA_DATA - in separate thread\n");
    printf("3. Pipelined frame routing from C2H to H2C buffers\n");
    printf("4. Inter-thread communication via frame queue\n");
    if (g_enable_performance_monitoring) {
        printf("Performance monitoring: ENABLED\n");
    } else {
        printf("Performance monitoring: DISABLED (use -p to enable)\n");
    }
    printf("\n");
    
    // Open IWFG device
    g_iwfg_fd = open("/dev/iwfg", O_RDWR);
    if (g_iwfg_fd < 0) {
        perror("Failed to open IWFG device");
        return -1;
    }
    printf("IWFG device opened successfully\n");
    
    // Initialize frame queues
    if (init_frame_queue(&g_frame_queue) < 0) {
        return -1;
    }
    if (init_processed_frame_queue(&g_processed_queue) < 0) {
        destroy_frame_queue(&g_frame_queue);
        return -1;
    }
    printf("Frame queues initialized (C2H->Copy: %d, Copy->H2C: %d)\n", FRAME_QUEUE_SIZE, FRAME_QUEUE_SIZE);
    
    // Setup C2H buffers first
    if (setup_c2h_buffers() < 0) {
        return -1;
    }
    
    // Setup H2C buffers
    if (setup_h2c_buffers() < 0) {
        return -1;
    }
    
    // Start streaming for C2H transfers
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_START) < 0) {
        perror("IWFG_IOCTL_STREAM_START failed");
        return -1;
    }
    
    printf("Streaming started. Press Ctrl+C to stop.\n");
    printf("C2H thread -> Copy thread -> H2C thread pipeline active\n\n");
    
    // Create and start threads
    if (pthread_create(&g_c2h_thread, NULL, c2h_thread_func, NULL) != 0) {
        perror("Failed to create C2H thread");
        return -1;
    }
    
    if (pthread_create(&g_copy_thread, NULL, copy_thread_func, NULL) != 0) {
        perror("Failed to create Copy thread");
        pthread_cancel(g_c2h_thread);
        return -1;
    }
    
    if (pthread_create(&g_h2c_thread, NULL, h2c_thread_func, NULL) != 0) {
        perror("Failed to create H2C thread");
        pthread_cancel(g_c2h_thread);
        pthread_cancel(g_copy_thread);
        return -1;
    }
    
    printf("C2H, Copy, and H2C threads started successfully\n");
    
    // Main thread - monitor and display statistics
    while (g_running) {
        sleep(10);  // Update stats every 10 seconds
        
        if (g_enable_performance_monitoring) {
            pthread_mutex_lock(&g_stats_mutex);
            printf("\n=== Pipeline Status ===\n");
            printf("Captured: %u, Transmitted: %u, Dropped: %u\n", 
                   g_total_frames_captured, g_total_frames_transmitted, g_frames_dropped);
            printf("Queue items: C2H->Copy %d/%d, Copy->H2C %d/%d\n", 
                   g_frame_queue.count, FRAME_QUEUE_SIZE,
                   g_processed_queue.count, FRAME_QUEUE_SIZE);
            
            if (g_frame_count >= 2) {
                print_performance_stats();
            }
            pthread_mutex_unlock(&g_stats_mutex);
        } else {
            printf("Pipeline status - Captured: %u, Transmitted: %u, Dropped: %u\n",
                   g_total_frames_captured, g_total_frames_transmitted, g_frames_dropped);
        }
    }
    
    printf("\nShutting down pipeline...\n");
    cleanup_and_exit(0);
    return 0;
}