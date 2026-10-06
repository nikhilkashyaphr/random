/*
 * IWFG Audio Capture Example
 *
 * Captures single-channel 24-bit PCM audio from /dev/iwfg1 using
 * 23040-byte DMA buffers and plays it through an ALSA output device.
 *
 * Audio defaults (override with CLI options):
 *   Device      : /dev/iwfg1
 *   Sample rate : 48000 Hz
 *   Format      : S24_3LE (packed 3 bytes/sample = 24-bit)
 *   Channels    : 1
 *   Buffer size : 24576 bytes  →  8192 frames  →  ~171 ms per transfer
 *                 (LCM(4096,3)=12288 → 24576 = 6 pages = 8192×3 bytes, IOMMU-safe)
 *
 * Usage:
 *   iwfg_audio_capture [options]
 *
 * Options:
 *   -A <dev>       ALSA playback device (default: plughw:0,0)
 *   -r <rate>      Sample rate in Hz   (default: 48000)
 *   -b 16|24|32    Bit depth           (default: 24)
 *   -n <count>     Stop after <count> buffers (0 = run forever, default: 0)
 *   -s             Save to capture.wav instead of ALSA playback (Ctrl-C finalizes WAV header)
 *   -q             Quiet – suppress per-buffer progress lines
 *   -h             Help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#include <alsa/asoundlib.h>

#include "iwfg_user.h"

/* ─── constants ──────────────────────────────────────────────────────────── */

#define NUM_BUFS         10
/* Fixed frame count per DMA transfer: ~85 ms at 48 kHz.
 * Actual DMA byte size = AUDIO_BUF_FRAMES x bytes_per_frame, always a multiple of 4096:
 *   1ch S24_3LE -> 4096 x 3 = 12288 B =  3 x 4096  (IOMMU-safe, 4K-aligned)
 *   2ch S24_3LE -> 4096 x 6 = 24576 B =  6 x 4096  (IOMMU-safe, 4K-aligned)
 *   1ch S32_LE  -> 4096 x 4 = 16384 B =  4 x 4096  (IOMMU-safe, 4K-aligned)
 *   2ch S32_LE  -> 4096 x 8 = 32768 B =  8 x 4096  (IOMMU-safe, 4K-aligned) */
// #define AUDIO_BUF_FRAMES 4096
#define AUDIO_BUF_FRAMES 1600

/* ─── configurable parameters ────────────────────────────────────────────── */

static const char       *g_alsa_dev    = "plughw:0,0";
static unsigned int      g_rate        = 48000;
// static snd_pcm_format_t  g_fmt         = SND_PCM_FORMAT_S16_LE;
static snd_pcm_format_t  g_fmt         = SND_PCM_FORMAT_S24_3LE;
static unsigned int      g_channels    = 2;
static uint64_t          g_max_bufs    = 0;      /* 0 = unlimited */
static bool              g_save_wav    = false;  /* -s: save to WAV instead of ALSA playback */
static bool              g_quiet       = false;
static bool              g_debug_dump  = false;  /* -D: hex-dump first buffer */
static uint64_t          g_raw_dump_bytes = 0;   /* -R <size>: capture this many raw PCM bytes then exit (0 = disabled) */
static const char       *g_raw_dump_file  = "capture.raw"; /* -o <file>: output filename for -R */

/* ─── global state ───────────────────────────────────────────────────────── */

static int          g_audio_fd         = -1;
static snd_pcm_t   *g_pcm              = NULL;
static int          g_raw_fd           = -1;    /* WAV output fd (-1 = not saving) */
static int          g_raw_dump_fd      = -1;    /* raw dump fd for -R */
static void        *g_bufs[NUM_BUFS];
static snd_pcm_uframes_t g_frames_per_buf = 0;
static size_t       g_buf_size             = 0;   /* bytes per DMA transfer = AUDIO_BUF_FRAMES x bpf */

static uint64_t     g_total_bufs       = 0;
static uint64_t     g_total_bytes      = 0;     /* PCM data bytes written to WAV */

/* ─── WAV helpers ────────────────────────────────────────────────────────── */

/* Write a 44-byte PCM WAV header with placeholder sizes (filled in later). */
static void wav_write_header(int fd, uint32_t rate, uint16_t channels,
                              uint16_t bits_per_sample)
{
    uint16_t block_align  = (uint16_t)(channels * bits_per_sample / 8);
    uint32_t byte_rate    = rate * block_align;
    uint8_t  h[44];
    /* RIFF */
    memcpy(h,      "RIFF", 4);
    memcpy(h + 4,  &(uint32_t){0}, 4);          /* filled in by finalize */
    memcpy(h + 8,  "WAVE", 4);
    /* fmt  */
    memcpy(h + 12, "fmt ", 4);
    memcpy(h + 16, &(uint32_t){16}, 4);         /* PCM fmt chunk size */
    memcpy(h + 20, &(uint16_t){1}, 2);          /* PCM = 1 */
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits_per_sample, 2);
    /* data */
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &(uint32_t){0}, 4);          /* filled in by finalize */
    write(fd, h, 44);
}

/* Seek back and patch the two size fields.  All calls are async-signal-safe. */
static void wav_finalize(int fd, uint64_t data_bytes)
{
    if (fd < 0) return;
    uint32_t data_size = (uint32_t)(data_bytes > 0xFFFFFFFFULL
                                    ? 0xFFFFFFFFU : data_bytes);
    uint32_t riff_size = 36 + data_size;
    lseek(fd, 4,  SEEK_SET);
    write(fd, &riff_size, 4);
    lseek(fd, 40, SEEK_SET);
    write(fd, &data_size, 4);
    fsync(fd);
}

/* ─── signal handler ─────────────────────────────────────────────────────── */

static void sig_handler(int s __attribute__((unused)))
{
    /* Close the device fd while potentially blocked inside ioctl() –
     * this is async-signal-safe and causes the kernel to return EBADF
     * immediately, unblocking the capture loop. */
    if (g_audio_fd >= 0) {
        close(g_audio_fd);
        g_audio_fd = -1;
    }

    /* Finalize WAV header with actual data size, then close. */
    if (g_raw_fd >= 0) {
        wav_finalize(g_raw_fd, g_total_bytes);
        close(g_raw_fd);
        g_raw_fd = -1;
    }

    /* Flush raw dump file if open. */
    if (g_raw_dump_fd >= 0) {
        fsync(g_raw_dump_fd);
        close(g_raw_dump_fd);
        g_raw_dump_fd = -1;
    }

    write(STDOUT_FILENO, "\nInterrupted – WAV finalized, exiting.\n", 39);
    _exit(0);
}

/* ─── format debug dump ──────────────────────────────────────────────────── */

/* Interpret the first N bytes of a buffer as various PCM formats and print
 * the decoded sample values.  This is the fastest way to figure out what
 * the hardware is actually sending. */
static void debug_dump_buffer(const uint8_t *buf, int nbytes)
{
    printf("\n─── First-buffer hex dump (first 64 bytes) ───\n");
    for (int i = 0; i < 64 && i < nbytes; i++) {
        if (i % 16 == 0) printf("  %04x: ", i);
        printf("%02x ", buf[i]);
        if (i % 16 == 15) printf("\n");
    }
    printf("\n");

    printf("─── Sample interpretation (first 8 samples each format) ───\n");

    /* S16_LE: 2 bytes/sample */
    printf("  S16_LE (2B/smp)  : ");
    for (int i = 0; i < 8 && i*2+1 < nbytes; i++) {
        int16_t s;
        memcpy(&s, buf + i*2, 2);
        printf("%7d ", (int)s);
    }
    printf("\n");

    /* S24_3LE: packed 3 bytes/sample, sign-extend from bit 23 */
    printf("  S24_3LE (3B/smp) : ");
    for (int i = 0; i < 8 && i*3+2 < nbytes; i++) {
        int32_t s = (int32_t)((uint32_t)buf[i*3]
                            | ((uint32_t)buf[i*3+1] << 8)
                            | ((uint32_t)buf[i*3+2] << 16));
        if (s & 0x800000) s |= (int32_t)0xFF000000; /* sign extend */
        printf("%9d ", s);
    }
    printf("\n");

    /* S24_LE: right-justified 24-bit in 32-bit word (lower 3 bytes = audio, upper byte = sign) */
    printf("  S24_LE (4B/smp)  : ");
    for (int i = 0; i < 8 && i*4+3 < nbytes; i++) {
        int32_t s = (int32_t)((uint32_t)buf[i*4]
                            | ((uint32_t)buf[i*4+1] << 8)
                            | ((uint32_t)buf[i*4+2] << 16));
        if (s & 0x800000) s |= (int32_t)0xFF000000;
        printf("%9d ", s);
    }
    printf("  [%s]  pad bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           "upper byte should be 0x00 or 0xFF",
           buf[3], buf[7], buf[11], buf[15],
           buf[19], buf[23], buf[27], buf[31]);

    /* S32_LE: full 32-bit word */
    printf("  S32_LE (4B/smp)  : ");
    for (int i = 0; i < 8 && i*4+3 < nbytes; i++) {
        int32_t s;
        memcpy(&s, buf + i*4, 4);
        printf("%11d ", s);
    }
    printf("\n");

    /* Stereo S24_LE: every other sample is L or R */
    printf("  S24_LE stereo L  : ");
    for (int i = 0; i < 8 && i*8+3 < nbytes; i++) {
        int32_t s = (int32_t)((uint32_t)buf[i*8]
                            | ((uint32_t)buf[i*8+1] << 8)
                            | ((uint32_t)buf[i*8+2] << 16));
        if (s & 0x800000) s |= (int32_t)0xFF000000;
        printf("%9d ", s);
    }
    printf("\n");
    printf("  S24_LE stereo R  : ");
    for (int i = 0; i < 8 && i*8+7 < nbytes; i++) {
        int32_t s = (int32_t)((uint32_t)buf[i*8+4]
                            | ((uint32_t)buf[i*8+5] << 8)
                            | ((uint32_t)buf[i*8+6] << 16));
        if (s & 0x800000) s |= (int32_t)0xFF000000;
        printf("%9d ", s);
    }
    printf("\n");

    printf("─────────────────────────────────────────────────────────\n");
    printf("Hint: pick the format where samples show smooth audio values (non-zero, varying).\n");
    printf("      If S24_LE stereo L/R both non-zero → use -c 2.\n");
    printf("      If upper bytes of S24_LE are not 0x00/0xFF → try S32_LE or S24_3LE.\n\n");
}

/* ─── ALSA helpers ───────────────────────────────────────────────────────── */

static int alsa_open(void)
{
    int err = snd_pcm_open(&g_pcm, g_alsa_dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "ALSA open '%s' failed: %s\n",
                g_alsa_dev, snd_strerror(err));
        return -1;
    }

    /* Frame count is fixed; byte size scales with format/channels via g_buf_size. */
    g_frames_per_buf = AUDIO_BUF_FRAMES;   /* 8192 frames ~171 ms at 48 kHz */

    /* Ring buffer = 8x DMA period -> ~1365 ms at 48 kHz.
     * Gives ALSA plenty of slack to absorb DMA jitter without underrunning.
     * Combined with 6-period silence pre-fill, playback won't start until
     * ~1024 ms of audio is queued, making underruns extremely rare. */
    snd_pcm_uframes_t period_size = g_frames_per_buf;           /* 8192 frames ~171 ms */
    snd_pcm_uframes_t buffer_size = g_frames_per_buf * 8;       /* 65536 frames ~1365 ms */

    snd_pcm_hw_params_t *hwp;
    snd_pcm_hw_params_alloca(&hwp);
    snd_pcm_hw_params_any(g_pcm, hwp);

    if ((err = snd_pcm_hw_params_set_access(g_pcm, hwp,
                    SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(g_pcm, hwp, g_fmt)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(g_pcm, hwp, g_channels)) < 0 ||
        (err = snd_pcm_hw_params_set_rate(g_pcm, hwp, g_rate, 0)) < 0 ||
        (err = snd_pcm_hw_params_set_period_size_near(g_pcm, hwp,
                    &period_size, NULL)) < 0 ||
        (err = snd_pcm_hw_params_set_buffer_size_near(g_pcm, hwp,
                    &buffer_size)) < 0 ||
        (err = snd_pcm_hw_params(g_pcm, hwp)) < 0) {
        fprintf(stderr, "ALSA hw_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }

    /* Set up software params: start playing only once the ring buffer
     * has at least one period of data (avoids an immediate underrun). */
    snd_pcm_sw_params_t *swp;
    snd_pcm_sw_params_alloca(&swp);
    snd_pcm_sw_params_current(g_pcm, swp);
    /* Delay start until 2 full periods are queued, so the ring buffer
     * has meaningful headroom before the first frame is played out. */
    snd_pcm_sw_params_set_start_threshold(g_pcm, swp, period_size * 2);
    snd_pcm_sw_params_set_avail_min(g_pcm, swp, period_size);
    snd_pcm_sw_params(g_pcm, swp);

    printf("ALSA: device='%s'  rate=%u Hz  fmt=%s  ch=%u\n"
           "      period=%lu frames (%lu ms)  buffer=%lu frames (%lu ms)\n",
           g_alsa_dev, g_rate, snd_pcm_format_name(g_fmt), g_channels,
           (unsigned long)period_size,
           (unsigned long)(period_size * 1000 / g_rate),
           (unsigned long)buffer_size,
           (unsigned long)(buffer_size * 1000 / g_rate));
    return 0;
}

/* Write one buffer, with XRUN recovery.
 * On underrun: inject silence to restore ring-buffer headroom before
 * resuming real data – this eliminates the click/pop on restart and
 * prevents an immediate re-underrun when the ring buffer is empty. */
static int alsa_write(const void *data, snd_pcm_uframes_t frames)
{
    const char *ptr = (const char *)data;
    snd_pcm_uframes_t rem = frames;
    int bpf = (snd_pcm_format_width(g_fmt) / 8) * (int)g_channels;

    while (rem > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(g_pcm, ptr, rem);
        if (n == -EAGAIN) {
            snd_pcm_wait(g_pcm, 100);
            continue;
        }
        if (n < 0) {
            int err = snd_pcm_recover(g_pcm, (int)n, 0);
            if (err < 0) {
                fprintf(stderr, "ALSA write unrecoverable: %s\n",
                        snd_strerror(err));
                return -1;
            }
            /* Ring buffer is empty after recovery – re-inject silence to
             * rebuild headroom before writing real audio again.
             * Without this the ring empties and underruns loop indefinitely. */
            size_t silence_bytes = g_frames_per_buf * (snd_pcm_uframes_t)bpf;
            void *silence = calloc(1, silence_bytes);
            if (silence) {
                for (int p = 0; p < 6; p++) {
                    snd_pcm_sframes_t r = snd_pcm_writei(
                            g_pcm, silence, g_frames_per_buf);
                    if (r < 0) snd_pcm_recover(g_pcm, (int)r, 1);
                }
                free(silence);
            }
            /* Restart real-data write from the beginning of the buffer
             * so we don't play a truncated/partial period. */
            ptr = (const char *)data;
            rem = frames;
            continue;
        }
        ptr += (size_t)n * (unsigned int)bpf;
        rem -= (snd_pcm_uframes_t)n;
    }
    return 0;
}

/* ─── cleanup ─────────────────────────────────────────────────────────────── */

static void cleanup(void)
{
    if (g_audio_fd >= 0) { close(g_audio_fd); g_audio_fd = -1; }

    if (g_pcm) {
        snd_pcm_drain(g_pcm);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }

    if (g_raw_fd >= 0) {
        wav_finalize(g_raw_fd, g_total_bytes);
        close(g_raw_fd);
        g_raw_fd = -1;
        printf("WAV file saved: capture.wav\n");
        printf("\nPlay back:\n");
        printf("  aplay capture.wav\n");
        printf("\nConvert to MP3:\n");
        printf("  ffmpeg -i capture.wav capture.mp3\n");
        printf("  lame capture.wav capture.mp3\n");
    }

    if (g_raw_dump_fd >= 0) {
        fsync(g_raw_dump_fd);
        close(g_raw_dump_fd);
        g_raw_dump_fd = -1;
        printf("Raw PCM dump saved: %s\n", g_raw_dump_file);
        printf("  Inspect : xxd %s | head -4\n", g_raw_dump_file);
        printf("  Play    : aplay -r %u -f %s -c %u %s\n",
               g_rate, snd_pcm_format_name(g_fmt), g_channels, g_raw_dump_file);
    }

    for (int i = 0; i < NUM_BUFS; i++) {
        if (g_bufs[i]) { free(g_bufs[i]); g_bufs[i] = NULL; }
    }
}

/* ─── usage ──────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("  -A <dev>       ALSA playback device  (default: plughw:0,0)\n");
    printf("  -r <rate>      Sample rate in Hz     (default: 48000)\n");
    printf("  -b 16|24|32    Bit depth             (default: 24 = S24_3LE, 3B/sample)\n");
    printf("  -c 1|2         Channel count         (default: 1)\n");
    printf("  -n <count>     Stop after <count> DMA buffers (0 = forever)\n");
    printf("  -s             Save to capture.wav instead of ALSA playback\n");
    printf("  -R <size>      Capture <size> raw PCM bytes to file then exit (no ALSA)\n");
    printf("                 Suffix K=KiB, M=MiB  e.g. -R 49152, -R 48K, -R 2M\n");
    printf("  -o <file>      Output filename for -R dump (default: capture.raw)\n");
    printf("  -D             Dump first buffer as hex + multi-format decode (diagnosis)\n");
    printf("  -q             Quiet mode\n");
    printf("  -h             Show this help\n");
    printf("\nBuffer: %d frames -> %.1f ms per transfer at 48 kHz\n",
           AUDIO_BUF_FRAMES, (AUDIO_BUF_FRAMES / 48000.0) * 1000.0);
    printf("        (DMA bytes always a multiple of 4096, 4K page-aligned, IOMMU-safe)\n");
    printf("        e.g. 2ch S24_3LE: 4096 x 6 = 24576 B = 6 x 4096\n");
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* ── argument parsing ───────────────────────────────────────────────── */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-A") && i + 1 < argc) {
            g_alsa_dev = argv[++i];
        } else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            g_rate = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            int bits = atoi(argv[++i]);
            if      (bits == 16) g_fmt = SND_PCM_FORMAT_S16_LE;
            else if (bits == 24) g_fmt = SND_PCM_FORMAT_S24_3LE;  /* packed 3B/sample – correct for this hardware */
            else if (bits == 243) g_fmt = SND_PCM_FORMAT_S24_LE;  /* 4B/sample right-justified, use 243 to select */
            else if (bits == 32) g_fmt = SND_PCM_FORMAT_S32_LE;
            else {
                fprintf(stderr, "Unsupported bit depth %d (use 16, 24, 32)\n", bits);
                return 1;
            }
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            g_channels = (unsigned int)atoi(argv[++i]);
            if (g_channels < 1 || g_channels > 2) {
                fprintf(stderr, "Channels must be 1 or 2\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "-D")) {
            g_debug_dump = true;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            g_max_bufs = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "-s")) {
            g_save_wav = true;
        } else if (!strcmp(argv[i], "-R") && i + 1 < argc) {
            char *end;
            g_raw_dump_bytes = (uint64_t)strtoull(argv[++i], &end, 10);
            if (*end == 'K' || *end == 'k') g_raw_dump_bytes *= 1024ULL;
            else if (*end == 'M' || *end == 'm') g_raw_dump_bytes *= 1024ULL * 1024ULL;
            if (g_raw_dump_bytes == 0) {
                fprintf(stderr, "Invalid -R size\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            g_raw_dump_file = argv[++i];
        } else if (!strcmp(argv[i], "-q")) {
            g_quiet = true;
        } else if (!strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Compute actual DMA buffer byte size (fixed frame count x bytes_per_frame).
     * Must be done before the startup printf and before buffer allocation. */
    g_buf_size = (size_t)AUDIO_BUF_FRAMES *
                 (size_t)((snd_pcm_format_width(g_fmt) / 8) * (int)g_channels);

    printf("IWFG Audio Capture\n");
    printf("==================\n");
    printf("Source  : /dev/iwfg1\n");
    printf("Rate    : %u Hz\n", g_rate);
    printf("Format  : %s\n", snd_pcm_format_name(g_fmt));
    printf("Channels: %u\n", g_channels);
    printf("Buf size: %zu bytes  (%d frames x %u ch x %d B/sample, %zu pages)\n",
           g_buf_size, AUDIO_BUF_FRAMES, g_channels,
           snd_pcm_format_width(g_fmt) / 8, g_buf_size / 4096);
    printf("Output  : %s\n", g_raw_dump_bytes ? "Raw dump file" :
                             g_save_wav       ? "WAV file (capture.wav)" : "ALSA playback");
    if (g_raw_dump_bytes)
        printf("Raw dump: %llu bytes -> %s\n",
               (unsigned long long)g_raw_dump_bytes, g_raw_dump_file);
    if (g_max_bufs)
        printf("Limit   : %llu buffers\n", (unsigned long long)g_max_bufs);
    else
        printf("Limit   : unlimited (Ctrl-C to stop)\n");
    printf("\n");

    g_audio_fd = open("/dev/iwfg1", O_RDWR);
    if (g_audio_fd < 0) {
        perror("open /dev/iwfg1");
        return 1;
    }

    /* ── ALSA setup (only when not saving to WAV and not raw dumping) ─── */
    if (!g_save_wav && !g_raw_dump_bytes) {
        if (alsa_open() < 0) {
            close(g_audio_fd);
            return 1;
        }
    }

    /* ── WAV file setup (only when -s given) ───────────────────────────── */
    if (g_save_wav) {
        g_raw_fd = open("capture.wav", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_raw_fd < 0) {
            perror("open capture.wav");
            close(g_audio_fd);
            return 1;
        }
        /* Determine bits per sample from format (S24_3LE → 24) */
        uint16_t bps = (uint16_t)snd_pcm_format_width(g_fmt);
        wav_write_header(g_raw_fd, g_rate, (uint16_t)g_channels, bps);

        /* Compute frames_per_buf here since alsa_open() was skipped */
        g_frames_per_buf = AUDIO_BUF_FRAMES;

        printf("Saving to capture.wav  (Ctrl-C to stop and finalize)\n");
    }

    /* ── raw dump setup (only when -R given) ────────────────────────────── */
    if (g_raw_dump_bytes) {
        g_raw_dump_fd = open(g_raw_dump_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_raw_dump_fd < 0) {
            perror("open raw dump file");
            close(g_audio_fd);
            return 1;
        }
        if (!g_frames_per_buf)
            g_frames_per_buf = AUDIO_BUF_FRAMES;
        printf("Capturing %llu bytes of raw PCM to %s…\n",
               (unsigned long long)g_raw_dump_bytes, g_raw_dump_file);
    }

    /* ── allocate DMA buffers ───────────────────────────────────────────── */
    for (int i = 0; i < NUM_BUFS; i++) {
        g_bufs[i] = aligned_alloc(4096, g_buf_size);
        if (!g_bufs[i]) {
            perror("aligned_alloc");
            cleanup();
            return 1;
        }
        memset(g_bufs[i], 0, g_buf_size);
    }

    printf("Buffers allocated. Starting capture…\n\n");

    /* ── pre-fill ALSA ring buffer with silence ───────────────────────────
     * Only needed for live ALSA playback mode. */
    /* Pre-fill 2 periods of silence (~170 ms at 48 kHz) before the first
     * real DMA buffer arrives.  Enough headroom to absorb normal DMA jitter
     * without adding excessive latency (was 6 periods = ~1 second delay). */
    if (!g_save_wav && !g_raw_dump_bytes) {
        void *silence = calloc(1, g_buf_size);
        if (silence) {
            for (int p = 0; p < 2; p++)
                alsa_write(silence, g_frames_per_buf);
            free(silence);
        }
    }

    /* ── capture / playback loop ────────────────────────────────────────── */
    uint32_t buf_idx = 0;
    struct timespec t_start, t_now;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    while (1) {

        /* DMA: card → host */
        struct iwfg_user_dma_req req = {
            .buf       = g_bufs[buf_idx],
            .size      = g_buf_size,
            .offset    = 0,
            .direction = IWFG_DIR_C2H_BUF,
            .flags     = 0,
        };

        if (ioctl(g_audio_fd, IWFG_IOCTL_DMA_DATA, &req) < 0) {
            if (errno == EAGAIN) {
                usleep(1000);   /* no data yet – brief spin */
                continue;
            }
            if (errno == EINTR)
                break;
            perror("IWFG_IOCTL_DMA_DATA");
            break;
        }

        g_total_bufs++;
        if (!g_save_wav && !g_raw_dump_bytes)
            g_total_bytes += g_buf_size;  /* tracked per-write in WAV/raw-dump mode */

        /* Debug dump: print raw bytes + multi-format decode on first buffer */
        if (g_debug_dump && g_total_bufs == 1) {
            debug_dump_buffer((const uint8_t *)g_bufs[buf_idx], (int)g_buf_size);
            g_debug_dump = false; /* only once */
        }

        /* ALSA playback (live mode only) */
        if (!g_save_wav && !g_raw_dump_bytes) {
            if (alsa_write(g_bufs[buf_idx], g_frames_per_buf) < 0) {
                fprintf(stderr, "ALSA write failed re-preparing PCM\n");
                snd_pcm_prepare(g_pcm);
            }
        }

        /* WAV file write (save mode only) */
        if (g_raw_fd >= 0) {
            ssize_t written = write(g_raw_fd, g_bufs[buf_idx], g_buf_size);
            if (written > 0)
                g_total_bytes += (uint64_t)written;
        }

        /* Raw dump write: capture exactly g_raw_dump_bytes then exit */
        if (g_raw_dump_fd >= 0) {
            uint64_t remaining = g_raw_dump_bytes - g_total_bytes;
            size_t   to_write  = (g_buf_size <= (size_t)remaining)
                                 ? g_buf_size : (size_t)remaining;
            ssize_t  wr = write(g_raw_dump_fd, g_bufs[buf_idx], to_write);
            if (wr > 0)
                g_total_bytes += (uint64_t)wr;
            if (!g_quiet) {
                printf("\rRaw dump: %llu / %llu bytes (%.1f%%)   ",
                       (unsigned long long)g_total_bytes,
                       (unsigned long long)g_raw_dump_bytes,
                       100.0 * (double)g_total_bytes / (double)g_raw_dump_bytes);
                fflush(stdout);
            }
            if (g_total_bytes >= g_raw_dump_bytes) {
                printf("\nRaw dump complete: %llu bytes -> %s\n",
                       (unsigned long long)g_total_bytes, g_raw_dump_file);
                break;
            }
        }

        /* Progress (live/WAV mode only) */
        if (!g_quiet && !g_raw_dump_bytes && g_total_bufs % 10 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t_now);
            double elapsed = (double)(t_now.tv_sec  - t_start.tv_sec) +
                             (double)(t_now.tv_nsec - t_start.tv_nsec) / 1e9;
            double kbps = (g_total_bytes / 1024.0) / elapsed;
            printf("\rBufs: %6llu  |  Bytes: %8.2f KB  |  Elapsed: %6.1f s  |  %.1f KB/s   ",
                   (unsigned long long)g_total_bufs,
                   g_total_bytes / 1024.0,
                   elapsed, kbps);
            fflush(stdout);
        }

        /* Stop condition */
        if (g_max_bufs && g_total_bufs >= g_max_bufs) {
            printf("\nReached buffer limit (%llu).\n",
                   (unsigned long long)g_max_bufs);
            break;
        }

        buf_idx = (buf_idx + 1) % NUM_BUFS;
    }

    printf("\n\nCapture complete.\n");
    printf("  Total buffers : %llu\n",  (unsigned long long)g_total_bufs);
    printf("  Total bytes   : %llu\n",  (unsigned long long)g_total_bytes);
    printf("  Total samples : %llu\n",
           (unsigned long long)(g_total_bytes / (unsigned int)
               ((snd_pcm_format_width(g_fmt) / 8) * g_channels)));

    cleanup();
    return 0;
}
