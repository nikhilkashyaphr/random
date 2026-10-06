/* iqring_sim — publish a synthetic IQRING01 ring into /dev/shm/iqring.
 *
 * WHY: "cannot open /dev/shm/iqring" means no receiver is running. Standing up
 * the full RoCEv2 path (namespaces, Mellanox NIC, rdma_tx/rdma_rx) to find out
 * whether the GUI plots correctly is the wrong order — it mixes two unknowns.
 *
 * This produces a ring byte-identical in layout to the one rdma_rx publishes,
 * so the GUI exercises exactly the same code path. If the GUI plots this, the
 * GUI is fine and any remaining problem is in the RDMA stack. If it does not,
 * the problem is in the GUI and no amount of NIC configuration will help.
 *
 * Layout mirrored from rdma_common.h:
 *   ring_ctrl  4096 B   magic "IQRING01" written LAST, as the real one does
 *   32 slots x (64 B frame_hdr + 1 MiB payload), stride rounded up to 4 KB
 *
 * Build:  gcc -O2 -o iqring_sim iqring_sim.c -lm -lrt
 * Run:    ./iqring_sim                      10 MHz tone, 200 MSPS, 30 fps
 *         ./iqring_sim --tone 25e6 --fps 60
 *         ./iqring_sim --noise              flat noise instead of a tone
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>

#define IQ_FRAME_MAGIC   0x49515246524d4131ULL   /* "IQRFRMA1" */
#define RING_MAGIC       0x495152494e473031ULL   /* "IQRING01" */
#define FRAME_SAMPLES    (256u*1024u)
#define FRAME_PAYLOAD    (FRAME_SAMPLES*4u)
#define RING_SLOTS       32u
#define RING_CTRL_BYTES  4096u
#define HDR_BYTES        64u
#define SLOT_STRIDE      ((((HDR_BYTES+FRAME_PAYLOAD)+4095u)/4096u)*4096u)
#define TOTAL            ((size_t)RING_CTRL_BYTES + (size_t)RING_SLOTS*SLOT_STRIDE)

/* ring_ctrl offsets */
#define C_MAGIC 0
#define C_VER   8
#define C_NSLOT 12
#define C_STRID 16
#define C_NSAMP 20
#define C_FS    24
#define C_FC    32
#define C_WCNT  40
#define C_BYTES 48
#define C_GAPS  56

static volatile sig_atomic_t stop_flag = 0;
static void on_sig(int s){ (void)s; stop_flag = 1; }

int main(int argc, char** argv)
{
    const char* path = "/dev/shm/iqring";
    double tone = 10e6, fs = 200e6, fc = 3.1e9, fps = 30.0, amp = 0.8;
    int noise = 0;
    /* Only 1/16 of a frame is generated per publish by default: the display
     * never shows a whole 262,144-sample frame anyway, and filling 1 MiB at
     * 30 fps for no reason just burns CPU. */
    uint32_t nsamp = FRAME_SAMPLES;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"--tone") && i+1<argc) tone = atof(argv[++i]);
        else if (!strcmp(argv[i],"--fs")   && i+1<argc) fs   = atof(argv[++i]);
        else if (!strcmp(argv[i],"--fc")   && i+1<argc) fc   = atof(argv[++i]);
        else if (!strcmp(argv[i],"--fps")  && i+1<argc) fps  = atof(argv[++i]);
        else if (!strcmp(argv[i],"--amp")  && i+1<argc) amp  = atof(argv[++i]);
        else if (!strcmp(argv[i],"--path") && i+1<argc) path = argv[++i];
        else if (!strcmp(argv[i],"--noise")) noise = 1;
        else { fprintf(stderr,
            "usage: %s [--tone Hz] [--fs Hz] [--fc Hz] [--fps N] [--amp 0..1]\n"
            "          [--noise] [--path /dev/shm/iqring]\n", argv[0]); return 2; }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    int fd = open(path, O_RDWR|O_CREAT, 0666);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, (off_t)TOTAL)) { perror("ftruncate"); return 1; }
    void* base = mmap(NULL, TOTAL, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    memset(base, 0, RING_CTRL_BYTES);

    char* c = base;
    *(uint32_t*)(c+C_VER)   = 1;
    *(uint32_t*)(c+C_NSLOT) = RING_SLOTS;
    *(uint32_t*)(c+C_STRID) = SLOT_STRIDE;
    *(uint32_t*)(c+C_NSAMP) = nsamp;
    *(uint64_t*)(c+C_FS)    = (uint64_t)fs;
    *(int64_t*) (c+C_FC)    = (int64_t)fc;
    /* magic last: a reader that maps mid-setup sees an invalid ring rather
     * than a half-initialised one. Same discipline as the real receiver. */
    __atomic_store_n((uint64_t*)(c+C_MAGIC), RING_MAGIC, __ATOMIC_RELEASE);

    printf("iqring_sim: %s\n", path);
    printf("  %u slots x %u B stride, %u samples/frame\n",
           RING_SLOTS, SLOT_STRIDE, nsamp);
    printf("  fs %.3f MSPS   fc %.3f GHz   %s   amp %.2f   %.0f fps\n",
           fs/1e6, fc/1e9, noise ? "noise" : "tone", amp, fps);
    if (!noise) printf("  tone %.3f MHz  -> expect a peak there in the FFT\n", tone/1e6);
    printf("  full scale 8191<<2 = 32764 (14-bit MSB-aligned, as the RFSoC sends)\n");
    printf("  Ctrl-C to stop.\n\n");

    double ph = 0.0, step = 2.0*M_PI*tone/fs;
    uint32_t rng = 0x12345678u;
    uint64_t wcnt = 0, total_bytes = 0;
    const long period_ns = (long)(1e9/fps);

    while (!stop_flag) {
        uint32_t slot = (uint32_t)(wcnt % RING_SLOTS);
        char* s = (char*)base + RING_CTRL_BYTES + (size_t)slot*SLOT_STRIDE;
        int16_t* iq = (int16_t*)(s + HDR_BYTES);

        for (uint32_t i = 0; i < nsamp; i++) {
            int I, Q;
            if (noise) {
                rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5;
                I = (int)((double)(int32_t)rng/2147483648.0*8191.0*amp);
                rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5;
                Q = (int)((double)(int32_t)rng/2147483648.0*8191.0*amp);
            } else {
                I = (int)lrint(8191.0*amp*cos(ph));
                Q = (int)lrint(8191.0*amp*sin(ph));
                ph += step; if (ph > 2*M_PI) ph -= 2*M_PI;
            }
            /* 14-bit MSB-aligned: quantise THEN shift, so bits[1:0] are 00 —
             * the packing the RFSoC actually produces. */
            if (I >  8191) I =  8191;
            if (I < -8191) I = -8191;
            if (Q >  8191) Q =  8191;
            if (Q < -8191) Q = -8191;
            iq[2*i]   = (int16_t)(I<<2);
            iq[2*i+1] = (int16_t)(Q<<2);
        }

        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        *(uint64_t*)(s+0)  = IQ_FRAME_MAGIC;
        *(uint64_t*)(s+8)  = wcnt;
        *(uint64_t*)(s+16) = (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
        *(uint32_t*)(s+24) = nsamp;
        *(uint32_t*)(s+28) = 0;
        *(uint64_t*)(s+32) = (uint64_t)fs;
        *(int64_t*) (s+40) = (int64_t)fc;

        total_bytes += HDR_BYTES + (uint64_t)nsamp*4u;
        __atomic_store_n((uint64_t*)(c+C_BYTES), total_bytes, __ATOMIC_RELEASE);
        /* write_count published LAST, with release ordering: a reader that
         * sees the new count is guaranteed to see the payload. */
        __atomic_store_n((uint64_t*)(c+C_WCNT), ++wcnt, __ATOMIC_RELEASE);

        if ((wcnt % (uint64_t)fps) == 0)
            printf("\r  %llu frames, %.1f MB published",
                   (unsigned long long)wcnt, (double)total_bytes/1e6), fflush(stdout);

        struct timespec sl = {0, period_ns};
        nanosleep(&sl, NULL);
    }

    printf("\n  stopped after %llu frames\n", (unsigned long long)wcnt);
    /* Clear the magic so the GUI reports "no receiver" rather than reading a
     * ring whose producer has gone. */
    __atomic_store_n((uint64_t*)(c+C_MAGIC), 0ull, __ATOMIC_RELEASE);
    munmap(base, TOTAL);
    return 0;
}
