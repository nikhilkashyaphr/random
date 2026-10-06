/*
 * IWFG DMA-BUF C2H Frame Capture to Files
 * 
 * This example demonstrates:
 * 1. Driver-allocated DMA buffers using IWFG_BUF_TYPE_DMA_ALLOC
 * 2. C2H DMA capture from FPGA
 * 3. Write captured frames to raw files for analysis
 * 
 * Purpose: Isolate C2H path testing, capture frames for offline viewing
 * 
 * Output: Raw XRGB8888 files that can be converted with ffmpeg:
 *   ffmpeg -f rawvideo -pix_fmt bgra -s 1280x1024 -i frame_000.raw frame_000.jpg
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

#define DEFAULT_NUM_FRAMES 10
#define MAX_FILENAME_LEN 256

/* ============================================================================
 * Global State
 * ============================================================================ */

static int g_iwfg_fd = -1;
static int g_dmabuf_fd = -1;
static uint32_t g_buf_index = 0;
static void *g_mapped_addr = NULL;
static volatile bool g_running = true;

/* ============================================================================
 * Signal Handler
 * ============================================================================ */

static void signal_handler(int sig __attribute__((unused))) {
    printf("\nStopping...\n");
    g_running = false;
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
    
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_ADD_BUF, &user_buf) < 0) {
        perror("IWFG_IOCTL_ADD_BUF (DMA_ALLOC) failed");
        return -1;
    }
    
    g_dmabuf_fd = user_buf.dmabuf_fd;
    g_buf_index = user_buf.buf_index;
    
    printf("Buffer allocated: DMA-BUF fd=%d, index=%u\n", g_dmabuf_fd, g_buf_index);
    
    /* mmap the DMA-BUF for CPU access */
    g_mapped_addr = mmap(NULL, FRAME_SIZE, 
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED, 
                         g_dmabuf_fd, 0);
    if (g_mapped_addr == MAP_FAILED) {
        perror("Failed to mmap DMA-BUF");
        return -1;
    }
    
    printf("Buffer mmap'd at %p\n", g_mapped_addr);
    return 0;
}

/* ============================================================================
 * C2H Transfer
 * ============================================================================ */

static int do_c2h_transfer(void) {
    struct iwfg_user_dma_req dma_req = {
        .buf = NULL,
        .size = FRAME_SIZE,
        .direction = IWFG_DIR_C2H_BUF,
        .offset = 0,
        .buf_index = g_buf_index,
        .flags = 0
    };
    
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req) < 0) {
        if (errno == EAGAIN) {
            return -EAGAIN;
        }
        perror("C2H DMA transfer failed");
        return -1;
    }
    
    return 0;
}

/* ============================================================================
 * File Operations
 * ============================================================================ */

static int save_frame_to_file(const char *output_dir, int frame_num) {
    char filename[MAX_FILENAME_LEN];
    FILE *fp;
    
    snprintf(filename, sizeof(filename), "%s/frame_%03d.raw", output_dir, frame_num);
    
    fp = fopen(filename, "wb");
    if (!fp) {
        perror("Failed to open output file");
        return -1;
    }
    
    size_t written = fwrite(g_mapped_addr, 1, FRAME_SIZE, fp);
    fclose(fp);
    
    if (written != FRAME_SIZE) {
        fprintf(stderr, "Short write: %zu of %d bytes\n", written, FRAME_SIZE);
        return -1;
    }
    
    printf("Saved frame %d to %s\n", frame_num, filename);
    return 0;
}

/* Analyze frame content - check if it's all zeros or has actual data */
static void analyze_frame(int frame_num) {
    uint32_t *pixels = (uint32_t *)g_mapped_addr;
    uint32_t first_pixel = pixels[0];
    uint32_t last_pixel = pixels[WIDTH * HEIGHT - 1];
    uint32_t center_pixel = pixels[(HEIGHT / 2) * WIDTH + (WIDTH / 2)];
    
    int zero_count = 0;
    int unique_count = 0;
    uint32_t prev_pixel = pixels[0];
    
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        if (pixels[i] == 0) zero_count++;
        if (pixels[i] != prev_pixel) {
            unique_count++;
            prev_pixel = pixels[i];
        }
    }
    
    printf("Frame %d analysis:\n", frame_num);
    printf("  First pixel:  0x%08X\n", first_pixel);
    printf("  Center pixel: 0x%08X\n", center_pixel);
    printf("  Last pixel:   0x%08X\n", last_pixel);
    printf("  Zero pixels:  %d (%.1f%%)\n", zero_count, 
           100.0 * zero_count / (WIDTH * HEIGHT));
    printf("  Transitions:  %d\n", unique_count);
    
    if (zero_count == WIDTH * HEIGHT) {
        printf("  WARNING: Frame is entirely black/zero!\n");
    } else if (unique_count < 10) {
        printf("  WARNING: Frame appears to be nearly solid color!\n");
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -n <count>     Number of frames to capture (default: %d)\n", DEFAULT_NUM_FRAMES);
    printf("  -o <dir>       Output directory (default: current directory)\n");
    printf("  -s <frames>    Skip this many frames before capturing\n");
    printf("  -a             Analyze frames (print pixel statistics)\n");
    printf("  -h             Show this help message\n");
    printf("\n");
    printf("Output files are raw XRGB8888 format.\n");
    printf("Convert to viewable format with ffmpeg:\n");
    printf("  ffmpeg -f rawvideo -pix_fmt bgra -s %dx%d -i frame_000.raw frame_000.jpg\n", 
           WIDTH, HEIGHT);
    printf("\n");
    printf("Or convert all frames:\n");
    printf("  for f in *.raw; do ffmpeg -f rawvideo -pix_fmt bgra -s %dx%d -i $f ${f%%.raw}.jpg; done\n",
           WIDTH, HEIGHT);
}

int main(int argc, char *argv[]) {
    int num_frames = DEFAULT_NUM_FRAMES;
    int skip_frames = 0;
    const char *output_dir = ".";
    bool analyze = false;
    int opt;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "n:o:s:ah")) != -1) {
        switch (opt) {
        case 'n':
            num_frames = atoi(optarg);
            break;
        case 'o':
            output_dir = optarg;
            break;
        case 's':
            skip_frames = atoi(optarg);
            break;
        case 'a':
            analyze = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    
    printf("IWFG DMA-BUF C2H Frame Capture\n");
    printf("==============================\n");
    printf("Resolution: %dx%d\n", WIDTH, HEIGHT);
    printf("Frame size: %d bytes (%.2f MB)\n", FRAME_SIZE, FRAME_SIZE / (1024.0 * 1024.0));
    printf("Frames to capture: %d\n", num_frames);
    printf("Skip frames: %d\n", skip_frames);
    printf("Output directory: %s\n", output_dir);
    printf("Analyze frames: %s\n", analyze ? "yes" : "no");
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
    
    /* Skip frames if requested */
    if (skip_frames > 0) {
        printf("Skipping %d frames...\n", skip_frames);
        for (int i = 0; i < skip_frames && g_running; i++) {
            int ret = do_c2h_transfer();
            if (ret < 0 && ret != -EAGAIN) {
                fprintf(stderr, "C2H transfer failed during skip\n");
                break;
            }
            if (ret == -EAGAIN) {
                usleep(1000);
                i--;  /* Retry */
                continue;
            }
        }
        printf("Skipped %d frames\n", skip_frames);
    }
    
    /* Capture frames */
    printf("Capturing %d frames...\n", num_frames);
    int frames_captured = 0;
    int retry_count = 0;
    const int max_retries = 1000;
    
    while (g_running && frames_captured < num_frames) {
        int ret = do_c2h_transfer();
        
        if (ret == -EAGAIN) {
            retry_count++;
            if (retry_count > max_retries) {
                fprintf(stderr, "Too many retries, giving up\n");
                break;
            }
            usleep(1000);
            continue;
        }
        
        if (ret < 0) {
            fprintf(stderr, "C2H transfer failed at frame %d\n", frames_captured);
            break;
        }
        
        retry_count = 0;
        
        /* Analyze frame if requested */
        if (analyze) {
            analyze_frame(frames_captured);
        }
        
        /* Save frame to file */
        if (save_frame_to_file(output_dir, frames_captured) < 0) {
            fprintf(stderr, "Failed to save frame %d\n", frames_captured);
            break;
        }
        
        frames_captured++;
    }
    
    printf("\nTotal frames captured: %d\n", frames_captured);
    
    /* Cleanup */
    ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_STOP);
    
    if (g_mapped_addr && g_mapped_addr != MAP_FAILED) {
        munmap(g_mapped_addr, FRAME_SIZE);
    }
    if (g_dmabuf_fd >= 0) {
        close(g_dmabuf_fd);
    }
    close(g_iwfg_fd);
    
    printf("\nTo convert raw files to viewable images:\n");
    printf("  cd %s\n", output_dir);
    printf("  for f in *.raw; do ffmpeg -f rawvideo -pix_fmt bgra -s %dx%d -i \"$f\" \"${f%%.raw}.jpg\"; done\n",
           WIDTH, HEIGHT);
    
    printf("\nDone.\n");
    return 0;
}
