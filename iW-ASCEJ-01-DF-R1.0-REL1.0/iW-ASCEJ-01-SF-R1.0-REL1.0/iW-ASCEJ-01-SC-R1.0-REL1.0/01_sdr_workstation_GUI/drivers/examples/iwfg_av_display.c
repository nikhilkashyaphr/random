/*
 * IWFG Audio+Video Display Example
 *
 * Captures video frames from /dev/iwfg0 (RGBA8888, up to 4K) and displays
 * them in an OpenGL window using NVIDIA GPU acceleration.  Simultaneously
 * captures single-channel PCM audio from /dev/iwfg1 in 23040-byte DMA
 * buffers and plays it through the system default ALSA output device.
 *
 * Architecture:
 *   Main thread  – GLFW event loop + OpenGL rendering (video)
 *   Audio thread – DMA capture from /dev/iwfg1 → ALSA playback
 *
 * Audio parameters (HDMI typical defaults, change with -r / -b / -A):
 *   Sample rate : 48000 Hz
 *   Format      : S24_3LE (24-bit signed LE, packed 3 bytes/sample)
 *   Channels    : 2  (stereo)
 *   Buffer size : 8192 frames (~171 ms); bytes = frames x channels x bytes/sample
 *
 * Controls:
 *   ESC  – exit
 *   P    – toggle per-frame performance statistics
 *   M    – mute / unmute audio
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

/* OpenGL / GLFW */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GLFW/glfw3.h>

/* ALSA */
#include <alsa/asoundlib.h>

#include "iwfg_user.h"

/* ─── constants ──────────────────────────────────────────────────────────── */

#define NUM_VIDEO_BUFFERS   8
#define NUM_AUDIO_BUFFERS   4
/* Fixed frame count per DMA transfer: ~171 ms at 48 kHz.
 * Actual bytes = AUDIO_BUF_FRAMES x bytes_per_frame, always a multiple of 4096:
 *   2ch S24_3LE -> 8192 x 6 = 49152 B = 12 x 4096  (IOMMU-safe, 4K-aligned)
 *   1ch S24_3LE -> 8192 x 3 = 24576 B =  6 x 4096  (IOMMU-safe, 4K-aligned) */
#define AUDIO_BUF_FRAMES    8192

/* ─── audio configuration (may be overridden via CLI) ───────────────────── */

static unsigned int  g_audio_rate     = 48000;
static snd_pcm_format_t g_audio_fmt   = SND_PCM_FORMAT_S24_3LE;
static unsigned int  g_audio_channels = 2;
static const char   *g_alsa_device    = "plughw:0,0"; /* may be overridden by fallback logic */

/* Derived: set after format/channel count are known */
static snd_pcm_uframes_t g_audio_frames_per_buf = 0;
static size_t            g_audio_buf_size        = 0; /* bytes = AUDIO_BUF_FRAMES x bpf */

/* ─── video/stream parameters (queried from driver) ─────────────────────── */

static uint32_t g_width      = 0;
static uint32_t g_height     = 0;
static uint32_t g_bpp        = 0;
static uint32_t g_frame_size = 0;

/* ─── global state ───────────────────────────────────────────────────────── */

static int         g_video_fd  = -1;
static int         g_audio_fd  = -1;
static void       *g_video_bufs[NUM_VIDEO_BUFFERS];
static void       *g_audio_bufs[NUM_AUDIO_BUFFERS];

static GLFWwindow *g_window         = NULL;
static GLuint      g_texture_id     = 0;
static GLuint      g_shader_program = 0;
static GLuint      g_vao            = 0;
static GLuint      g_vbo            = 0;

static snd_pcm_t  *g_alsa_pcm       = NULL;

/* ─── shared state (atomic) ─────────────────────────────────────────────── */

static atomic_int  g_running         = 1;   /* 0 → all threads should stop   */
static atomic_int  g_audio_muted     = 0;   /* 1 → audio thread skips ALSA   */
static atomic_uint g_total_video_frames = 0;
static atomic_uint g_total_audio_bufs   = 0;

/* ─── video ring buffer state for DMA producer thread ───────────────────── */
typedef enum { VID_BUF_FREE, VID_BUF_FILLED } vid_buf_state_t;
static vid_buf_state_t  g_vid_buf_state[NUM_VIDEO_BUFFERS];
static pthread_mutex_t  g_vid_ring_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_vid_cond_filled = PTHREAD_COND_INITIALIZER;
static pthread_cond_t   g_vid_cond_free   = PTHREAD_COND_INITIALIZER;
static int              g_vid_write_idx   = 0;
static int              g_vid_read_idx    = 0;
static int              g_vid_filled_count = 0;

/* ─── performance monitoring ─────────────────────────────────────────────── */

#define PERF_WINDOW 100

static struct timespec g_frame_times[PERF_WINDOW];
static struct timespec g_wait_times[PERF_WINDOW];
static struct timespec g_render_times[PERF_WINDOW];
static int  g_frame_count = 0;
static bool g_perf_enabled = false;

/* ─── shaders ────────────────────────────────────────────────────────────── */

static const char *vertex_shader_src =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec2 aTexCoord;\n"
    "out vec2 TexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 1.0);\n"
    "    TexCoord = aTexCoord;\n"
    "}\n";

static const char *fragment_shader_src =
    "#version 330 core\n"
    "out vec4 FragColor;\n"
    "in vec2 TexCoord;\n"
    "uniform sampler2D ourTexture;\n"
    "void main() {\n"
    "    FragColor = texture(ourTexture, TexCoord);\n"
    "}\n";

/* Full-screen quad: position (xyz) + texcoord (uv) */
static float g_vertices[] = {
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f,
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,
};
static unsigned int g_indices[] = { 0, 1, 3,   1, 2, 3 };

/* ═══════════════════════════════════════════════════════════════════════════
 * Cleanup
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cleanup(void)
{
    atomic_store(&g_running, 0);

    /* Video device */
    if (g_video_fd >= 0) { close(g_video_fd); g_video_fd = -1; }

    /* Audio device */
    if (g_audio_fd >= 0) { close(g_audio_fd); g_audio_fd = -1; }

    /* ALSA */
    if (g_alsa_pcm) {
        snd_pcm_drain(g_alsa_pcm);
        snd_pcm_close(g_alsa_pcm);
        g_alsa_pcm = NULL;
    }

    /* OpenGL */
    if (g_texture_id)     { glDeleteTextures(1,      &g_texture_id);     g_texture_id     = 0; }
    if (g_vao)            { glDeleteVertexArrays(1,  &g_vao);            g_vao            = 0; }
    if (g_vbo)            { glDeleteBuffers(1,        &g_vbo);            g_vbo            = 0; }
    if (g_shader_program) { glDeleteProgram(g_shader_program);            g_shader_program = 0; }

    /* GLFW */
    if (g_window) { glfwDestroyWindow(g_window); g_window = NULL; }
    glfwTerminate();

    /* Video buffers */
    for (int i = 0; i < NUM_VIDEO_BUFFERS; i++) {
        if (g_video_bufs[i]) {
            munlock(g_video_bufs[i], g_frame_size);
            munmap(g_video_bufs[i], g_frame_size);
            g_video_bufs[i] = NULL;
        }
    }

    /* Audio buffers */
    for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        if (g_audio_bufs[i]) {
            free(g_audio_bufs[i]);
            g_audio_bufs[i] = NULL;
        }
    }

    printf("Total video frames: %u\n", atomic_load(&g_total_video_frames));
    printf("Total audio buffers: %u\n", atomic_load(&g_total_audio_bufs));
}

static void signal_handler(int sig __attribute__((unused)))
{
    /* Close both device fds while threads may be blocked inside ioctl().
     * This is async-signal-safe and causes the kernel to return EBADF
     * immediately, unblocking the video render loop and audio thread. */
    if (g_audio_fd >= 0) { close(g_audio_fd); g_audio_fd = -1; }
    if (g_video_fd >= 0) { close(g_video_fd); g_video_fd = -1; }

    atomic_store(&g_running, 0);
    pthread_cond_broadcast(&g_vid_cond_filled);
    pthread_cond_broadcast(&g_vid_cond_free);

    write(STDOUT_FILENO, "\nSignal received – exiting.\n", 28);
    /* Force exit immediately – avoids blocking on pthread_join() or
     * snd_pcm_writei() which cannot be interrupted any other way. */
    _exit(0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GLFW callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void glfw_error_cb(int error, const char *desc)
{
    fprintf(stderr, "GLFW error %d: %s\n", error, desc);
}

static void key_cb(GLFWwindow *win, int key, int scancode __attribute__((unused)),
                   int action, int mods __attribute__((unused)))
{
    if (action != GLFW_PRESS) return;

    switch (key) {
    case GLFW_KEY_ESCAPE:
        glfwSetWindowShouldClose(win, GLFW_TRUE);
        break;
    case GLFW_KEY_P:
        g_perf_enabled = !g_perf_enabled;
        printf("Performance monitoring %s\n", g_perf_enabled ? "ON" : "OFF");
        break;
    case GLFW_KEY_M:
        {
            int muted = atomic_fetch_xor(&g_audio_muted, 1) ^ 1;
            printf("Audio %s\n", muted ? "muted" : "unmuted");
        }
        break;
    }
}

static void framebuffer_size_cb(GLFWwindow *win __attribute__((unused)),
                                int w, int h)
{
    glViewport(0, 0, w, h);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * OpenGL helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static GLuint compile_shader(const char *src, GLenum type)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    int ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint create_shader_program(void)
{
    GLuint vs = compile_shader(vertex_shader_src,   GL_VERTEX_SHADER);
    GLuint fs = compile_shader(fragment_shader_src, GL_FRAGMENT_SHADER);
    if (!vs || !fs) return 0;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    int ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "Shader link error: %s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static int setup_opengl(void)
{
    /* VAO */
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);

    /* VBO */
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertices), g_vertices, GL_STATIC_DRAW);

    /* EBO (local – not stored globally since it lives in the VAO) */
    GLuint ebo;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(g_indices), g_indices, GL_STATIC_DRAW);

    /* Position: location 0, 3 floats */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);

    /* Texcoord: location 1, 2 floats */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    /* Shader */
    g_shader_program = create_shader_program();
    if (!g_shader_program) return -1;

    /* Texture */
    glGenTextures(1, &g_texture_id);
    glBindTexture(GL_TEXTURE_2D, g_texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glfwSwapInterval(1); /* vsync */
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ALSA helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int setup_alsa(void)
{
    /* Try the requested device first, then fall back through common names.
     * plughw:X,Y fails with EBUSY when PulseAudio/PipeWire holds the hw
     * device open.  "pulse" / "pipewire" work via the session daemon even
     * under sudo (if the user daemon socket is reachable), while
     * "plughw:0,0" works when the hw device is free (e.g. PA not running). */
    const char *try_devs[] = {
        g_alsa_device,   /* user-requested / default */
        "pulse",
        "pipewire",
        "plughw:0,0",
        "hw:0,0",
        NULL
    };

    int err = -1;
    for (int d = 0; try_devs[d] != NULL; d++) {
        /* skip duplicates */
        if (d > 0 && strcmp(try_devs[d], g_alsa_device) == 0) continue;

        err = snd_pcm_open(&g_alsa_pcm, try_devs[d], SND_PCM_STREAM_PLAYBACK, 0);
        if (err == 0) {
            if (d > 0)
                printf("ALSA: '%s' busy/unavailable, using '%s' instead\n",
                       g_alsa_device, try_devs[d]);
            g_alsa_device = try_devs[d];
            break;
        }
        fprintf(stderr, "ALSA open '%s' failed: %s\n",
                try_devs[d], snd_strerror(err));
    }
    if (err < 0) {
        fprintf(stderr, "ALSA: all devices failed\n");
        return -1;
    }

    /* Frame count is fixed; byte size scales with format/channels. */
    g_audio_frames_per_buf = AUDIO_BUF_FRAMES;  /* 8192 frames ~171 ms at 48 kHz */

    /* Ring buffer = 8x DMA period -> ~1365 ms of headroom.
     * Absorbs DMA jitter; combined with 6-period pre-fill makes underruns
     * extremely rare regardless of channel count. */
    snd_pcm_uframes_t period_size = g_audio_frames_per_buf;
    snd_pcm_uframes_t buffer_size = g_audio_frames_per_buf * 8;

    snd_pcm_hw_params_t *hwp;
    snd_pcm_hw_params_alloca(&hwp);
    snd_pcm_hw_params_any(g_alsa_pcm, hwp);

    if ((err = snd_pcm_hw_params_set_access(g_alsa_pcm, hwp,
                    SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(g_alsa_pcm, hwp, g_audio_fmt)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(g_alsa_pcm, hwp, g_audio_channels)) < 0 ||
        (err = snd_pcm_hw_params_set_rate(g_alsa_pcm, hwp, g_audio_rate, 0)) < 0 ||
        (err = snd_pcm_hw_params_set_period_size_near(g_alsa_pcm, hwp,
                    &period_size, NULL)) < 0 ||
        (err = snd_pcm_hw_params_set_buffer_size_near(g_alsa_pcm, hwp,
                    &buffer_size)) < 0 ||
        (err = snd_pcm_hw_params(g_alsa_pcm, hwp)) < 0) {
        fprintf(stderr, "ALSA hw_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(g_alsa_pcm);
        g_alsa_pcm = NULL;
        return -1;
    }

    /* Start playback only once 2 periods are queued – avoids immediate underrun. */
    snd_pcm_sw_params_t *swp;
    snd_pcm_sw_params_alloca(&swp);
    snd_pcm_sw_params_current(g_alsa_pcm, swp);
    snd_pcm_sw_params_set_start_threshold(g_alsa_pcm, swp, period_size * 2);
    snd_pcm_sw_params_set_avail_min(g_alsa_pcm, swp, period_size);
    snd_pcm_sw_params(g_alsa_pcm, swp);

    printf("ALSA: device='%s'  rate=%u Hz  fmt=%s  ch=%u\n"
           "      period=%lu frames (%lu ms)  buffer=%lu frames (%lu ms)\n",
           g_alsa_device, g_audio_rate,
           snd_pcm_format_name(g_audio_fmt), g_audio_channels,
           (unsigned long)period_size,
           (unsigned long)(period_size * 1000 / g_audio_rate),
           (unsigned long)buffer_size,
           (unsigned long)(buffer_size * 1000 / g_audio_rate));
    return 0;
}

/* Write one DMA buffer worth of PCM data to ALSA, handling XRUN recovery.
 * On underrun: injects silence to restore ring-buffer headroom before
 * resuming real data – eliminates click/pop and prevents re-underrun loops. */
static int alsa_write_buf(const void *data, snd_pcm_uframes_t frames)
{
    snd_pcm_sframes_t written;
    const char *ptr = (const char *)data;
    snd_pcm_uframes_t remaining = frames;
    int bpf = (snd_pcm_format_width(g_audio_fmt) / 8) * (int)g_audio_channels;

    while (remaining > 0) {
        written = snd_pcm_writei(g_alsa_pcm, ptr, remaining);
        if (written == -EAGAIN) {
            snd_pcm_wait(g_alsa_pcm, 100);
            continue;
        }
        if (written < 0) {
            int err = snd_pcm_recover(g_alsa_pcm, (int)written, 0);
            if (err < 0) {
                fprintf(stderr, "ALSA write error (unrecoverable): %s\n",
                        snd_strerror(err));
                return -1;
            }
            /* Ring buffer is empty after recovery – inject silence to rebuild
             * headroom before writing real audio, preventing immediate re-underrun. */
            size_t silence_bytes = (size_t)g_audio_frames_per_buf * (size_t)bpf;
            void *silence = calloc(1, silence_bytes);
            if (silence) {
                for (int p = 0; p < 6; p++) {
                    snd_pcm_sframes_t r = snd_pcm_writei(
                            g_alsa_pcm, silence, g_audio_frames_per_buf);
                    if (r < 0) snd_pcm_recover(g_alsa_pcm, (int)r, 1);
                }
                free(silence);
            }
            /* Restart from the beginning of the buffer to avoid a truncated period. */
            ptr       = (const char *)data;
            remaining = frames;
            continue;
        }
        ptr       += (size_t)written * (size_t)bpf;
        remaining -= (snd_pcm_uframes_t)written;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio thread
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *audio_thread(void *arg __attribute__((unused)))
{
    uint32_t buf_idx = 0;

    printf("[audio] thread started  (device=/dev/iwfg1, buf=%zu bytes, %u ch)\n",
           g_audio_buf_size, g_audio_channels);

    /* Pre-fill 6 periods of silence (~1024 ms) before first real DMA buffer.
     * Gives the ring buffer enough headroom that DMA jitter won't drain it. */
    if (g_alsa_pcm) {
        size_t silence_bytes = (size_t)g_audio_frames_per_buf *
                               (size_t)((snd_pcm_format_width(g_audio_fmt) / 8) *
                                        (int)g_audio_channels);
        void *silence = calloc(1, silence_bytes);
        if (silence) {
            for (int p = 0; p < 6; p++)
                alsa_write_buf(silence, g_audio_frames_per_buf);
            free(silence);
        }
    }

    while (atomic_load(&g_running)) {
        /* DMA: card → host */
        struct iwfg_user_dma_req req = {
            .buf       = g_audio_bufs[buf_idx],
            .size      = g_audio_buf_size,
            .offset    = 0,
            .direction = IWFG_DIR_C2H_BUF,
            .flags     = 0,
        };

        if (ioctl(g_audio_fd, IWFG_IOCTL_DMA_DATA, &req) < 0) {
            if (errno == EAGAIN) {
                /* No audio data yet – brief spin is fine at audio rates */
                usleep(1000);
                continue;
            }
            if (errno == EINTR || errno == EBADF)
                break;
            perror("[audio] IWFG_IOCTL_DMA_DATA");
            break;
        }

        atomic_fetch_add(&g_total_audio_bufs, 1);

        /* Playback (skip if muted) */
        if (!atomic_load(&g_audio_muted) && g_alsa_pcm) {
            if (alsa_write_buf(g_audio_bufs[buf_idx], g_audio_frames_per_buf) < 0) {
                fprintf(stderr, "[audio] ALSA write failed – restarting PCM\n");
                snd_pcm_prepare(g_alsa_pcm);
            }
        }

        buf_idx = (buf_idx + 1) % NUM_AUDIO_BUFFERS;
    }

    printf("[audio] thread exiting\n");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Performance reporting
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_perf_stats(void)
{
    if (!g_perf_enabled || g_frame_count < 2) return;

    uint64_t total_ft = 0, total_wt = 0, total_rt = 0;
    uint64_t min_ft = UINT64_MAX, max_ft = 0;

    for (int i = 1; i < g_frame_count; i++) {
        uint64_t ft = (uint64_t)(g_frame_times[i].tv_sec  - g_frame_times[i-1].tv_sec)  * 1000000000ULL
                    + (uint64_t)(g_frame_times[i].tv_nsec - g_frame_times[i-1].tv_nsec);
        uint64_t wt = (uint64_t)g_wait_times[i].tv_sec   * 1000000000ULL + (uint64_t)g_wait_times[i].tv_nsec;
        uint64_t rt = (uint64_t)g_render_times[i].tv_sec * 1000000000ULL + (uint64_t)g_render_times[i].tv_nsec;

        total_ft += ft; total_wt += wt; total_rt += rt;
        if (ft < min_ft) min_ft = ft;
        if (ft > max_ft) max_ft = ft;
    }

    int n = g_frame_count - 1;
    uint64_t avg_ft = total_ft / (uint64_t)n;
    double fps = 1e9 / (double)avg_ft;

    printf("--- video perf (last %d frames) ---\n", n);
    printf("  avg frame: %.2f ms  (%.1f FPS)\n", avg_ft / 1e6, fps);
    printf("  avg wait:  %.2f ms  avg render: %.2f ms\n",
           (total_wt / (uint64_t)n) / 1e6,
           (total_rt / (uint64_t)n) / 1e6);
    printf("  min/max frame: %.2f / %.2f ms\n", min_ft / 1e6, max_ft / 1e6);
    printf("  audio buffers captured: %u\n", atomic_load(&g_total_audio_bufs));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Usage
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Video DMA producer thread.
 *
 * Continuously fills video ring-buffer slots with DMA (C2H) data.
 * Only blocks when ALL slots are filled (ring full); otherwise it grabs
 * the next free slot and starts the next ioctl immediately — ensuring the
 * DMA engine is never idle while render capacity exists.
 */
static void *video_dma_thread(void *arg)
{
    (void)arg;
    struct iwfg_user_dma_req req;

    while (atomic_load(&g_running)) {
        /* Wait until at least one slot is free */
        pthread_mutex_lock(&g_vid_ring_mutex);
        while (g_vid_filled_count == NUM_VIDEO_BUFFERS && atomic_load(&g_running)) {
            pthread_cond_wait(&g_vid_cond_free, &g_vid_ring_mutex);
        }
        if (!atomic_load(&g_running)) {
            pthread_mutex_unlock(&g_vid_ring_mutex);
            break;
        }
        int slot = g_vid_write_idx;
        pthread_mutex_unlock(&g_vid_ring_mutex);

        /* DMA transfer into slot — performed outside the lock (slow blocking op) */
        req.buf       = g_video_bufs[slot];
        req.size      = g_frame_size;
        req.direction = IWFG_DIR_C2H_BUF;
        req.offset    = 0;
        req.flags     = 0;

        if (ioctl(g_video_fd, IWFG_IOCTL_DMA_DATA, &req) < 0) {
            if (errno == EAGAIN) {
                /* No frame available yet — retry immediately */
                continue;
            }
            if (errno == EBADF || errno == EINTR) {
                /* fd closed by signal handler — exit cleanly */
                break;
            }
            perror("[video-dma] IWFG_IOCTL_DMA_DATA failed");
            atomic_store(&g_running, 0);
            pthread_cond_broadcast(&g_vid_cond_filled);
            break;
        }

        /* Mark slot filled and advance write pointer */
        pthread_mutex_lock(&g_vid_ring_mutex);
        g_vid_buf_state[slot] = VID_BUF_FILLED;
        g_vid_write_idx = (g_vid_write_idx + 1) % NUM_VIDEO_BUFFERS;
        g_vid_filled_count++;
        pthread_cond_signal(&g_vid_cond_filled);
        pthread_mutex_unlock(&g_vid_ring_mutex);
    }

    printf("[video-dma] thread exiting\n");
    return NULL;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("\nVideo options:\n");
    printf("  -f, --fullscreen     Fullscreen mode\n");
    printf("  -w, --windowed       Windowed mode (default)\n");
    printf("\nAudio options:\n");
    printf("  -A <dev>             ALSA device name (default: plughw:0,0)\n");
    printf("  -r <rate>            Sample rate in Hz (default: 48000)\n");
    printf("  -b <bits>            Bit depth: 16, 24, or 32 (default: 24)\n");
    printf("  -m, --mute           Start with audio muted\n");
    printf("\nGeneral:\n");
    printf("  -p, --performance    Enable performance monitoring\n");
    printf("  -h, --help           Show this help\n");
    printf("\nRuntime keys:\n");
    printf("  ESC   Exit\n");
    printf("  P     Toggle performance stats\n");
    printf("  M     Toggle audio mute\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    bool fullscreen = false;

    /* ── argument parsing ─────────────────────────────────────────────── */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--performance")) {
            g_perf_enabled = true;
        } else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fullscreen")) {
            fullscreen = true;
        } else if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--windowed")) {
            fullscreen = false;
        } else if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--mute")) {
            atomic_store(&g_audio_muted, 1);
        } else if (!strcmp(argv[i], "-A") && i + 1 < argc) {
            g_alsa_device = argv[++i];
        } else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            g_audio_rate = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            int bits = atoi(argv[++i]);
            if (bits == 32)
                g_audio_fmt = SND_PCM_FORMAT_S32_LE;
            else if (bits == 24)
                g_audio_fmt = SND_PCM_FORMAT_S24_3LE;
            else if (bits == 16)
                g_audio_fmt = SND_PCM_FORMAT_S16_LE;
            else {
                fprintf(stderr, "Unsupported bit depth %d (use 16, 24, or 32)\n", bits);
                return 1;
            }
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    printf("IWFG Audio+Video Display\n");
    printf("========================\n");

    /* ── GLFW init ────────────────────────────────────────────────────── */
    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    /* ── open video device & query stream params ──────────────────────── */
    g_video_fd = open("/dev/iwfg0", O_RDWR);
    if (g_video_fd < 0) {
        perror("open /dev/iwfg0");
        glfwTerminate();
        return 1;
    }

    struct iwfg_stream_params sp;

    /* Retry until the driver reports a valid signal (up to 30 s) */
    printf("Waiting for video signal on /dev/iwfg0...");
    fflush(stdout);
    int sp_attempts;
    for (sp_attempts = 0; sp_attempts < 300; sp_attempts++) {
        if (ioctl(g_video_fd, IWFG_IOCTL_QUERY_STREAM_PARAMS, &sp) < 0) {
            perror("\nIWFG_IOCTL_QUERY_STREAM_PARAMS");
            close(g_video_fd);
            glfwTerminate();
            return 1;
        }
        if (sp.width > 0 && sp.height > 0)
            break;
        usleep(100000); /* 100 ms */
        if (sp_attempts % 10 == 9) { printf("."); fflush(stdout); }
    }
    if (sp.width == 0 || sp.height == 0) {
        fprintf(stderr, "\nTimed out waiting for valid stream params\n");
        close(g_video_fd);
        glfwTerminate();
        return 1;
    }
    printf(" OK\n");
    g_width      = sp.width;
    g_height     = sp.height;
    g_bpp        = sp.bpp;
    g_frame_size = g_width * g_height * g_bpp;

    printf("Video: %ux%u  %u byte/px  frame=%u bytes\n",
           g_width, g_height, g_bpp, g_frame_size);
    printf("Mode: %s\n", fullscreen ? "fullscreen" : "windowed");
    printf("Performance monitoring: %s (P to toggle)\n",
           g_perf_enabled ? "ON" : "OFF");
    printf("Audio: rate=%uHz  muted=%s\n",
           g_audio_rate, atomic_load(&g_audio_muted) ? "yes" : "no");
    printf("\n");

    /* ── open audio device ────────────────────────────────────────────── */
    g_audio_fd = open("/dev/iwfg1", O_RDWR);
    if (g_audio_fd < 0) {
        perror("open /dev/iwfg1");
        close(g_video_fd);
        glfwTerminate();
        return 1;
    }

    /* ── ALSA setup ───────────────────────────────────────────────────── */
    /* Compute audio buffer byte size before setup_alsa() and buffer alloc. */
    g_audio_buf_size = (size_t)AUDIO_BUF_FRAMES *
                       (size_t)((snd_pcm_format_width(g_audio_fmt) / 8) *
                                (int)g_audio_channels);

    if (setup_alsa() < 0) {
        fprintf(stderr, "ALSA setup failed – audio will be disabled\n");
        /* non-fatal: continue without audio output */
        if (g_alsa_pcm) { snd_pcm_close(g_alsa_pcm); g_alsa_pcm = NULL; }
        /* still set frames_per_buf so the audio thread can run without ALSA */
        g_audio_frames_per_buf = AUDIO_BUF_FRAMES;
    }

    /* ── create GLFW window ───────────────────────────────────────────── */
    GLFWmonitor *monitor = fullscreen ? glfwGetPrimaryMonitor() : NULL;
    int win_w = fullscreen ? (int)g_width : (int)g_width  / 2;
    int win_h = fullscreen ? (int)g_height : (int)g_height / 2;

    g_window = glfwCreateWindow(win_w, win_h,
                                "IWFG A/V Display – 4K", monitor, NULL);
    if (!g_window) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        cleanup();
        return 1;
    }

    glfwMakeContextCurrent(g_window);
    glfwSetKeyCallback(g_window, key_cb);
    glfwSetFramebufferSizeCallback(g_window, framebuffer_size_cb);

    printf("OpenGL  vendor  : %s\n", glGetString(GL_VENDOR));
    printf("OpenGL  renderer: %s\n", glGetString(GL_RENDERER));
    printf("OpenGL  version : %s\n\n", glGetString(GL_VERSION));

    if (setup_opengl() < 0) {
        fprintf(stderr, "setup_opengl failed\n");
        cleanup();
        return 1;
    }

    /* ── allocate video DMA buffers ───────────────────────────────────── */
    for (int i = 0; i < NUM_VIDEO_BUFFERS; i++) {
        g_video_bufs[i] = mmap(NULL, g_frame_size,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_LOCKED,
                               -1, 0);
        if (g_video_bufs[i] == MAP_FAILED) {
            perror("video mmap (MAP_LOCKED) failed, retrying without");
            g_video_bufs[i] = mmap(NULL, g_frame_size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                   -1, 0);
            if (g_video_bufs[i] == MAP_FAILED) {
                perror("video mmap failed");
                cleanup();
                return 1;
            }
            if (mlock(g_video_bufs[i], g_frame_size) != 0)
                perror("mlock (video) warning – DMA may be unreliable");
        }
        madvise(g_video_bufs[i], g_frame_size, MADV_DONTFORK);

        /* Write-fault all pages with a test gradient */
        uint32_t *px = (uint32_t *)g_video_bufs[i];
        for (uint32_t y = 0; y < g_height; y++) {
            for (uint32_t x = 0; x < g_width; x++) {
                uint8_t r = (uint8_t)((x * 255) / g_width);
                uint8_t g = (uint8_t)((y * 255) / g_height);
                uint8_t b = (uint8_t)(((x + y) * 255) / (g_width + g_height));
                px[y * g_width + x] = (0xFFu << 24) | ((uint32_t)b << 16)
                                    | ((uint32_t)g << 8) | r;
            }
        }
        __sync_synchronize();

        printf("Video buffer %d: %p (%u bytes)\n", i, g_video_bufs[i], g_frame_size);
    }

    /* ── allocate audio DMA buffers ───────────────────────────────────── */
    for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        g_audio_bufs[i] = aligned_alloc(4096, g_audio_buf_size);
        if (!g_audio_bufs[i]) {
            perror("audio buffer alloc failed");
            cleanup();
            return 1;
        }
        memset(g_audio_bufs[i], 0, g_audio_buf_size);
        printf("Audio buffer %d: %p (%zu bytes)\n", i, g_audio_bufs[i], g_audio_buf_size);
    }

    printf("\nStarting audio thread and video DMA+render threads…\n");
    printf("Keys: ESC=exit  P=perf  M=mute\n\n");

    /* ── initialize video ring buffer state ───────────────────────────── */
    for (int i = 0; i < NUM_VIDEO_BUFFERS; i++)
        g_vid_buf_state[i] = VID_BUF_FREE;

    /* ── start audio thread ───────────────────────────────────────────── */
    pthread_t audio_tid;
    if (pthread_create(&audio_tid, NULL, audio_thread, NULL) != 0) {
        perror("pthread_create (audio)");
        cleanup();
        return 1;
    }

    /* ── start video DMA producer thread ─────────────────────────────── */
    pthread_t vid_dma_tid;
    if (pthread_create(&vid_dma_tid, NULL, video_dma_thread, NULL) != 0) {
        perror("pthread_create (video-dma)");
        atomic_store(&g_running, 0);
        pthread_join(audio_tid, NULL);
        cleanup();
        return 1;
    }
    printf("[video-dma] producer thread started (%d ring slots). "
           "DMA runs continuously until ring is full.\n", NUM_VIDEO_BUFFERS);

    /* ── video render loop (main thread — consumer) ───────────────────── */
    while (!glfwWindowShouldClose(g_window) && atomic_load(&g_running)) {

        /* -- timing ---------------------------------------------------- */
        if (g_perf_enabled && g_frame_count < PERF_WINDOW)
            clock_gettime(CLOCK_MONOTONIC, &g_frame_times[g_frame_count]);

        struct timespec t0, t1;

        /* -- wait for a filled slot from DMA thread -------------------- */
        if (g_perf_enabled) clock_gettime(CLOCK_MONOTONIC, &t0);

        pthread_mutex_lock(&g_vid_ring_mutex);
        while (g_vid_filled_count == 0 && atomic_load(&g_running) &&
               !glfwWindowShouldClose(g_window)) {
            pthread_cond_wait(&g_vid_cond_filled, &g_vid_ring_mutex);
        }
        if (!atomic_load(&g_running) || glfwWindowShouldClose(g_window)) {
            pthread_mutex_unlock(&g_vid_ring_mutex);
            break;
        }
        int slot = g_vid_read_idx;
        pthread_mutex_unlock(&g_vid_ring_mutex);

        if (g_perf_enabled && g_frame_count < PERF_WINDOW) {
            struct timespec tmp;
            clock_gettime(CLOCK_MONOTONIC, &tmp);
            g_wait_times[g_frame_count].tv_sec  = tmp.tv_sec  - t0.tv_sec;
            g_wait_times[g_frame_count].tv_nsec = tmp.tv_nsec - t0.tv_nsec;
            if (g_wait_times[g_frame_count].tv_nsec < 0) {
                g_wait_times[g_frame_count].tv_sec--;
                g_wait_times[g_frame_count].tv_nsec += 1000000000;
            }
        }

        /* -- render (outside lock — does not stall DMA thread) --------- */
        if (g_perf_enabled) clock_gettime(CLOCK_MONOTONIC, &t0);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindTexture(GL_TEXTURE_2D, g_texture_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     (GLsizei)g_width, (GLsizei)g_height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, g_video_bufs[slot]);

        glUseProgram(g_shader_program);
        glBindVertexArray(g_vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        glfwSwapBuffers(g_window);

        if (g_perf_enabled && g_frame_count < PERF_WINDOW) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            g_render_times[g_frame_count].tv_sec  = t1.tv_sec  - t0.tv_sec;
            g_render_times[g_frame_count].tv_nsec = t1.tv_nsec - t0.tv_nsec;
            if (g_render_times[g_frame_count].tv_nsec < 0) {
                g_render_times[g_frame_count].tv_sec--;
                g_render_times[g_frame_count].tv_nsec += 1000000000;
            }
            g_frame_count++;
        }

        /* -- release consumed slot back to DMA producer ---------------- */
        pthread_mutex_lock(&g_vid_ring_mutex);
        g_vid_buf_state[slot] = VID_BUF_FREE;
        g_vid_read_idx = (g_vid_read_idx + 1) % NUM_VIDEO_BUFFERS;
        g_vid_filled_count--;
        pthread_cond_signal(&g_vid_cond_free);
        pthread_mutex_unlock(&g_vid_ring_mutex);

        /* -- counters / stats ------------------------------------------ */
        uint32_t tf = atomic_fetch_add(&g_total_video_frames, 1) + 1;

        if (g_perf_enabled && tf % 60 == 0) {
            printf("[video] frame %u | audio bufs %u\n",
                   tf, atomic_load(&g_total_audio_bufs));
            print_perf_stats();
        } else if (!g_perf_enabled && tf % 300 == 0) {
            printf("[video] rendered %u frames | audio bufs %u\n",
                   tf, atomic_load(&g_total_audio_bufs));
        }

        glfwPollEvents();
    }

    /* ── shutdown ─────────────────────────────────────────────────────── */
    atomic_store(&g_running, 0);
    pthread_cond_broadcast(&g_vid_cond_free);
    pthread_cond_broadcast(&g_vid_cond_filled);
    if (g_video_fd >= 0) { close(g_video_fd); g_video_fd = -1; }
    pthread_join(vid_dma_tid, NULL);
    pthread_join(audio_tid, NULL);
    cleanup();
    return 0;
}
