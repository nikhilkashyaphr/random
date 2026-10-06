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
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/moduleparam.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

#include "iwfg.h"
#include "iwfg_hardware.h"
#include "iwfg_intr.h"
#include "iwfg_common.h"
#include "iwfg_dma.h"

#define DRV_STR "iWave Frame Grabber Linux Kernel Driver"
char iwfg_drv_name[] = IWFG_DRIVER_NAME;
#define IWFG_CDEV_CLASS_NAME IWFG_DRIVER_NAME


static struct class *iwfg_class;

/* Module parameter for debug control */
static bool debug_enable = false;
module_param(debug_enable, bool, 0644);
MODULE_PARM_DESC(debug_enable, "Enable debug messages (default: false)");

#define DRV_VER "0.1"
const char iwfg_drv_str[] = DRV_STR;
const char iwfg_drv_ver[] = DRV_VER;

MODULE_AUTHOR("iWave Systems Technologies Pvt. Ltd.");
MODULE_DESCRIPTION(DRV_STR);
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRV_VER);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif

static const struct pci_device_id iwfg_pci_tbl[] = {
	/** Gen 1 PF */
	/** PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0x9011), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9111), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9211), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9311), },	/** PF 3 */
	/** PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0x9012), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9112), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9212), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9312), },	/** PF 3 */
	/** PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0x9014), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9114), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9214), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9314), },	/** PF 3 */
	/** PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0x9018), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9118), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9218), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9318), },	/** PF 3 */
	/** PCIe lane width x16 */
	{ PCI_DEVICE(0x10ee, 0x901f), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x911f), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x921f), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x931f), },	/** PF 3 */

	/** Gen 2 PF */
	/** PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0x9021), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9121), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9221), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9321), },	/** PF 3 */
	/** PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0x9022), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9122), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9222), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9322), },	/** PF 3 */
	/** PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0x9024), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9124), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9224), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9324), },	/** PF 3 */
	/** PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0x9028), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9128), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9228), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9328), },	/** PF 3 */
	/** PCIe lane width x16 */
	{ PCI_DEVICE(0x10ee, 0x902f), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x912f), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x922f), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x932f), },	/** PF 3 */

	/** Gen 3 PF */
	/** PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0x9031), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9131), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9231), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9331), },	/** PF 3 */
	/** PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0x9032), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9132), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9232), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9332), },	/** PF 3 */
	/** PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0x9034), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9134), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9234), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9334), },	/** PF 3 */
	/** PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0x9038), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9138), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9238), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9338), },	/** PF 3 */
	/** PCIe lane width x16 */
	{ PCI_DEVICE(0x10ee, 0x903f), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x913f), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x923f), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x933f), },	/** PF 3 */
	/* { PCI_DEVICE(0x10ee, 0x6a9f), }, */       /** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x6aa0), },	/** PF 1 */

	/** Gen 4 PF */
	/** PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0x9041), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9141), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9241), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9341), },	/** PF 3 */
	/** PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0x9042), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9142), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9242), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9342), },	/** PF 3 */
	/** PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0x9044), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9144), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9244), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9344), },	/** PF 3 */
	/** PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0x9048), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0x9148), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0x9248), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0x9348), },	/** PF 3 */

	/** Gen 1 PF */
	/** PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0xb011), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb111), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb211), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb311), },	/** PF 3 */
	/** PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0xb012), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb112), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb212), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb312), },	/** PF 3 */
	/** PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0xb014), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb114), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb214), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb314), },	/** PF 3 */
	/** PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0xb018), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb118), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb218), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb318), },	/** PF 3 */
	/** PCIe lane width x16 */
	{ PCI_DEVICE(0x10ee, 0xb01f), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb11f), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb21f), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb31f), },	/** PF 3 */

	/** Gen 2 PF */
	/** PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0xb021), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb121), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb221), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb321), },	/** PF 3 */
	/** PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0xb022), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb122), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb222), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb322), },	/** PF 3 */
	/** PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0xb024), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb124), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb224), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb324), },	/** PF 3 */
	/** PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0xb028), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb128), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb228), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb328), },	/** PF 3 */
	/** PCIe lane width x16 */
	{ PCI_DEVICE(0x10ee, 0xb02f), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb12f), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb22f), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb32f), },	/** PF 3 */

	/** Gen 3 PF */
	/** PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0xb031), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb131), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb231), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb331), },	/** PF 3 */
	/** PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0xb032), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb132), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb232), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb332), },	/** PF 3 */
	/** PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0xb034), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb134), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb234), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb334), },	/** PF 3 */
	/** PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0xb038), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb138), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb238), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb338), },	/** PF 3 */
	/** PCIe lane width x16 */
	{ PCI_DEVICE(0x10ee, 0xb03f), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb13f), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb23f), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb33f), },	/** PF 3 */

	/** Gen 4 PF */
	/** PCIe lane width x1 */
	{ PCI_DEVICE(0x10ee, 0xb041), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb141), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb241), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb341), },	/** PF 3 */
	/** PCIe lane width x2 */
	{ PCI_DEVICE(0x10ee, 0xb042), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb142), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb242), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb342), },	/** PF 3 */
	/** PCIe lane width x4 */
	{ PCI_DEVICE(0x10ee, 0xb044), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb144), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb244), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb344), },	/** PF 3 */
	/** PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0xb048), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb148), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb248), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb348), },	/** PF 3 */

	/** Gen 5 PF */
	/** PCIe lane width x8 */
	{ PCI_DEVICE(0x10ee, 0xb058), },	/** PF 0 */
	{ PCI_DEVICE(0x10ee, 0xb158), },	/** PF 1 */
	{ PCI_DEVICE(0x10ee, 0xb258), },	/** PF 2 */
	{ PCI_DEVICE(0x10ee, 0xb358), },	/** PF 3 */

	{0,}
};

MODULE_DEVICE_TABLE(pci, iwfg_pci_tbl);

// static void iwfg_signal_pci_linkup(struct iwfg_private *priv)
// {
// 	u32 reg_val = 0;
// 	reg_val |= IWFG_H2C_INTR_BIT_MASK;
// 	/**
// 	 * Signal firmware that BAR2 is ready to be accessed
// 	 */
// 	iwfg_write_reg(&priv->hw, IWFG_H2C_INTR_REG_OFFSET, reg_val);
// }

// TODO: move this function to a separate file
static struct iwfg_stream_params iwfg_query_stream_params(struct iwfg_private *priv, int stream_id)
{
    u32 reg_val = 0;
    u32 width, height, bpp;
 
    // mandatory delay before read
    msleep(100);
    reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_CARD_VID_STREAM_PRAM_REG_OFFSET);
    width = BITFIELD_GET(IWFG_CARD_VID_STREAM_PRAM_WIDTH_MASK, reg_val);
    height = BITFIELD_GET(IWFG_CARD_VID_STREAM_PRAM_HEIGHT_MASK, reg_val);
    bpp = BITFIELD_GET(IWFG_CARD_VID_STREAM_PRAM_BPP_MASK, reg_val);
 
    struct iwfg_stream_params params = {
        .width = width,
        .height = height,
        .bpp = bpp
    };
 
    return params;
}

static void iwfg_query_card_config(struct iwfg_private *priv)
{
	u32 reg_val = 0;
	u32 num_hdmi_inputs, num_hdmi_outputs, has_audio;

	reg_val |= IWFG_H2C_INTR_BIT_MASK | 
				IWFG_H2C_INTR_CARD_CONFIG_QUERY_MASK;
	/**
	 * generate query signal for card configuration
	 */
	iwfg_write_reg(&priv->hw, IWFG_H2C_INTR_REG_OFFSET, reg_val);

	msleep(100);
	reg_val = iwfg_read_reg(&priv->hw, IWFG_CARD_CONFIG_REG_OFFSET);
	num_hdmi_inputs = BITFIELD_GET(IWFG_CARD_CONFIG_NUM_HDMI_INPUTS_MASK, reg_val);
	num_hdmi_outputs = BITFIELD_GET(IWFG_CARD_CONFIG_NUM_HDMI_OUTPUTS_MASK, reg_val);
	has_audio = BITFIELD_GET(IWFG_CARD_CONFIG_HAS_AUDIO_MASK, reg_val);

	priv->num_hdmi_inputs = num_hdmi_inputs;
	priv->num_hdmi_outputs = num_hdmi_outputs;
	priv->has_audio = has_audio;
}

static long iwfg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
struct iwfg_cdev *icdev = file->private_data;
struct iwfg_private *priv = icdev->priv;
int ret = 0;
u32 dir;

switch (cmd) {
case IWFG_IOCTL_ADD_BUF: {
struct iwfg_user_buf user_buf_req;
struct iwfg_user_page_buf *user_buf_hndl;
u32 index = 0;

if (copy_from_user(&user_buf_req, (void __user *)arg, sizeof(user_buf_req))) {
ret = -EFAULT;
break;
}

switch (user_buf_req.buf_type) {
case IWFG_BUF_TYPE_USERPTR:
	user_buf_hndl = iwfg_add_user_buf(priv, icdev,
		user_buf_req.buf, user_buf_req.size,
		user_buf_req.is_chunked, user_buf_req.chunk_size,
		icdev->is_audio);
	if (!user_buf_hndl) {
		ret = -EINVAL;
		break;
	}
	list_for_each_entry(user_buf_hndl, &icdev->dma_buf_list, list) {
		if (user_buf_hndl->user_addr == user_buf_req.buf &&
		    user_buf_hndl->usr_buf_size == user_buf_req.size)
			break;
		index++;
	}
	user_buf_req.buf_index = index;
	user_buf_req.dmabuf_fd = -1;
	break;
#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
case IWFG_BUF_TYPE_DMA_ALLOC:
	ret = iwfg_add_dma_alloc_buf(priv, &user_buf_req);
	break;
case IWFG_BUF_TYPE_DMA_IMPORT:
	ret = iwfg_add_dma_import_buf(priv, &user_buf_req);
	break;
#endif
default:
	ret = -EINVAL;
	break;
}

if (ret == 0 &&
    copy_to_user((void __user *)arg, &user_buf_req, sizeof(user_buf_req)))
	ret = -EFAULT;
break;
}
case IWFG_IOCTL_DMA_DATA: {
struct iwfg_user_dma_req   dma_req;
struct iwfg_c2h_prearm     prearm;
struct iwfg_user_page_buf *user_buf      = NULL;
struct iwfg_user_page_buf *next_user_buf = NULL;

if (copy_from_user(&dma_req, (void __user *)arg, sizeof(dma_req))) {
ret = -EFAULT;
break;
}

if (dma_req.buf)
	user_buf = iwfg_add_user_buf(priv, icdev, dma_req.buf, dma_req.size,
	     0, 0, icdev->is_audio);
else
	user_buf = iwfg_get_user_buf_by_index(priv, icdev, dma_req.buf_index);

dev_dbg(&priv->pdev->dev,
"DMA data request: buf=%p, size=%u, direction=0x%x, minor=%d",
dma_req.buf, dma_req.size, icdev->direction, icdev->minor);

if (user_buf == NULL) {
dev_err(&priv->pdev->dev,
"Failed to find the buffer: addr=%p, index=%u, size=%u",
dma_req.buf, dma_req.buf_index, dma_req.size);
ret = -EINVAL;
break;
}

if (icdev->direction == DMA_TO_DEVICE) { /* h2c */
ret = iwfg_xmit_data(priv, user_buf, icdev->qid);
} else if (icdev->direction == DMA_FROM_DEVICE) { /* c2h */
/*
 * Read the optional pre-arm block that userspace
 * appends right after dma_req in the same ioctl arg.
 * When present, the driver can post next_user_buf descriptors
 * before the current buffer drains, keeping the QDMA ring from
 * reaching descriptor starvation between IOCTLs.
 */
memset(&prearm, 0, sizeof(prearm));
if (copy_from_user(&prearm,
	(void __user *)arg + sizeof(dma_req),
	sizeof(prearm))) {
	/* Optional extension block may be absent for older userspace. */
	memset(&prearm, 0, sizeof(prearm));
}

/*
 * BUGFIX: userspace that passes only struct iwfg_user_dma_req (no
 * prearm block appended) leaves whatever happens to sit on its stack
 * after the struct - copy_from_user() then *succeeds* and returns
 * garbage (seen in the field as a bogus ~1.6 GB pin attempt:
 * "Failed to pin user pages: requested 407174, pinned -14").
 * A prearm buffer is only plausible if it matches the shape of a real
 * follow-up buffer for this stream, so require it to be the same size
 * as the current request; anything else is stack noise and is ignored.
 * A failed add is also non-fatal now (see iwfg_add_user_buf): it just
 * means no pre-arming for this iteration.
 */
if (prearm.next_buf != NULL &&
    prearm.next_size == dma_req.size) {
	next_user_buf = iwfg_add_user_buf(priv, icdev,
	prearm.next_buf, prearm.next_size,
	0, 0, icdev->is_audio);
	if (next_user_buf == NULL)
		dev_warn(&priv->pdev->dev,
			"Ignoring invalid prearm buffer %p size %u",
			prearm.next_buf, prearm.next_size);
}

ret = iwfg_recv_data(priv, user_buf, icdev->qid,
     icdev->stream_id, next_user_buf);

if (ret == 0) {
	if (copy_to_user((void __user *)arg, &dma_req,
 	sizeof(dma_req))) {
	ret = -EFAULT;
	break;
}
}
} else {
	dev_err(&priv->pdev->dev,
	"Invalid DMA operation direction: 0x%x",
	icdev->direction);
	ret = -EINVAL;
	break;
}
	dir = icdev->direction;
	break;
}
case IWFG_IOCTL_QUERY_STREAM_PARAMS: {
struct iwfg_stream_params params;

params = iwfg_query_stream_params(priv, icdev->stream_id);

if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
	ret = -EFAULT;
	break;
}
	break;
}
	default:
	ret = -ENOTTY;
	break;
}

return ret;
}

static int iwfg_open(struct inode *inode, struct file *file)
{
	int ret = 0;
    struct iwfg_cdev *icdev = container_of(inode->i_cdev, struct iwfg_cdev, cdev);
    struct iwfg_private *priv = icdev->priv;

	// TODO: create queue resource here
	if (icdev->direction == DMA_FROM_DEVICE) { // c2h
		ret = iwfg_init_rx_queue(priv, icdev->qid);
	} else if (icdev->direction == DMA_TO_DEVICE) { // h2c
		ret = iwfg_init_tx_queue(priv, icdev->qid);
	} else {
		dev_err(&priv->pdev->dev, "Invalid device direction: 0x%x", icdev->direction);
		return -EINVAL;
	}

	if (ret < 0) {
		dev_err(&priv->pdev->dev, "Failed to initialize %s queue for minor %d, err = %d", 
			(icdev->direction == DMA_FROM_DEVICE) ? "RX" : "TX", icdev->minor, ret);
		return ret;
	}

	if (atomic_read(&icdev->stream_active) == 0 && icdev->direction == DMA_FROM_DEVICE) {
		if (icdev->is_audio) {
			iwfg_qdma_audio_start(priv, icdev);
		} else {
			iwfg_qdma_c2h_start(priv, icdev);
		}
	} else if (atomic_read(&icdev->stream_active) == 0 &&
		   icdev->direction == DMA_TO_DEVICE) {
		/*
		 * H2C-only sessions previously never enabled the PL H2C
		 * datapath (that register write lived only in the removed
		 * STREAM_START ioctl and in the C2H open path), so the PL
		 * FIFO filled after the first chunk and every DMA_DATA
		 * ioctl after that hung forever. Enable it at open, same
		 * lifecycle point as the C2H stream start above.
		 */
		iwfg_qdma_h2c_start(priv, icdev);
	}

	/* Initialize DMA buffer list */
	INIT_LIST_HEAD(&icdev->dma_buf_list);

    file->private_data = icdev;
    return ret;
}

static int iwfg_release(struct inode *inode, struct file *file)
{
	struct iwfg_cdev *icdev = file->private_data;
	struct iwfg_private *priv = icdev->priv;
	int ret = 0;

	dev_info(&priv->pdev->dev, "Releasing device minor %d, direction 0x%x", icdev->minor, icdev->direction);

	if (icdev->direction == DMA_FROM_DEVICE) { // c2h
		iwfg_clear_rx_queue(priv, icdev->qid);
	} else if (icdev->direction == DMA_TO_DEVICE) { // h2c
		iwfg_clear_tx_queue(priv, icdev->qid);
	} else {
		dev_err(&priv->pdev->dev, "Invalid device direction on release: 0x%x", icdev->direction);
		return -EINVAL;
	}

	if (ret < 0) {
		dev_err(&priv->pdev->dev, "Failed to cleanup %s queue for minor %d, err = %d", 
			(icdev->direction == DMA_FROM_DEVICE) ? "RX" : "TX", icdev->minor, ret);
		return ret;
	}

	dev_info(&priv->pdev->dev, "stream_active %d", atomic_read(&icdev->stream_active));

	if (atomic_read(&icdev->stream_active) == 1 && icdev->direction == DMA_FROM_DEVICE) {
		if (icdev->is_audio) {
			iwfg_qdma_audio_stop(priv, icdev);
		} else {
			iwfg_qdma_c2h_stop(priv, icdev);
		}
	} else if (atomic_read(&icdev->stream_active) == 1 &&
		   icdev->direction == DMA_TO_DEVICE) {
		/* Mark H2C inactive BEFORE the active-stream scan below so
		 * an H2C-only close still takes the reset-skip branch. */
		iwfg_qdma_h2c_stop(priv, icdev);
	}

	iwfg_delete_user_buf(priv, icdev);

	struct iwfg_cdev *cdev_entry;
	bool active_stream_found = false;
	list_for_each_entry(cdev_entry, &priv->cdev_list, list) {
		if (atomic_read(&cdev_entry->stream_active) == 1) {
			active_stream_found = true;
			break;
		}
	}
	
	/*
	 * SECOND-RUN HANG FIX: stream_active is only ever set by the C2H
	 * open path, so an H2C-only session ALWAYS lands here with
	 * active_stream_found == false, and the old code then ran
	 * iwfg_reset_hardware() on every single H2C close. That routine
	 * soft-resets the whole QDMA IP, pci_iounmap()s the BAR, memsets
	 * priv->hw and re-initializes it - while the user/error IRQ
	 * threads can still be reading those registers, and while any
	 * still-queued queue work could be touching the old qdev handle.
	 * The result was a wedged device/CPU on the next open. The reset
	 * exists purely as the C2H fifo-overflow recovery workaround, so
	 * only perform it when the device being closed is a C2H node.
	 */
	if (!active_stream_found) {
		if (icdev->direction == DMA_FROM_DEVICE) {
			dev_info(&priv->pdev->dev, "No active stream found, stopping all streams");
			/* Perform soft reset of the QDMA hardware to complete the stop */
			iwfg_reset_hardware(priv);
		} else {
			dev_info(&priv->pdev->dev,
				"H2C close: skipping global hardware reset (C2H-only workaround)");
		}
	}

	file->private_data = NULL;
    return ret;
}

static const struct file_operations iwfg_fops = {
    .owner = THIS_MODULE,
    .open = iwfg_open,
    .release = iwfg_release,
    .unlocked_ioctl = iwfg_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = iwfg_ioctl,
#endif
};

static void iwfg_cdev_destroy(struct iwfg_private *priv)
{
	struct iwfg_cdev *cdev_entry, *tmp;

	if (!priv) {
		pr_err("priv is NULL.\n");
		return;
	}
	pr_debug("destroying cdev");

	list_for_each_entry_safe(cdev_entry, tmp, &priv->cdev_list, list) {
		pr_debug("destroying cdev %s", cdev_entry->name);

		if (cdev_entry->sys_device)
			device_destroy(iwfg_class, cdev_entry->cdevno);

		cdev_del(&cdev_entry->cdev);
		list_del(&cdev_entry->list);
		kfree(cdev_entry);
	}

	INIT_LIST_HEAD(&priv->cdev_list);
}

static int iwfg_cdev_create(struct iwfg_private *priv, u32 dma_direction, int minor, int stream_id, bool is_audio)
{
	struct iwfg_cdev *iwfg_cdev;
	struct pci_dev *pdev = priv->pdev;
	int rv;
	

	iwfg_cdev = kzalloc(sizeof(struct iwfg_cdev), GFP_KERNEL);
	if (!iwfg_cdev) {
		pr_err("failed to allocate cdev %lu.\n",
		       sizeof(struct iwfg_cdev));
		return -ENOMEM;
	}

	iwfg_cdev->cdev.owner = THIS_MODULE;
	// priv_data = (qconf->q_type == Q_C2H) ?
	// 		&xcdev->c2h_qhndl : &xcdev->h2c_qhndl;
	// *priv_data = qhndl;
	iwfg_cdev->direction = dma_direction;

	// TODO: create the device name here based on card config
	// strcpy(xcdev->name, qconf->name);

	iwfg_cdev->minor = iwfg_cdev->qid = minor;
	iwfg_cdev->stream_id = stream_id;
	
	if (iwfg_cdev->minor >= IWFG_MAX_QUEUES) {
		pr_err("no char dev. left.\n");
		rv = -ENOSPC;
		goto err_out;
	}
	iwfg_cdev->cdevno = MKDEV(MAJOR(priv->cdevno_base), iwfg_cdev->minor);

	cdev_init(&iwfg_cdev->cdev, &iwfg_fops);

	iwfg_cdev->name[0] = '\0';
	snprintf(iwfg_cdev->name, sizeof(iwfg_cdev->name), "%s%d", 
			iwfg_drv_name, minor);

	/* bring character device live */
	rv = cdev_add(&iwfg_cdev->cdev, iwfg_cdev->cdevno, 1);
	if (rv < 0) {
		pr_err("cdev_add failed %d, %s.\n", rv, iwfg_cdev->name);
		goto err_out;
	}

	/* create device on our class */
	if (iwfg_class) {
		iwfg_cdev->sys_device = device_create(iwfg_class, &(pdev->dev),
				iwfg_cdev->cdevno, NULL, "%s", iwfg_cdev->name);
		if (IS_ERR(iwfg_cdev->sys_device)) {
			rv = PTR_ERR(iwfg_cdev->sys_device);
			pr_err("%s device_create failed %d.\n",
				iwfg_cdev->name, rv);
			goto del_cdev;
		}
	}

	/* Initialize streaming control - initially inactive */
	atomic_set(&iwfg_cdev->stream_active, 0);
	iwfg_cdev->is_audio = is_audio;
	iwfg_cdev->priv = priv;

	// add iwfg_cdev to list
	list_add_tail(&iwfg_cdev->list, &priv->cdev_list);

	return 0;

del_cdev:
	cdev_del(&iwfg_cdev->cdev);

err_out:
	kfree(iwfg_cdev);
	return rv;
}

/**
 * init class and allocate char device region for the PCI device
 */
static int iwfg_cdev_init(struct iwfg_private *priv)
{
	int rv;

#ifdef RHEL_RELEASE_VERSION
	#if RHEL_RELEASE_VERSION(9, 4) > RHEL_RELEASE_CODE
		iwfg_class = class_create(THIS_MODULE, IWFG_CDEV_CLASS_NAME);
	#else
		iwfg_class = class_create(IWFG_CDEV_CLASS_NAME);
	#endif
#else
	#if KERNEL_VERSION(6, 4, 0) > LINUX_VERSION_CODE
		iwfg_class = class_create(THIS_MODULE, IWFG_CDEV_CLASS_NAME);
	#else
		iwfg_class = class_create(IWFG_CDEV_CLASS_NAME);
	#endif
#endif
	if (IS_ERR(iwfg_class)) {
		pr_err("%s: failed to create class 0x%lx.",
			IWFG_CDEV_CLASS_NAME, (unsigned long)iwfg_class);
		iwfg_class = NULL;
		return -ENODEV;
	}

	rv = alloc_chrdev_region(&priv->cdevno_base, 0, IWFG_MAX_QUEUES, iwfg_drv_name);
    if (rv < 0){
        dev_err(&priv->pdev->dev, "alloc_chrdev_region, err = %d", rv);
    }

	/* using kmem_cache_create to enable sequential cleanup */
	// cdev_cache = kmem_cache_create("cdev_cache",
	// 				sizeof(struct cdev_async_io),
	// 				0,
	// 				SLAB_HWCACHE_ALIGN,
	// 				NULL);
	// if (!cdev_cache) {
	// 	pr_err("failed to allocate cdev_cache\n");
	// 	return -ENOMEM;
	// }

	return rv;
}

static void iwfg_cdev_cleanup(struct iwfg_private *priv)
{
	unregister_chrdev_region(priv->cdevno_base, IWFG_MAX_QUEUES);

	// kmem_cache_destroy(cdev_cache);
	if (iwfg_class)
		class_destroy(iwfg_class);
}

/**
 * iwfg_probe - Probe and initialize PCI device
 * @pdev: pointer to PCI device
 * @ent: pointer to PCI device ID entries
 *
 * Return 0 on success, negative on failure
 **/
static int iwfg_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{

	struct iwfg_private *priv;
	int rv;
	int i = 0;
	int j = 0;

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->pdev = pdev;

	rv = pci_enable_device_mem(pdev);
	if (rv < 0) {
		dev_err(&pdev->dev, "pci_enable_device_mem, err = %d", rv);
		return rv;
	}

	/* QDMA only supports 32-bit consistent DMA for descriptor ring */
	rv = dma_set_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (rv < 0) {
		dev_err(&pdev->dev, "Failed to set DMA masks");
		goto disable_device;
	} else {
		dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	}

	rv = pci_request_mem_regions(pdev, iwfg_drv_name);
	if (rv < 0) {
		dev_err(&pdev->dev, "pci_request_mem_regions, err = %d", rv);
		goto disable_device;
	}

	/* enable relaxed ordering */
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN);
	/* enable extended tag */
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_EXT_TAG);
	pci_set_master(pdev);
	pci_save_state(pdev);
	pcie_set_readrq(pdev, 512);

    if (PCI_FUNC(pdev->devfn) == 0) {
		dev_info(&pdev->dev, "device is a master PF");
		set_bit(IWFG_FLAG_MASTER_PF, priv->flags);
	}

	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->rx_lock);

    // qdma hardware initialization
	rv = iwfg_init_capacity(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "iwfg_init_capacity, err = %d", rv);
		goto release_pci_mem;
	}

	rv = iwfg_init_hardware(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "iwfg_init_hardware, err = %d", rv);
		goto clear_capacity;
	}

	rv = iwfg_init_interrupt(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "iwfg_init_interrupt, err = %d", rv);
		goto clear_hardware;
	}

	// query card capabilities and stream parameters
	// iwfg_signal_pci_linkup(priv);
	iwfg_query_card_config(priv);

	dev_info(&pdev->dev, "Card config: num_hdmi_inputs=%u, num_hdmi_outputs=%u, has_audio=%u", 
		priv->num_hdmi_inputs, priv->num_hdmi_outputs, priv->has_audio);

	// Create cdev files based on card capabilities
	INIT_LIST_HEAD(&priv->cdev_list);

	rv = iwfg_cdev_init(priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "Failed to initialize char device, err = %d", rv);
		goto clear_interrupt;
	}

	

	for (i = 0; i < 1; i++) {

    		rv = iwfg_cdev_create(priv, DMA_FROM_DEVICE, j, i, false);
    		if (rv < 0) {
        		dev_err(&pdev->dev,
            			"Failed to create c2h char device queue %d, err=%d",
            		i, rv);
        	goto clear_cdev;
    		}

    		break;
	}

	priv->num_streams = i;

	/*
	 * H2C (Host-to-Card) device node.
	 *
	 * The QDMA H2C engine itself (iwfg_init_tx_queue(), iwfg_xmit_data(),
	 * iwfg_tx_roll()/iwfg_tx_clean(), iwfg_map_sg_to_tx_descs(), and the
	 * DMA_TO_DEVICE branch already present in iwfg_ioctl()) was already
	 * fully implemented, but iwfg_probe() never called iwfg_cdev_create()
	 * for the H2C direction, so no /dev node ever existed to reach it
	 * from userspace. iwfg.h documents the intended queue numbering as
	 * "even rx, odd tx" (IWFG_MAX_QUEUES comment), so qid/minor 1 is used
	 * here for the single H2C queue, matching that scheme and avoiding
	 * any collision with the C2H queue created above (qid 0).
	 */
	rv = iwfg_cdev_create(priv, DMA_TO_DEVICE, 1, 0, false);
	if (rv < 0) {
		dev_err(&pdev->dev,
			"Failed to create h2c char device queue, err=%d", rv);
		goto clear_cdev;
	}

	pci_set_drvdata(pdev, priv);

	dev_info(&pdev->dev, "iwfg_probe: %s, %s", iwfg_drv_str, iwfg_drv_ver);

	return 0;

clear_cdev:
    iwfg_cdev_destroy(priv);
// unregister_chrdev:
	iwfg_cdev_cleanup(priv);
clear_interrupt:
	iwfg_clear_interrupt(priv);
clear_hardware:
	iwfg_clear_hardware(priv);
clear_capacity:
	iwfg_clear_capacity(priv);
release_pci_mem:
	pci_release_mem_regions(pdev);
disable_device:
	pci_disable_device(pdev);
	kfree(priv);

	return rv;
}

/**
 * iwfg_remove - remove PCI device
 * @pdev: pointer to PCI device
 **/
static void iwfg_remove(struct pci_dev *pdev)
{
	struct iwfg_private *priv = pci_get_drvdata(pdev);
	if (!priv) {
		dev_err(&pdev->dev, "iwfg_remove: priv is NULL");
		return;
	}

	dev_info(&pdev->dev, "iwfg_remove: %s, %s", iwfg_drv_str, iwfg_drv_ver);

	// device_destroy(iwfg_class, priv->iwfg_cdev.cdevno);
	// class_destroy(iwfg_class);
	// cdev_del(&priv->iwfg_cdev.cdev);
	// unregister_chrdev_region(priv->iwfg_cdev.cdevno, 1);

	iwfg_cdev_destroy(priv);
	iwfg_cdev_cleanup(priv);

	iwfg_clear_interrupt(priv);
	iwfg_clear_hardware(priv);
	iwfg_clear_capacity(priv);

	pci_set_drvdata(pdev, NULL);
	pci_release_mem_regions(pdev);
	pci_disable_device(pdev);
	kfree(priv);
}

static struct pci_driver pci_driver = {
	.name = iwfg_drv_name,
	.id_table = iwfg_pci_tbl,
	.probe = iwfg_probe,
	.remove = iwfg_remove,
};

static int __init iwfg_init_module(void)
{
	int ret;
	
	pr_info("%s %s", iwfg_drv_str, iwfg_drv_ver);
	
	/* Show debug status if requested */
	if (debug_enable) {
		pr_info("iwfg: Module loaded with debug_enable=1\n");
		pr_info("iwfg: To enable dev_dbg messages, use: echo 'module iwfg +p' > /sys/kernel/debug/dynamic_debug/control\n");
	}
	
	ret = pci_register_driver(&pci_driver);
	
	return ret;
}

static void __exit iwfg_exit_module(void)
{
	pci_unregister_driver(&pci_driver);
}

module_init(iwfg_init_module);
module_exit(iwfg_exit_module);
