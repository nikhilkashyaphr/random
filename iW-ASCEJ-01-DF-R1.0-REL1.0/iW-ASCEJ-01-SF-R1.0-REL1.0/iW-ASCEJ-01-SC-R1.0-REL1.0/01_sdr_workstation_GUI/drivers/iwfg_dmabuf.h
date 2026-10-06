/*
 * IWFG DMA-BUF Support Header
 *
 * This header defines the DMA-BUF export and import functionality
 * for the IWFG frame grabber driver.
 *
 * Copyright (c) 2020 Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#ifndef IWFG_DMABUF_H
#define IWFG_DMABUF_H

/* Forward declarations */
struct iwfg_private;
struct iwfg_user_page_buf;
struct iwfg_user_buf;

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)

#include <linux/dma-buf.h>
#include <linux/dma-resv.h>

/**
 * iwfg_dmabuf_export - Export a buffer as DMA-BUF
 * @user_buf_hndl: user buffer handle to export
 *
 * Only DMA_ALLOC buffers can be exported. USERPTR and DMA_IMPORT
 * buffers will be rejected.
 *
 * Returns: 0 on success, negative error code on failure
 */
int iwfg_dmabuf_export(struct iwfg_user_page_buf *user_buf_hndl);

/**
 * iwfg_get_dmabuf_fd - Get a file descriptor for an exported DMA-BUF
 * @priv: device private data
 * @buf_index: index of the buffer in the buffer list
 *
 * If the buffer is not already exported, it will be exported first.
 * Only DMA_ALLOC buffers can be exported.
 *
 * Returns: file descriptor on success, negative error code on failure
 */
int iwfg_get_dmabuf_fd(struct iwfg_private *priv, u32 buf_index);

#else /* !CONFIG_ARM && !CONFIG_ARM64 */

static inline int iwfg_dmabuf_export(struct iwfg_user_page_buf *user_buf_hndl)
{
	pr_err("iwfg: DMA-BUF export is only supported on ARM platforms\n");
	return -EOPNOTSUPP;
}

static inline int iwfg_get_dmabuf_fd(struct iwfg_private *priv, u32 buf_index)
{
	pr_err("iwfg: DMA-BUF fd export is only supported on ARM platforms\n");
	return -EOPNOTSUPP;
}

#endif /* CONFIG_ARM || CONFIG_ARM64 */

#endif /* IWFG_DMABUF_H */
