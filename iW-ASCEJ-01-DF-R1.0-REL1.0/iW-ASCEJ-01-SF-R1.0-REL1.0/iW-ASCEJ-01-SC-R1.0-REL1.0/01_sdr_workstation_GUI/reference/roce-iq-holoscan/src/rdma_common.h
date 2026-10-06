/*
 * rdma_common.h
 *
 * Shared definitions for the RoCEv2 IQ streaming demo:
 *   - the on-the-wire frame format (rdma_tx -> rdma_rx)
 *   - the shared-memory ring ABI   (rdma_rx -> Holoscan Python app)
 *
 * IMPORTANT: viz/holoscan_iq_viz.py parses these structs by byte offset.
 * If you change anything here, update the Python side as well.
 */
#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Open-or-create a /dev/shm object across mixed sudo/user runs.
 *
 * Ubuntu sets fs.protected_regular=2, which makes an O_CREAT open of a
 * file owned by ANOTHER user inside the sticky, world-writable /dev/shm
 * fail with EACCES -- even for root. So if a previous run (as a different
 * user) left the file behind, plain re-open it instead of creating.
 * fchmod is best-effort: it fails harmlessly (EPERM) for non-owners. */
static inline int iq_shm_open_rw(const char *name)
{
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0 && errno == EACCES)
        fd = shm_open(name, O_RDWR, 0);
    if (fd >= 0)
        (void)fchmod(fd, 0666);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Wire protocol: one RDMA SEND == one "frame" = 64 B header + payload */
/* ------------------------------------------------------------------ */

#define IQ_FRAME_MAGIC   0x49515246524d4131ULL   /* "IQRFRMA1" */
#define RING_MAGIC       0x495152494e473031ULL   /* "IQRING01" */

#define DEFAULT_TCP_PORT 7471          /* rdma_cm service port               */
#define FRAME_SAMPLES    (256u * 1024u)/* IQ samples per frame (262,144)     */
#define BYTES_PER_SAMPLE 4u            /* int16 I + int16 Q                  */
#define FRAME_PAYLOAD    (FRAME_SAMPLES * BYTES_PER_SAMPLE)   /* 1 MiB       */

struct frame_hdr {
    uint64_t magic;            /* off  0: IQ_FRAME_MAGIC                     */
    uint64_t seq;              /* off  8: frame sequence number              */
    uint64_t t_ns;             /* off 16: sender CLOCK_REALTIME, ns          */
    uint32_t n_samples;        /* off 24: IQ samples in this frame           */
    uint32_t flags;            /* off 28: reserved                           */
    uint64_t sample_rate_hz;   /* off 32: nominal ADC sample rate            */
    int64_t  center_freq_hz;   /* off 40: RF center frequency for display    */
    uint8_t  pad[16];          /* off 48..63                                 */
};

#define FRAME_MSG_BYTES ((uint32_t)(sizeof(struct frame_hdr) + FRAME_PAYLOAD))

/* ------------------------------------------------------------------ */
/* Shared-memory ring: NIC DMA-writes frames straight into this file  */
/* (registered as one big MR). The Holoscan app mmaps it read-only.   */
/* Overwrite policy: drop-oldest. The reader always grabs the newest  */
/* published slot, so a slow visualizer never back-pressures the link.*/
/* ------------------------------------------------------------------ */

#define RING_SHM_NAME    "/iqring"     /* appears as /dev/shm/iqring         */
#define RING_SLOTS       32u
#define RING_CTRL_BYTES  4096u
/* round each slot up to a 4 KiB boundary */
#define RING_SLOT_STRIDE ((uint32_t)(((FRAME_MSG_BYTES + 4095u) / 4096u) * 4096u))
#define RING_TOTAL_BYTES ((size_t)RING_CTRL_BYTES + (size_t)RING_SLOTS * RING_SLOT_STRIDE)

struct ring_ctrl {
    uint64_t magic;                 /* off  0: RING_MAGIC (written last)     */
    uint32_t version;               /* off  8                                */
    uint32_t num_slots;             /* off 12                                */
    uint32_t slot_stride;           /* off 16: bytes between slot starts     */
    uint32_t frame_samples;         /* off 20                                */
    uint64_t sample_rate_hz;        /* off 24: copied from first frame       */
    int64_t  center_freq_hz;        /* off 32                                */
    volatile uint64_t write_count;  /* off 40: frames published (release)    */
    volatile uint64_t bytes_total;  /* off 48: payload+hdr bytes received    */
    volatile uint64_t seq_gaps;     /* off 56: missing frames detected       */
    uint8_t  pad[RING_CTRL_BYTES - 64];
};

/* ------------------------------------------------------------------ */
/* Transport operation mode (selected with -w on both ends).           */
/*                                                                     */
/*   XPORT_SEND   two-sided SEND/RECV. Receiver posts RECVs; the NIC   */
/*                matches each incoming SEND to one. Built-in RNR flow  */
/*                control and a completion per frame. Simple, robust.   */
/*                                                                     */
/*   XPORT_WRITE  one-sided RDMA WRITE. Receiver advertises its ring    */
/*                (addr+rkey) once at connect; the sender writes frames */
/*                directly into remote memory. The receiver's CPU posts */
/*                nothing and gets NO per-frame completion -- it polls   */
/*                the ring. Flow control is the sender's responsibility  */
/*                (it must not lap a slot still being read).            */
/*                                                                     */
/*   XPORT_WRITE_IMM  one-sided RDMA WRITE_WITH_IMM. Same one-sided      */
/*                write, but each carries a 32-bit immediate (the slot   */
/*                index) that lands as a RECV completion -- true         */
/*                one-sided RDMA AND a per-frame arrival signal. This is */
/*                what most FPGA RDMA cores emit; recommended default.   */
/* ------------------------------------------------------------------ */
enum xport_mode { XPORT_SEND = 0, XPORT_WRITE = 1, XPORT_WRITE_IMM = 2 };

static inline const char *xport_name(int m)
{
    switch (m) {
    case XPORT_WRITE:     return "RDMA_WRITE (one-sided, polled)";
    case XPORT_WRITE_IMM: return "RDMA_WRITE_WITH_IMM (one-sided + signal)";
    default:              return "SEND/RECV (two-sided)";
    }
}

/* Sent receiver->sender in the rdma_cm CONNECT private_data so the sender
 * knows where to WRITE. Payload lands at
 *   ring_addr + ctrl_bytes + slot*slot_stride + sizeof(frame_hdr)
 * and the header at the slot start, mirroring the SEND layout so the
 * visualizer's parser is identical across all three modes. */
struct rdma_region_info {
    uint64_t magic;         /* RINFO_MAGIC                               */
    uint64_t addr;          /* base VA of the receiver's registered ring */
    uint32_t rkey;          /* remote key for that region                */
    uint32_t num_slots;     /* RING_SLOTS (sender wraps modulo this)     */
    uint32_t slot_stride;   /* RING_SLOT_STRIDE                          */
    uint32_t ctrl_bytes;    /* RING_CTRL_BYTES (offset to slot 0)        */
};
#define RINFO_MAGIC 0x52494E464F303031ULL   /* "RINFO001" */

/* ------------------------------------------------------------------ */
/* GPUDirect mode (rdma_rx_gpu): payloads land in GPU memory.          */
/*                                                                     */
/* Host shm file layout changes to:                                    */
/*   [0..4095]   struct ring_ctrl, but magic = RING_MAGIC_GPU and the  */
/*               64-byte cudaIpcMemHandle_t stored at offset 1024      */
/*   [4096.. ]   RING_SLOTS x 64 B frame headers (NIC scatters SGE[0]  */
/*               here, SGE[1] = payload into the GPU ring)             */
/* GPU ring: RING_SLOTS x FRAME_PAYLOAD, opened by Python via CUDA IPC.*/
/* ------------------------------------------------------------------ */
#define RING_MAGIC_GPU   0x495152494e474731ULL   /* "IQRINGG1" */
#define CTRL_IPC_OFF     1024u                   /* cudaIpcMemHandle_t */
#define GPU_HDR_BASE     RING_CTRL_BYTES         /* header slots start  */
#define GPU_HOST_BYTES   ((size_t)RING_CTRL_BYTES + (size_t)RING_SLOTS * 64u)
#define GPU_SLOT_STRIDE  FRAME_PAYLOAD           /* payload-only slots  */

/* ------------------------------------------------------------------ */
/* Live runtime control: a tiny shm block the visualizer writes and    */
/* rdma_tx polls (1 Hz). /dev/shm is shared across network namespaces, */
/* so this works even though tx lives in ns_tx. Bump 'seq' after       */
/* changing fields; tx applies on seq change.                          */
/* ------------------------------------------------------------------ */
#define IQCTL_SHM_NAME   "/iqctl"
#define IQCTL_MAGIC      0x495143544c303031ULL   /* "IQCTL001" */
#define IQCTL_BYTES      4096u

struct iq_ctl {
    uint64_t magic;            /* off  0                                */
    uint32_t version;          /* off  8                                */
    volatile uint32_t seq;     /* off 12: bump after writing fields     */
    double   pace_gbps;        /* off 16: 0 = unlimited                 */
    uint64_t sample_rate_hz;   /* off 24                                */
    int64_t  center_freq_hz;   /* off 32                                */
    uint8_t  pad[IQCTL_BYTES - 40];
};

/* Lock the ABI so the Python parser can rely on fixed offsets. */
_Static_assert(sizeof(struct frame_hdr) == 64,            "frame_hdr must be 64 B");
_Static_assert(offsetof(struct frame_hdr, n_samples) == 24, "frame_hdr ABI");
_Static_assert(offsetof(struct frame_hdr, sample_rate_hz) == 32, "frame_hdr ABI");
_Static_assert(offsetof(struct frame_hdr, center_freq_hz) == 40, "frame_hdr ABI");
_Static_assert(sizeof(struct ring_ctrl) == RING_CTRL_BYTES, "ring_ctrl must be 4096 B");
_Static_assert(offsetof(struct ring_ctrl, write_count) == 40, "ring_ctrl ABI");
_Static_assert(offsetof(struct ring_ctrl, bytes_total) == 48, "ring_ctrl ABI");
_Static_assert(offsetof(struct ring_ctrl, seq_gaps)    == 56, "ring_ctrl ABI");
_Static_assert(sizeof(struct iq_ctl) == IQCTL_BYTES,        "iq_ctl must be 4096 B");
_Static_assert(offsetof(struct iq_ctl, pace_gbps)      == 16, "iq_ctl ABI");
_Static_assert(offsetof(struct iq_ctl, sample_rate_hz) == 24, "iq_ctl ABI");
_Static_assert(offsetof(struct iq_ctl, center_freq_hz) == 32, "iq_ctl ABI");

#endif /* RDMA_COMMON_H */
