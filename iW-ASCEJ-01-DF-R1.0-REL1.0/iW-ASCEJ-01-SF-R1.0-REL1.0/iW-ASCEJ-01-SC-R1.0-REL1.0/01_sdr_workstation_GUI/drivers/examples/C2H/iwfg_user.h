/*
 * IWFG User-Space API Header
 * 
 * This header defines the user-space interface for the IWFG frame grabber
 * driver. It includes IOCTL definitions and data structures needed by
 * user-space applications.
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
    __s32 import_fd;     /* DMA-BUF fd to import (for DMA_IMPORT only) */
    bool is_chunked;     /* true if the buffer is chunked */
    __u32 chunk_size;    /* size of each chunk if chunked */
    
    /* Output fields (filled by driver) */
    __s32 dmabuf_fd;     /* Returned DMA-BUF fd (for DMA_ALLOC only, -1 otherwise) */
    __u32 buf_index;     /* Returned buffer index in driver's list */
};

/* DMA data transfer request structure */
struct iwfg_user_dma_req {
    void* buf;        /* Virtual address of the user buffer (NULL for DMA_ALLOC/IMPORT) */
    __u32 size;       /* Size of data to transfer */
    __u32 offset;     /* Offset within the buffer for chunked transfers */
    __u32 direction;
    __u32 buf_index;  /* Buffer index (for DMA_ALLOC/DMA_IMPORT when buf is NULL) */

    /**
     * during Chunked transfers, flags are used to indicate if the last chunk
     * is aquired by the DMA in case of c2h.
     * In case of h2c user has to flag the last packet.
     * 
     * @todo: make flags bit wise
     */
    __u32 flags;
};

/* Buffer flags for direction */
#define IWFG_DIR_C2H_BUF		0  /* Card-to-Host (RX) buffer */
#define IWFG_DIR_H2C_BUF		1  /* Host-to-Card (TX) buffer */

#define IWFG_FLAG_NORM_PACKET	0
#define IWFG_FLAG_LAST_PACKET	1

/* Buffer type for IWFG_IOCTL_ADD_BUF */
enum iwfg_buf_type {
	IWFG_BUF_TYPE_USERPTR = 0,    /* User pointer buffer (no export/import allowed) */
	IWFG_BUF_TYPE_DMA_ALLOC = 1,  /* Driver-allocated coherent DMA buffer (exportable) */
	IWFG_BUF_TYPE_DMA_IMPORT = 2, /* Import external DMA-BUF */
};

/* IOCTL definitions */
#define IWFG_MAGIC 'i'
#define IWFG_IOCTL_ADD_BUF			_IOWR(IWFG_MAGIC, 1, struct iwfg_user_buf)
#define IWFG_IOCTL_STREAM_START  	_IO(IWFG_MAGIC, 2) // DMA START
#define IWFG_IOCTL_STREAM_STOP  	_IO(IWFG_MAGIC, 3) // DMA STOP
#define IWFG_IOCTL_DMA_DATA			_IOWR(IWFG_MAGIC, 4, struct iwfg_user_dma_req) // DMA data transfer ioctl

#endif /* _IWFG_USER_H */
