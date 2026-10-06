/*
 * Copyright (c) 2020 Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */
#ifndef __IWFG_H__
#define __IWFG_H__

#include <linux/ioctl.h>
#include <linux/cpumask.h>
#include <linux/bitops.h>
#include <linux/cdev.h>

#include "iwfg_hardware.h"
#include "iwfg_register.h"

#define IWFG_DRIVER_NAME "iwfg"
// #include "iwfg_dma.h"

// #define IWFG_MAX_QUEUES			64
#define IWFG_MAX_QUEUES			6 // even rx, odd tx
#define IWFG_MAX_VECTORS		IWFG_MAX_QUEUES + 2 // even rx, odd tx

/* state bits */
#define IWFG_ERROR_INTR			0
#define IWFG_USER_INTR			1

/* flag bits */
#define IWFG_FLAG_MASTER_PF		0

#define IWFG_DIR_C2H_BUF		0
#define IWFG_DIR_H2C_BUF		1

#define IWFG_FLAG_NORM_PACKET	0
#define IWFG_FLAG_LAST_PACKET	1

#define IWFG_CDEV_NAME_MAXLEN	32

/* Buffer type for IWFG_IOCTL_ADD_BUF */
enum iwfg_buf_type {
	IWFG_BUF_TYPE_USERPTR = 0,    /* User pointer buffer (no export/import allowed) */
	IWFG_BUF_TYPE_DMA_ALLOC = 1,  /* Driver-allocated coherent DMA buffer (exportable) */
	IWFG_BUF_TYPE_DMA_IMPORT = 2, /* Import external DMA-BUF */
};

#define IWFG_MAGIC 'i'
#define IWFG_IOCTL_ADD_BUF			_IOWR(IWFG_MAGIC, 1, struct iwfg_user_buf) // map list of user buffer to qdma queues
#define IWFG_IOCTL_STREAM_START  	_IO(IWFG_MAGIC, 2) // DMA START
#define IWFG_IOCTL_STREAM_STOP  	_IO(IWFG_MAGIC, 3) // DMA STOP
#define IWFG_IOCTL_DMA_DATA		_IOWR(IWFG_MAGIC, 4, struct iwfg_user_dma_req) // DMA data transfer ioctl

#define IWFG_IOCTL_QUERY_STREAM_PARAMS	_IOR(IWFG_MAGIC, 5, struct iwfg_stream_params)

struct iwfg_user_buf {
	/* Input fields */
	void *buf;           /* User virtual address (for USERPTR), NULL for DMA_ALLOC, ignored for DMA_IMPORT */
	u32 size;            /* Buffer size */
	u32 buf_type;        /* enum iwfg_buf_type */
	s32 import_fd;       /* DMA-BUF fd to import (for DMA_IMPORT only) */
	bool is_chunked;     /* true if the buffer is chunked */
	u32 chunk_size;      /* size of each chunk if chunked */
	
	/* Output fields */
	s32 dmabuf_fd;       /* Returned DMA-BUF fd (for DMA_ALLOC only, -1 otherwise) */
	u32 buf_index;       /* Returned buffer index in driver's list */
};

struct iwfg_user_dma_req {
	void* buf; // Virtual address of the user buffer (NULL for DMA_ALLOC/IMPORT)
	u32 size; // Size of the data of current transfer
	u32 offset; // Offset within the buffer for chunked transfers
	u32 direction;
	u32 buf_index; // Buffer index (for DMA_ALLOC/DMA_IMPORT when buf is NULL)

	/** In case of c2h the flag is set by the driver
	 * and in case of h2c the flag is set by the user.
	 * In case of chunked transfer,
	 * flag norm_packet --> 0, last_packet --> 1 
	 * 
	 * @todo: make flags bit wise
	 */
	u32 flags;
};

/*
 * Supplied alongside every C2H DMA_DATA IOCTL.
 * See iwfg_user.h for full documentation.
 */
struct iwfg_c2h_prearm {
	void __user *next_buf;
	u32          next_size;
};

struct iwfg_cdev {
	/** lsit of qdma character devices */
	struct list_head list;
	/** minor number */
	int minor;
	/** character device number */
	dev_t cdevno;
	/** pointer to kernel device(struct device) */
	struct device *sys_device;
	/** pointer to kernel cdev(struct cdev) */
	struct cdev cdev;

	u32 qid;
	u32 direction;

	u32 stream_id;

	/* flag to indicate if memcpy is required */
	unsigned char no_memcpy;

	struct iwfg_private *priv; //back reference

	bool is_audio;

	struct list_head dma_buf_list; // List of iwfg_user_page_buf
	atomic_t stream_active;

	bool is_video_locked;
	bool is_audio_locked;

	char name[IWFG_CDEV_NAME_MAXLEN];
};

struct iwfg_stream_params {
	u32 width;
	u32 height;
	u32 bpp;
};

/**
 * struct iwfg_private - iwfg driver private data
 **/
struct iwfg_private {
	struct list_head dev_list;
	struct list_head cdev_list; // List of iwfg_cdev

	struct pci_dev *pdev;
	DECLARE_BITMAP(state, 32);
	DECLARE_BITMAP(flags, 32);

	u16 num_q_vectors;
	u16 num_tx_queues;
	u16 num_rx_queues;

	spinlock_t tx_lock;
	spinlock_t rx_lock;

	struct iwfg_q_vector *q_vector[IWFG_MAX_VECTORS];
	struct iwfg_qdma_queue *queue[IWFG_MAX_QUEUES]; // TODO: make this "queues"

	struct iwfg_hardware hw;

	/** currently using this as a workaround to reset the hardware 
	 * when fifo full error is encountered from fpga.
	 * This will prevent qdma from reseting while h2c is busy.
	 * 0 --> idle, 1 --> busy, 2 --> reset */
	atomic_t h2c_busy;

	dev_t cdevno_base;

	u32 num_streams;
	u32 num_hdmi_inputs;
	u32 num_hdmi_outputs;
	bool has_audio;
};

static inline u32 iwfg_stream_read_reg(struct iwfg_hardware *hw, int stream_id, u32 offset)
{
	return iwfg_read_reg(hw, stream_id * IWFG_VID_STREAM_BRAM_LEN + offset);
}

static inline void iwfg_stream_write_reg(struct iwfg_hardware *hw, int stream_id, u32 offset, u32 val)
{
	iwfg_write_reg(hw, stream_id * IWFG_VID_STREAM_BRAM_LEN + offset, val);
}

#endif
