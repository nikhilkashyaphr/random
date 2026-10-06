/*
 * iwfg_uapi.h - Userspace ABI mirror of the ioctl interface defined in
 * iwfg.h.
 *
 * WHY THIS FILE EXISTS (gap identified during analysis):
 * -------------------------------------------------------
 * iwfg.h is written to be included from BOTH the kernel driver and
 * userspace, but it actually pulls in kernel-only headers
 * (<linux/ioctl.h>, <linux/cpumask.h>, <linux/bitops.h>, <linux/cdev.h>)
 * and kernel-only types (u32, bool via kernel typedefs, dev_t, struct
 * cdev, struct device, ...) through struct iwfg_cdev / iwfg_private.
 * A plain userspace .c file cannot #include "iwfg.h" directly - it will
 * fail to compile outside the kernel build system.
 *
 * This header extracts exactly the subset of iwfg.h that userspace
 * actually needs to drive the character device: the IOCTL numbers, the
 * request/response structs, and the enum/flag values. The struct layouts
 * below are bit-for-bit identical to the kernel-side definitions (u32 ==
 * uint32_t, kernel bool == C99 _Bool, same member order/count), so
 * copy_from_user()/copy_to_user() in the driver will interpret them
 * identically regardless of which header produced them.
 *
 * If iwfg.h's ioctl structs ever change, this file must be updated to
 * match.
 */
#ifndef __IWFG_UAPI_H__
#define __IWFG_UAPI_H__

#include <stdint.h>
#include <stdbool.h>
#include <sys/ioctl.h>

/* DMA direction used for iwfg_user_dma_req.direction (informational only;
 * the driver actually derives direction from which /dev node was opened,
 * see icdev->direction in iwfg_ioctl()). */
#define IWFG_DIR_C2H_BUF		0
#define IWFG_DIR_H2C_BUF		1

/* Chunked-transfer packet flags (H2C: set by userspace; C2H: set by driver) */
#define IWFG_FLAG_NORM_PACKET	0
#define IWFG_FLAG_LAST_PACKET	1

/* Buffer type for IWFG_IOCTL_ADD_BUF */
enum iwfg_buf_type {
	IWFG_BUF_TYPE_USERPTR = 0,    /* User pointer buffer (no export/import allowed) */
	IWFG_BUF_TYPE_DMA_ALLOC = 1,  /* Driver-allocated coherent DMA buffer (exportable) */
	IWFG_BUF_TYPE_DMA_IMPORT = 2, /* Import external DMA-BUF */
};

#define IWFG_MAGIC 'i'
#define IWFG_IOCTL_ADD_BUF			_IOWR(IWFG_MAGIC, 1, struct iwfg_user_buf)
#define IWFG_IOCTL_STREAM_START		_IO(IWFG_MAGIC, 2)
#define IWFG_IOCTL_STREAM_STOP		_IO(IWFG_MAGIC, 3)
#define IWFG_IOCTL_DMA_DATA			_IOWR(IWFG_MAGIC, 4, struct iwfg_user_dma_req)
#define IWFG_IOCTL_QUERY_STREAM_PARAMS	_IOR(IWFG_MAGIC, 5, struct iwfg_stream_params)

struct iwfg_user_buf {
	/* Input fields */
	void *buf;
	uint32_t size;
	uint32_t buf_type;
	int32_t  import_fd;
	bool     is_chunked;
	uint32_t chunk_size;

	/* Output fields */
	int32_t  dmabuf_fd;
	uint32_t buf_index;
};

struct iwfg_user_dma_req {
	void *buf;
	uint32_t size;
	uint32_t offset;
	uint32_t direction;
	uint32_t buf_index;
	uint32_t flags;
};

struct iwfg_stream_params {
	uint32_t width;
	uint32_t height;
	uint32_t bpp;
};

#endif /* __IWFG_UAPI_H__ */
