/*
 * IWFG User-Space API Header  (v2 - current driver ABI)
 *
 * Mirrors the kernel-side definitions in iwfg.h bit-for-bit. If iwfg.h's
 * ioctl structs change, this file must be updated to match.
 *
 * DRIVER CONTRACT SUMMARY (source of truth: iwfg_main.c / iwfg_dma.c):
 *
 *  Device nodes   /dev/iwfg0 = C2H (qid 0)   /dev/iwfg1 = H2C (qid 1)
 *                 ("/dev/iwfg" does not exist.) Transfer direction is
 *                 derived in-kernel from which node was opened; the
 *                 dma_req.direction field is informational only.
 *
 *  Lifecycle      open()  -> queue init (+ C2H hardware stream start)
 *                 close() -> queue teardown, all buffers unpinned,
 *                            C2H-only QDMA soft reset when idle.
 *                 The old STREAM_START/STREAM_STOP ioctls were REMOVED
 *                 (-ENOTTY). Do not call them.
 *
 *  ADD_BUF        buf_type = IWFG_BUF_TYPE_USERPTR and import_fd = -1
 *                 are mandatory. Pages are pinned, DMA-mapped, and the
 *                 direction-appropriate descriptor list is prebuilt.
 *                 Returns buf_index. Duplicate (addr,size) registrations
 *                 dedupe to the existing handle.
 *
 *  DMA_DATA       Blocking. Pass buf = NULL + buf_index for registered
 *                 buffers (or a raw pointer, which auto-registers).
 *                 H2C: posts the buffer's entire descriptor list and
 *                      returns when the card has consumed it. offset and
 *                      flags are ignored on this path. Never EAGAIN.
 *                      Errors: EINTR (signal), EBUSY (reset in progress),
 *                      ETIMEDOUT (engine busy).
 *                 C2H: returns when the buffer is completely filled.
 *                      EAGAIN = FPGA fifo-overflow recovery ran; retry.
 *                      An optional struct iwfg_c2h_prearm may be appended
 *                      directly after the dma_req in the SAME ioctl arg;
 *                      the driver then posts the next buffer's
 *                      descriptors before the current one drains
 *                      (prevents descriptor starvation between ioctls).
 *                      next_size MUST equal the current dma_req.size or
 *                      the prearm block is ignored.
 *
 *  Teardown order (userspace): close(FIFO/pipes) -> close(device fd)
 *                 [driver unpins pages HERE] -> free() buffers. Never
 *                 free a registered buffer while the device fd is open.
 */

#ifndef _IWFG_USER_H
#define _IWFG_USER_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include <stdbool.h>

/* User buffer configuration */
struct iwfg_user_buf {
    /* Input fields */
    void *buf;           /* User virtual address (for USERPTR), NULL for DMA_ALLOC, ignored for DMA_IMPORT */
    __u32 size;          /* Buffer size */
    __u32 buf_type;      /* enum iwfg_buf_type */
    __s32 import_fd;     /* DMA-BUF fd to import (for DMA_IMPORT only), else -1 */
    bool is_chunked;     /* true if the buffer is chunked */
    __u32 chunk_size;    /* size of each chunk if chunked */

    /* Output fields (filled by driver) */
    __s32 dmabuf_fd;     /* Returned DMA-BUF fd (for DMA_ALLOC only, -1 otherwise) */
    __u32 buf_index;     /* Returned buffer index in driver's per-cdev list */
};

/* DMA data transfer request structure */
struct iwfg_user_dma_req {
    void *buf;        /* Virtual address of the user buffer (NULL to use buf_index) */
    __u32 size;       /* Size of data to transfer */
    __u32 offset;     /* Offset within the buffer for chunked transfers (H2C: ignored) */
    __u32 direction;  /* IWFG_DIR_* - informational; direction comes from the node */
    __u32 buf_index;  /* Buffer index returned by ADD_BUF (used when buf == NULL) */
    __u32 flags;      /* IWFG_FLAG_* (H2C: ignored by the current driver) */
};

/*
 * Optional pre-arm block for C2H: append IMMEDIATELY after
 * struct iwfg_user_dma_req in the same ioctl argument, e.g.
 *
 *   struct { struct iwfg_user_dma_req req; struct iwfg_c2h_prearm prearm; } arg;
 *
 * The driver validates next_size == req.size; anything else is treated
 * as absent (older userspace / stack noise) and ignored.
 */
struct iwfg_c2h_prearm {
    void  *next_buf;   /* user VA of the NEXT ring buffer (must be registered
                          or registrable), or NULL for no pre-arming */
    __u32  next_size;  /* must equal the current dma_req.size */
};

/* Direction values for iwfg_user_dma_req.direction */
#define IWFG_DIR_C2H_BUF        0  /* Card-to-Host (RX) */
#define IWFG_DIR_H2C_BUF        1  /* Host-to-Card (TX) */

#define IWFG_FLAG_NORM_PACKET   0
#define IWFG_FLAG_LAST_PACKET   1

/* Buffer type for IWFG_IOCTL_ADD_BUF */
enum iwfg_buf_type {
    IWFG_BUF_TYPE_USERPTR    = 0, /* User pointer buffer (no export/import) */
    IWFG_BUF_TYPE_DMA_ALLOC  = 1, /* Driver-allocated coherent DMA buffer (ARM only) */
    IWFG_BUF_TYPE_DMA_IMPORT = 2, /* Import external DMA-BUF (ARM only) */
};

/* Stream parameters returned by IWFG_IOCTL_QUERY_STREAM_PARAMS */
struct iwfg_stream_params {
    __u32 width;
    __u32 height;
    __u32 bpp;
};

/* IOCTL definitions.
 * NOTE: numbers 2 and 3 (the old STREAM_START/STREAM_STOP) are retired;
 * the current driver returns -ENOTTY for them. They are intentionally
 * NOT defined here so stale call sites fail at compile time. */
#define IWFG_MAGIC 'i'
#define IWFG_IOCTL_ADD_BUF              _IOWR(IWFG_MAGIC, 1, struct iwfg_user_buf)
#define IWFG_IOCTL_DMA_DATA             _IOWR(IWFG_MAGIC, 4, struct iwfg_user_dma_req)
#define IWFG_IOCTL_QUERY_STREAM_PARAMS  _IOR (IWFG_MAGIC, 5, struct iwfg_stream_params)

#endif /* _IWFG_USER_H */
