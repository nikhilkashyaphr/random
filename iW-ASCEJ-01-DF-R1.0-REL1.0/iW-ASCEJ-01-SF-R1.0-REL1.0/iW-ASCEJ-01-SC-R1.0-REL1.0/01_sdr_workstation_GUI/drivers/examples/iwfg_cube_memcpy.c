/*
 * IWFG C2H to H2C Transfer Example (Multi-threaded) with OpenGL Cube Overlay
 * 
 * This example demonstrates:
 * 1. Capturing frames to C2H buffers using IWFG_IOCTL_DMA_DATA in a separate thread
 * 2. Routing captured frames to H2C buffers for H2C transfers using IWFG_IOCTL_DMA_DATA in another thread
 * 3. Overlaying an OpenGL-rendered rotating cube with colored faces on captured frames
 * 
 * Features:
 * - Dual buffer setup: C2H (receive) and H2C (transmit) buffers
 * - Multi-threaded pipeline: C2H capture thread and H2C transmission thread
 * - Frame capture via C2H DMA using new IOCTL in dedicated thread
 * - Frame transmission via H2C DMA using new IOCTL in dedicated thread
 * - Inter-thread communication via lock-free frame queue
 * - OpenGL ES 2.0 offscreen rendering for cube overlay
 * - Rotating cube with different colored faces
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
#include <math.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "iwfg_user.h"

#define NUM_C2H_BUFFERS 2  // C2H (receive) buffers
#define NUM_H2C_BUFFERS 2  // H2C (transmit) buffers
#define WIDTH 1280
#define HEIGHT 1024
#define BYTES_PER_PIXEL 4  // XRGB8888 format
#define FRAME_SIZE (WIDTH * HEIGHT * BYTES_PER_PIXEL)

// OpenGL cube overlay dimensions
#define CUBE_OVERLAY_WIDTH 400
#define CUBE_OVERLAY_HEIGHT 400
#define CUBE_OVERLAY_X ((WIDTH - CUBE_OVERLAY_WIDTH) / 2)   // Center horizontally
#define CUBE_OVERLAY_Y ((HEIGHT - CUBE_OVERLAY_HEIGHT) / 2) // Center vertically

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

// OpenGL/EGL context for offscreen rendering
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;
static GLuint g_cube_program = 0;
static GLuint g_bg_program = 0;  // Background/frame rendering program
static GLuint g_framebuffer = 0;
static GLuint g_render_texture = 0;
static GLuint g_frame_texture = 0;  // Texture for C2H frame
static GLuint g_depth_renderbuffer = 0;
static uint32_t *g_composite_pixels = NULL;  // Output buffer for composited frame
static float g_cube_rotation = 0.0f;
static pthread_mutex_t g_gl_mutex = PTHREAD_MUTEX_INITIALIZER;

// Shader attribute/uniform locations
static GLint g_position_loc = -1;
static GLint g_color_loc = -1;
static GLint g_mvp_loc = -1;
static GLint g_bg_position_loc = -1;
static GLint g_bg_texcoord_loc = -1;
static GLint g_bg_texture_loc = -1;

// Vertex shader for cube
static const char *vertex_shader_src =
    "attribute vec3 a_position;\n"
    "attribute vec3 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

// Fragment shader for cube
static const char *fragment_shader_src =
    "precision mediump float;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(v_color, 1.0);\n"
    "}\n";

// Vertex shader for background frame
static const char *bg_vertex_shader_src =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

// Fragment shader for background frame (converts XRGB to display)
static const char *bg_fragment_shader_src =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    vec4 color = texture2D(u_texture, v_texcoord);\n"
    "    gl_FragColor = vec4(color.rgb, 1.0);\n"
    "}\n";

// Fullscreen quad vertices for background rendering
static const GLfloat bg_vertices[] = {
    // Position (x,y)  TexCoord (u,v)
    -1.0f, -1.0f,      0.0f, 0.0f,
     1.0f, -1.0f,      1.0f, 0.0f,
     1.0f,  1.0f,      1.0f, 1.0f,
    -1.0f, -1.0f,      0.0f, 0.0f,
     1.0f,  1.0f,      1.0f, 1.0f,
    -1.0f,  1.0f,      0.0f, 1.0f,
};

// Cube vertex data - 6 faces with different colors
// Each face has 2 triangles (6 vertices)
static const GLfloat cube_vertices[] = {
    // Front face (Red) - Z = 0.5
    -0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
    -0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 0.0f,
    -0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 0.0f,

    // Back face (Green) - Z = -0.5
    -0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
    -0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
     0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
    -0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
     0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 0.0f,
     0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 0.0f,

    // Top face (Blue) - Y = 0.5
    -0.5f,  0.5f, -0.5f,   0.0f, 0.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
    -0.5f,  0.5f, -0.5f,   0.0f, 0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,   0.0f, 0.0f, 1.0f,
     0.5f,  0.5f, -0.5f,   0.0f, 0.0f, 1.0f,

    // Bottom face (Yellow) - Y = -0.5
    -0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.0f,
     0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.0f,
    -0.5f, -0.5f, -0.5f,   1.0f, 1.0f, 0.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.0f,
    -0.5f, -0.5f,  0.5f,   1.0f, 1.0f, 0.0f,

    // Right face (Magenta) - X = 0.5
     0.5f, -0.5f, -0.5f,   1.0f, 0.0f, 1.0f,
     0.5f,  0.5f, -0.5f,   1.0f, 0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 1.0f,
     0.5f, -0.5f, -0.5f,   1.0f, 0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,   1.0f, 0.0f, 1.0f,
     0.5f, -0.5f,  0.5f,   1.0f, 0.0f, 1.0f,

    // Left face (Cyan) - X = -0.5
    -0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f, -0.5f,  0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f, -0.5f, -0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,   0.0f, 1.0f, 1.0f,
    -0.5f,  0.5f, -0.5f,   0.0f, 1.0f, 1.0f,
};

// Matrix multiplication helper for column-major matrices (OpenGL convention)
// Computes result = a * b where matrices are stored column-major
static void mat4_multiply(float *result, const float *a, const float *b) {
    float temp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            temp[col * 4 + row] = 0.0f;
            for (int k = 0; k < 4; k++) {
                // result[col][row] = sum(a[k][row] * b[col][k])
                temp[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
            }
        }
    }
    memcpy(result, temp, sizeof(temp));
}

// Create identity matrix
static void mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Create rotation matrix around Y axis
static void mat4_rotate_y(float *m, float angle) {
    mat4_identity(m);
    float c = cosf(angle);
    float s = sinf(angle);
    m[0] = c;
    m[2] = s;
    m[8] = -s;
    m[10] = c;
}

// Create rotation matrix around X axis
static void mat4_rotate_x(float *m, float angle) {
    mat4_identity(m);
    float c = cosf(angle);
    float s = sinf(angle);
    m[5] = c;
    m[6] = s;
    m[9] = -s;
    m[10] = c;
}

// Create scale matrix
static void mat4_scale(float *m, float sx, float sy, float sz) {
    mat4_identity(m);
    m[0] = sx;
    m[5] = sy;
    m[10] = sz;
}

// Create perspective projection matrix
static void mat4_perspective(float *m, float fovy, float aspect, float near, float far) {
    memset(m, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fovy / 2.0f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far + near) / (near - far);
    m[11] = -1.0f;
    m[14] = (2.0f * far * near) / (near - far);
}

// Create translation matrix
static void mat4_translate(float *m, float x, float y, float z) {
    mat4_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

// Compile shader
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

// Initialize OpenGL for offscreen rendering
static int init_opengl(void) {
    // Get EGL display
    g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGL display\n");
        return -1;
    }
    
    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        fprintf(stderr, "Failed to initialize EGL\n");
        return -1;
    }
    printf("EGL initialized: version %d.%d\n", major, minor);
    
    // Choose EGL config for offscreen rendering
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
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
        fprintf(stderr, "Failed to choose EGL config\n");
        return -1;
    }
    
    // Create pbuffer surface for full frame rendering
    EGLint pbuffer_attribs[] = {
        EGL_WIDTH, WIDTH,
        EGL_HEIGHT, HEIGHT,
        EGL_NONE
    };
    
    g_egl_surface = eglCreatePbufferSurface(g_egl_display, config, pbuffer_attribs);
    if (g_egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "Failed to create EGL pbuffer surface\n");
        return -1;
    }
    
    // Create OpenGL ES 2.0 context
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    
    g_egl_context = eglCreateContext(g_egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (g_egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "Failed to create EGL context\n");
        return -1;
    }
    
    // Make context current
    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        return -1;
    }
    
    printf("OpenGL ES version: %s\n", glGetString(GL_VERSION));
    printf("OpenGL ES renderer: %s\n", glGetString(GL_RENDERER));
    
    // Compile cube shaders and create program
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);
    
    if (!vertex_shader || !fragment_shader) {
        fprintf(stderr, "Failed to compile cube shaders\n");
        return -1;
    }
    
    g_cube_program = glCreateProgram();
    glAttachShader(g_cube_program, vertex_shader);
    glAttachShader(g_cube_program, fragment_shader);
    glLinkProgram(g_cube_program);
    
    GLint linked;
    glGetProgramiv(g_cube_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        fprintf(stderr, "Failed to link cube program\n");
        return -1;
    }
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    // Get cube shader attribute and uniform locations
    g_position_loc = glGetAttribLocation(g_cube_program, "a_position");
    g_color_loc = glGetAttribLocation(g_cube_program, "a_color");
    g_mvp_loc = glGetUniformLocation(g_cube_program, "u_mvp");
    
    // Compile background shaders and create program
    GLuint bg_vertex_shader = compile_shader(GL_VERTEX_SHADER, bg_vertex_shader_src);
    GLuint bg_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, bg_fragment_shader_src);
    
    if (!bg_vertex_shader || !bg_fragment_shader) {
        fprintf(stderr, "Failed to compile background shaders\n");
        return -1;
    }
    
    g_bg_program = glCreateProgram();
    glAttachShader(g_bg_program, bg_vertex_shader);
    glAttachShader(g_bg_program, bg_fragment_shader);
    glLinkProgram(g_bg_program);
    
    glGetProgramiv(g_bg_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        fprintf(stderr, "Failed to link background program\n");
        return -1;
    }
    
    glDeleteShader(bg_vertex_shader);
    glDeleteShader(bg_fragment_shader);
    
    // Get background shader locations
    g_bg_position_loc = glGetAttribLocation(g_bg_program, "a_position");
    g_bg_texcoord_loc = glGetAttribLocation(g_bg_program, "a_texcoord");
    g_bg_texture_loc = glGetUniformLocation(g_bg_program, "u_texture");
    
    // Create texture for C2H frame input
    glGenTextures(1, &g_frame_texture);
    glBindTexture(GL_TEXTURE_2D, g_frame_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Allocate texture storage (will be updated each frame)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, WIDTH, HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    
    // Allocate buffer for reading composited pixels
    g_composite_pixels = malloc(WIDTH * HEIGHT * 4);
    if (!g_composite_pixels) {
        fprintf(stderr, "Failed to allocate composite pixel buffer\n");
        return -1;
    }
    
    // Test render to verify OpenGL is working
    glViewport(0, 0, WIDTH, HEIGHT);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);  // Red
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    
    uint32_t test_pixel;
    glReadPixels(WIDTH/2, HEIGHT/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &test_pixel);
    printf("OpenGL test pixel (should be red): 0x%08X\n", test_pixel);
    
    // Release context from main thread - it will be acquired by copy thread
    eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    
    printf("OpenGL GPU compositing initialized (%dx%d)\n", WIDTH, HEIGHT);
    return 0;
}

// Render frame with cube overlay - all compositing done on GPU
// Takes C2H frame as input, outputs composited frame to g_composite_pixels
static void render_frame_with_cube(void *frame_buffer) {
    pthread_mutex_lock(&g_gl_mutex);
    
    // Make sure our context is current in this thread
    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        EGLint error = eglGetError();
        fprintf(stderr, "Failed to make EGL context current, error: 0x%x\n", error);
        pthread_mutex_unlock(&g_gl_mutex);
        return;
    }
    
    // Render to default framebuffer (pbuffer) at full frame size
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, WIDTH, HEIGHT);
    
    // Step 1: Upload C2H frame as texture
    // The frame is in XRGB8888 format (0xXXRRGGBB), upload as RGBA
    glBindTexture(GL_TEXTURE_2D, g_frame_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, frame_buffer);
    
    // Step 2: Render the frame as background (fullscreen quad)
    glDisable(GL_DEPTH_TEST);  // No depth for background
    glUseProgram(g_bg_program);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_frame_texture);
    glUniform1i(g_bg_texture_loc, 0);
    
    glVertexAttribPointer(g_bg_position_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bg_vertices);
    glEnableVertexAttribArray(g_bg_position_loc);
    
    glVertexAttribPointer(g_bg_texcoord_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), bg_vertices + 2);
    glEnableVertexAttribArray(g_bg_texcoord_loc);
    
    glDrawArrays(GL_TRIANGLES, 0, 6);
    
    // Step 3: Render the cube on top
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClear(GL_DEPTH_BUFFER_BIT);  // Clear only depth, keep color
    
    glUseProgram(g_cube_program);
    
    // Create MVP matrix for cube (centered in frame)
    float projection[16], view[16], model[16], rot_y[16], rot_x[16], scale[16];
    float temp1[16], temp2[16], mv[16], mvp[16];
    
    // Perspective projection - adjust for full frame aspect ratio
    mat4_perspective(projection, 60.0f * M_PI / 180.0f, 
                    (float)WIDTH / (float)HEIGHT, 
                    0.1f, 100.0f);
    
    // View matrix - move camera back
    mat4_translate(view, 0.0f, 0.0f, -2.5f);
    
    // Model matrix: scale * rotate_x * rotate_y
    // Scale down the cube (0.4 = 40% of original size)
    mat4_scale(scale, 0.4f, 0.4f, 0.4f);
    
    // Tilt cube ~30 degrees on X axis so top/bottom faces are visible
    mat4_rotate_x(rot_x, 30.0f * M_PI / 180.0f);
    
    // Rotate around Y axis (animated)
    mat4_rotate_y(rot_y, g_cube_rotation);
    
    // Combine: model = rot_y * rot_x * scale
    mat4_multiply(temp1, rot_x, scale);
    mat4_multiply(model, rot_y, temp1);
    
    // MVP = projection * view * model
    mat4_multiply(mv, view, model);
    mat4_multiply(mvp, projection, mv);
    
    glUniformMatrix4fv(g_mvp_loc, 1, GL_FALSE, mvp);
    
    glVertexAttribPointer(g_position_loc, 3, GL_FLOAT, GL_FALSE, 
                         6 * sizeof(GLfloat), cube_vertices);
    glEnableVertexAttribArray(g_position_loc);
    
    glVertexAttribPointer(g_color_loc, 3, GL_FLOAT, GL_FALSE,
                         6 * sizeof(GLfloat), cube_vertices + 3);
    glEnableVertexAttribArray(g_color_loc);
    
    glDrawArrays(GL_TRIANGLES, 0, 36);
    
    // Step 4: Read back composited frame
    glFinish();
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, g_composite_pixels);
    
    // Debug: print info for first few frames
    static int debug_count = 0;
    if (debug_count < 3) {
        printf("GPU composite: rotation=%.2f, frame rendered\n", g_cube_rotation);
        debug_count++;
    }
    
    // Update rotation for next frame
    g_cube_rotation += 0.05f;
    if (g_cube_rotation > 2.0f * M_PI) {
        g_cube_rotation -= 2.0f * M_PI;
    }
    
    pthread_mutex_unlock(&g_gl_mutex);
}

// Cleanup OpenGL resources
static void cleanup_opengl(void) {
    pthread_mutex_lock(&g_gl_mutex);
    
    if (g_composite_pixels) {
        free(g_composite_pixels);
        g_composite_pixels = NULL;
    }
    
    if (g_frame_texture) {
        glDeleteTextures(1, &g_frame_texture);
    }
    if (g_framebuffer) {
        glDeleteFramebuffers(1, &g_framebuffer);
    }
    if (g_render_texture) {
        glDeleteTextures(1, &g_render_texture);
    }
    if (g_depth_renderbuffer) {
        glDeleteRenderbuffers(1, &g_depth_renderbuffer);
    }
    if (g_cube_program) {
        glDeleteProgram(g_cube_program);
    }
    if (g_bg_program) {
        glDeleteProgram(g_bg_program);
    }
    
    if (g_egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(g_egl_display, g_egl_surface);
        }
        if (g_egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(g_egl_display, g_egl_context);
        }
        eglTerminate(g_egl_display);
    }
    
    pthread_mutex_unlock(&g_gl_mutex);
    printf("OpenGL resources cleaned up\n");
}

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
    
    // Cleanup OpenGL resources
    cleanup_opengl();
    
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
            // printf("C2H: Captured %d frames\n", frame_number);
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
    (void)size;  // Size is always FRAME_SIZE
    
    // GPU compositing: upload C2H frame, render cube on top, read back result
    render_frame_with_cube(src_buffer);
    
    // Just memcpy the GPU-composited result to H2C buffer
    memcpy(dst_buffer, g_composite_pixels, FRAME_SIZE);
    
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
            // printf("Copy: Processed %d frames\n", frames_processed);
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
            // printf("H2C: Transmitted %d frames\n", frames_transmitted);
        }
        
        // Show performance stats periodically
        if (g_enable_performance_monitoring && frames_transmitted % 60 == 0) {
            printf("H2C: Frame %d - pipeline completed\n", processed_data.frame_number);
            print_performance_stats();
        }
    }
    
    printf("H2C transmission thread exiting\n");
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
    
    printf("IWFG C2H to H2C Transfer Example (Multi-threaded) with OpenGL Cube Overlay\n");
    printf("============================================================================\n");
    printf("This example demonstrates:\n");
    printf("1. C2H transfers using IWFG_IOCTL_DMA_DATA - in separate thread\n");
    printf("2. H2C transfers using IWFG_IOCTL_DMA_DATA - in separate thread\n");
    printf("3. Pipelined frame routing from C2H to H2C buffers\n");
    printf("4. Inter-thread communication via frame queue\n");
    printf("5. OpenGL-rendered rotating cube overlay with colored faces\n");
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
    
    // Initialize OpenGL for cube rendering
    if (init_opengl() < 0) {
        fprintf(stderr, "Failed to initialize OpenGL\n");
        close(g_iwfg_fd);
        return -1;
    }
    
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
            // printf("Pipeline status - Captured: %u, Transmitted: %u, Dropped: %u\n",
            //        g_total_frames_captured, g_total_frames_transmitted, g_frames_dropped);
        }
    }
    
    printf("\nShutting down pipeline...\n");
    cleanup_and_exit(0);
    return 0;
}