/*
 * IWFG OpenGL Display Example
 * 
 * This example demonstrates capturing frames from the IWFG frame grabber
 * and displaying them in an OpenGL window using NVIDIA GPU acceleration.
 * The frame grabber outputs RGB888 format at 3840x2160 resolution.
 * 
 * Features:
 * - OpenGL texture streaming from frame grabber buffers
 * - NVIDIA GPU accelerated rendering
 * - Real-time performance monitoring
 * - Full 4K resolution support (3840x2160)
 * - RGB format support (no conversion needed)
 * - Double buffering for smooth display
 */

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
#include <stdatomic.h>

// OpenGL and GLFW headers
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GLFW/glfw3.h>

#include "iwfg_user.h"

#define NUM_BUFFERS 5

// Stream parameters (queried from driver at runtime)
static uint32_t g_width = 0;
static uint32_t g_height = 0;
static uint32_t g_bpp = 0;
static uint32_t g_frame_size = 0;

// Global state for cleanup
static int g_iwfg_fd = -1;
static void *g_buffers[NUM_BUFFERS];
static GLFWwindow *g_window = NULL;
static GLuint g_texture_id = 0;
static GLuint g_shader_program = 0;
static GLuint g_vao = 0;
static GLuint g_vbo = 0;

// Performance monitoring
static struct timespec g_frame_times[100];
static struct timespec g_wait_times[100];
static struct timespec g_render_times[100];
static int g_frame_count = 0;
static bool g_enable_performance_monitoring = false;
static uint32_t g_total_frames = 0;

// Ring buffer state for producer-consumer threading
typedef enum { BUF_FREE, BUF_FILLED } buf_state_t;
static buf_state_t      g_buf_state[NUM_BUFFERS];
static pthread_mutex_t  g_ring_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cond_filled = PTHREAD_COND_INITIALIZER;
static pthread_cond_t   g_cond_free   = PTHREAD_COND_INITIALIZER;
static int              g_write_idx   = 0;
static int              g_read_idx    = 0;
static int              g_filled_count = 0;
static atomic_int       g_running     = 1;

// Vertex shader source
const char *vertex_shader_source = 
"#version 330 core\n"
"layout (location = 0) in vec3 aPos;\n"
"layout (location = 1) in vec2 aTexCoord;\n"
"out vec2 TexCoord;\n"
"void main()\n"
"{\n"
"    gl_Position = vec4(aPos, 1.0);\n"
"    TexCoord = aTexCoord;\n"
"}\n";

// Fragment shader source
const char *fragment_shader_source = 
"#version 330 core\n"
"out vec4 FragColor;\n"
"in vec2 TexCoord;\n"
"uniform sampler2D ourTexture;\n"
"void main()\n"
"{\n"
"    FragColor = texture(ourTexture, TexCoord);\n"
"}\n";

// Quad vertices (position + texture coordinates)
float vertices[] = {
    // positions          // texture coords
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f, // top right
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f, // bottom right
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f, // bottom left
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f  // top left 
};

unsigned int indices[] = {
    0, 1, 3, // first triangle
    1, 2, 3  // second triangle
};

void cleanup_and_exit(int sig __attribute__((unused))) {
    /* Async-signal-safe: only use write(), close(), atomic ops, _exit() */
    const char msg[] = "\nCaught signal — force exiting.\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    if (g_iwfg_fd >= 0) {
        close(g_iwfg_fd);   /* unblocks any blocked ioctl in DMA thread */
        g_iwfg_fd = -1;
    }
    atomic_store(&g_running, 0);
    pthread_cond_broadcast(&g_cond_filled);
    pthread_cond_broadcast(&g_cond_free);
    _exit(0);
}

void error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

void key_callback(GLFWwindow* window, int key, int scancode __attribute__((unused)), int action, int mods __attribute__((unused))) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    if (key == GLFW_KEY_P && action == GLFW_PRESS) {
        g_enable_performance_monitoring = !g_enable_performance_monitoring;
        printf("Performance monitoring %s\n", g_enable_performance_monitoring ? "enabled" : "disabled");
    }
}

void framebuffer_size_callback(GLFWwindow* window __attribute__((unused)), int width, int height) {
    glViewport(0, 0, width, height);
}

GLuint compile_shader(const char* source, GLenum shader_type) {
    GLuint shader = glCreateShader(shader_type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    int success;
    char info_log[512];
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(shader, 512, NULL, info_log);
        fprintf(stderr, "Shader compilation failed: %s\n", info_log);
        return 0;
    }
    
    return shader;
}

GLuint create_shader_program() {
    GLuint vertex_shader = compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
    GLuint fragment_shader = compile_shader(fragment_shader_source, GL_FRAGMENT_SHADER);
    
    if (!vertex_shader || !fragment_shader) {
        return 0;
    }
    
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    
    int success;
    char info_log[512];
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(program, 512, NULL, info_log);
        fprintf(stderr, "Shader program linking failed: %s\n", info_log);
        return 0;
    }
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return program;
}

int setup_opengl() {
    // Generate and bind VAO
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    
    // Generate and bind VBO
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    // Generate and bind EBO
    GLuint ebo;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Texture coordinate attribute
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Create shader program
    g_shader_program = create_shader_program();
    if (!g_shader_program) {
        return -1;
    }
    
    // Generate texture
    glGenTextures(1, &g_texture_id);
    glBindTexture(GL_TEXTURE_2D, g_texture_id);
    
    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    // RGB rows are 3 bytes/pixel — disable the default 4-byte row alignment
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
    // Enable vsync
    glfwSwapInterval(1);
    
    return 0;
}

static void print_performance_stats(void) {
    if (!g_enable_performance_monitoring || g_frame_count < 2) return;
    
    uint64_t total_frame_time = 0;
    uint64_t total_wait_time = 0;
    uint64_t total_render_time = 0;
    uint64_t min_frame_time = UINT64_MAX;
    uint64_t max_frame_time = 0;
    uint64_t min_wait_time = UINT64_MAX;
    uint64_t max_wait_time = 0;
    uint64_t min_render_time = UINT64_MAX;
    uint64_t max_render_time = 0;
    
    for (int i = 1; i < g_frame_count; i++) {
        uint64_t frame_time = (g_frame_times[i].tv_sec - g_frame_times[i-1].tv_sec) * 1000000000ULL +
                             (g_frame_times[i].tv_nsec - g_frame_times[i-1].tv_nsec);
        uint64_t wait_time = g_wait_times[i].tv_sec * 1000000000ULL + g_wait_times[i].tv_nsec;
        uint64_t render_time = g_render_times[i].tv_sec * 1000000000ULL + g_render_times[i].tv_nsec;
        
        total_frame_time += frame_time;
        total_wait_time += wait_time;
        total_render_time += render_time;
        
        if (frame_time < min_frame_time) min_frame_time = frame_time;
        if (frame_time > max_frame_time) max_frame_time = frame_time;
        if (wait_time < min_wait_time) min_wait_time = wait_time;
        if (wait_time > max_wait_time) max_wait_time = wait_time;
        if (render_time < min_render_time) min_render_time = render_time;
        if (render_time > max_render_time) max_render_time = render_time;
    }
    
    uint64_t avg_frame_time = total_frame_time / (g_frame_count - 1);
    uint64_t avg_wait_time = total_wait_time / (g_frame_count - 1);
    uint64_t avg_render_time = total_render_time / (g_frame_count - 1);
    double fps = 1000000000.0 / avg_frame_time;
    
    printf("Performance Stats (last %d frames):\n", g_frame_count - 1);
    printf("  Average frame time: %.2f ms (%.1f FPS)\n", avg_frame_time / 1000000.0, fps);
    printf("  Average wait time: %.2f ms (%.1f%% of frame)\n", 
           avg_wait_time / 1000000.0, (avg_wait_time * 100.0) / avg_frame_time);
    printf("  Average render time: %.2f ms (%.1f%% of frame)\n", 
           avg_render_time / 1000000.0, (avg_render_time * 100.0) / avg_frame_time);
    printf("  Min/Max frame time: %.2f/%.2f ms\n", 
           min_frame_time / 1000000.0, max_frame_time / 1000000.0);
    printf("  Min/Max wait time: %.2f/%.2f ms\n", 
           min_wait_time / 1000000.0, max_wait_time / 1000000.0);
    printf("  Min/Max render time: %.2f/%.2f ms\n", 
           min_render_time / 1000000.0, max_render_time / 1000000.0);
    printf("  OpenGL texture streaming - NVIDIA GPU accelerated!\n");
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -p, --performance    Enable performance monitoring and statistics\n");
    printf("  -f, --fullscreen     Run in fullscreen mode\n");
    printf("  -w, --windowed       Run in windowed mode (default)\n");
    printf("  -h, --help          Show this help message\n");
    printf("\n");
    printf("Runtime controls:\n");
    printf("  ESC                 Exit application\n");
    printf("  P                   Toggle performance monitoring\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                  Run in windowed mode\n", prog_name);
    printf("  %s -p               Run with performance monitoring enabled\n", prog_name);
    printf("  %s -f               Run in fullscreen mode\n", prog_name);
}

/*
 * DMA producer thread.
 *
 * Continuously fills ring-buffer slots with DMA (C2H) data.
 * Only blocks when ALL slots are filled (ring full); otherwise it grabs
 * the next free slot and starts the next ioctl immediately — ensuring the
 * DMA engine is never idle while render capacity exists.
 */
static void *dma_thread(void *arg) {
    (void)arg;
    struct iwfg_user_dma_req dma_req;

    while (atomic_load(&g_running)) {
        /* Wait until at least one slot is free */
        pthread_mutex_lock(&g_ring_mutex);
        while (g_filled_count == NUM_BUFFERS && atomic_load(&g_running)) {
            pthread_cond_wait(&g_cond_free, &g_ring_mutex);
        }
        if (!atomic_load(&g_running)) {
            pthread_mutex_unlock(&g_ring_mutex);
            break;
        }
        int slot = g_write_idx;
        pthread_mutex_unlock(&g_ring_mutex);

        /* DMA transfer into slot — performed outside the lock (slow blocking op) */
        dma_req.buf       = g_buffers[slot];
        dma_req.size      = g_frame_size;
        dma_req.direction = IWFG_DIR_C2H_BUF;
        dma_req.offset    = 0;
        dma_req.flags     = 0;

        if (ioctl(g_iwfg_fd, IWFG_IOCTL_DMA_DATA, &dma_req) < 0) {
            if (errno == EAGAIN) {
                /* No frame available yet — retry immediately */
                continue;
            }
            if (errno == EBADF || errno == EINTR) {
                /* fd closed by signal handler — exit cleanly */
                break;
            }
            perror("DMA thread: IWFG_IOCTL_DMA_DATA failed");
            atomic_store(&g_running, 0);
            pthread_cond_broadcast(&g_cond_filled);
            break;
        }

        /* Mark slot filled and advance write pointer */
        pthread_mutex_lock(&g_ring_mutex);
        g_buf_state[slot] = BUF_FILLED;
        g_write_idx = (g_write_idx + 1) % NUM_BUFFERS;
        g_filled_count++;
        pthread_cond_signal(&g_cond_filled);
        pthread_mutex_unlock(&g_ring_mutex);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    bool fullscreen = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--performance") == 0) {
            g_enable_performance_monitoring = true;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen = true;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--windowed") == 0) {
            fullscreen = false;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    printf("IWFG OpenGL Display Example\n");
    printf("===========================\n");
    
    // Initialize GLFW
    glfwSetErrorCallback(error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }
    
    // Configure GLFW
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    
    // Open IWFG device first to query stream parameters
    g_iwfg_fd = open("/dev/iwfg0", O_RDWR);
    if (g_iwfg_fd < 0) {
        perror("Failed to open IWFG device");
        glfwTerminate();
        return -1;
    }
    
    // Query stream parameters from the driver
    struct iwfg_stream_params stream_params;
    if (ioctl(g_iwfg_fd, IWFG_IOCTL_QUERY_STREAM_PARAMS, &stream_params) < 0) {
        perror("IWFG_IOCTL_QUERY_STREAM_PARAMS failed");
        close(g_iwfg_fd);
        glfwTerminate();
        return -1;
    }
    g_width = stream_params.width;
    g_height = stream_params.height;
    g_bpp = stream_params.bpp;
    g_frame_size = g_width * g_height * g_bpp;
    
    printf("Stream parameters: %ux%u, %u bytes/pixel\n", g_width, g_height, g_bpp);
    printf("Frame size: %u bytes\n", g_frame_size);
    printf("Mode: %s\n", fullscreen ? "Fullscreen" : "Windowed");
    if (g_enable_performance_monitoring) {
        printf("Performance monitoring: ENABLED\n");
    } else {
        printf("Performance monitoring: DISABLED (press P to toggle)\n");
    }
    printf("Controls: ESC to exit, P to toggle performance stats\n");
    printf("\n");
    
    // Create window
    GLFWmonitor* monitor = fullscreen ? glfwGetPrimaryMonitor() : NULL;
    int window_width = fullscreen ? (int)g_width : (int)g_width / 2;  // Half resolution for windowed mode
    int window_height = fullscreen ? (int)g_height : (int)g_height / 2;
    
    g_window = glfwCreateWindow(window_width, window_height, 
                               "IWFG OpenGL Display - Frame Grabber 4K", monitor, NULL);
    if (!g_window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }
    
    glfwMakeContextCurrent(g_window);
    glfwSetKeyCallback(g_window, key_callback);
    glfwSetFramebufferSizeCallback(g_window, framebuffer_size_callback);
    
    // Check OpenGL version
    printf("OpenGL Vendor: %s\n", glGetString(GL_VENDOR));
    printf("OpenGL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("OpenGL Version: %s\n", glGetString(GL_VERSION));
    printf("\n");
    
    // Setup OpenGL
    if (setup_opengl() < 0) {
        fprintf(stderr, "Failed to setup OpenGL\n");
        return -1;
    }
    
    // Allocate page-aligned buffers using mmap for DMA compatibility
    for (int i = 0; i < NUM_BUFFERS; i++) {
        // Use mmap with MAP_POPULATE | MAP_LOCKED for reliable DMA with IOMMU
        g_buffers[i] = mmap(NULL, g_frame_size, 
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_LOCKED,
                            -1, 0);
        if (g_buffers[i] == MAP_FAILED) {
            // Fallback without MAP_LOCKED if it fails (e.g., ulimit)
            perror("mmap with MAP_LOCKED failed, trying without");
            g_buffers[i] = mmap(NULL, g_frame_size, 
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                -1, 0);
            if (g_buffers[i] == MAP_FAILED) {
                perror("Buffer mmap failed");
                return -1;
            }
            if (mlock(g_buffers[i], g_frame_size) != 0) {
                perror("Warning: mlock also failed - DMA may have issues");
            }
        }
        
        // Prevent fork from copying these pages (important for DMA)
        madvise(g_buffers[i], g_frame_size, MADV_DONTFORK);
        
        // Fill with a test pattern initially (also ensures all pages are write-faulted)
        uint8_t *pixels = (uint8_t*)g_buffers[i];
        for (uint32_t y = 0; y < g_height; y++) {
            for (uint32_t x = 0; x < g_width; x++) {
                // Create a test pattern to verify the display
                uint8_t r = (x * 255) / g_width;
                uint8_t gv = (y * 255) / g_height;
                uint8_t b = ((x + y) * 255) / (g_width + g_height);
                
                uint32_t idx = (y * g_width + x) * 3;
                pixels[idx + 0] = r;  // RGB888
                pixels[idx + 1] = gv;
                pixels[idx + 2] = b;
            }
        }
        __sync_synchronize();  // Memory barrier to ensure all writes complete

		// struct iwfg_user_buf user_buf = {
		// 	.buf = g_buffers[i],
		// 	.size = g_frame_size,
		// 	.is_chunked = false,
		// 	.chunk_size = 0
		// };

		// if (ioctl(g_iwfg_fd, IWFG_IOCTL_ADD_BUF, &user_buf) < 0) {
		// 	perror("IWFG_IOCTL_ADD_BUF failed");
		// 	return -1;
		// }
        
        printf("Allocated buffer %d: %p (%u bytes)\n", i, g_buffers[i], g_frame_size);
    }
    
    printf("Buffers added to IWFG driver\n");
    
    // Start streaming
    // if (ioctl(g_iwfg_fd, IWFG_IOCTL_STREAM_START) < 0) {
    //     perror("IWFG_IOCTL_STREAM_START failed");
    //     return -1;
    // }
    
    printf("Streaming started. Frame grabber -> OpenGL texture -> NVIDIA GPU rendering\n");
    printf("Window ready - displaying 4K RGBA frames from frame grabber\n");

    // Initialize ring buffer state
    for (int i = 0; i < NUM_BUFFERS; i++) {
        g_buf_state[i] = BUF_FREE;
    }

    // Launch DMA producer thread
    pthread_t dma_tid;
    if (pthread_create(&dma_tid, NULL, dma_thread, NULL) != 0) {
        perror("Failed to create DMA thread");
        return -1;
    }

    printf("DMA producer thread started (%d ring slots). DMA runs continuously until ring is full.\n",
           NUM_BUFFERS);

    // Main render loop (consumer)
    // The DMA thread fills slots as fast as the hardware delivers frames.
    // This thread waits for a filled slot, renders it, then releases it back.
    while (!glfwWindowShouldClose(g_window) && atomic_load(&g_running)) {
        struct timespec wait_start, wait_end, render_start, render_end;

        // Performance monitoring: frame timestamp
        if (g_enable_performance_monitoring && g_frame_count < 100) {
            clock_gettime(CLOCK_MONOTONIC, &g_frame_times[g_frame_count]);
        }

        // Wait for a filled buffer slot from the DMA thread
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &wait_start);
        }
        pthread_mutex_lock(&g_ring_mutex);
        while (g_filled_count == 0 && atomic_load(&g_running) &&
               !glfwWindowShouldClose(g_window)) {
            pthread_cond_wait(&g_cond_filled, &g_ring_mutex);
        }
        if (!atomic_load(&g_running) || glfwWindowShouldClose(g_window)) {
            pthread_mutex_unlock(&g_ring_mutex);
            break;
        }
        int slot = g_read_idx;
        pthread_mutex_unlock(&g_ring_mutex);

        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &wait_end);
            if (g_frame_count < 100) {
                g_wait_times[g_frame_count].tv_sec  = wait_end.tv_sec  - wait_start.tv_sec;
                g_wait_times[g_frame_count].tv_nsec = wait_end.tv_nsec - wait_start.tv_nsec;
                if (g_wait_times[g_frame_count].tv_nsec < 0) {
                    g_wait_times[g_frame_count].tv_sec--;
                    g_wait_times[g_frame_count].tv_nsec += 1000000000;
                }
            }
        }

        // Render frame (outside lock — does not block the DMA thread)
        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &render_start);
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindTexture(GL_TEXTURE_2D, g_texture_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, g_width, g_height, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, g_buffers[slot]);

        glUseProgram(g_shader_program);
        glBindVertexArray(g_vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        glfwSwapBuffers(g_window);

        if (g_enable_performance_monitoring) {
            clock_gettime(CLOCK_MONOTONIC, &render_end);
            if (g_frame_count < 100) {
                g_render_times[g_frame_count].tv_sec  = render_end.tv_sec  - render_start.tv_sec;
                g_render_times[g_frame_count].tv_nsec = render_end.tv_nsec - render_start.tv_nsec;
                if (g_render_times[g_frame_count].tv_nsec < 0) {
                    g_render_times[g_frame_count].tv_sec--;
                    g_render_times[g_frame_count].tv_nsec += 1000000000;
                }
                g_frame_count++;
            }
        }

        // Release the consumed slot back to the DMA producer
        pthread_mutex_lock(&g_ring_mutex);
        g_buf_state[slot] = BUF_FREE;
        g_read_idx = (g_read_idx + 1) % NUM_BUFFERS;
        g_filled_count--;
        pthread_cond_signal(&g_cond_free);
        pthread_mutex_unlock(&g_ring_mutex);

        g_total_frames++;

        // Performance stats output
        if (g_enable_performance_monitoring && g_total_frames % 60 == 0) {
            if (g_frame_count > 0) {
                uint64_t wait_time_ns   = g_wait_times[g_frame_count-1].tv_sec * 1000000000ULL +
                                          g_wait_times[g_frame_count-1].tv_nsec;
                uint64_t render_time_ns = g_render_times[g_frame_count-1].tv_sec * 1000000000ULL +
                                          g_render_times[g_frame_count-1].tv_nsec;
                printf("Frame %u - Wait: %.2fms, Render: %.2fms\n",
                       g_total_frames,
                       wait_time_ns  / 1000000.0,
                       render_time_ns / 1000000.0);
            }
            print_performance_stats();
        } else if (!g_enable_performance_monitoring && g_total_frames % 300 == 0) {
            printf("Rendered %u frames...\n", g_total_frames);
        }

        glfwPollEvents();
    }

    // Graceful shutdown (ESC key or window close)
    printf("\nTotal frames rendered: %u\n", g_total_frames);
    atomic_store(&g_running, 0);
    pthread_cond_broadcast(&g_cond_free);
    pthread_cond_broadcast(&g_cond_filled);
    if (g_iwfg_fd >= 0) {
        close(g_iwfg_fd);
        g_iwfg_fd = -1;
    }
    pthread_join(dma_tid, NULL);

    // Clean up OpenGL resources
    if (g_texture_id)     glDeleteTextures(1, &g_texture_id);
    if (g_vao)            glDeleteVertexArrays(1, &g_vao);
    if (g_vbo)            glDeleteBuffers(1, &g_vbo);
    if (g_shader_program) glDeleteProgram(g_shader_program);
    if (g_window)         glfwDestroyWindow(g_window);
    glfwTerminate();

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_buffers[i]) {
            munlock(g_buffers[i], g_frame_size);
            munmap(g_buffers[i], g_frame_size);
        }
    }

    return 0;
}
