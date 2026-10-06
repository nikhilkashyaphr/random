/*
 * IWFG DMA-BUF GPU H2C Test Pattern Generator
 * 
 * This example demonstrates:
 * 1. Driver-allocated DMA buffers exported via DMA-BUF
 * 2. Zero-copy GPU rendering using EGL_EXT_image_dma_buf_import
 * 3. GPU-generated moving bar pattern for H2C vsync debugging
 * 4. H2C DMA transfer to FPGA
 * 
 * Purpose: Debug vsync in H2C path using GPU-generated animated patterns
 * 
 * Architecture:
 * - Driver allocates DMA buffers using IWFG_BUF_TYPE_DMA_ALLOC
 * - Buffers are exported as DMA-BUF file descriptors
 * - GPU imports DMA-BUF as EGLImage (zero-copy)
 * - EGLImage is bound to OpenGL texture
 * - GPU renders moving bar pattern via OpenGL ES
 * - H2C transfers GPU-rendered frames to FPGA
 * 
 * Requirements:
 * - EGL for DMA-BUF import (eglCreateImageKHR)
 * - Mesa/NVIDIA drivers with EGL_EXT_image_dma_buf_import
 * - OpenGL ES 2.0
 * 
 * Build:
 *   gcc -o iwfg_dmabuf_gpu_h2c_pattern iwfg_dmabuf_gpu_h2c_pattern.c \
 *       -lEGL -lGLESv2 -ldrm -lgbm -lm
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
#include <math.h>

// DRM/GBM for device access and surfaceless EGL
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <gbm.h>

// EGL and OpenGL ES headers
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "iwfg_user.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

#define WIDTH 1280
#define HEIGHT 1024
#define BYTES_PER_PIXEL 4   /* XRGB8888 format */
#define FRAME_SIZE (WIDTH * HEIGHT * BYTES_PER_PIXEL)

#define BAR_WIDTH 64        /* Width of moving bar */
#define BAR_SPEED 10        /* Pixels per frame */

/* ============================================================================
 * Global State
 * ============================================================================ */

static int g_iwfg_fd = -1;
static int g_drm_fd = -1;
static struct gbm_device *g_gbm_device = NULL;
static volatile bool g_running = true;

/* Buffer state */
static int g_dmabuf_fd = -1;
static uint32_t g_buf_index = 0;
static void *g_mapped_addr = NULL;  /* For debugging only */

/* EGL state */
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;

/* OpenGL state */
static GLuint g_shader_program = 0;
static GLuint g_framebuffer = 0;
static GLuint g_render_texture = 0;
static EGLImageKHR g_egl_image = EGL_NO_IMAGE_KHR;

/* Shader locations */
static GLint g_position_loc = -1;
static GLint g_bar_position_loc = -1;
static GLint g_bar_width_loc = -1;
static GLint g_frame_number_loc = -1;
static GLint g_resolution_loc = -1;

/* EGL function pointers */
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

/* Animation state */
static int g_frame_number = 0;
static float g_bar_position = 0.0f;

/* Performance monitoring */
static bool g_enable_perf_monitoring = false;
static uint64_t g_gpu_render_time_total = 0;
static uint64_t g_gpu_render_time_min = UINT64_MAX;
static uint64_t g_gpu_render_time_max = 0;
static uint64_t g_h2c_dma_time_total = 0;
static uint64_t g_h2c_dma_time_min = UINT64_MAX;
static uint64_t g_h2c_dma_time_max = 0;
static uint64_t g_frame_count = 0;

/* ============================================================================
 * Timing Utilities
 * ============================================================================ */

static inline uint64_t timespec_to_us(const struct timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000000ULL + (uint64_t)ts->tv_nsec / 1000ULL;
}

static inline uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec_to_us(&ts);
}

/* ============================================================================
 * Signal Handler
 * ============================================================================ */

static void signal_handler(int sig __attribute__((unused))) {
    printf("\nStopping...\n");
    g_running = false;
}

/* ============================================================================
 * Shader Sources
 * ============================================================================ */

/* Vertex shader - simple passthrough */
static const char *vertex_shader_src =
    "attribute vec2 a_position;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = (a_position + 1.0) * 0.5;\n"
    "}\n";

/* Fragment shader - moving bar pattern */
static const char *fragment_shader_src =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform float u_bar_position;\n"
    "uniform float u_bar_width;\n"
    "uniform int u_frame_number;\n"
    "uniform vec2 u_resolution;\n"
    "\n"
    "void main() {\n"
    "    vec2 pixel = v_texcoord * u_resolution;\n"
    "    float x = pixel.x;\n"
    "    \n"
    "    // Moving white vertical bar\n"
    "    if (x >= u_bar_position && x < u_bar_position + u_bar_width) {\n"
    "        gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
    "    } else {\n"
    "        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    }\n"
    "    \n"
    "    // Frame counter indicator (colored square in top-left corner)\n"
    "    if (pixel.x < 32.0 && pixel.y < 32.0) {\n"
    "        float r = mod(float(u_frame_number) * 17.0, 256.0) / 255.0;\n"
    "        float g = mod(float(u_frame_number) * 31.0, 256.0) / 255.0;\n"
    "        float b = mod(float(u_frame_number) * 47.0, 256.0) / 255.0;\n"
    "        gl_FragColor = vec4(r, g, b, 1.0);\n"
    "    }\n"
    "}\n";

/* Fullscreen quad vertices */
static const GLfloat quad_vertices[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
    -1.0f,  1.0f,
     1.0f,  1.0f,
};

/* ============================================================================
 * OpenGL Shader Utilities
 * ============================================================================ */

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info_log = malloc(info_len);
            glGetShaderInfoLog(shader, info_len, NULL, info_log);
            fprintf(stderr, "Shader compile error: %s\n", info_log);
            free(info_log);
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(const char *vertex_src, const char *fragment_src) {
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_src);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
    
    if (!vertex_shader || !fragment_shader) {
        return 0;
    }
    
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info_log = malloc(info_len);
            glGetProgramInfoLog(program, info_len, NULL, info_log);
            fprintf(stderr, "Program link error: %s\n", info_log);
            free(info_log);
        }
        glDeleteProgram(program);
        return 0;
    }
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return program;
}

/* ============================================================================
 * EGL/OpenGL Setup
 * ============================================================================ */

static int init_egl(void) {
    /* Open DRM device for surfaceless context */
    const char *drm_devices[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        "/dev/dri/card0",
        NULL
    };
    
    for (int i = 0; drm_devices[i] != NULL; i++) {
        g_drm_fd = open(drm_devices[i], O_RDWR);
        if (g_drm_fd >= 0) {
            printf("Opened DRM device: %s\n", drm_devices[i]);
            break;
        }
    }
    
    if (g_drm_fd < 0) {
        fprintf(stderr, "Failed to open any DRM device\n");
        return -1;
    }
    
    /* Create GBM device (optional, not all platforms need it) */
    g_gbm_device = gbm_create_device(g_drm_fd);
    if (g_gbm_device) {
        printf("GBM device created\n");
        
        /* Try GBM-based EGL display first (x86/Mesa) */
        g_egl_display = eglGetDisplay((EGLNativeDisplayType)g_gbm_device);
        if (g_egl_display != EGL_NO_DISPLAY) {
            printf("Using GBM-based EGL display\n");
        }
    }
    
    /* Fall back to default display (ARM Mali, etc.) */
    if (g_egl_display == EGL_NO_DISPLAY) {
        printf("GBM-based display not available, trying EGL_DEFAULT_DISPLAY\n");
        g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_egl_display == EGL_NO_DISPLAY) {
            fprintf(stderr, "Failed to get EGL display\n");
            return -1;
        }
        printf("Using EGL_DEFAULT_DISPLAY\n");
    }
    
    /* Initialize EGL */
    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return -1;
    }
    printf("EGL initialized: version %d.%d\n", major, minor);
    
    /* Check for required extensions */
    const char *egl_extensions = eglQueryString(g_egl_display, EGL_EXTENSIONS);
    printf("EGL extensions: %s\n", egl_extensions);
    
    if (!strstr(egl_extensions, "EGL_KHR_image_base") ||
        !strstr(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr, "Required EGL extensions not available\n");
        return -1;
    }
    
    /* Get EGL function pointers */
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    
    if (!eglCreateImageKHR || !eglDestroyImageKHR || !glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "Failed to get EGL function pointers\n");
        return -1;
    }
    
    /* Choose EGL config */
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(g_egl_display, config_attribs, &config, 1, &num_configs) ||
        num_configs == 0) {
        fprintf(stderr, "Failed to choose EGL config\n");
        return -1;
    }
    
    /* Bind OpenGL ES API */
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "Failed to bind OpenGL ES API\n");
        return -1;
    }
    
    /* Create EGL context */
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    
    g_egl_context = eglCreateContext(g_egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (g_egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return -1;
    }
    printf("EGL context created\n");
    
    /* Create pbuffer surface for offscreen rendering */
    EGLint pbuffer_attribs[] = {
        EGL_WIDTH, WIDTH,
        EGL_HEIGHT, HEIGHT,
        EGL_NONE
    };
    
    g_egl_surface = eglCreatePbufferSurface(g_egl_display, config, pbuffer_attribs);
    if (g_egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create pbuffer surface\n");
        return -1;
    }
    
    /* Make context current */
    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        return -1;
    }
    printf("EGL context made current\n");
    
    return 0;
}

static int init_gl_resources(void) {
    /* Create shader program */
    g_shader_program = create_program(vertex_shader_src, fragment_shader_src);
    if (!g_shader_program) {
        fprintf(stderr, "Failed to create shader program\n");
        return -1;
    }
    
    /* Get shader locations */
    g_position_loc = glGetAttribLocation(g_shader_program, "a_position");
    g_bar_position_loc = glGetUniformLocation(g_shader_program, "u_bar_position");
    g_bar_width_loc = glGetUniformLocation(g_shader_program, "u_bar_width");
    g_frame_number_loc = glGetUniformLocation(g_shader_program, "u_frame_number");
    g_resolution_loc = glGetUniformLocation(g_shader_program, "u_resolution");
    
    printf("Shader program created\n");
    printf("  a_position: %d\n", g_position_loc);
    printf("  u_bar_position: %d\n", g_bar_position_loc);
    printf("  u_bar_width: %d\n", g_bar_width_loc);
    printf("  u_frame_number: %d\n", g_frame_number_loc);
    printf("  u_resolution: %d\n", g_resolution_loc);
    
    return 0;
}

/* ============================================================================
 * DMA-BUF Setup
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
        perror("IWFG_IOCTL_ADD_BUF failed");
        return -1;
    }
    
    g_dmabuf_fd = user_buf.dmabuf_fd;
    g_buf_index = user_buf.buf_index;
    
    printf("Buffer registered: index=%u, DMA-BUF fd=%d\n", g_buf_index, g_dmabuf_fd);
    
    /* Optional: mmap for debugging/verification */
    g_mapped_addr = mmap(NULL, FRAME_SIZE, 
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED, 
                         g_dmabuf_fd, 0);
    if (g_mapped_addr == MAP_FAILED) {
        fprintf(stderr, "Warning: Failed to mmap DMA-BUF (not critical for GPU rendering)\n");
        g_mapped_addr = NULL;
    } else {
        printf("DMA-BUF mmap'd at %p (for debugging)\n", g_mapped_addr);
    }
    
    return 0;
}

static int import_dmabuf_to_gl(void) {
    /* Import DMA-BUF as EGLImage */
    EGLint attribs[] = {
        EGL_WIDTH, WIDTH,
        EGL_HEIGHT, HEIGHT,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, g_dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, WIDTH * BYTES_PER_PIXEL,
        EGL_NONE
    };
    
    g_egl_image = eglCreateImageKHR(g_egl_display,
                                     EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT,
                                     (EGLClientBuffer)NULL,
                                     attribs);
    if (g_egl_image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "Failed to create EGLImage from DMA-BUF\n");
        return -1;
    }
    printf("EGLImage created from DMA-BUF\n");
    
    /* Create OpenGL texture from EGLImage */
    glGenTextures(1, &g_render_texture);
    glBindTexture(GL_TEXTURE_2D, g_render_texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, g_egl_image);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    printf("OpenGL texture created from EGLImage (texture ID: %u)\n", g_render_texture);
    
    /* Create framebuffer and attach texture */
    glGenFramebuffers(1, &g_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 
                           GL_TEXTURE_2D, g_render_texture, 0);
    
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Framebuffer not complete: 0x%x\n", status);
        return -1;
    }
    printf("Framebuffer created and complete\n");
    
    /* Unbind for now */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    return 0;
}

/* ============================================================================
 * Rendering
 * ============================================================================ */

static uint64_t render_moving_bar(void) {
    uint64_t start_time = get_time_us();
    
    /* Bind framebuffer (renders to DMA-BUF texture) */
    glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);
    
    /* Set viewport */
    glViewport(0, 0, WIDTH, HEIGHT);
    
    /* Clear to black */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    /* Use shader program */
    glUseProgram(g_shader_program);
    
    /* Update bar position (wrap around) */
    g_bar_position += BAR_SPEED;
    if (g_bar_position >= WIDTH) {
        g_bar_position = -BAR_WIDTH;
    }
    
    /* Set uniforms */
    glUniform1f(g_bar_position_loc, g_bar_position);
    glUniform1f(g_bar_width_loc, (float)BAR_WIDTH);
    glUniform1i(g_frame_number_loc, g_frame_number);
    glUniform2f(g_resolution_loc, (float)WIDTH, (float)HEIGHT);
    
    /* Draw fullscreen quad */
    glEnableVertexAttribArray(g_position_loc);
    glVertexAttribPointer(g_position_loc, 2, GL_FLOAT, GL_FALSE, 0, quad_vertices);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(g_position_loc);
    
    /* Flush to ensure GPU finishes rendering */
    glFlush();
    glFinish();
    
    /* Unbind framebuffer */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    g_frame_number++;
    
    uint64_t elapsed = get_time_us() - start_time;
    
    if (g_enable_perf_monitoring) {
        g_gpu_render_time_total += elapsed;
        if (elapsed < g_gpu_render_time_min) g_gpu_render_time_min = elapsed;
        if (elapsed > g_gpu_render_time_max) g_gpu_render_time_max = elapsed;
    }
    
    return elapsed;
}

/* ============================================================================
 * H2C Transfer
 * ============================================================================ */

static int do_h2c_transfer(uint64_t *elapsed_us) {
    uint64_t start_time = get_time_us();
    
    struct iwfg_user_dma_req dma_req = {
        .buf = NULL,
        .size = FRAME_SIZE,
        .direction = IWFG_DIR_H2C_BUF,
        .offset = 0,
        .buf_index = g_buf_index,
    };
    
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req) < 0) {
        perror("H2C DMA transfer failed");
        return -1;
    }
    
    uint64_t elapsed = get_time_us() - start_time;
    
    if (g_enable_perf_monitoring) {
        g_h2c_dma_time_total += elapsed;
        if (elapsed < g_h2c_dma_time_min) g_h2c_dma_time_min = elapsed;
        if (elapsed > g_h2c_dma_time_max) g_h2c_dma_time_max = elapsed;
    }
    
    if (elapsed_us) *elapsed_us = elapsed;
    
    return 0;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

static void cleanup(void) {
    printf("\nCleaning up...\n");
    
    /* Stop streaming */
    if (g_iwfg_fd >= 0) {
        ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_STOP);
    }
    
    /* Clean up OpenGL resources */
    if (g_framebuffer) {
        glDeleteFramebuffers(1, &g_framebuffer);
    }
    if (g_render_texture) {
        glDeleteTextures(1, &g_render_texture);
    }
    if (g_shader_program) {
        glDeleteProgram(g_shader_program);
    }
    
    /* Clean up EGL resources */
    if (g_egl_image != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(g_egl_display, g_egl_image);
    }
    
    if (g_egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(g_egl_display, g_egl_context);
        }
        if (g_egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(g_egl_display, g_egl_surface);
        }
        eglTerminate(g_egl_display);
    }
    
    /* Clean up GBM */
    if (g_gbm_device) {
        gbm_device_destroy(g_gbm_device);
    }
    
    /* Close DRM device */
    if (g_drm_fd >= 0) {
        close(g_drm_fd);
    }
    
    /* Clean up DMA-BUF */
    if (g_mapped_addr && g_mapped_addr != MAP_FAILED) {
        munmap(g_mapped_addr, FRAME_SIZE);
    }
    if (g_dmabuf_fd >= 0) {
        close(g_dmabuf_fd);
    }
    
    /* Close IWFG device */
    if (g_iwfg_fd >= 0) {
        close(g_iwfg_fd);
    }
    
    printf("Cleanup complete\n");
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -d <delay>     Delay between frames in ms (default: 0 = max speed)\n");
    printf("  -p             Enable performance monitoring\n");
    printf("  -h             Show this help message\n");
    printf("\n");
    printf("Generates GPU-rendered moving bar pattern and sends via H2C.\n");
    printf("Runs continuously until Ctrl+C is pressed.\n");
    printf("\n");
    printf("Purpose: Debug vsync in H2C path using GPU-generated animation.\n");
}

int main(int argc, char *argv[]) {
    int delay_ms = 0;  /* 0 = no delay, maximum speed */
    int opt;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "d:ph")) != -1) {
        switch (opt) {
        case 'd':
            delay_ms = atoi(optarg);
            break;
        case 'p':
            g_enable_perf_monitoring = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    
    printf("IWFG DMA-BUF GPU H2C Test Pattern Generator\n");
    printf("============================================\n");
    printf("Resolution: %dx%d\n", WIDTH, HEIGHT);
    printf("Frame size: %d bytes (%.2f MB)\n", FRAME_SIZE, FRAME_SIZE / (1024.0 * 1024.0));
    printf("Pattern: Moving bar (GPU-generated)\n");
    printf("Bar width: %d pixels\n", BAR_WIDTH);
    printf("Bar speed: %d pixels/frame\n", BAR_SPEED);
    printf("Delay: %d ms\n", delay_ms);
    printf("Performance monitoring: %s\n", g_enable_perf_monitoring ? "enabled" : "disabled");
    printf("Press Ctrl+C to stop.\n");
    printf("\n");
    
    /* Open IWFG device */
    g_iwfg_fd = open("/dev/iwfg", O_RDWR);
    if (g_iwfg_fd < 0) {
        perror("Failed to open IWFG device");
        return -1;
    }
    printf("IWFG device opened\n");
    
    /* Initialize EGL */
    if (init_egl() < 0) {
        cleanup();
        return -1;
    }
    
    /* Initialize OpenGL resources */
    if (init_gl_resources() < 0) {
        cleanup();
        return -1;
    }
    
    /* Setup DMA-BUF buffer */
    if (setup_dmabuf_buffer() < 0) {
        cleanup();
        return -1;
    }
    
    /* Import DMA-BUF to OpenGL */
    if (import_dmabuf_to_gl() < 0) {
        cleanup();
        return -1;
    }
    
    /* Start streaming */
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_START) < 0) {
        perror("IWFG_IOCTL_STREAM_START failed");
        cleanup();
        return -1;
    }
    printf("Streaming started\n\n");
    
    /* Main loop - render and send frames */
    printf("Rendering and sending frames via H2C...\n");
    
    int frames_sent = 0;
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    while (g_running) {
        /* Render moving bar pattern using GPU */
        render_moving_bar();
        
        /* Send frame via H2C */
        uint64_t h2c_time = 0;
        if (do_h2c_transfer(&h2c_time) < 0) {
            fprintf(stderr, "H2C transfer failed at frame %d\n", frames_sent);
            break;
        }
        
        frames_sent++;
        g_frame_count++;
        
        /* Print statistics every 60 frames */
        if (frames_sent % 60 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            double elapsed = (current_time.tv_sec - start_time.tv_sec) +
                           (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
            double fps = frames_sent / elapsed;
            
            if (g_enable_perf_monitoring) {
                double gpu_avg = g_gpu_render_time_total / (double)g_frame_count;
                double h2c_avg = g_h2c_dma_time_total / (double)g_frame_count;
                double total_avg = gpu_avg + h2c_avg;
                
                printf("[%d frames @ %.2f fps | bar: %.0f]\n", frames_sent, fps, g_bar_position);
                printf("  GPU render: avg=%.2fms min=%.2fms max=%.2fms\n",
                       gpu_avg / 1000.0,
                       g_gpu_render_time_min / 1000.0,
                       g_gpu_render_time_max / 1000.0);
                printf("  H2C DMA:    avg=%.2fms min=%.2fms max=%.2fms\n",
                       h2c_avg / 1000.0,
                       g_h2c_dma_time_min / 1000.0,
                       g_h2c_dma_time_max / 1000.0);
                printf("  Total:      avg=%.2fms (%.1f fps max)\n",
                       total_avg / 1000.0, 1000000.0 / total_avg);
            } else {
                printf("Sent %d frames (%.2f fps, bar pos: %.0f)\n", 
                       frames_sent, fps, g_bar_position);
            }
        }
        
        /* Frame rate control */
        if (delay_ms > 0) {
            usleep(delay_ms * 1000);
        }
        /* No delay for maximum speed when delay_ms == 0 */
    }
    
    printf("\nTotal frames sent: %d\n", frames_sent);
    
    /* Print final performance summary */
    if (g_enable_perf_monitoring && g_frame_count > 0) {
        double gpu_avg = g_gpu_render_time_total / (double)g_frame_count;
        double h2c_avg = g_h2c_dma_time_total / (double)g_frame_count;
        double total_avg = gpu_avg + h2c_avg;
        
        printf("\n=== Performance Summary ===\n");
        printf("GPU Rendering:\n");
        printf("  Average: %.2f ms (%.1f fps)\n", gpu_avg / 1000.0, 1000000.0 / gpu_avg);
        printf("  Min:     %.2f ms\n", g_gpu_render_time_min / 1000.0);
        printf("  Max:     %.2f ms\n", g_gpu_render_time_max / 1000.0);
        printf("\nH2C DMA Transfer:\n");
        printf("  Average: %.2f ms (%.1f fps)\n", h2c_avg / 1000.0, 1000000.0 / h2c_avg);
        printf("  Min:     %.2f ms\n", g_h2c_dma_time_min / 1000.0);
        printf("  Max:     %.2f ms\n", g_h2c_dma_time_max / 1000.0);
        printf("\nTotal Pipeline:\n");
        printf("  Average: %.2f ms (%.1f fps max)\n", total_avg / 1000.0, 1000000.0 / total_avg);
        printf("  GPU:     %.1f%% of total\n", (gpu_avg / total_avg) * 100.0);
        printf("  H2C DMA: %.1f%% of total\n", (h2c_avg / total_avg) * 100.0);
    }
    
    /* Cleanup */
    cleanup();
    
    printf("Done.\n");
    return 0;
}
