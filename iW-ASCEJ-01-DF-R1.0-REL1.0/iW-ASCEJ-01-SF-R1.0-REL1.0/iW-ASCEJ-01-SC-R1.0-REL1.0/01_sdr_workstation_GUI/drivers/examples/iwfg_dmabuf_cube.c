/*
 * IWFG DMA-BUF Zero-Copy Cube Overlay Example
 * 
 * This example demonstrates:
 * 1. Driver-allocated DMA buffers exported via DMA-BUF
 * 2. Zero-copy GPU import using EGL_EXT_image_dma_buf_import
 * 3. C2H capture -> H2C (GPU cube overlay + transmission) pipeline
 * 4. Multi-buffering with FIFO queue synchronization
 * 
 * Architecture (2-thread simplified design):
 * - Driver allocates DMA buffers using IWFG_BUF_TYPE_DMA_ALLOC
 * - Buffers are exported as DMA-BUF file descriptors
 * - GPU imports DMA-BUF as EGLImage (zero-copy)
 * - EGLImage is bound to OpenGL texture
 * - C2H thread captures frames into buffers
 * - H2C thread renders cube overlay using GPU, then transmits
 * 
 * Buffer Flow:
 *   empty_queue -> C2H (capture) -> filled_queue -> H2C (GPU render + transmit) -> empty_queue
 * 
 * Requirements (x86 PC):
 * - EGL is required for DMA-BUF import (eglCreateImageKHR)
 * - GBM is optional (only needed for headless/surfaceless contexts)
 * - Mesa/NVIDIA drivers with EGL_EXT_image_dma_buf_import support
 * 
 * Build: Uses EGL + OpenGL ES 2.0 for portability
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
#include <math.h>
#include <stdatomic.h>  /* For memory barriers */

// DRM/GBM for device access and surfaceless EGL
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <gbm.h>

// DMA-BUF sync support
#include <linux/dma-buf.h>

// EGL and OpenGL ES headers
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "iwfg_user.h"

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

#define NUM_BUFFERS 5       /* Triple buffering for optimal pipeline */
#define WIDTH 1280
#define HEIGHT 1024
#define BYTES_PER_PIXEL 4   /* XRGB8888 format */
#define FRAME_SIZE (WIDTH * HEIGHT * BYTES_PER_PIXEL)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================================
 * DMA-BUF Buffer Descriptor
 * ============================================================================ */

typedef struct {
    int dmabuf_fd;              /* DMA-BUF file descriptor from driver */
    uint32_t buf_index;         /* Buffer index in driver's list */
    void *mapped_addr;          /* mmap'd address for CPU access if needed */
    EGLImageKHR egl_image;      /* EGL image handle for GPU access */
    GLuint gl_texture;          /* OpenGL texture ID */
} dmabuf_desc_t;

/* ============================================================================
 * Global State
 * ============================================================================ */

static int g_iwfg_fd = -1;
static int g_drm_fd = -1;
static struct gbm_device *g_gbm_device = NULL;
static dmabuf_desc_t g_buffers[NUM_BUFFERS];
static volatile bool g_running = true;

/* EGL state */
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;

/* OpenGL state */
static GLuint g_cube_program = 0;
static GLuint g_framebuffer = 0;
static GLuint g_render_texture = 0;
static GLuint g_depth_renderbuffer = 0;
static float g_cube_rotation = 0.0f;

/* Shader locations */
static GLint g_position_loc = -1;
static GLint g_color_loc = -1;
static GLint g_mvp_loc = -1;

/* EGL function pointers */
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

/* EGL sync function pointers (for proper GPU synchronization) */
static PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR = NULL;
static PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR = NULL;
static PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR = NULL;
static bool g_egl_sync_supported = false;

/* ============================================================================
 * Circular FIFO Buffer Synchronization
 * 
 * Uses simple 2-queue FIFO for C2H -> H2C pipeline:
 * - empty_queue: Buffers available for C2H capture
 * - filled_queue: Buffers filled by C2H, ready for GPU processing + H2C transmission
 * 
 * Flow: empty_queue -> C2H -> filled_queue -> H2C (GPU render + transmit) -> empty_queue
 * ============================================================================ */

/* Buffer state tracking */
typedef enum {
    BUF_STATE_EMPTY,
    BUF_STATE_IN_USE_C2H,    /* Being written by C2H */
    BUF_STATE_FILLED,        /* Filled, waiting for H2C */
    BUF_STATE_IN_USE_H2C     /* Being processed/read by H2C */
} buffer_state_t;

static buffer_state_t g_buf_states[NUM_BUFFERS];

/* Circular FIFO queue structure */
typedef struct {
    int data[NUM_BUFFERS];   /* Buffer indices */
    int head;                /* Read position (dequeue) */
    int tail;                /* Write position (enqueue) */
    int count;               /* Number of items in queue */
} fifo_queue_t;

/* Two FIFO queues for pipeline stages */
static fifo_queue_t g_empty_queue;      /* Buffers available for C2H */
static fifo_queue_t g_filled_queue;     /* Buffers ready for H2C (GPU + transmit) */

/* Thread handles */
static pthread_t g_c2h_thread;
static pthread_t g_h2c_thread;

/* Synchronization primitives */
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t g_empty_slots;     /* Count of empty buffers available for C2H */
static sem_t g_filled_slots;    /* Count of filled buffers ready for H2C */

/* Statistics */
static uint32_t g_c2h_frames = 0;
static uint32_t g_h2c_frames = 0;
static uint32_t g_c2h_errors = 0;
static uint32_t g_h2c_errors = 0;
static uint32_t g_c2h_eagain_count = 0;
static uint32_t g_state_inconsistencies = 0;

/* Performance monitoring */
static bool g_enable_performance_monitoring = false;
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Sync mode: 0=none (fastest), 1=lightweight (GPU only), 2=full (all paths) */
static int g_sync_mode = 1;  /* Default: lightweight - GPU sync only */

/* Vsync monitoring */
static struct timespec g_last_c2h_time = {0, 0};
static bool g_vsync_warning_printed = false;

/* Timing statistics (in microseconds) */
static uint64_t g_c2h_time_total = 0;
static uint64_t g_c2h_time_min = 0;
static uint64_t g_c2h_time_max = 0;
static uint64_t g_c2h_time_count = 0;
static uint64_t g_h2c_time_total = 0;  /* Includes GPU render + DMA */
static uint64_t g_h2c_time_min = 0;
static uint64_t g_h2c_time_max = 0;
static uint64_t g_h2c_time_count = 0;
static uint64_t g_pipeline_time_total = 0;
static uint64_t g_pipeline_time_min = 0;
static uint64_t g_pipeline_time_max = 0;
static uint64_t g_pipeline_time_count = 0;

/* Per-buffer timestamps for pipeline timing */
static struct timespec g_buf_capture_time[NUM_BUFFERS];

/* ============================================================================
 * Timing Helpers
 * ============================================================================ */

static inline uint64_t timespec_diff_us(const struct timespec *start, const struct timespec *end) {
    /* Handle nanosecond wraparound properly */
    long sec_diff = end->tv_sec - start->tv_sec;
    long nsec_diff = end->tv_nsec - start->tv_nsec;
    
    if (nsec_diff < 0) {
        sec_diff--;
        nsec_diff += 1000000000L;
    }
    
    /* Sanity check - if negative time (clock issue), return 0 */
    if (sec_diff < 0) {
        return 0;
    }
    
    return (uint64_t)sec_diff * 1000000ULL + (uint64_t)nsec_diff / 1000ULL;
}

static inline void update_timing_stats(uint64_t *total, uint64_t *min, uint64_t *max, 
                                        uint64_t *count, uint64_t value) {
    /* Skip invalid measurements */
    if (value == 0 || value > 1000000000ULL) {  /* Skip if > 1000 seconds */
        return;
    }
    *total += value;
    (*count)++;
    if (*min == 0 || value < *min) *min = value;
    if (value > *max) *max = value;
}

/* ============================================================================
 * FIFO Queue Operations (all require g_queue_mutex to be held)
 * ============================================================================ */

static void fifo_init(fifo_queue_t *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    memset(q->data, -1, sizeof(q->data));
}

static bool fifo_is_empty(const fifo_queue_t *q) {
    return q->count == 0;
}

static bool fifo_is_full(const fifo_queue_t *q) {
    return q->count == NUM_BUFFERS;
}

/* Enqueue a buffer index. Returns true on success, false if full */
static bool fifo_enqueue(fifo_queue_t *q, int buf_idx) {
    if (fifo_is_full(q)) {
        return false;
    }
    q->data[q->tail] = buf_idx;
    q->tail = (q->tail + 1) % NUM_BUFFERS;
    q->count++;
    return true;
}

/* Dequeue a buffer index. Returns the index, or -1 if empty */
static int fifo_dequeue(fifo_queue_t *q) {
    if (fifo_is_empty(q)) {
        return -1;
    }
    int buf_idx = q->data[q->head];
    q->data[q->head] = -1;  /* Clear for debugging */
    q->head = (q->head + 1) % NUM_BUFFERS;
    q->count--;
    return buf_idx;
}

/* Peek at the front of the queue without removing */
static int fifo_peek(const fifo_queue_t *q) {
    if (fifo_is_empty(q)) {
        return -1;
    }
    return q->data[q->head];
}

/* Debug: print queue contents */
static void fifo_debug_print(const char *name, const fifo_queue_t *q) {
    printf("%s queue (count=%d, head=%d, tail=%d): [", name, q->count, q->head, q->tail);
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % NUM_BUFFERS;
        printf("%d", q->data[idx]);
        if (i < q->count - 1) printf(", ");
    }
    printf("]\n");
}

/* ============================================================================
 * DMA-BUF Synchronization Helpers
 * ============================================================================ */

/**
 * Begin CPU/GPU access to a DMA-BUF
 * Call this BEFORE reading from or writing to the buffer
 */
static int dmabuf_sync_start(int dmabuf_fd, bool write) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ)
    };
    
    int ret = ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret < 0 && errno != EINVAL) {  /* EINVAL means sync not supported, which is OK */
        fprintf(stderr, "DMA-BUF sync start failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * End CPU/GPU access to a DMA-BUF
 * Call this AFTER reading from or writing to the buffer
 */
static int dmabuf_sync_end(int dmabuf_fd, bool write) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ)
    };
    
    int ret = ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret < 0 && errno != EINVAL) {
        fprintf(stderr, "DMA-BUF sync end failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * Combined read-write sync for GPU operations that both read and modify
 */
static int dmabuf_sync_start_rw(int dmabuf_fd) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW
    };
    
    int ret = ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret < 0 && errno != EINVAL) {
        fprintf(stderr, "DMA-BUF sync start RW failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int dmabuf_sync_end_rw(int dmabuf_fd) {
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW
    };
    
    int ret = ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret < 0 && errno != EINVAL) {
        fprintf(stderr, "DMA-BUF sync end RW failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* ============================================================================
 * Shader Sources
 * ============================================================================ */

/* Vertex shader for cube rendering */
static const char *cube_vertex_shader_src =
    "attribute vec3 a_position;\n"
    "attribute vec3 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

/* Fragment shader for cube rendering */
static const char *cube_fragment_shader_src =
    "precision mediump float;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(v_color, 1.0);\n"
    "}\n";

/* Cube vertex data - 6 faces with different colors */
static const GLfloat cube_vertices[] = {
    /* Front face (Red) */
    -0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
    -0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
    -0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 0.0f,

    /* Back face (Green) */
    -0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
    -0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
     0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
    -0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
     0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
     0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 0.0f,

    /* Top face (Blue) */
    -0.5f,  0.5f, -0.5f,   0.0f, 0.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
    -0.5f,  0.5f, -0.5f,   0.0f, 0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
     0.5f,  0.5f, -0.5f,   0.0f, 0.0f, 1.0f,

    /* Bottom face (Yellow) */
    -0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.0f,
     0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.0f,
    -0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.0f,
    -0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.0f,

    /* Right face (Magenta) */
     0.5f, -0.5f, -0.5f,   1.0f, 0.0f, 1.0f,
     0.5f,  0.5f, -0.5f,   1.0f, 0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 1.0f,
     0.5f, -0.5f, -0.5f,   1.0f, 0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 1.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 1.0f,

    /* Left face (Cyan) */
    -0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f, -0.5f,  0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 1.0f,
};

/* ============================================================================
 * Matrix Math Functions
 * ============================================================================ */

static void mat4_multiply(float *result, const float *a, const float *b) {
    float temp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            temp[col * 4 + row] = 0.0f;
            for (int k = 0; k < 4; k++) {
                temp[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
            }
        }
    }
    memcpy(result, temp, sizeof(temp));
}

static void mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_rotate_y(float *m, float angle) {
    mat4_identity(m);
    float c = cosf(angle);
    float s = sinf(angle);
    m[0] = c;
    m[2] = s;
    m[8] = -s;
    m[10] = c;
}

static void mat4_rotate_x(float *m, float angle) {
    mat4_identity(m);
    float c = cosf(angle);
    float s = sinf(angle);
    m[5] = c;
    m[6] = s;
    m[9] = -s;
    m[10] = c;
}

static void mat4_scale(float *m, float sx, float sy, float sz) {
    mat4_identity(m);
    m[0] = sx;
    m[5] = sy;
    m[10] = sz;
}

static void mat4_perspective(float *m, float fovy, float aspect, float near, float far) {
    memset(m, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fovy / 2.0f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far + near) / (near - far);
    m[11] = -1.0f;
    m[14] = (2.0f * far * near) / (near - far);
}

static void mat4_translate(float *m, float x, float y, float z) {
    mat4_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

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
 * EGL Initialization with GBM (for surfaceless context)
 * ============================================================================ */

static int init_egl_gbm(void) {
    const char *drm_device = "/dev/dri/renderD128";
    
    /* Open DRM render node */
    g_drm_fd = open(drm_device, O_RDWR);
    if (g_drm_fd < 0) {
        /* Try card0 as fallback */
        drm_device = "/dev/dri/card0";
        g_drm_fd = open(drm_device, O_RDWR);
        if (g_drm_fd < 0) {
            perror("Failed to open DRM device");
            return -1;
        }
    }
    printf("Opened DRM device: %s\n", drm_device);
    
    /* Create GBM device */
    g_gbm_device = gbm_create_device(g_drm_fd);
    if (!g_gbm_device) {
        fprintf(stderr, "Failed to create GBM device\n");
        close(g_drm_fd);
        return -1;
    }
    printf("Created GBM device\n");
    
    /* Get EGL display from GBM device */
    g_egl_display = eglGetDisplay((EGLNativeDisplayType)g_gbm_device);
    if (g_egl_display == EGL_NO_DISPLAY) {
        /* Try platform-specific API */
        PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT = 
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (eglGetPlatformDisplayEXT) {
            g_egl_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, g_gbm_device, NULL);
        }
    }
    
    if (g_egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        gbm_device_destroy(g_gbm_device);
        close(g_drm_fd);
        return -1;
    }
    
    /* Initialize EGL */
    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        gbm_device_destroy(g_gbm_device);
        close(g_drm_fd);
        return -1;
    }
    printf("EGL initialized: version %d.%d\n", major, minor);
    
    /* Check for required extensions */
    const char *egl_extensions = eglQueryString(g_egl_display, EGL_EXTENSIONS);
    printf("EGL Extensions: %s\n", egl_extensions);
    
    if (!strstr(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr, "ERROR: EGL_EXT_image_dma_buf_import not supported!\n");
        fprintf(stderr, "This extension is required for DMA-BUF import.\n");
        eglTerminate(g_egl_display);
        gbm_device_destroy(g_gbm_device);
        close(g_drm_fd);
        return -1;
    }
    printf("DMA-BUF import extension available\n");
    
    /* Get EGL function pointers */
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    
    if (!eglCreateImageKHR || !eglDestroyImageKHR || !glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "Failed to get EGL/GL extension functions\n");
        eglTerminate(g_egl_display);
        gbm_device_destroy(g_gbm_device);
        close(g_drm_fd);
        return -1;
    }
    
    /* Get EGL sync functions (optional but recommended for proper synchronization) */
    if (strstr(egl_extensions, "EGL_KHR_fence_sync")) {
        eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        eglClientWaitSyncKHR = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
        
        if (eglCreateSyncKHR && eglDestroySyncKHR && eglClientWaitSyncKHR) {
            g_egl_sync_supported = true;
            printf("EGL_KHR_fence_sync available - using fence sync for GPU synchronization\n");
        }
    }
    if (!g_egl_sync_supported) {
        printf("EGL_KHR_fence_sync not available - falling back to glFinish()\n");
    }
    
    /* Choose EGL config for offscreen rendering */
    /* Use surfaceless context (EGL_KHR_surfaceless_context) which is better supported with GBM */
    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(g_egl_display, config_attribs, &config, 1, &num_configs) || num_configs == 0) {
        fprintf(stderr, "Failed to choose EGL config (num_configs=%d, error=0x%x)\n", 
                num_configs, eglGetError());
        eglTerminate(g_egl_display);
        gbm_device_destroy(g_gbm_device);
        close(g_drm_fd);
        return -1;
    }
    printf("EGL config selected (num_configs=%d)\n", num_configs);
    
    /* Bind OpenGL ES API */
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "Failed to bind OpenGL ES API\n");
        eglTerminate(g_egl_display);
        gbm_device_destroy(g_gbm_device);
        close(g_drm_fd);
        return -1;
    }
    
    /* Create OpenGL ES 2.0 context */
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    
    g_egl_context = eglCreateContext(g_egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (g_egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context (error=0x%x)\n", eglGetError());
        eglTerminate(g_egl_display);
        gbm_device_destroy(g_gbm_device);
        close(g_drm_fd);
        return -1;
    }
    
    /* Use surfaceless context (EGL_KHR_surfaceless_context) - we render to FBO */
    g_egl_surface = EGL_NO_SURFACE;
    
    /* Make context current with no surface (surfaceless) */
    if (!eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, g_egl_context)) {
        fprintf(stderr, "Failed to make EGL context current (error=0x%x)\n", eglGetError());
        eglDestroyContext(g_egl_display, g_egl_context);
        eglTerminate(g_egl_display);
        gbm_device_destroy(g_gbm_device);
        close(g_drm_fd);
        return -1;
    }
    
    printf("OpenGL ES version: %s\n", glGetString(GL_VERSION));
    printf("OpenGL ES renderer: %s\n", glGetString(GL_RENDERER));
    
    return 0;
}

/* ============================================================================
 * Initialize OpenGL Resources
 * ============================================================================ */

static int init_opengl(void) {
    /* Create cube shader program */
    g_cube_program = create_program(cube_vertex_shader_src, cube_fragment_shader_src);
    if (!g_cube_program) {
        fprintf(stderr, "Failed to create cube shader program\n");
        return -1;
    }
    
    /* Get shader locations */
    g_position_loc = glGetAttribLocation(g_cube_program, "a_position");
    g_color_loc = glGetAttribLocation(g_cube_program, "a_color");
    g_mvp_loc = glGetUniformLocation(g_cube_program, "u_mvp");
    
    /* Create framebuffer for offscreen rendering */
    glGenFramebuffers(1, &g_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);
    
    /* Create render texture */
    glGenTextures(1, &g_render_texture);
    glBindTexture(GL_TEXTURE_2D, g_render_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, WIDTH, HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_render_texture, 0);
    
    /* Create depth renderbuffer */
    glGenRenderbuffers(1, &g_depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth_renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, WIDTH, HEIGHT);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth_renderbuffer);
    
    /* Check framebuffer completeness */
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Framebuffer not complete!\n");
        return -1;
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    printf("OpenGL resources initialized\n");
    return 0;
}

/* ============================================================================
 * DMA-BUF Import to EGL/OpenGL
 * ============================================================================ */

static int import_dmabuf_to_gl(dmabuf_desc_t *buf) {
    /* Create EGLImage from DMA-BUF fd */
    EGLint attribs[] = {
        EGL_WIDTH, WIDTH,
        EGL_HEIGHT, HEIGHT,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, buf->dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, WIDTH * BYTES_PER_PIXEL,
        EGL_NONE
    };
    
    buf->egl_image = eglCreateImageKHR(g_egl_display, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (buf->egl_image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "Failed to create EGLImage from DMA-BUF fd %d: 0x%x\n", 
                buf->dmabuf_fd, eglGetError());
        return -1;
    }
    
    /* Create OpenGL texture and bind EGLImage to it */
    glGenTextures(1, &buf->gl_texture);
    glBindTexture(GL_TEXTURE_2D, buf->gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    /* Bind the EGLImage to the texture - this is the zero-copy magic! */
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buf->egl_image);
    
    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error binding EGLImage: 0x%x\n", gl_error);
        eglDestroyImageKHR(g_egl_display, buf->egl_image);
        buf->egl_image = EGL_NO_IMAGE_KHR;
        return -1;
    }
    
    printf("Buffer %u: DMA-BUF fd %d -> EGLImage %p -> GL texture %u (zero-copy)\n",
           buf->buf_index, buf->dmabuf_fd, buf->egl_image, buf->gl_texture);
    
    return 0;
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
        g_buffers[i].egl_image = EGL_NO_IMAGE_KHR;
        g_buffers[i].gl_texture = 0;
        
        printf("Buffer %d: driver allocated, DMA-BUF fd=%d, index=%u\n",
               i, g_buffers[i].dmabuf_fd, g_buffers[i].buf_index);
        
        /* Import DMA-BUF to GPU for zero-copy rendering */
        if (import_dmabuf_to_gl(&g_buffers[i]) < 0) {
            fprintf(stderr, "Failed to import buffer %d to GPU\n", i);
            return -1;
        }
        
        /* Note: buffer state is initialized when populating the FIFO queue in main() */
    }
    
    printf("All %d buffers set up with zero-copy GPU import\n", NUM_BUFFERS);
    return 0;
}

/* ============================================================================
 * Cube Rendering with DMA-BUF Texture
 * ============================================================================ */

static void render_cube_overlay(int buf_idx) {
    dmabuf_desc_t *buf = &g_buffers[buf_idx];
    
    /* 
     * Sync DMA-BUF before GPU access (lightweight and full modes)
     * This ensures C2H DMA writes are visible to GPU
     */
    if (g_sync_mode >= 1) {
        dmabuf_sync_start_rw(buf->dmabuf_fd);
    }
    
    /* Bind framebuffer for offscreen rendering */
    glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);
    
    /* Attach the DMA-BUF texture as render target */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 
                           GL_TEXTURE_2D, buf->gl_texture, 0);
    
    /* Verify framebuffer is complete */
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Framebuffer not complete for buffer %d!\n", buf_idx);
        return;
    }
    
    glViewport(0, 0, WIDTH, HEIGHT);
    
    /* Don't clear - we want to render on top of the captured frame! */
    /* Only clear depth buffer for proper cube rendering */
    glClear(GL_DEPTH_BUFFER_BIT);
    
    /* Enable depth test for cube */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    
    /* Use cube shader */
    glUseProgram(g_cube_program);
    
    /* Create MVP matrix */
    float projection[16], view[16], model[16], rot_y[16], rot_x[16], scale[16];
    float temp1[16], mv[16], mvp[16];
    
    /* Perspective projection */
    mat4_perspective(projection, 60.0f * M_PI / 180.0f, 
                     (float)WIDTH / (float)HEIGHT, 0.1f, 100.0f);
    
    /* View matrix - camera back */
    mat4_translate(view, 0.0f, 0.0f, -2.5f);
    
    /* Model: scale * rotate_x * rotate_y */
    mat4_scale(scale, 0.3f, 0.3f, 0.3f);
    mat4_rotate_x(rot_x, 30.0f * M_PI / 180.0f);
    mat4_rotate_y(rot_y, g_cube_rotation);
    
    mat4_multiply(temp1, rot_x, scale);
    mat4_multiply(model, rot_y, temp1);
    
    /* MVP = projection * view * model */
    mat4_multiply(mv, view, model);
    mat4_multiply(mvp, projection, mv);
    
    glUniformMatrix4fv(g_mvp_loc, 1, GL_FALSE, mvp);
    
    /* Draw cube */
    glVertexAttribPointer(g_position_loc, 3, GL_FLOAT, GL_FALSE, 
                          6 * sizeof(GLfloat), cube_vertices);
    glEnableVertexAttribArray(g_position_loc);
    
    glVertexAttribPointer(g_color_loc, 3, GL_FLOAT, GL_FALSE,
                          6 * sizeof(GLfloat), cube_vertices + 3);
    glEnableVertexAttribArray(g_color_loc);
    
    glDrawArrays(GL_TRIANGLES, 0, 36);
    
    /* Update rotation for next frame */
    g_cube_rotation += 0.05f;
    if (g_cube_rotation > 2.0f * M_PI) {
        g_cube_rotation -= 2.0f * M_PI;
    }
    
    /* Unbind framebuffer */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    /*
     * CRITICAL: Ensure GPU rendering is complete and sync DMA-BUF
     * Use EGL fence sync if available for more reliable synchronization
     */
    if (g_egl_sync_supported) {
        /* Create a fence sync object - this is inserted into the GPU command stream */
        EGLSyncKHR sync = eglCreateSyncKHR(g_egl_display, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            /* Flush to ensure the fence is submitted to GPU */
            glFlush();
            /* Wait for GPU to reach the fence (timeout 1 second) */
            EGLint result = eglClientWaitSyncKHR(g_egl_display, sync, 
                                                  EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                                  1000000000LL); /* 1 second in nanoseconds */
            if (result == EGL_TIMEOUT_EXPIRED_KHR) {
                fprintf(stderr, "WARNING: GPU fence sync timeout!\n");
            }
            eglDestroySyncKHR(g_egl_display, sync);
        } else {
            /* Fallback if fence creation fails */
            glFinish();
        }
    } else {
        /* Fallback: use glFinish which blocks until all GL commands complete */
        glFinish();
    }
    
    /* 
     * End DMA-BUF sync after GPU is done (lightweight and full modes)
     * This ensures GPU writes are flushed and visible to H2C DMA
     */
    if (g_sync_mode >= 1) {
        dmabuf_sync_end_rw(buf->dmabuf_fd);
    }
}

/* ============================================================================
 * C2H Capture Thread (Producer)
 * ============================================================================ */

static void *c2h_thread_func(void *arg __attribute__((unused))) {
    printf("C2H thread started\n");
    
    struct iwfg_user_dma_req dma_req;
    struct timespec dma_start, dma_end;
    
    while (g_running) {
        /* Wait for an empty slot to be available */
        if (sem_wait(&g_empty_slots) != 0) {
            if (!g_running) break;
            continue;
        }
        
        if (!g_running) {
            sem_post(&g_empty_slots);
            break;
        }
        
        /* Dequeue a buffer from the empty queue */
        pthread_mutex_lock(&g_queue_mutex);
        
        int buf_idx = fifo_dequeue(&g_empty_queue);
        if (buf_idx < 0) {
            /* Queue empty despite semaphore - should never happen */
            fprintf(stderr, "C2H: ERROR - empty queue is empty but semaphore signaled!\n");
            pthread_mutex_lock(&g_stats_mutex);
            g_state_inconsistencies++;
            pthread_mutex_unlock(&g_stats_mutex);
            pthread_mutex_unlock(&g_queue_mutex);
            sem_post(&g_empty_slots);
            continue;
        }
        
        /* Verify buffer is in EMPTY state */
        if (g_buf_states[buf_idx] != BUF_STATE_EMPTY) {
            fprintf(stderr, "C2H: ERROR - buffer %d dequeued from empty_queue but state is %d (expected EMPTY)!\n",
                    buf_idx, g_buf_states[buf_idx]);
            pthread_mutex_lock(&g_stats_mutex);
            g_state_inconsistencies++;
            pthread_mutex_unlock(&g_stats_mutex);
            /* Return buffer to empty queue to avoid losing it */
            fifo_enqueue(&g_empty_queue, buf_idx);
            pthread_mutex_unlock(&g_queue_mutex);
            sem_post(&g_empty_slots);
            continue;
        }
        
        /* Mark buffer as in-use by C2H */
        g_buf_states[buf_idx] = BUF_STATE_IN_USE_C2H;
        pthread_mutex_unlock(&g_queue_mutex);
        
        /*
         * DMA-BUF sync for C2H write access
         * Only needed in full sync mode - semaphores already ensure buffer ownership
         * The blocking sync_start was causing 33ms delays waiting for GPU/H2C
         */
        if (g_sync_mode >= 2) {
            dmabuf_sync_start(g_buffers[buf_idx].dmabuf_fd, true);
        }
        
        /* Perform C2H DMA transfer using buffer index (zero-copy) */
        dma_req.buf = NULL;  /* NULL for DMA_ALLOC - driver looks up by index */
        dma_req.size = FRAME_SIZE;
        dma_req.direction = IWFG_DIR_C2H_BUF;
        dma_req.offset = 0;
        dma_req.buf_index = g_buffers[buf_idx].buf_index;
        dma_req.flags = 0;
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &dma_start);
        }
        
        int ret = ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req);
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &dma_end);
        }
        
        if (ret < 0) {
            /* Release DMA-BUF sync on error if we started it */
            if (g_sync_mode >= 2) {
                dmabuf_sync_end(g_buffers[buf_idx].dmabuf_fd, true);
            }
            
            if (errno == EAGAIN) {
                /* No frame available yet - vsync might be lost */
                pthread_mutex_lock(&g_stats_mutex);
                g_c2h_eagain_count++;
                
                /* Check if we're getting too many EAGAINs (vsync loss indicator) */
                if (g_c2h_eagain_count % 100 == 0 && !g_vsync_warning_printed) {
                    fprintf(stderr, "WARNING: C2H getting excessive EAGAIN (%u) - possible vsync loss!\n", 
                            g_c2h_eagain_count);
                    g_vsync_warning_printed = true;
                }
                pthread_mutex_unlock(&g_stats_mutex);
                
                /* Return buffer to empty queue on EAGAIN */
                pthread_mutex_lock(&g_queue_mutex);
                g_buf_states[buf_idx] = BUF_STATE_EMPTY;
                fifo_enqueue(&g_empty_queue, buf_idx);
                pthread_mutex_unlock(&g_queue_mutex);
                sem_post(&g_empty_slots);
                usleep(1000);
                continue;
            }
            
            pthread_mutex_lock(&g_stats_mutex);
            g_c2h_errors++;
            pthread_mutex_unlock(&g_stats_mutex);
            
            fprintf(stderr, "C2H: DMA transfer failed on buffer %d: %s\n", 
                    buf_idx, strerror(errno));
            
            /* Return buffer to empty queue on error */
            pthread_mutex_lock(&g_queue_mutex);
            g_buf_states[buf_idx] = BUF_STATE_EMPTY;
            fifo_enqueue(&g_empty_queue, buf_idx);
            pthread_mutex_unlock(&g_queue_mutex);
            sem_post(&g_empty_slots);
            continue;
        }
        
        /*
         * Signal that DMA write is complete (only in full sync mode)
         * In lightweight mode, the GPU-side sync handles coherency
         */
        if (g_sync_mode >= 2) {
            dmabuf_sync_end(g_buffers[buf_idx].dmabuf_fd, true);
        }
        
        /* Memory barrier to ensure all writes are visible before state change */
        atomic_thread_fence(memory_order_release);
        
        /* Capture successful - track timing for vsync monitoring */
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        pthread_mutex_lock(&g_queue_mutex);
        
        /* Store capture timestamp for pipeline timing */
        g_buf_capture_time[buf_idx] = current_time;
        
        /* Transition buffer: IN_USE_C2H -> FILLED, enqueue to filled_queue */
        g_buf_states[buf_idx] = BUF_STATE_FILLED;
        if (!fifo_enqueue(&g_filled_queue, buf_idx)) {
            fprintf(stderr, "C2H: ERROR - filled_queue full, cannot enqueue buffer %d!\n", buf_idx);
            /* This should never happen if semaphores are working correctly */
            g_buf_states[buf_idx] = BUF_STATE_EMPTY;
            fifo_enqueue(&g_empty_queue, buf_idx);
            pthread_mutex_unlock(&g_queue_mutex);
            sem_post(&g_empty_slots);
            continue;
        }
        
        pthread_mutex_unlock(&g_queue_mutex);
        
        /* Update statistics and timing */
        pthread_mutex_lock(&g_stats_mutex);
        g_c2h_frames++;
        
        if (g_enable_performance_monitoring) {
            uint64_t dma_time = timespec_diff_us(&dma_start, &dma_end);
            update_timing_stats(&g_c2h_time_total, &g_c2h_time_min, &g_c2h_time_max,
                               &g_c2h_time_count, dma_time);
        }
        
        /* Check frame timing (should be ~16ms for 60fps) */
        if (g_last_c2h_time.tv_sec != 0) {
            long delta_ms = (current_time.tv_sec - g_last_c2h_time.tv_sec) * 1000 +
                           (current_time.tv_nsec - g_last_c2h_time.tv_nsec) / 1000000;
            if (delta_ms > 50) {  /* More than 50ms between frames */
                fprintf(stderr, "C2H: WARNING - Frame gap of %ld ms detected (vsync issue?)\n", delta_ms);
            }
        }
        g_last_c2h_time = current_time;
        
        /* Reset EAGAIN counter and warning on successful capture */
        if (g_c2h_eagain_count > 0 && g_c2h_frames % 60 == 0) {
            g_c2h_eagain_count = 0;
            g_vsync_warning_printed = false;
        }
        
        pthread_mutex_unlock(&g_stats_mutex);
        
        /* Signal H2C thread that a filled buffer is available */
        sem_post(&g_filled_slots);
    }
    
    printf("C2H thread exiting\n");
    return NULL;
}

/* ============================================================================
 * H2C Transmission Thread (Consumer) - includes GPU rendering
 * ============================================================================ */

static void *h2c_thread_func(void *arg __attribute__((unused))) {
    printf("H2C thread started (with GPU rendering)\n");
    
    /* Make EGL context current in this thread for GPU rendering */
    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        fprintf(stderr, "H2C thread: Failed to make EGL context current\n");
        return NULL;
    }
    
    struct iwfg_user_dma_req dma_req;
    struct timespec h2c_start, h2c_end;
    
    while (g_running) {
        /* Wait for a filled buffer to be available */
        if (sem_wait(&g_filled_slots) != 0) {
            if (!g_running) break;
            continue;
        }
        
        if (!g_running) {
            sem_post(&g_filled_slots);
            break;
        }
        
        /* Dequeue a buffer from the filled queue */
        pthread_mutex_lock(&g_queue_mutex);
        
        int buf_idx = fifo_dequeue(&g_filled_queue);
        if (buf_idx < 0) {
            /* Queue empty despite semaphore - should never happen */
            fprintf(stderr, "H2C: ERROR - filled queue is empty but semaphore signaled!\n");
            pthread_mutex_lock(&g_stats_mutex);
            g_state_inconsistencies++;
            pthread_mutex_unlock(&g_stats_mutex);
            pthread_mutex_unlock(&g_queue_mutex);
            sem_post(&g_filled_slots);
            continue;
        }
        
        /* Verify buffer is in FILLED state */
        if (g_buf_states[buf_idx] != BUF_STATE_FILLED) {
            fprintf(stderr, "H2C: ERROR - buffer %d dequeued from filled_queue but state is %d (expected FILLED)!\n",
                    buf_idx, g_buf_states[buf_idx]);
            pthread_mutex_lock(&g_stats_mutex);
            g_state_inconsistencies++;
            pthread_mutex_unlock(&g_stats_mutex);
            /* Try to recover by returning to empty queue */
            g_buf_states[buf_idx] = BUF_STATE_EMPTY;
            fifo_enqueue(&g_empty_queue, buf_idx);
            pthread_mutex_unlock(&g_queue_mutex);
            sem_post(&g_empty_slots);
            continue;
        }
        
        /* Mark buffer as in-use by H2C */
        g_buf_states[buf_idx] = BUF_STATE_IN_USE_H2C;
        
        /* Get capture time for pipeline calculation */
        struct timespec capture_time = g_buf_capture_time[buf_idx];
        
        pthread_mutex_unlock(&g_queue_mutex);
        
        /* Memory barrier to ensure we see all writes from C2H thread */
        atomic_thread_fence(memory_order_acquire);
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &h2c_start);
        }
        
        /* Render cube overlay on the captured frame (GPU processing)
         * Note: render_cube_overlay handles GPU synchronization internally
         * including EGL fence sync and DMA-BUF sync
         */
        render_cube_overlay(buf_idx);
        
        /* Memory barrier to ensure GPU writes are complete before DMA */
        atomic_thread_fence(memory_order_release);
        
        /* Perform H2C DMA transfer using buffer index (zero-copy) */
        dma_req.buf = NULL;  /* NULL for DMA_ALLOC - driver looks up by index */
        dma_req.size = FRAME_SIZE;
        dma_req.direction = IWFG_DIR_H2C_BUF;
        dma_req.offset = 0;
        dma_req.buf_index = g_buffers[buf_idx].buf_index;
        dma_req.flags = 0;
        
        int ret = ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req);
        
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &h2c_end);
        }
        
        if (ret < 0) {
            pthread_mutex_lock(&g_stats_mutex);
            g_h2c_errors++;
            pthread_mutex_unlock(&g_stats_mutex);
            fprintf(stderr, "H2C: DMA transfer failed on buffer %d: %s\n", 
                    buf_idx, strerror(errno));
        } else {
            pthread_mutex_lock(&g_stats_mutex);
            g_h2c_frames++;
            
            if (g_enable_performance_monitoring) {
                /* H2C time includes GPU render + DMA */
                uint64_t h2c_time = timespec_diff_us(&h2c_start, &h2c_end);
                update_timing_stats(&g_h2c_time_total, &g_h2c_time_min, &g_h2c_time_max,
                                   &g_h2c_time_count, h2c_time);
                
                /* Calculate total pipeline time: capture -> h2c complete */
                uint64_t pipeline_time = timespec_diff_us(&capture_time, &h2c_end);
                update_timing_stats(&g_pipeline_time_total, &g_pipeline_time_min, &g_pipeline_time_max,
                                   &g_pipeline_time_count, pipeline_time);
            }
            
            pthread_mutex_unlock(&g_stats_mutex);
        }
        
        /* Transmission complete (or failed) - return buffer to empty queue */
        pthread_mutex_lock(&g_queue_mutex);
        
        /* Transition buffer: IN_USE_H2C -> EMPTY, enqueue to empty_queue */
        g_buf_states[buf_idx] = BUF_STATE_EMPTY;
        if (!fifo_enqueue(&g_empty_queue, buf_idx)) {
            fprintf(stderr, "H2C: ERROR - empty_queue full, cannot enqueue buffer %d!\n", buf_idx);
            /* This should never happen - we just dequeued from filled */
        }
        
        pthread_mutex_unlock(&g_queue_mutex);
        
        /* Signal C2H thread that an empty slot is available */
        sem_post(&g_empty_slots);
    }
    
    /* Release EGL context */
    eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    
    printf("H2C thread exiting\n");
    return NULL;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

static void cleanup_and_exit(int sig __attribute__((unused))) {
    printf("\nCleaning up and exiting...\n");
    g_running = false;
    
    /* Wake up waiting threads */
    sem_post(&g_empty_slots);
    sem_post(&g_filled_slots);
    
    /* Wait for threads */
    pthread_join(g_c2h_thread, NULL);
    pthread_join(g_h2c_thread, NULL);
    
    /* Stop streaming */
    if (g_iwfg_fd >= 0) {
        ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_STOP);
    }
    
    /* Cleanup OpenGL resources */
    if (g_cube_program) glDeleteProgram(g_cube_program);
    if (g_framebuffer) glDeleteFramebuffers(1, &g_framebuffer);
    if (g_render_texture) glDeleteTextures(1, &g_render_texture);
    if (g_depth_renderbuffer) glDeleteRenderbuffers(1, &g_depth_renderbuffer);
    
    /* Cleanup DMA-BUF resources */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_buffers[i].gl_texture) {
            glDeleteTextures(1, &g_buffers[i].gl_texture);
        }
        if (g_buffers[i].egl_image != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(g_egl_display, g_buffers[i].egl_image);
        }
        /* Note: DMA-BUF fd is closed by the driver when buffers are cleaned up */
        if (g_buffers[i].dmabuf_fd >= 0) {
            close(g_buffers[i].dmabuf_fd);
        }
    }
    
    /* Cleanup EGL */
    if (g_egl_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(g_egl_display, g_egl_context);
    }
    if (g_egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(g_egl_display, g_egl_surface);
    }
    if (g_egl_display != EGL_NO_DISPLAY) {
        eglTerminate(g_egl_display);
    }
    
    /* Cleanup GBM */
    if (g_gbm_device) {
        gbm_device_destroy(g_gbm_device);
    }
    if (g_drm_fd >= 0) {
        close(g_drm_fd);
    }
    
    /* Cleanup device */
    if (g_iwfg_fd >= 0) {
        close(g_iwfg_fd);
    }
    
    /* Cleanup synchronization */
    pthread_mutex_destroy(&g_queue_mutex);
    pthread_mutex_destroy(&g_stats_mutex);
    sem_destroy(&g_empty_slots);
    sem_destroy(&g_filled_slots);
    
    /* Print final statistics */
    printf("\n=== Final Statistics ===\n");
    printf("C2H frames captured: %u (errors: %u, EAGAIN: %u)\n", 
           g_c2h_frames, g_c2h_errors, g_c2h_eagain_count);
    printf("H2C frames transmitted (with GPU render): %u (errors: %u)\n", g_h2c_frames, g_h2c_errors);
    printf("State inconsistencies detected: %u\n", g_state_inconsistencies);
    
    if (g_enable_performance_monitoring && g_c2h_time_count > 0) {
        printf("\n=== Performance Timing (milliseconds) ===\n");
        printf("C2H DMA:        avg=%.2f  min=%.2f  max=%.2f  (samples=%lu)\n",
               (double)(g_c2h_time_total / g_c2h_time_count) / 1000.0,
               (double)g_c2h_time_min / 1000.0,
               (double)g_c2h_time_max / 1000.0,
               (unsigned long)g_c2h_time_count);
        printf("H2C (GPU+DMA):  avg=%.2f  min=%.2f  max=%.2f  (samples=%lu)\n",
               g_h2c_time_count > 0 ? (double)(g_h2c_time_total / g_h2c_time_count) / 1000.0 : 0,
               (double)g_h2c_time_min / 1000.0,
               (double)g_h2c_time_max / 1000.0,
               (unsigned long)g_h2c_time_count);
        printf("Pipeline:       avg=%.2f  min=%.2f  max=%.2f  (samples=%lu)\n",
               g_pipeline_time_count > 0 ? (double)(g_pipeline_time_total / g_pipeline_time_count) / 1000.0 : 0,
               (double)g_pipeline_time_min / 1000.0,
               (double)g_pipeline_time_max / 1000.0,
               (unsigned long)g_pipeline_time_count);
        
        /* Calculate theoretical max FPS based on pipeline time */
        if (g_pipeline_time_count > 0 && g_pipeline_time_total > 0) {
            double avg_pipeline_ms = (double)(g_pipeline_time_total / g_pipeline_time_count) / 1000.0;
            printf("\nPipeline avg: %.2f ms -> theoretical max: %.1f FPS\n",
                   avg_pipeline_ms, 1000.0 / avg_pipeline_ms);
        }
    }
    
    if (g_c2h_eagain_count > 1000) {
        printf("\n*** WARNING: High EAGAIN count suggests vsync loss! ***\n");
        printf("    Check driver interrupt handling and FPGA vsync signal.\n");
    }
    
    exit(0);
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -p, --performance    Enable performance monitoring\n");
    printf("  -s, --sync <mode>    Sync mode: 0=none, 1=lightweight (default), 2=full\n");
    printf("                       0: No DMA-BUF sync (fastest, may tear)\n");
    printf("                       1: GPU-only sync (balanced, recommended)\n");
    printf("                       2: Full sync all paths (safest, slower)\n");
    printf("  -h, --help           Show this help message\n");
    printf("\n");
    printf("This example demonstrates:\n");
    printf("  - Driver-allocated DMA buffers (IWFG_BUF_TYPE_DMA_ALLOC)\n");
    printf("  - DMA-BUF export from kernel driver\n");
    printf("  - Zero-copy GPU import via EGL_EXT_image_dma_buf_import\n");
    printf("  - C2H capture -> GPU cube overlay -> H2C transmission\n");
    printf("  - Triple buffering with semaphore synchronization\n");
    printf("\n");
    printf("Requirements:\n");
    printf("  - EGL with EGL_EXT_image_dma_buf_import extension\n");
    printf("  - GBM for surfaceless EGL context\n");
    printf("  - OpenGL ES 2.0 support\n");
}

int main(int argc, char *argv[]) {
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--performance") == 0) {
            g_enable_performance_monitoring = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sync") == 0) {
            if (i + 1 < argc) {
                g_sync_mode = atoi(argv[++i]);
                if (g_sync_mode < 0 || g_sync_mode > 2) {
                    fprintf(stderr, "Invalid sync mode: %d (must be 0, 1, or 2)\n", g_sync_mode);
                    return 1;
                }
            } else {
                fprintf(stderr, "Missing argument for -s/--sync\n");
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
    
    printf("IWFG DMA-BUF Zero-Copy Cube Overlay Example\n");
    printf("============================================\n");
    printf("Resolution: %dx%d\n", WIDTH, HEIGHT);
    printf("Frame size: %d bytes\n", FRAME_SIZE);
    printf("Number of buffers: %d (triple buffering)\n", NUM_BUFFERS);
    printf("Performance monitoring: %s\n", 
           g_enable_performance_monitoring ? "ENABLED" : "DISABLED");
    printf("Sync mode: %d (%s)\n", g_sync_mode,
           g_sync_mode == 0 ? "none" : g_sync_mode == 1 ? "lightweight/GPU-only" : "full");
    printf("\n");
    
    /* Open IWFG device */
    g_iwfg_fd = open("/dev/iwfg", O_RDWR);
    if (g_iwfg_fd < 0) {
        perror("Failed to open IWFG device");
        return -1;
    }
    printf("IWFG device opened successfully\n");
    
    /* Initialize EGL with GBM backend (required for DMA-BUF import) */
    printf("\n--- Initializing EGL/GBM ---\n");
    if (init_egl_gbm() < 0) {
        fprintf(stderr, "Failed to initialize EGL/GBM\n");
        fprintf(stderr, "\nNote: EGL is REQUIRED on x86 PCs for DMA-BUF import.\n");
        fprintf(stderr, "Make sure you have:\n");
        fprintf(stderr, "  - Mesa with GBM support, or NVIDIA with EGLStreams\n");
        fprintf(stderr, "  - libdrm, libgbm, libEGL, libGLESv2 installed\n");
        fprintf(stderr, "  - DRM render node accessible (/dev/dri/renderD128)\n");
        close(g_iwfg_fd);
        return -1;
    }
    
    /* Initialize OpenGL resources */
    printf("\n--- Initializing OpenGL ---\n");
    if (init_opengl() < 0) {
        fprintf(stderr, "Failed to initialize OpenGL\n");
        close(g_iwfg_fd);
        return -1;
    }
    
    /* Initialize FIFO queues */
    fifo_init(&g_empty_queue);
    fifo_init(&g_filled_queue);
    
    /* Pre-populate empty queue with all buffer indices */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        fifo_enqueue(&g_empty_queue, i);
        g_buf_states[i] = BUF_STATE_EMPTY;
    }
    printf("FIFO queues initialized: empty=%d buffers [0-%d]\n", NUM_BUFFERS, NUM_BUFFERS - 1);
    
    /* Initialize semaphores */
    if (sem_init(&g_empty_slots, 0, NUM_BUFFERS) != 0 ||
        sem_init(&g_filled_slots, 0, 0) != 0) {
        perror("Failed to initialize semaphores");
        close(g_iwfg_fd);
        return -1;
    }
    printf("Semaphores initialized (empty=%d, filled=0)\n", NUM_BUFFERS);
    
    /* Setup DMA-BUF buffers with driver allocation */
    printf("\n--- Setting up DMA-BUF buffers ---\n");
    
    /* Release EGL context before buffer setup (will be acquired by H2C thread) */
    eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    
    /* Re-acquire for buffer setup */
    eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context);
    
    if (setup_dmabuf_buffers() < 0) {
        fprintf(stderr, "Failed to setup DMA-BUF buffers\n");
        close(g_iwfg_fd);
        return -1;
    }
    
    /* Release context for H2C thread */
    eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    
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
    
    printf("Pipeline: C2H (capture) -> H2C (GPU render + transmit)\n");
    printf("\nPress Ctrl+C to stop.\n\n");
    
    /* Main thread: periodic status updates */
    while (g_running) {
        sleep(5);
        
        if (!g_running) break;
        
        pthread_mutex_lock(&g_stats_mutex);
        printf("Status: C2H=%u, H2C=%u | Errors: C2H=%u, H2C=%u, EAGAIN=%u, StateErr=%u\n",
               g_c2h_frames, g_h2c_frames,
               g_c2h_errors, g_h2c_errors, g_c2h_eagain_count, g_state_inconsistencies);
        
        if (g_enable_performance_monitoring && g_c2h_time_count > 0) {
            printf("  Timing (ms): C2H avg=%.2f min=%.2f max=%.2f (n=%lu)\n",
                   (double)(g_c2h_time_total / g_c2h_time_count) / 1000.0,
                   (double)g_c2h_time_min / 1000.0,
                   (double)g_c2h_time_max / 1000.0,
                   (unsigned long)g_c2h_time_count);
            printf("              H2C (GPU+DMA) avg=%.2f min=%.2f max=%.2f (n=%lu)\n",
                   g_h2c_time_count > 0 ? (double)(g_h2c_time_total / g_h2c_time_count) / 1000.0 : 0,
                   (double)g_h2c_time_min / 1000.0,
                   (double)g_h2c_time_max / 1000.0,
                   (unsigned long)g_h2c_time_count);
            printf("         Pipeline avg=%.2f min=%.2f max=%.2f (n=%lu)\n",
                   g_pipeline_time_count > 0 ? (double)(g_pipeline_time_total / g_pipeline_time_count) / 1000.0 : 0,
                   (double)g_pipeline_time_min / 1000.0,
                   (double)g_pipeline_time_max / 1000.0,
                   (unsigned long)g_pipeline_time_count);
        }
        pthread_mutex_unlock(&g_stats_mutex);
    }
    
    cleanup_and_exit(0);
    return 0;
}
