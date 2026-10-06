/* Synthetic producer for /dev/shm/iqring, byte-compatible with
 * rdma_common.h. Lets the GUI-side ring reader be validated with no NIC,
 * no RDMA and no GPU. Emits a known CW tone so frequency can be checked. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

#define FRAME_SAMPLES    (256u*1024u)
#define BYTES_PER_SAMPLE 4u
#define FRAME_PAYLOAD    (FRAME_SAMPLES*BYTES_PER_SAMPLE)
#define FRAME_MSG_BYTES  (64u + FRAME_PAYLOAD)
#define RING_SLOTS       32u
#define RING_CTRL_BYTES  4096u
#define RING_SLOT_STRIDE (((FRAME_MSG_BYTES + 4095u)/4096u)*4096u)
#define RING_TOTAL       ((size_t)RING_CTRL_BYTES + (size_t)RING_SLOTS*RING_SLOT_STRIDE)
#define RING_MAGIC       0x495152494e473031ULL
#define IQ_FRAME_MAGIC   0x49515246524d4131ULL

static volatile sig_atomic_t run = 1;
static void onsig(int s){ (void)s; run = 0; }

int main(int argc, char** argv)
{
    const char* path = (argc>1)? argv[1] : "/dev/shm/iqring";
    double fs   = (argc>2)? atof(argv[2]) : 200.25e6;
    double tone = (argc>3)? atof(argv[3]) : -10.0e6;
    int    nframes = (argc>4)? atoi(argv[4]) : 0;   /* 0 = until signalled */

    signal(SIGINT, onsig); signal(SIGTERM, onsig);

    int fd = open(path, O_RDWR|O_CREAT|O_TRUNC, 0666);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, RING_TOTAL) != 0) { perror("ftruncate"); return 1; }
    char* base = mmap(NULL, RING_TOTAL, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    memset(base, 0, RING_CTRL_BYTES);

    /* ring_ctrl, magic written LAST as the ABI requires */
    uint32_t u32; uint64_t u64; int64_t i64;
    u32 = 1;               memcpy(base+8,  &u32, 4);   /* version       */
    u32 = RING_SLOTS;      memcpy(base+12, &u32, 4);   /* num_slots     */
    u32 = RING_SLOT_STRIDE;memcpy(base+16, &u32, 4);   /* slot_stride   */
    u32 = FRAME_SAMPLES;   memcpy(base+20, &u32, 4);   /* frame_samples */
    u64 = (uint64_t)fs;    memcpy(base+24, &u64, 8);   /* sample_rate   */
    i64 = 0;               memcpy(base+32, &i64, 8);   /* center_freq   */
    __sync_synchronize();
    u64 = RING_MAGIC;      memcpy(base+0,  &u64, 8);

    int16_t* iq = malloc(FRAME_PAYLOAD);
    uint64_t seq = 0, wc = 0, bytes = 0;
    double phase = 0.0, dphi = 2.0*M_PI*tone/fs;

    while (run && (nframes == 0 || (int)seq < nframes)) {
        for (unsigned i = 0; i < FRAME_SAMPLES; ++i) {
            iq[2*i+0] = (int16_t)lrint(24000.0*cos(phase));
            iq[2*i+1] = (int16_t)lrint(24000.0*sin(phase));
            phase += dphi;
            if (phase >  M_PI) phase -= 2.0*M_PI;
            if (phase < -M_PI) phase += 2.0*M_PI;
        }
        char* slot = base + RING_CTRL_BYTES + (wc % RING_SLOTS)*RING_SLOT_STRIDE;
        u64 = IQ_FRAME_MAGIC;  memcpy(slot+0,  &u64, 8);
        u64 = seq;             memcpy(slot+8,  &u64, 8);
        u64 = 0;               memcpy(slot+16, &u64, 8);
        u32 = FRAME_SAMPLES;   memcpy(slot+24, &u32, 4);
        u32 = 0;               memcpy(slot+28, &u32, 4);
        u64 = (uint64_t)fs;    memcpy(slot+32, &u64, 8);
        i64 = 0;               memcpy(slot+40, &i64, 8);
        memcpy(slot+64, iq, FRAME_PAYLOAD);
        __sync_synchronize();
        ++wc; ++seq; bytes += FRAME_MSG_BYTES;
        memcpy(base+40, &wc,    8);
        memcpy(base+48, &bytes, 8);
        usleep(2000);
    }
    printf("fake_iqring: published %llu frames\n", (unsigned long long)wc);
    munmap(base, RING_TOTAL); close(fd);
    return 0;
}
