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
#include <linux/delay.h>
#include <linux/pci.h>

#include "iwfg_hardware.h"
#include "iwfg_register.h"
#include "iwfg.h"
#include "qdma_register.h"
#include "qdma_context.h"
#include "qdma_error_info.h"

/* default CSR values for QDMA */
#define DEFAULT_MAX_DESC_FETCH			6
#define DEFAULT_WB_INTVL			QDMA_WB_INTVL_4
#define DEFAULT_PFCH_STOP_THRES			256
#define DEFAULT_PFCH_NUM_ENTRIES_PER_Q		8
#define DEFAULT_PFCH_MAX_Q_CNT			16
#define DEFAULT_C2H_INTR_TIMER_TICK		25
#define DEFAULT_CMPL_COAL_TIMER_CNT		5
#define DEFAULT_CMPL_COAL_TIMER_TICK		25
#define DEFAULT_CMPL_COAL_MAX_BUFSZ		32
#define DEFAULT_H2C_THROT_DATA_THRES		0x4000
#define DEFAULT_THROT_EN_DATA			1
#define DEFAULT_THROT_EN_REQ			0
#define DEFAULT_H2C_THROT_REQ_THRES		0x60

#define RX_ALIGN_TIMEOUT_MS			1000
#define XMAC_RESET_WAIT_MS			1

static const u16 rngcnt_pool[QDMA_NUM_DESC_RNGCNT] = {
	4096, 64, 128, 192, 256, 384, 512, 768,
	1024, 1536, 3072, 4096, 6144, 8192, 12288, 16384
};

static const u16 c2h_bufsz_pool[QDMA_NUM_C2H_BUFSZ] = {
	4096, 256, 512, 1024, 2048, 3968, 4096, 4096,
	4096, 4096, 4096, 4096, 4096, 8192, 9018, 16384
};

static const u16 c2h_timer_pool[QDMA_NUM_C2H_TIMERS] = {
	10, 2, 4, 5, 8, 10, 15, 20, 25,
	30, 50, 75, 100, 125, 150, 200
};

static const u16 c2h_thres_pool[QDMA_NUM_C2H_COUNTERS] = {
	64, 2, 4, 8, 16, 24, 32, 48,
	80, 96, 112, 128, 144, 160, 176, 192
};

u16 iwfg_ring_count(u8 idx)
{
	return (idx < QDMA_NUM_DESC_RNGCNT) ? rngcnt_pool[idx] : 0;
}

u16 iwfg_get_c2h_bufsz(u8 idx)
{
	return (idx < QDMA_NUM_C2H_BUFSZ) ? c2h_bufsz_pool[idx] : 0;
}

u8 iwfg_get_c2h_bufsz_idx(u32 dma_bufsz)
{
	int i;

	for (i = QDMA_NUM_C2H_BUFSZ - 1; i >= 0; i--) {
		if (dma_bufsz >= c2h_bufsz_pool[i])
			return i;
	}

	return 0;
}

/**
 * iwfg_qdma_init_csr - initialize QDMA config/status registers
 * @qdev: pointer to QDMA device
 *
 * This function writes to various H2C and C2H registers, getting QDMA ready for
 * queue operations.  Values of these registers are hard-coded.
 **/
static void iwfg_qdma_init_csr(struct qdma_dev *qdev)
{
	u32 offset, val;
	int i;

	/* initialize descriptor ring size registers */
	for (i = 0; i < QDMA_NUM_DESC_RNGCNT; ++i) {
		offset = QDMA_OFFSET_GLBL_RNG_SZ + (i * 4);
		val = rngcnt_pool[i];
		qdma_write_reg(qdev, offset, val);
	}

	/* initialize C2H buffer size registers */
	for (i = 0; i < QDMA_NUM_C2H_BUFSZ; ++i) {
		offset = QDMA_OFFSET_C2H_BUF_SZ + (i * 4);
		val = c2h_bufsz_pool[i];
		qdma_write_reg(qdev, offset, val);
	}

	/* set QDMA_C2H_INT_TIMER_TICK (0xB0C) register to 25, which corresponds
	 * to 100ns (1 tick = 4ns for 250MHz user clock)
	 */
	offset = QDMA_OFFSET_C2H_INT_TIMER_TICK;
	val = DEFAULT_C2H_INTR_TIMER_TICK;
	qdma_write_reg(qdev, offset, val);

	/* initialize C2H timer counter registers. */
	for (i = 0; i < QDMA_NUM_C2H_TIMERS; ++i) {
		offset = QDMA_OFFSET_C2H_TIMER_CNT + (i * 4);
		val = c2h_timer_pool[i];
		qdma_write_reg(qdev, offset, val);
	}

	/* initialize C2H counter threshold registers */
	for (i = 0; i < QDMA_NUM_C2H_COUNTERS; ++i) {
		offset = QDMA_OFFSET_C2H_CNT_TH + (i * 4);
		val = c2h_thres_pool[i];
		qdma_write_reg(qdev, offset, val);
	}

	/* set QDMA_GLBL_DSC_CFG (0x250) register for max descriptor fetch and
	 * writeback interval
	 */
	offset = QDMA_OFFSET_GLBL_DSC_CFG;
	val = (FIELD_SET(QDMA_GLBL_DSC_CFG_MAX_DSC_FETCH_MASK,
			 DEFAULT_MAX_DESC_FETCH) |
	       FIELD_SET(QDMA_GLBL_DSC_CFG_WB_ACC_INT_MASK,
			 DEFAULT_WB_INTVL));
	qdma_write_reg(qdev, offset, val);

	/* read QDMA_C2H_PFCH_CACHE_DEPTH (0xBE0) register and set
	 * QDMA_C2H_PFCH_CFG (0xB08) register accordingly
	 */
	val = qdma_read_reg(qdev, QDMA_OFFSET_C2H_PFCH_CACHE_DEPTH);
	offset = QDMA_OFFSET_C2H_PFCH_CFG;
	val = (FIELD_SET(QDMA_C2H_PFCH_FL_TH_MASK,
			 DEFAULT_PFCH_STOP_THRES) |
	       FIELD_SET(QDMA_C2H_NUM_PFCH_MASK,
			 DEFAULT_PFCH_NUM_ENTRIES_PER_Q) |
	       FIELD_SET(QDMA_C2H_PFCH_QCNT_MASK,
			 (val >> 1)) |
	       FIELD_SET(QDMA_C2H_EVT_QCNT_TH_MASK,
			 ((val >> 1) - 2)));
	qdma_write_reg(qdev, offset, val);

	/* read QDMA_C2H_CMPL_COAL_BUF_DEPTH (0xBE4) register and set
	 * QDMA_C2H_WB_COAL_CFG (0xB50) register accordingly
	 *
	 * Note that the tick field is set to 25, which corresponds to 100ns (1
	 * tick = 4ns for 250MHz user clock).
	 *
	 * TODO: verify the value for C2H_MAX_BUF_SZ.  QDMA document says that
	 * this should be set to QDMA_C2H_CMPL_COAL_BUF_DEPTH.buf_depth - 2; but
	 * the libqdma code ignores the minus 2 part.
	 */
	val = qdma_read_reg(qdev, QDMA_OFFSET_C2H_CMPL_COAL_BUF_DEPTH);
	offset = QDMA_OFFSET_C2H_WB_COAL_CFG;
	val = (FIELD_SET(QDMA_C2H_TICK_CNT_MASK,
			 DEFAULT_CMPL_COAL_TIMER_CNT) |
	       FIELD_SET(QDMA_C2H_TICK_VAL_MASK,
			 DEFAULT_CMPL_COAL_TIMER_TICK) |
	       FIELD_SET(QDMA_C2H_MAX_BUF_SZ_MASK, val));
	qdma_write_reg(qdev, offset, val);

	/* set QDMA_H2C_REQ_THROT (0xE24) register.
	 *
	 * Data and request-based throttle are enabled only if the respective
	 * threshold is set to a nonzero value.
	 */
	offset = QDMA_OFFSET_H2C_REQ_THROT;
	val = (FIELD_SET(QDMA_H2C_DATA_THRESH_MASK,
			 DEFAULT_H2C_THROT_DATA_THRES) |
	       FIELD_SET(QDMA_H2C_REQ_THROT_EN_DATA_MASK,
			 DEFAULT_THROT_EN_DATA) |
	       FIELD_SET(QDMA_H2C_REQ_THRESH_MASK,
			 DEFAULT_H2C_THROT_REQ_THRES) |
	       FIELD_SET(QDMA_H2C_REQ_THROT_EN_REQ_MASK,
			 DEFAULT_THROT_EN_REQ));
	qdma_write_reg(qdev, offset, val);
}

int iwfg_init_hardware(struct iwfg_private *priv)
{
	struct iwfg_hardware *hw = &priv->hw;
	struct pci_dev *pdev = priv->pdev;
	struct qdma_dev *qdev;
	struct qdma_fmap_ctxt fmap_ctxt;
	u16 qbase, qmax, func_id;
	// u32 val;
	u8 master_pf = test_bit(IWFG_FLAG_MASTER_PF, priv->flags);
	int rv, map_len;

	/* user logic registers uses BAR-2 */
	map_len = pci_resource_len(pdev, 2);
	if (map_len > QDMA_MAX_BAR_LEN_MAPPED)
		map_len = QDMA_MAX_BAR_LEN_MAPPED;
	hw->addr = pci_iomap(pdev, 2, map_len);
	if (!hw->addr)
		return -EINVAL;

	/* QDMA IP registers uses BAR-0 */
	qdev = qdma_create_dev(pdev, 0);
	if (!qdev)
		return -ENOMEM;

	func_id = PCI_FUNC(pdev->devfn);
	qbase = func_id * IWFG_MAX_QUEUES;
	qmax = IWFG_MAX_QUEUES;

	/* initialize QDMA function map context */
	memset(&fmap_ctxt, 0, sizeof(struct qdma_fmap_ctxt));
	fmap_ctxt.qbase = qbase;
	fmap_ctxt.qmax = qmax;
	rv = qdma_clear_fmap_ctxt(qdev);
	if (rv < 0)
		goto clear_hardware;
	rv = qdma_write_fmap_ctxt(qdev, &fmap_ctxt);
	if (rv < 0)
		goto clear_hardware;

	/* initialize global registers if device is a master PF */
	if (master_pf)
		iwfg_qdma_init_csr(qdev);

    	hw->qdma = (unsigned long)qdev;

	return 0;

clear_hardware:
	iwfg_clear_hardware(priv);
	return rv;
}

void iwfg_clear_hardware(struct iwfg_private *priv)
{
	struct iwfg_hardware *hw = &priv->hw;
	struct pci_dev *pdev = priv->pdev;
	struct qdma_dev *qdev = (struct qdma_dev *)hw->qdma;
	// u16 func_id = PCI_FUNC(pdev->devfn);

	qdma_invalidate_fmap_ctxt(qdev);
	qdma_destroy_dev(qdev);

	pci_iounmap(pdev, hw->addr);

	memset(hw, 0, sizeof(struct iwfg_hardware));
}

void iwfg_reset_hardware(struct iwfg_private *priv)
{
	struct iwfg_hardware *hw = &priv->hw;
	struct pci_dev *pdev = priv->pdev;

	if(!priv || !hw) {
		dev_err(&pdev->dev, "Invalid hardware or PCI device");
		return;
	}

	/* soft reset the QDMA hardware */
	iwfg_write_reg(&priv->hw, 0xa0, 0x1);

	udelay(17000);

	iwfg_clear_hardware(priv);

	/* reinitialize the hardware context */
	iwfg_init_hardware(priv);
}

void iwfg_qdma_init_error_interrupt(unsigned long qdma, u16 vid)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 offset, val;
	int i;

	offset = QDMA_OFFSET_GLBL_ERR_INT;
	val = (FIELD_SET(QDMA_GLBL_ERR_FUNC_MASK, qdev->func_id) |
	       FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, vid) |
	       FIELD_SET(QDMA_GLBL_ERR_ARM_MASK, 0));
	qdma_write_reg(qdev, offset, val);

	for (i = 0; i < NUM_LEAF_ERROR_AGGREGATORS; i++) {
		u32 err_idx = leaf_error_aggregators[i];

		offset = qdma_error_info[err_idx].mask_reg_addr;
		val = qdma_error_info[err_idx].leaf_err_mask;
		qdma_write_reg(qdev, offset, val);

		offset = QDMA_OFFSET_GLBL_ERR_MASK;
		val = qdma_read_reg(qdev, offset);
		val |= FIELD_SET(qdma_error_info[err_idx].glbl_err_mask, 1);
		qdma_write_reg(qdev, offset, val);
	}

	offset = QDMA_OFFSET_GLBL_ERR_INT;
	val = (FIELD_SET(QDMA_GLBL_ERR_FUNC_MASK, qdev->func_id) |
	       FIELD_SET(QDMA_GLBL_ERR_VEC_MASK, vid) |
	       FIELD_SET(QDMA_GLBL_ERR_ARM_MASK, 1));
	qdma_write_reg(qdev, offset, val);
}

void iwfg_qdma_clear_error_interrupt(unsigned long qdma)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;

	qdma_write_reg(qdev, QDMA_OFFSET_GLBL_ERR_INT, 0);
}

void iwfg_qdma_ack_and_rearm_error_interrupt(unsigned long qdma, u16 vid)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 val;

	/*
	 * Clear all W1C (write-1-to-clear) error status registers.
	 * Reading the current value and writing it back clears only the bits
	 * that were set, leaving any new errors that arrived after the read.
	 */

	/* Global error status (0x248) */
	val = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_ERR_STAT);
	if (val)
		qdma_write_reg(qdev, QDMA_OFFSET_GLBL_ERR_STAT, val);

	/* Global descriptor error status (0x254) */
	val = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_DSC_ERR_STS);
	if (val)
		qdma_write_reg(qdev, QDMA_OFFSET_GLBL_DSC_ERR_STS, val);

	/* Global TRQ error status (0x264) */
	val = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_TRQ_ERR_STAT);
	if (val)
		qdma_write_reg(qdev, QDMA_OFFSET_GLBL_TRQ_ERR_STAT, val);

	/* C2H error status (0xAF0) */
	val = qdma_read_reg(qdev, QDMA_OFFSET_C2H_ERR_STAT);
	if (val)
		qdma_write_reg(qdev, QDMA_OFFSET_C2H_ERR_STAT, val);

	/* C2H fatal error status (0xAF8) */
	val = qdma_read_reg(qdev, QDMA_OFFSET_C2H_FATAL_ERR_STAT);
	if (val)
		qdma_write_reg(qdev, QDMA_OFFSET_C2H_FATAL_ERR_STAT, val);

	/* H2C error status (0xE00) */
	val = qdma_read_reg(qdev, QDMA_OFFSET_H2C_ERR_STAT);
	if (val)
		qdma_write_reg(qdev, QDMA_OFFSET_H2C_ERR_STAT, val);

	/*
	 * Rearm the MSI-X error interrupt by writing GLBL_ERR_INT (0xB04)
	 * with the ARM bit set.  This mirrors the final step of
	 * iwfg_qdma_init_error_interrupt().
	 */
	val = (FIELD_SET(QDMA_GLBL_ERR_FUNC_MASK, qdev->func_id) |
	       FIELD_SET(QDMA_GLBL_ERR_VEC_MASK,  vid)           |
	       FIELD_SET(QDMA_GLBL_ERR_ARM_MASK,  1));
	qdma_write_reg(qdev, QDMA_OFFSET_GLBL_ERR_INT, val);
}

int iwfg_qdma_init_tx_queue(unsigned long qdma, u16 qid,
			    const struct iwfg_qdma_h2c_param *param)
{
	const enum qdma_dir dir = QDMA_H2C;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	struct qdma_sw_ctxt sw_ctxt;
	int rv;

	if (qid < 0)
		return qid;

	/* initialize software context */
	memset(&sw_ctxt, 0, sizeof(struct qdma_sw_ctxt));
	sw_ctxt.func_id = qdev->func_id;
	sw_ctxt.qen = 1;
	sw_ctxt.wbk_en = 1;
	sw_ctxt.is_mm = 0;
	sw_ctxt.irq_arm = 1;
	sw_ctxt.irq_en = 1;
	sw_ctxt.desc_sz = 1; /* 1: 16B for H2C stream */
	sw_ctxt.fcrd_en = 0;
	sw_ctxt.wbi_chk = 1;
	sw_ctxt.wbi_intvl_en = 1;
	sw_ctxt.at = 0;
	sw_ctxt.rngsz_idx = param->rngcnt_idx;
	sw_ctxt.desc_base = param->dma_addr;
	sw_ctxt.vec = param->vid;
	sw_ctxt.intr_aggr = 0;

	rv = qdma_clear_sw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_tx_queue;
	rv = qdma_write_sw_ctxt(qdev, qid, dir, &sw_ctxt);
	if (rv < 0)
		goto clear_tx_queue;

	/* initialize hardware and credit context */
	rv = qdma_clear_hw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_tx_queue;
	rv = qdma_clear_cr_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_tx_queue;

	return 0;

clear_tx_queue:
	iwfg_qdma_clear_tx_queue(qdma, qid);
	return rv;
}

int iwfg_qdma_init_rx_queue(unsigned long qdma, u16 qid,
			    const struct iwfg_qdma_c2h_param *param)
{
	const enum qdma_dir dir = QDMA_C2H;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	struct qdma_sw_ctxt sw_ctxt;
	struct qdma_pfch_ctxt pfch_ctxt;
	struct qdma_cmpl_ctxt cmpl_ctxt;
	int rv;

	if (qid < 0)
		return qid;

	/* initialize software context */
	memset(&sw_ctxt, 0, sizeof(struct qdma_sw_ctxt));
	sw_ctxt.func_id = qdev->func_id;
	sw_ctxt.qen = 1;
	sw_ctxt.wbk_en = 1;
	sw_ctxt.is_mm = 0;
	sw_ctxt.desc_sz = 0; /* 0: 8B for C2H stream */
	sw_ctxt.fcrd_en = 1;
	sw_ctxt.rngsz_idx = param->desc_rngcnt_idx;
	sw_ctxt.desc_base = param->desc_dma_addr;

	rv = qdma_clear_sw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_write_sw_ctxt(qdev, qid, dir, &sw_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	/* initialize hardware and credit context */
	rv = qdma_clear_hw_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_clear_cr_ctxt(qdev, qid, dir);
	if (rv < 0)
		goto clear_rx_queue;

	/* initialize prefetch and completion contexts */
	memset(&pfch_ctxt, 0, sizeof(struct qdma_pfch_ctxt));
	pfch_ctxt.bufsz_idx = param->bufsz_idx;
	pfch_ctxt.pfch_en = 1;
	pfch_ctxt.valid = 1;

	rv = qdma_clear_pfch_ctxt(qdev, qid);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_write_pfch_ctxt(qdev, qid, &pfch_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	memset(&cmpl_ctxt, 0, sizeof(struct qdma_cmpl_ctxt));
	cmpl_ctxt.stat_en = 1;
	// cmpl_ctxt.stat_en = 0;
	cmpl_ctxt.intr_en = 1;
	cmpl_ctxt.trig_mode = 0x5;
	cmpl_ctxt.func_id = qdev->func_id;
	cmpl_ctxt.counter_idx = 0;
	cmpl_ctxt.timer_idx = 0;
	cmpl_ctxt.color = 1;
	cmpl_ctxt.rngsz_idx = param->cmpl_rngcnt_idx;
	cmpl_ctxt.baddr = param->cmpl_dma_addr;
	cmpl_ctxt.desc_sz = param->cmpl_desc_sz;
	cmpl_ctxt.valid = 1;
	cmpl_ctxt.full_upd = 0;
	cmpl_ctxt.ovf_chk_dis = 0;
	cmpl_ctxt.vec = param->vid;
	cmpl_ctxt.intr_aggr = 0;

	rv = qdma_clear_cmpl_ctxt(qdev, qid);
	if (rv < 0)
		goto clear_rx_queue;
	rv = qdma_write_cmpl_ctxt(qdev, qid, &cmpl_ctxt);
	if (rv < 0)
		goto clear_rx_queue;

	return 0;

clear_rx_queue:
	iwfg_qdma_clear_rx_queue(qdma, qid);
	return rv;
}

void iwfg_qdma_clear_tx_queue(unsigned long qdma, u16 qid)
{
	const enum qdma_dir dir = QDMA_H2C;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;

	if (qid < 0)
		return;

	qdma_invalidate_sw_ctxt(qdev, qid, dir);
	qdma_invalidate_hw_ctxt(qdev, qid, dir);
	qdma_invalidate_cr_ctxt(qdev, qid, dir);
}

void iwfg_qdma_clear_rx_queue(unsigned long qdma, u16 qid)
{
	const enum qdma_dir dir = QDMA_C2H;
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;

	if (qid < 0)
		return;

	qdma_invalidate_sw_ctxt(qdev, qid, dir);
	qdma_invalidate_hw_ctxt(qdev, qid, dir);
	qdma_invalidate_cr_ctxt(qdev, qid, dir);
	qdma_invalidate_pfch_ctxt(qdev, qid);
	qdma_invalidate_cmpl_ctxt(qdev, qid);
}

/**
 * iwfg_qdma_set_q_pidx - set QDMA queue producer index
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @dir: queue direction
 * @pidx: producer index
 * @irq_arm: interrupt arm bit for next interrupt generation
 **/
static void iwfg_qdma_set_q_pidx(unsigned long qdma, u16 qid,
				 enum qdma_dir dir, u16 pidx, u8 irq_arm)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 offset, val;
	bool debug = 0;
	if (debug) dev_info(&qdev->pdev->dev, "iwfg_qdma_set_q_pidx(qid:%u, dir:%x, pidx:%u, irq_qrm:%u)", qid, dir, pidx, irq_arm);

	if (qid < 0)
		return;

	if (dir == QDMA_C2H)
		offset = QDMA_OFFSET_DMAP_SEL_C2H_DESC_PIDX + (qid * 16);
	else
		offset = QDMA_OFFSET_DMAP_SEL_H2C_DESC_PIDX + (qid * 16);

	val = (FIELD_SET(QDMA_DMAP_SEL_DESC_PIDX_MASK, pidx) |
	       FIELD_SET(QDMA_DMAP_SEL_DESC_IRQ_ARM_MASK, irq_arm));
	qdma_write_reg(qdev, offset, val);
}

void iwfg_set_tx_head(unsigned long qdma, u16 qid, u16 head)
{
	iwfg_qdma_set_q_pidx(qdma, qid, QDMA_H2C, head, 1);
}

void iwfg_set_rx_head(unsigned long qdma, u16 qid, u16 head)
{
	iwfg_qdma_set_q_pidx(qdma, qid, QDMA_C2H, head, 0);
}

/**
 * iwfg_qdma_set_cmpl_cidx - set QDMA completion queue consumer index
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @cidx: consumer index
 * @counter_idx: index to C2H counter threshold registers
 * @timer_idx: index to C2H timer registers
 * @trig_mode: interrupt and status descriptor trigger mode
 * @stat_en: enable completion status writeback
 * @irq_arm: interrupt arm bit for next interrupt generation
 **/
static void iwfg_qdma_set_cmpl_cidx(unsigned long qdma, u16 qid, u16 cidx,
				       u8 counter_idx, u8 timer_idx,
				       u8 trig_mode, u8 stat_en, u8 irq_arm)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u32 offset, val;
	bool debug = 0;
	if (debug) dev_info(&qdev->pdev->dev, "iwfg_qdma_set_cmpl_cidx(qid:%u, cidx:%u, idx:%u, timser_idx:%u, trig_mode:%u irq_arm:%u)", qid, cidx, counter_idx, timer_idx, trig_mode, irq_arm);

	if (qid < 0)
		return;

	offset = QDMA_OFFSET_DMAP_SEL_CMPL_CIDX + (qid * 16);

	val = (FIELD_SET(QDMA_DMAP_SEL_CMPL_CIDX_MASK, cidx) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_COUNTER_IDX_MASK, counter_idx) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_TIMER_IDX_MASK, timer_idx) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_TRIG_MODE_MASK, trig_mode) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_STAT_EN_MASK, stat_en) |
	       FIELD_SET(QDMA_DMAP_SEL_CMPL_IRQ_ARM_MASK, irq_arm));
	qdma_write_reg(qdev, offset, val);
}

void iwfg_set_completion_tail(unsigned long qdma, u16 qid, u16 tail, u8 irq_arm)
{
	struct qdma_dev *qdev = (struct qdma_dev *)qdma;
	u8 trig_mode = 5; // trigger from: user, count, or timer
	u8 stat_en = 1;  // enabled is necessary for getting proper completion_status, e.g. for knowing pidx
	bool debug = 0;
	if (debug) dev_info(&qdev->pdev->dev, "iwfg_set_completion_tail (qid:%u, tail:%u, irq_arm:%u)", qid, tail, irq_arm);
	iwfg_qdma_set_cmpl_cidx(qdma, qid, tail, 0, 0, trig_mode, stat_en, irq_arm);
}

/**
 * iwfg_process_qdma_errors - Process and decode QDMA errors
 * @priv: pointer to driver private data
 * @err_info: buffer for error information (can be NULL)
 * @buffer_size: size of the error info buffer
 *
 * This function reads the QDMA global and detailed error registers, formats
 * the error information into the provided buffer, and clears the error status
 * registers. It uses the qdma_reg_access functions for register operations.
 *
 * Return: 0 if no errors found, non-zero (global error status) if errors
 *         were detected.
 **/
int iwfg_process_qdma_errors(struct iwfg_private *priv, char *err_info, size_t buffer_size)
{
	// struct qdma_dev *qdev = (struct qdma_dev *)priv->hw.qdma;
	// uint32_t error_status = 0;
	uint32_t glbl_err_stat = 0;
	// uint32_t dsc_err_stat = 0;
	// uint32_t trq_err_stat = 0;
	// uint32_t c2h_err_stat = 0;
	// uint32_t c2h_fatal_err_stat = 0;
	// uint32_t err_log0 = 0, err_log1 = 0, err_log2 = 0;
	// uint32_t trq_err_log = 0;
	// int pos = 0;
	// int i;

	/* Read global error status register */
	// glbl_err_stat = qdma_read_reg(qdev, QDMA_GLBL_ERR_STAT);

	if (!glbl_err_stat) {
		/* No errors found */
		return 0;
	}

	// if (err_info) {
	// 	pos += snprintf(err_info + pos, buffer_size - pos,
	// 		"Global Error Status: 0x%08x\n", glbl_err_stat);

	// 	/* Decode global error status */
	// 	if (glbl_err_stat & QDMA_GLBL_ERR_DSC)
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"- Descriptor error detected\n");

	// 	if (glbl_err_stat & QDMA_GLBL_ERR_TRQ)
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"- TRQ error detected\n");

	// 	if (glbl_err_stat & QDMA_GLBL_ERR_H2C_MM)
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"- H2C MM error detected\n");

	// 	if (glbl_err_stat & QDMA_GLBL_ERR_C2H_MM)
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"- C2H MM error detected\n");

	// 	if (glbl_err_stat & QDMA_GLBL_ERR_C2H_ST)
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"- C2H ST error detected\n");
	// 	if (glbl_err_stat & QDMA_GLBL_ERR_H2C_ST)
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"- H2C ST error detected\n");

	// 	if (glbl_err_stat & QDMA_GLBL_ERR_FLR)
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"- Function Level Reset error detected\n");

	// 	if (glbl_err_stat & QDMA_GLBL_ERR_TCE)
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"- Traffic Control error detected\n");
	// }

	// /* Read and decode detailed descriptor error status */
	// if (glbl_err_stat & QDMA_GLBL_ERR_DSC) {
	// 	dsc_err_stat = qdma_read_reg(qdev, QDMA_GLBL_DSC_ERR_STAT);
	// 	err_log0 = qdma_read_reg(qdev, QDMA_GLBL_DSC_ERR_LOG0);
	// 	err_log1 = qdma_read_reg(qdev, QDMA_GLBL_DSC_ERR_LOG1);
	// 	err_log2 = qdma_read_reg(qdev, QDMA_GLBL_DSC_ERR_LOG2);

	// 	if (err_info) {
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"\nDescriptor Error Details:\n"
	// 			"Status: 0x%08x, Log0: 0x%08x, Log1: 0x%08x, Log2: 0x%08x\n",
	// 			dsc_err_stat, err_log0, err_log1, err_log2);
	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_POISON)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Poison error detected\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_UR_CA)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Unsupported request or completer aborted error\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_PARAM)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Parameter mismatch error\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_ADDR)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Address mismatch error\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_TAG)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Unexpected tag error\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_FLR)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- FLR error\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_TIMEOUT)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Timeout error\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_DAT_POISON)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Data poison error\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_FLR_CANCEL)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Descriptor fetch cancelled due to FLR\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_DMA)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- DMA engine error\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_DSC)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Invalid PIDX update error\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_RQ_CANCEL)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Descriptor fetch cancelled due to queue disable\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_DBE)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Double-bit error detected\n");

	// 		if (dsc_err_stat & QDMA_GLBL_DSC_ERR_SBE)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Single-bit error detected\n");

	// 	}
	// 	/* Clear the descriptor error status */
	// 	qdma_write_reg(qdev, QDMA_GLBL_DSC_ERR_STAT, dsc_err_stat);
	// }

	// /* Read and decode detailed TRQ error status */
	// if (glbl_err_stat & QDMA_GLBL_ERR_TRQ) {
	// 	trq_err_stat = qdma_read_reg(qdev, QDMA_GLBL_TRQ_ERR_STAT);
	// 	trq_err_log = qdma_read_reg(qdev, QDMA_GLBL_TRQ_ERR_LOG);

	// 	if (err_info) {
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"\nTRQ Error Details:\n"
	// 			"Status: 0x%08x, Log: 0x%08x\n",
	// 			trq_err_stat, trq_err_log);
	// 		if (trq_err_stat & QDMA_GLBL_TRQ_ERR_UNMAPPED)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Access to unmapped register space\n");

	// 		if (trq_err_stat & QDMA_GLBL_TRQ_ERR_QID_RANGE)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- QID range error\n");

	// 		if (trq_err_stat & QDMA_GLBL_TRQ_ERR_VF_ACCESS)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Invalid VF access error\n");

	// 		if (trq_err_stat & QDMA_GLBL_TRQ_ERR_TCP_TIMEOUT)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Timeout on request error\n");
	// 	}

	// 	/* Clear the TRQ error status */
	// 	qdma_write_reg(qdev, QDMA_GLBL_TRQ_ERR_STAT, trq_err_stat);
	// }

	// /* Read and decode C2H error status */
	// if (glbl_err_stat & QDMA_GLBL_ERR_C2H_ST) {
	// 	c2h_err_stat = qdma_read_reg(qdev, QDMA_C2H_ERR_STAT);
	// 	c2h_fatal_err_stat = qdma_read_reg(qdev, QDMA_C2H_FATAL_ERR_STAT);

	// 	if (err_info) {
	// 		pos += snprintf(err_info + pos, buffer_size - pos,
	// 			"\nC2H Error Details:\n"
	// 			"Status: 0x%08x, Fatal Status: 0x%08x\n",
	// 			c2h_err_stat, c2h_fatal_err_stat);
	// 		if (c2h_err_stat & QDMA_C2H_ERR_MTY_MISMATCH)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- MTY mismatch error\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_LEN_MISMATCH)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Length mismatch error\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_QID_MISMATCH)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- QID mismatch error\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_DESC_RSP_ERR)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Descriptor error bit set\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_ENG_WPL_DATA_PAR)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Data parity error\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_MSI_INT_FAIL)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- MSI interrupt failed\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_ERR_DESC_CNT)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Descriptor count error\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_PORTID_CTXT_MISMATCH)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Port ID mismatch error\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_CMPT_INV_Q_ERR)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Completion invalid queue error\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_CMPT_QFULL_ERR)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Completion queue full error\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_CMPT_CIDX_ERR)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Completion CIDX error\n");

	// 		if (c2h_err_stat & QDMA_C2H_ERR_CMPT_PRTY_ERR)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"- Completion parity error\n");
	// 	}

	// 	/* Clear the C2H error status */
	// 	qdma_write_reg(qdev, QDMA_C2H_ERR_STAT, c2h_err_stat);
	// 	qdma_write_reg(qdev, QDMA_C2H_FATAL_ERR_STAT, c2h_fatal_err_stat);
	// }

	// /* Read any other error register details */
	// if (err_info) {
	// 	for (i = 0; i < QDMA_GLBL_ERR_MAX_REGISTERS; i++) {
	// 		error_status = qdma_read_reg(qdev, QDMA_GLBL_ERR_STAT_0 + (i * 4));
	// 		if (error_status)
	// 			pos += snprintf(err_info + pos, buffer_size - pos,
	// 				"Error Register %d: 0x%08x\n", i, error_status);
	// 	}
	// }

	// /* Clear the global error status register */
	// qdma_write_reg(qdev, QDMA_GLBL_ERR_STAT, glbl_err_stat);

	// /* Clear other error registers that might not have been cleared yet */
	// for (i = 0; i < QDMA_GLBL_ERR_MAX_REGISTERS; i++) {
	// 	error_status = qdma_read_reg(qdev, QDMA_GLBL_ERR_STAT_0 + (i * 4));
	// 	if (error_status)
	// 		qdma_write_reg(qdev, QDMA_GLBL_ERR_STAT_0 + (i * 4), error_status);
	// }

	return glbl_err_stat;  /* Return the error status */
}

/**
 * iwfg_print_qdma_debug_registers - Print QDMA debug fabric registers
 * @priv: pointer to driver private data
 *
 * This function reads and prints the QDMA Global Debug Fabric registers
 * (QDMA_GLBL2_DBG_FAB0 and QDMA_GLBL2_DBG_FAB1) for debugging purposes.
 * These registers provide status information about various QDMA internal
 * fabric components including H2C/C2H data paths, async FIFOs, and credits.
 **/
void iwfg_print_qdma_debug_registers(struct iwfg_private *priv)
{
	struct qdma_dev *qdev = (struct qdma_dev *)priv->hw.qdma;
	u32 fab0_val, fab1_val, dsc_dat0_val, dsc_dat1_val;
	
	if (!qdev) {
		dev_err(&priv->pdev->dev, "QDMA device not available\n");
		return;
	}

	/* Read QDMA_GLBL2_DBG_FAB0 (0x1D0) register */
	fab0_val = qdma_read_reg(qdev, 0x1D0);
	
	/* Read QDMA_GLBL2_DBG_FAB1 (0x1D4) register */
	fab1_val = qdma_read_reg(qdev, 0x1D4);

	/* Read QDMA_GLBL_DSC_DBG_DAT0 (0x270) register */
	dsc_dat0_val = qdma_read_reg(qdev, 0x270);
	
	/* Read QDMA_GLBL_DSC_DBG_DAT1 (0x274) register */
	dsc_dat1_val = qdma_read_reg(qdev, 0x274);

	dev_info(&priv->pdev->dev, "QDMA Debug Fabric Registers:\n");
	dev_info(&priv->pdev->dev, "QDMA_GLBL2_DBG_FAB0 (0x1D0): 0x%08x\n", fab0_val);
	
	/* Decode QDMA_GLBL2_DBG_FAB0 fields */
	dev_info(&priv->pdev->dev, "  h2c_inb_conv_in_vld: %u\n", (fab0_val >> 31) & 0x1);
	dev_info(&priv->pdev->dev, "  h2c_inb_conv_in_rdy: %u\n", (fab0_val >> 30) & 0x1);
	dev_info(&priv->pdev->dev, "  h2c_seg_in_vld: %u\n", (fab0_val >> 29) & 0x1);
	dev_info(&priv->pdev->dev, "  h2c_seg_in_rdy: %u\n", (fab0_val >> 28) & 0x1);
	dev_info(&priv->pdev->dev, "  h2c_seg_out_vld: %u\n", (fab0_val >> 24) & 0xF);
	dev_info(&priv->pdev->dev, "  h2c_seg_out_rdy: %u\n", (fab0_val >> 23) & 0x1);
	dev_info(&priv->pdev->dev, "  h2c_mst_crdt_stat: %u (H2C Master available async credits)\n", (fab0_val >> 16) & 0x7F);
	dev_info(&priv->pdev->dev, "  c2h_slv_afifo_full: %u (C2H Slave async fifo full)\n", (fab0_val >> 15) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_slv_afifo_empty: %u (C2H Slave async fifo empty)\n", (fab0_val >> 14) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_deseg_seg_vld: %u\n", (fab0_val >> 10) & 0xF);
	dev_info(&priv->pdev->dev, "  c2h_deseg_seg_rdy: %u\n", (fab0_val >> 9) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_deseg_out_vld: %u\n", (fab0_val >> 8) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_deseg_out_rdy: %u\n", (fab0_val >> 7) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_inb_deconv_out_vld: %u\n", (fab0_val >> 6) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_inb_deconv_out_rdy: %u\n", (fab0_val >> 5) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_dsc_crdt_afifo_full: %u (C2H dsc crdt async fifo full)\n", (fab0_val >> 4) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_dsc_crdt_afifo_empty: %u (C2H dsc crdt async fifo empty)\n", (fab0_val >> 3) & 0x1);
	dev_info(&priv->pdev->dev, "  irq_in_afifo_full: %u (IRQ in async fifo full)\n", (fab0_val >> 2) & 0x1);
	dev_info(&priv->pdev->dev, "  irq_in_afifo_empty: %u (IRQ in async fifo empty)\n", (fab0_val >> 1) & 0x1);
	dev_info(&priv->pdev->dev, "  imm_crd_afifo_empty: %u (Imm crdt async fifo empty)\n", fab0_val & 0x1);

	dev_info(&priv->pdev->dev, "QDMA_GLBL2_DBG_FAB1 (0x1D4): 0x%08x\n", fab1_val);
	
	/* Decode QDMA_GLBL2_DBG_FAB1 fields */
	dev_info(&priv->pdev->dev, "  byp_out_crdt_stat: %u (Bypass out avail async fifo credits)\n", (fab1_val >> 25) & 0x7F);
	dev_info(&priv->pdev->dev, "  tm_dsc_sts_crdt_stat: %u (Tm dsc sts avail async fifo credits)\n", (fab1_val >> 18) & 0x7F);
	dev_info(&priv->pdev->dev, "  c2h_cmn_afifo_full: %u (C2H WRB async fifo full)\n", (fab1_val >> 17) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_cmn_afifo_empty: %u (C2H WRB async fifo empty)\n", (fab1_val >> 16) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_byp_in_afifo_full: %u (C2H Bypass In async fifo full)\n", (fab1_val >> 12) & 0x1);
	dev_info(&priv->pdev->dev, "  c2h_byp_in_afifo_empty: %u (C2H Bypass in async fifo empty)\n", (fab1_val >> 8) & 0x1);
	dev_info(&priv->pdev->dev, "  h2c_byp_in_afifo_full: %u (H2C Bypass in async fifo full)\n", (fab1_val >> 4) & 0x1);
	dev_info(&priv->pdev->dev, "  h2c_byp_in_afifo_empty: %u (H2C Bypass in async fifo empty)\n", fab1_val & 0x1);

	dev_info(&priv->pdev->dev, "QDMA_GLBL_DSC_DBG_DAT0 (0x270): 0x%08x\n", dsc_dat0_val);
	
	/* Decode QDMA_GLBL_DSC_DBG_DAT0 fields */
	dev_info(&priv->pdev->dev, "  ctxt_arb_dir: %u (DMA direction of arbitration request)\n", (dsc_dat0_val >> 29) & 0x1);
	dev_info(&priv->pdev->dev, "  ctxt_arb_qid: %u (QID of arbitration request)\n", (dsc_dat0_val >> 17) & 0xFFF);
	dev_info(&priv->pdev->dev, "  ctxt_arb_req: 0x%x (Vector of ctxt arbitration requesters)\n", (dsc_dat0_val >> 12) & 0x1F);
	dev_info(&priv->pdev->dev, "    EVT_SRC: %u, TRQ_SRC: %u, WBC_SRC: %u, CRD_SRC: %u, IND_SRC: %u\n", 
		(dsc_dat0_val >> 12) & 0x1, (dsc_dat0_val >> 13) & 0x1, (dsc_dat0_val >> 14) & 0x1, 
		(dsc_dat0_val >> 15) & 0x1, (dsc_dat0_val >> 16) & 0x1);
	dev_info(&priv->pdev->dev, "  irq_fifo_fl: %u (Immediate IRQ fifo full)\n", (dsc_dat0_val >> 11) & 0x1);
	dev_info(&priv->pdev->dev, "  tm_dsc_stall: %u (Tm_dsc_sts output backpressured)\n", (dsc_dat0_val >> 10) & 0x1);
	dev_info(&priv->pdev->dev, "  rrq_stall: 0x%x (C2H:%u H2C:%u read request stall)\n", 
		(dsc_dat0_val >> 8) & 0x3, (dsc_dat0_val >> 9) & 0x1, (dsc_dat0_val >> 8) & 0x1);
	dev_info(&priv->pdev->dev, "  rcp_fifo_spc_stall: 0x%x (C2H:%u H2C:%u read completion space stall)\n", 
		(dsc_dat0_val >> 6) & 0x3, (dsc_dat0_val >> 7) & 0x1, (dsc_dat0_val >> 6) & 0x1);
	dev_info(&priv->pdev->dev, "  rrq_fifo_spc_stall: 0x%x (C2H:%u H2C:%u read request fifo space stall)\n", 
		(dsc_dat0_val >> 4) & 0x3, (dsc_dat0_val >> 5) & 0x1, (dsc_dat0_val >> 4) & 0x1);
	dev_info(&priv->pdev->dev, "  fab_mrkr_rsp_stall: 0x%x (C2H:%u H2C:%u mrkr_rsp stall)\n", 
		(dsc_dat0_val >> 2) & 0x3, (dsc_dat0_val >> 3) & 0x1, (dsc_dat0_val >> 2) & 0x1);
	dev_info(&priv->pdev->dev, "  dsc_out_stall: 0x%x (C2H:%u H2C:%u descriptor bypass out stall)\n", 
		dsc_dat0_val & 0x3, (dsc_dat0_val >> 1) & 0x1, dsc_dat0_val & 0x1);

	dev_info(&priv->pdev->dev, "QDMA_GLBL_DSC_DBG_DAT1 (0x274): 0x%08x\n", dsc_dat1_val);
	
	/* Decode QDMA_GLBL_DSC_DBG_DAT1 fields */
	dev_info(&priv->pdev->dev, "  evt_spc_c2h: %u (Event space for C2H)\n", (dsc_dat1_val >> 22) & 0x3F);
	dev_info(&priv->pdev->dev, "  evt_spc_h2c: %u (Event space for H2C)\n", (dsc_dat1_val >> 16) & 0x3F);
	dev_info(&priv->pdev->dev, "  dsc_spc_c2h: %u (Descriptor fetch completion RAM space for C2H)\n", (dsc_dat1_val >> 8) & 0xFF);
	dev_info(&priv->pdev->dev, "  dsc_spc_h2c: %u (Descriptor fetch completion RAM space for H2C)\n", dsc_dat1_val & 0xFF);
}
