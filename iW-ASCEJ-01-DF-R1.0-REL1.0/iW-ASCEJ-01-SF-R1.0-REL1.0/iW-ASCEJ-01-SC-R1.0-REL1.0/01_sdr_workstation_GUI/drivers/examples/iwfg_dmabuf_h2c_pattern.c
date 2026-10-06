/*
 * IWFG DMA-BUF H2C Test Pattern Generator
 * 
 * This example demonstrates:
 * 1. Driver-allocated DMA buffers using IWFG_BUF_TYPE_DMA_ALLOC
 * 2. CPU writes test pattern to DMA buffer via mmap
 * 3. H2C DMA transfer to FPGA
 * 
 * Purpose: Isolate H2C path testing without C2H involvement
 * 
 * Test patterns available:
 * - Color bars (vertical stripes)
 * - Gradient (horizontal gradient)
 * - Checkerboard
 * - Solid color
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

#include "iwfg_user.h"

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

#define WIDTH 1280
#define HEIGHT 1024
#define BYTES_PER_PIXEL 4   /* XRGB8888 format */
#define FRAME_SIZE (WIDTH * HEIGHT * BYTES_PER_PIXEL)

/* Test pattern types */
typedef enum {
    PATTERN_COLOR_BARS,
    PATTERN_GRADIENT,
    PATTERN_CHECKERBOARD,
    PATTERN_SOLID,
    PATTERN_MOVING_BAR
} pattern_type_t;

/* Buffer allocation modes */
typedef enum {
    MODE_DMABUF,    /* Driver-allocated DMA buffer */
    MODE_USERPTR    /* User-allocated buffer */
} buffer_mode_t;

/* ============================================================================
 * Global State
 * ============================================================================ */

static int g_iwfg_fd = -1;
static int g_dmabuf_fd = -1;
static uint32_t g_buf_index = 0;
static void *g_mapped_addr = NULL;
static void *g_userptr_buf = NULL;  /* For USERPTR mode */
static buffer_mode_t g_mode = MODE_DMABUF;
static volatile bool g_running = true;

/* ============================================================================
 * Signal Handler
 * ============================================================================ */

static void signal_handler(int sig __attribute__((unused))) {
    printf("\nStopping...\n");
    g_running = false;
}

/* ============================================================================
 * Test Pattern Generators
 * ============================================================================ */

/* Generate vertical color bars */
static void generate_color_bars(uint32_t *pixels) {
    const uint32_t colors[] = {
        0x00FFFFFF,  /* White */
        0x00FFFF00,  /* Yellow */
        0x0000FFFF,  /* Cyan */
        0x0000FF00,  /* Green */
        0x00FF00FF,  /* Magenta */
        0x00FF0000,  /* Red */
        0x000000FF,  /* Blue */
        0x00000000   /* Black */
    };
    int num_bars = sizeof(colors) / sizeof(colors[0]);
    int bar_width = WIDTH / num_bars;
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int bar_idx = x / bar_width;
            if (bar_idx >= num_bars) bar_idx = num_bars - 1;
            pixels[y * WIDTH + x] = colors[bar_idx];
        }
    }
}

/* Generate horizontal gradient */
static void generate_gradient(uint32_t *pixels) {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            uint8_t r = (x * 255) / WIDTH;
            uint8_t g = (y * 255) / HEIGHT;
            uint8_t b = 255 - r;
            pixels[y * WIDTH + x] = (r << 16) | (g << 8) | b;
        }
    }
}

/* Generate checkerboard pattern */
static void generate_checkerboard(uint32_t *pixels, int block_size) {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int cx = x / block_size;
            int cy = y / block_size;
            if ((cx + cy) % 2 == 0) {
                pixels[y * WIDTH + x] = 0x00FFFFFF;  /* White */
            } else {
                pixels[y * WIDTH + x] = 0x00000000;  /* Black */
            }
        }
    }
}

/* Generate solid color */
static void generate_solid(uint32_t *pixels, uint32_t color) {
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        pixels[i] = color;
    }
}

/* Generate moving vertical bar (animated) */
static void generate_moving_bar(uint32_t *pixels, int frame_num) {
    int bar_width = 64;
    int bar_x = (frame_num * 10) % WIDTH;
    
    /* Clear to black */
    memset(pixels, 0, FRAME_SIZE);
    
    /* Draw white vertical bar */
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = bar_x; x < bar_x + bar_width && x < WIDTH; x++) {
            pixels[y * WIDTH + x] = 0x00FFFFFF;
        }
    }
    
    /* Draw frame number in corner (simple text representation) */
    /* Draw a small colored square that changes with frame number */
    uint32_t frame_color = ((frame_num * 17) & 0xFF) << 16 | 
                           ((frame_num * 31) & 0xFF) << 8 | 
                           ((frame_num * 47) & 0xFF);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            pixels[y * WIDTH + x] = frame_color;
        }
    }
}

/* ============================================================================
 * Buffer Setup
 * ============================================================================ */

static int setup_dmabuf_buffer(void) {
    struct iwfg_user_buf user_buf = {
        .buf = NULL,                        /* NULL for driver allocation */
        .size = FRAME_SIZE,
        .buf_type = IWFG_BUF_TYPE_DMA_ALLOC,
        .import_fd = -1,
        .is_chunked = false,
        .chunk_size = 0,
        .dmabuf_fd = -1,
        .buf_index = 0
    };
    
    if (g_mode == MODE_USERPTR) {
        /* Allocate page-aligned buffer for USERPTR mode */
        if (posix_memalign(&g_userptr_buf, 4096, FRAME_SIZE) != 0) {
            perror("Failed to allocate aligned buffer");
            return -1;
        }
        memset(g_userptr_buf, 0, FRAME_SIZE);
        
        user_buf.buf = g_userptr_buf;
        user_buf.buf_type = IWFG_BUF_TYPE_USERPTR;
        printf("Using USERPTR mode, buffer at %p\n", g_userptr_buf);
    } else {
        printf("Using DMABUF mode (driver allocation)\n");
    }
    
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_ADD_BUF, &user_buf) < 0) {
        perror("IWFG_IOCTL_ADD_BUF failed");
        if (g_userptr_buf) {
            free(g_userptr_buf);
            g_userptr_buf = NULL;
        }
        return -1;
    }
    
    g_dmabuf_fd = user_buf.dmabuf_fd;
    g_buf_index = user_buf.buf_index;
    
    printf("Buffer registered: index=%u", g_buf_index);
    if (g_mode == MODE_DMABUF) {
        printf(", DMA-BUF fd=%d", g_dmabuf_fd);
    }
    printf("\n");
    
    if (g_mode == MODE_DMABUF) {
        /* mmap the DMA-BUF for CPU access */
        g_mapped_addr = mmap(NULL, FRAME_SIZE, 
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, 
                             g_dmabuf_fd, 0);
        if (g_mapped_addr == MAP_FAILED) {
            perror("Failed to mmap DMA-BUF");
            return -1;
        }
        printf("DMA-BUF mmap'd at %p\n", g_mapped_addr);
    } else {
        /* For USERPTR, use the allocated buffer directly */
        g_mapped_addr = g_userptr_buf;
        printf("Using USERPTR buffer at %p\n", g_mapped_addr);
    }
    
    return 0;
}

/* ============================================================================
 * H2C Transfer
 * ============================================================================ */

static int do_h2c_transfer(void) {
    struct iwfg_user_dma_req dma_req = {
        .buf = NULL,
        .size = FRAME_SIZE,
        .direction = IWFG_DIR_H2C_BUF,
        .offset = 0,
        .buf_index = g_buf_index,
        // .flags = IWFG_FLAG_LAST_PACKET
    };
    
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req) < 0) {
        perror("H2C DMA transfer failed");
        return -1;
    }
    
    return 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -m <mode>      Buffer mode: dmabuf (default) or userptr\n");
    printf("  -p <pattern>   Pattern type: bars, gradient, checker, solid, moving\n");
    printf("  -c <color>     Solid color in hex (e.g., FF0000 for red)\n");
    printf("  -d <delay>     Delay between frames in ms (default: 16)\n");
    printf("  -h             Show this help message\n");
    printf("\n");
    printf("Runs continuously until Ctrl+C is pressed.\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -m dmabuf -p bars    Send color bars using DMABUF mode\n", prog_name);
    printf("  %s -m userptr -p bars   Send color bars using USERPTR mode\n", prog_name);
    printf("  %s -p moving            Send moving bar animation\n", prog_name);
}

int main(int argc, char *argv[]) {
    pattern_type_t pattern = PATTERN_COLOR_BARS;
    uint32_t solid_color = 0x00FF0000;  /* Default red */
    int delay_ms = 16;
    int opt;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "m:p:c:d:h")) != -1) {
        switch (opt) {
        case 'm':
            if (strcmp(optarg, "dmabuf") == 0) {
                g_mode = MODE_DMABUF;
            } else if (strcmp(optarg, "userptr") == 0) {
                g_mode = MODE_USERPTR;
            } else {
                fprintf(stderr, "Unknown mode: %s\n", optarg);
                print_usage(argv[0]);
                return 1;
            }
            break;
        case 'p':
            if (strcmp(optarg, "bars") == 0) {
                pattern = PATTERN_COLOR_BARS;
            } else if (strcmp(optarg, "gradient") == 0) {
                pattern = PATTERN_GRADIENT;
            } else if (strcmp(optarg, "checker") == 0) {
                pattern = PATTERN_CHECKERBOARD;
            } else if (strcmp(optarg, "solid") == 0) {
                pattern = PATTERN_SOLID;
            } else if (strcmp(optarg, "moving") == 0) {
                pattern = PATTERN_MOVING_BAR;
            } else {
                fprintf(stderr, "Unknown pattern: %s\n", optarg);
                print_usage(argv[0]);
                return 1;
            }
            break;
        case 'c':
            solid_color = strtoul(optarg, NULL, 16);
            break;
        case 'd':
            delay_ms = atoi(optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    
    printf("IWFG DMA-BUF H2C Test Pattern Generator\n");
    printf("========================================\n");
    printf("Buffer mode: %s\n", g_mode == MODE_DMABUF ? "DMABUF" : "USERPTR");
    printf("Resolution: %dx%d\n", WIDTH, HEIGHT);
    printf("Frame size: %d bytes (%.2f MB)\n", FRAME_SIZE, FRAME_SIZE / (1024.0 * 1024.0));
    printf("Pattern: %d\n", pattern);
    printf("Delay: %d ms\n", delay_ms);
    printf("Press Ctrl+C to stop.\n");
    printf("\n");
    
    /* Open IWFG device */
    g_iwfg_fd = open("/dev/iwfg", O_RDWR);
    if (g_iwfg_fd < 0) {
        perror("Failed to open IWFG device");
        return -1;
    }
    printf("IWFG device opened\n");
    
    /* Setup DMA-BUF buffer */
    if (setup_dmabuf_buffer() < 0) {
        close(g_iwfg_fd);
        return -1;
    }
    
    /* Start streaming */
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_START) < 0) {
        perror("IWFG_IOCTL_STREAM_START failed");
        close(g_iwfg_fd);
        return -1;
    }
    printf("Streaming started\n\n");
    
    /* Generate and send frames */
    uint32_t *pixels = (uint32_t *)g_mapped_addr;
    int frames_sent = 0;
    
    /* Generate initial pattern (for non-animated patterns) */
    if (pattern != PATTERN_MOVING_BAR) {
        switch (pattern) {
        case PATTERN_COLOR_BARS:
            printf("Generating color bars pattern...\n");
            generate_color_bars(pixels);
            break;
        case PATTERN_GRADIENT:
            printf("Generating gradient pattern...\n");
            generate_gradient(pixels);
            break;
        case PATTERN_CHECKERBOARD:
            printf("Generating checkerboard pattern...\n");
            generate_checkerboard(pixels, 64);
            break;
        case PATTERN_SOLID:
            printf("Generating solid color 0x%06X...\n", solid_color);
            generate_solid(pixels, solid_color);
            break;
        default:
            break;
        }
    }
    
    printf("Sending frames via H2C...\n");
    
    while (g_running) {
        /* For animated pattern, regenerate each frame */
        if (pattern == PATTERN_MOVING_BAR) {
            generate_moving_bar(pixels, frames_sent);
        }
        
        /* Send frame via H2C */
        if (do_h2c_transfer() < 0) {
            fprintf(stderr, "H2C transfer failed at frame %d\n", frames_sent);
            break;
        }
        
        frames_sent++;
        
        if (frames_sent % 60 == 0) {
            printf("Sent %d frames\n", frames_sent);
        }
        
        if (delay_ms > 0) {
            usleep(delay_ms * 1000);
        } else {
			usleep(16666);
		}
    }
    
    printf("\nTotal frames sent: %d\n", frames_sent);
    
    /* Cleanup */
    ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_STOP);
    
    if (g_mode == MODE_DMABUF) {
        if (g_mapped_addr && g_mapped_addr != MAP_FAILED) {
            munmap(g_mapped_addr, FRAME_SIZE);
        }
        if (g_dmabuf_fd >= 0) {
            close(g_dmabuf_fd);
        }
    } else {
        /* USERPTR mode - free allocated buffer */
        if (g_userptr_buf) {
            free(g_userptr_buf);
        }
    }
    close(g_iwfg_fd);
    
    printf("Done.\n");
    return 0;
}
