/*
 * IWFG H2C Pattern Generation Example
 * 
 * This example demonstrates:
 * 1. Generating moving test patterns in software
 * 2. Transmitting patterns via H2C DMA using IWFG_IOCTL_DMA_DATA
 * 3. No C2H buffers - pure H2C transmission test
 * 
 * Features:
 * - Software-generated moving test patterns
 * - H2C-only buffer setup
 * - Single-threaded pattern generation and transmission
 * - Performance monitoring for H2C transfers
 * - Various test patterns: moving bars, checkerboard, gradient
 * - Frame rate control and statistics
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
#include <math.h>

#include "iwfg_user.h"

#define NUM_H2C_BUFFERS 2  // H2C (transmit) buffers
#define WIDTH 3840
#define HEIGHT 2160
#define TARGET_FPS 60      // Target frame rate

// Runtime pixel format (3 = RGB888, 4 = RGBA8888/XRGB8888)
static uint32_t g_bpp = 3;
static uint32_t g_frame_size = 0;  // computed from WIDTH * HEIGHT * g_bpp

// Test pattern types
typedef enum {
    PATTERN_MOVING_BARS,
    PATTERN_CHECKERBOARD,
    PATTERN_GRADIENT,
    PATTERN_COLOR_BARS,
    PATTERN_DIAGONAL_LINES,
    PATTERN_COUNT
} pattern_type_t;

// Global state for cleanup
static int g_iwfg_fd = -1;
static void *g_h2c_buffers[NUM_H2C_BUFFERS];  // H2C (TX) buffers
static volatile bool g_running = true;

// Pattern generation state
static pattern_type_t g_current_pattern = PATTERN_MOVING_BARS;
static uint32_t g_frame_counter = 0;
static uint32_t g_pattern_offset = 0;
static bool g_enable_performance_monitoring = false;
static bool g_auto_cycle_patterns = false;

// Performance monitoring
static struct timespec g_frame_times[100];
static struct timespec g_h2c_times[100];
static int g_perf_frame_count = 0;
static uint32_t g_total_frames_transmitted = 0;

void cleanup_and_exit(int sig __attribute__((unused))) {
    printf("\nCleaning up and exiting...\n");
    g_running = false;
    
    // Stop streaming (if it was started)
    if (g_iwfg_fd >= 0) {
        // ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_STOP);
        close(g_iwfg_fd);
    }
    
    // Free H2C buffers
    for (int i = 0; i < NUM_H2C_BUFFERS; i++) {
        if (g_h2c_buffers[i]) {
            munlock(g_h2c_buffers[i], g_frame_size);
            munmap(g_h2c_buffers[i], g_frame_size);
        }
    }
    
    printf("Total frames transmitted: %u\n", g_total_frames_transmitted);
    
    exit(0);
}

static void print_performance_stats(void) {
    if (!g_enable_performance_monitoring || g_perf_frame_count < 2) return;
    
    uint64_t total_frame_time = 0;
    uint64_t total_h2c_time = 0;
    uint64_t min_frame_time = UINT64_MAX;
    uint64_t max_frame_time = 0;
    uint64_t min_h2c_time = UINT64_MAX;
    uint64_t max_h2c_time = 0;
    
    for (int i = 1; i < g_perf_frame_count; i++) {
        uint64_t frame_time = (g_frame_times[i].tv_sec - g_frame_times[i-1].tv_sec) * 1000000000ULL +
                             (g_frame_times[i].tv_nsec - g_frame_times[i-1].tv_nsec);
        uint64_t h2c_time = g_h2c_times[i].tv_sec * 1000000000ULL + g_h2c_times[i].tv_nsec;
        
        total_frame_time += frame_time;
        total_h2c_time += h2c_time;
        
        if (frame_time < min_frame_time) min_frame_time = frame_time;
        if (frame_time > max_frame_time) max_frame_time = frame_time;
        if (h2c_time < min_h2c_time) min_h2c_time = h2c_time;
        if (h2c_time > max_h2c_time) max_h2c_time = h2c_time;
    }
    
    uint64_t avg_frame_time = total_frame_time / (g_perf_frame_count - 1);
    uint64_t avg_h2c_time = total_h2c_time / (g_perf_frame_count - 1);
    double fps = 1000000000.0 / avg_frame_time;
    
    printf("Performance Stats (last %d frames):\n", g_perf_frame_count - 1);
    printf("  Average frame time: %.2f ms (%.1f FPS)\n", avg_frame_time / 1000000.0, fps);
    printf("  Average H2C time: %.2f ms (%.1f%% of frame)\n", 
           avg_h2c_time / 1000000.0, (avg_h2c_time * 100.0) / avg_frame_time);
    printf("  Min/Max frame time: %.2f/%.2f ms\n", 
           min_frame_time / 1000000.0, max_frame_time / 1000000.0);
    printf("  Min/Max H2C time: %.2f/%.2f ms\n", 
           min_h2c_time / 1000000.0, max_h2c_time / 1000000.0);
    printf("  H2C efficiency: %.1f%% (non-H2C overhead: %.1f%%)\n", 
           (avg_h2c_time * 100.0) / avg_frame_time,
           ((avg_frame_time - avg_h2c_time) * 100.0) / avg_frame_time);
}

static const char* pattern_name(pattern_type_t pattern) {
    switch (pattern) {
        case PATTERN_MOVING_BARS: return "Moving Bars";
        case PATTERN_CHECKERBOARD: return "Checkerboard";
        case PATTERN_GRADIENT: return "Gradient";
        case PATTERN_COLOR_BARS: return "Color Bars";
        case PATTERN_DIAGONAL_LINES: return "Diagonal Lines";
        default: return "Unknown";
    }
}

/* Write a single pixel from a 0x00RRGGBB color value, supporting 3 or 4 bpp.
 * For 4 bpp the alpha byte is written as 0xFF (fully opaque). */
static inline void set_pixel_rgb(uint8_t *buf, int idx, uint32_t color_rgb) {
    int off = idx * (int)g_bpp;
    buf[off + 0] = (color_rgb >> 16) & 0xFF; /* R */
    buf[off + 1] = (color_rgb >>  8) & 0xFF; /* G */
    buf[off + 2] =  color_rgb        & 0xFF; /* B */
    if (g_bpp == 4)
        buf[off + 3] = 0xFF;                 /* A - fully opaque */
}

// Generate moving vertical bars pattern
void generate_moving_bars(uint8_t *pixels, uint32_t frame_num) {
    int bar_width = 100;
    int num_bars = WIDTH / bar_width + 2;  // Extra bars for smooth scrolling
    int offset = (frame_num * 5) % bar_width;  // Move 5 pixels per frame
    
    uint32_t colors[] = {
        0xFF0000, // Red
        0x00FF00, // Green
        0x0000FF, // Blue
        0xFFFF00, // Yellow
        0xFF00FF, // Magenta
        0x00FFFF, // Cyan
        0xFFFFFF, // White
        0x000000  // Black
    };
    int num_colors = sizeof(colors) / sizeof(colors[0]);
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int bar_pos = (x + offset) / bar_width;
            set_pixel_rgb(pixels, y * WIDTH + x, colors[bar_pos % num_colors]);
        }
    }
}

// Generate animated checkerboard pattern
void generate_checkerboard(uint8_t *pixels, uint32_t frame_num) {
    int checker_size = 80;
    int animation_speed = frame_num / 10;  // Change every 10 frames
    
    uint32_t color1 = 0xFFFFFF;  // White
    uint32_t color2 = 0x000000;  // Black
    
    // Animate colors
    if (animation_speed % 4 == 1) {
        color1 = 0xFF0000; color2 = 0x0000FF;  // Red/Blue
    } else if (animation_speed % 4 == 2) {
        color1 = 0x00FF00; color2 = 0xFF00FF;  // Green/Magenta
    } else if (animation_speed % 4 == 3) {
        color1 = 0xFFFF00; color2 = 0x00FFFF;  // Yellow/Cyan
    }
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int checker_x = x / checker_size;
            int checker_y = y / checker_size;
            bool is_white = (checker_x + checker_y) % 2 == 0;
            set_pixel_rgb(pixels, y * WIDTH + x, is_white ? color1 : color2);
        }
    }
}

// Generate moving gradient pattern
void generate_gradient(uint8_t *pixels, uint32_t frame_num) {
    double phase = (frame_num * 0.02);  // Slow animation
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            // Create a moving wave pattern
            double wave = sin((double)x / WIDTH * 4 * M_PI + phase) * 
                         sin((double)y / HEIGHT * 4 * M_PI + phase * 0.7);
            
            // Convert wave to color
            uint8_t r = (uint8_t)((sin(wave + phase) + 1) * 127.5);
            uint8_t g = (uint8_t)((sin(wave + phase + M_PI/3) + 1) * 127.5);
            uint8_t b = (uint8_t)((sin(wave + phase + 2*M_PI/3) + 1) * 127.5);
            
            int idx = (y * WIDTH + x) * (int)g_bpp;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
            if (g_bpp == 4) pixels[idx + 3] = 0xFF;
        }
    }
}

// Generate color bars pattern
void generate_color_bars(uint8_t *pixels, uint32_t frame_num) {
    uint32_t colors[] = {
        0xFFFFFF, // White
        0xFFFF00, // Yellow
        0x00FFFF, // Cyan
        0x00FF00, // Green
        0xFF00FF, // Magenta
        0xFF0000, // Red
        0x0000FF, // Blue
        0x000000  // Black
    };
    int num_colors = sizeof(colors) / sizeof(colors[0]);
    int bar_width = WIDTH / num_colors;
    
    // Add some animation by shifting the pattern
    int shift = (frame_num * 2) % WIDTH;
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int shifted_x = (x + shift) % WIDTH;
            int color_index = shifted_x / bar_width;
            if (color_index >= num_colors) color_index = num_colors - 1;
            set_pixel_rgb(pixels, y * WIDTH + x, colors[color_index]);
        }
    }
}

// Generate diagonal lines pattern
void generate_diagonal_lines(uint8_t *pixels, uint32_t frame_num) {
    int line_spacing = 40;
    int offset = (frame_num * 3) % (line_spacing * 2);  // Move lines
    
    uint32_t line_color = 0xFFFFFF;  // White
    uint32_t bg_color = 0x000080;    // Dark blue
    
    // Cycle line color
    int color_cycle = (frame_num / 60) % 6;  // Change every 2 seconds at 30fps
    switch (color_cycle) {
        case 0: line_color = 0xFFFFFF; break; // White
        case 1: line_color = 0xFF0000; break; // Red
        case 2: line_color = 0x00FF00; break; // Green
        case 3: line_color = 0x0000FF; break; // Blue
        case 4: line_color = 0xFFFF00; break; // Yellow
        case 5: line_color = 0xFF00FF; break; // Magenta
    }
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            // Create diagonal lines
            int diag_pos = (x + y + offset) % (line_spacing * 2);
            bool on_line = diag_pos < line_spacing / 4;
            set_pixel_rgb(pixels, y * WIDTH + x, on_line ? line_color : bg_color);
        }
    }
}

void generate_test_pattern(void *buffer, pattern_type_t pattern, uint32_t frame_num) {
    uint8_t *pixels = (uint8_t*)buffer;
    
    switch (pattern) {
        case PATTERN_MOVING_BARS:
            generate_moving_bars(pixels, frame_num);
            break;
        case PATTERN_CHECKERBOARD:
            generate_checkerboard(pixels, frame_num);
            break;
        case PATTERN_GRADIENT:
            generate_gradient(pixels, frame_num);
            break;
        case PATTERN_COLOR_BARS:
            generate_color_bars(pixels, frame_num);
            break;
        case PATTERN_DIAGONAL_LINES:
            generate_diagonal_lines(pixels, frame_num);
            break;
        default:
            // Fill with solid red as fallback
            for (int i = 0; i < WIDTH * HEIGHT; i++) {
                set_pixel_rgb(pixels, i, 0xFF0000);  // Red
            }
            break;
    }
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
        .direction = IWFG_DIR_H2C_BUF,  // H2C direction
		.offset = 0
    };
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req) < 0) {
        perror("IWFG_IOCTL_DMA_DATA failed for H2C transfer");
        printf("H2C: IWFG_IOCTL_DMA_DATA failed with errno: %d\n", errno);
        return -1;
    }
    
    if (g_enable_performance_monitoring) {
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        
        // Record H2C transfer time (duration)
        if (g_perf_frame_count < 100) {
            g_h2c_times[g_perf_frame_count].tv_sec = end_time.tv_sec - start_time.tv_sec;
            g_h2c_times[g_perf_frame_count].tv_nsec = end_time.tv_nsec - start_time.tv_nsec;
            if (g_h2c_times[g_perf_frame_count].tv_nsec < 0) {
                g_h2c_times[g_perf_frame_count].tv_sec--;
                g_h2c_times[g_perf_frame_count].tv_nsec += 1000000000;
            }
        }
    }
    
    return 0;
}

int setup_h2c_buffers(void) {
    // Allocate page-aligned H2C buffers using mmap for DMA compatibility with IOMMU
    for (int i = 0; i < NUM_H2C_BUFFERS; i++) {
        // Use mmap with MAP_POPULATE | MAP_LOCKED for reliable DMA
        g_h2c_buffers[i] = mmap(NULL, g_frame_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_LOCKED,
                                 -1, 0);
        if (g_h2c_buffers[i] == MAP_FAILED) {
            // Fallback without MAP_LOCKED if it fails
            perror("mmap with MAP_LOCKED failed, trying without");
            g_h2c_buffers[i] = mmap(NULL, g_frame_size,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                     -1, 0);
            if (g_h2c_buffers[i] == MAP_FAILED) {
                perror("H2C buffer mmap failed");
                return -1;
            }
            if (mlock(g_h2c_buffers[i], g_frame_size) != 0) {
                perror("Warning: mlock also failed - DMA may have issues");
            }
        }
        
        // Prevent fork from copying these pages
        madvise(g_h2c_buffers[i], g_frame_size, MADV_DONTFORK);
        
        // Initialize with black (also ensures all pages are write-faulted)
        memset(g_h2c_buffers[i], 0, g_frame_size);
        __sync_synchronize();  // Memory barrier

		struct iwfg_user_buf user_buf = {
			.buf = g_h2c_buffers[i],
			.size = g_frame_size,
			.is_chunked = false,
			.chunk_size = 0
		};

		// if (ioctl(g_iwfg_fd, IWFG_IOCTL_ADD_BUF, &user_buf) < 0) {
		// 	perror("IWFG_IOCTL_ADD_BUF failed");
		// 	return -1;
		// }
        
        printf("Allocated H2C buffer %d: %p (%u bytes)\n", i, g_h2c_buffers[i], g_frame_size);
    }
    
    printf("H2C buffers added to IWFG driver\n");
    return 0;
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -p, --performance    Enable performance monitoring and statistics\n");
    printf("  -c, --cycle          Auto-cycle through all test patterns\n");
    printf("  -t, --pattern <n>    Select specific pattern (0-4):\n");
    printf("                       0 = Moving Bars (default)\n");
    printf("                       1 = Checkerboard\n");
    printf("                       2 = Gradient\n");
    printf("                       3 = Color Bars\n");
    printf("                       4 = Diagonal Lines\n");
    printf("  -f, --fps <fps>      Target frame rate (default: 30)\n");
    printf("  -h, --help          Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                  Run with moving bars pattern\n", prog_name);
    printf("  %s -p -c            Run with performance monitoring and auto-cycle patterns\n", prog_name);
    printf("  %s -t 2 -f 60       Run gradient pattern at 60 FPS\n", prog_name);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    int target_fps = TARGET_FPS;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--performance") == 0) {
            g_enable_performance_monitoring = true;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--cycle") == 0) {
            g_auto_cycle_patterns = true;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--pattern") == 0) {
            if (i + 1 < argc) {
                int pattern = atoi(argv[++i]);
                if (pattern >= 0 && pattern < PATTERN_COUNT) {
                    g_current_pattern = (pattern_type_t)pattern;
                } else {
                    fprintf(stderr, "Invalid pattern: %d (must be 0-%d)\n", pattern, PATTERN_COUNT - 1);
                    return 1;
                }
            } else {
                fprintf(stderr, "Pattern option requires a value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bpp") == 0) {
            if (i + 1 < argc) {
                int bpp = atoi(argv[++i]);
                if (bpp == 3 || bpp == 4) {
                    g_bpp = (uint32_t)bpp;
                } else {
                    fprintf(stderr, "Invalid bpp: %d (must be 3 or 4)\n", bpp);
                    return 1;
                }
            } else {
                fprintf(stderr, "BPP option requires a value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fps") == 0) {
            if (i + 1 < argc) {
                target_fps = atoi(argv[++i]);
                if (target_fps <= 0 || target_fps > 120) {
                    fprintf(stderr, "Invalid FPS: %d (must be 1-120)\n", target_fps);
                    return 1;
                }
            } else {
                fprintf(stderr, "FPS option requires a value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Compute frame size now that g_bpp is known
    g_frame_size = WIDTH * HEIGHT * g_bpp;

    printf("IWFG H2C Pattern Generation Example\n");
    printf("===================================\n");
    printf("This example demonstrates:\n");
    printf("1. Software-generated moving test patterns\n");
    printf("2. H2C transfers using IWFG_IOCTL_DMA_DATA\n");
    printf("3. No C2H buffers - pure H2C transmission test\n");
    printf("Pixel format: %u bpp (%s)\n", g_bpp, g_bpp == 4 ? "RGBA8888" : "RGB888");
    printf("Frame size: %u bytes\n", g_frame_size);
    printf("Target FPS: %d\n", target_fps);
    if (g_auto_cycle_patterns) {
        printf("Pattern mode: Auto-cycle through all patterns\n");
    } else {
        printf("Pattern mode: %s\n", pattern_name(g_current_pattern));
    }
    if (g_enable_performance_monitoring) {
        printf("Performance monitoring: ENABLED\n");
    } else {
        printf("Performance monitoring: DISABLED (use -p to enable)\n");
    }
    printf("\n");
    
    // Open IWFG device
    g_iwfg_fd = open("/dev/iwfg0", O_RDWR);
    if (g_iwfg_fd < 0) {
        perror("Failed to open IWFG device");
        return -1;
    }
    printf("IWFG device opened successfully\n");
    
    // Setup H2C buffers
    if (setup_h2c_buffers() < 0) {
        return -1;
    }
    
    printf("Pattern generation and H2C transmission started.\n");
    printf("Press Ctrl+C to stop.\n\n");
    
    // Calculate frame timing
    struct timespec frame_interval;
    frame_interval.tv_sec = 0;
    frame_interval.tv_nsec = 1000000000L / target_fps;
    
    struct timespec next_frame_time;
    clock_gettime(CLOCK_MONOTONIC, &next_frame_time);
    
    uint32_t current_buffer = 0;

	// Start streaming
    // if (ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_START) < 0) {
    //     perror("IWFG_IOCTL_STREAM_START failed");
    //     return -1;
    // }
    
    // Main generation and transmission loop
    while (g_running) {
        struct timespec frame_start, frame_end;
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &frame_start);
        }
        
        // Auto-cycle patterns if enabled
        if (g_auto_cycle_patterns) {
            // Change pattern every 5 seconds (at target FPS)
            g_current_pattern = (pattern_type_t)((g_frame_counter / (target_fps * 5)) % PATTERN_COUNT);
        }
        
        // Generate test pattern
        generate_test_pattern(g_h2c_buffers[current_buffer], g_current_pattern, g_frame_counter);
        
        // Transmit via H2C
        if (perform_h2c_transfer(g_h2c_buffers[current_buffer], g_frame_size) < 0) {
            fprintf(stderr, "H2C transfer failed for frame %u\n", g_frame_counter);
            // Continue anyway for testing
        } else {
            g_total_frames_transmitted++;
        }
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &frame_end);
            
            if (g_perf_frame_count < 100) {
                g_frame_times[g_perf_frame_count] = frame_end;
                g_perf_frame_count++;
            }
        }
        
        g_frame_counter++;
        current_buffer = (current_buffer + 1) % NUM_H2C_BUFFERS;
        
        // Show periodic stats
        if (g_frame_counter % (target_fps * 5) == 0) {  // Every 5 seconds
            printf("\n=== Status Update ===\n");
            printf("Frames transmitted: %u\n", g_total_frames_transmitted);
            printf("Current pattern: %s\n", pattern_name(g_current_pattern));
            if (g_enable_performance_monitoring && g_perf_frame_count >= 2) {
                print_performance_stats();
            }
            printf("\n");
        }
        
        // Frame rate control
        next_frame_time.tv_nsec += frame_interval.tv_nsec;
        if (next_frame_time.tv_nsec >= 1000000000L) {
            next_frame_time.tv_sec++;
            next_frame_time.tv_nsec -= 1000000000L;
        }
        
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame_time, NULL);
    }
    
    printf("\nPattern generation completed.\n");
    cleanup_and_exit(0);
    return 0;
}
