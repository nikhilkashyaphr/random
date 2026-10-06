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
#ifndef __IWFG_HARDWARE_H__
#define __IWFG_HARDWARE_H__

#include "qdma_export.h"

#define IWFG_MAX_XMACS			1 // assuming 1 10G MAC
#define IWFG_XMAC_CORE_VERSION		0x00000301

struct iwfg_hardware {
	unsigned long qdma;
	void __iomem *addr;	/* mapping of shell registers */
};

struct iwfg_qdma_h2c_param {
	u8 rngcnt_idx;
	dma_addr_t dma_addr;
	u16 vid;
};

struct iwfg_qdma_c2h_param {
	u8 bufsz_idx;
	u8 desc_rngcnt_idx;
	u8 cmpl_rngcnt_idx;
	u8 cmpl_desc_sz;
	dma_addr_t desc_dma_addr;
	dma_addr_t cmpl_dma_addr;
	u16 vid;
};

struct iwfg_private;

/**
 * iwfg_ring_count - get the number of descriptors from index
 * @idx: index into the pool
 *
 * Return the number of descriptors pointed at index
 **/
u16 iwfg_ring_count(u8 idx);

u16 iwfg_get_c2h_bufsz(u8 idx);

u8 iwfg_get_c2h_bufsz_idx(u32 dma_bufsz);

/**
 * iwfg_init_hardware - initialize NIC hardware
 * @priv: pointer to driver private data
 *
 * Return 0 on success, negative on failure
 **/
int iwfg_init_hardware(struct iwfg_private *priv);

/**
 * iwfg_clear_hardware - clear NIC hardware
 * @priv: pointer to driver private data
 **/
void iwfg_clear_hardware(struct iwfg_private *priv);

void iwfg_reset_hardware(struct iwfg_private *priv);

/**
 * iwfg_qdma_init_error_interrupt - initialize QDMA error interrupt
 * @qdma: handle to QDMA device
 * @vid: vector ID
 **/
void iwfg_qdma_init_error_interrupt(unsigned long qdma, u16 vid);

/**
 * iwfg_qdma_clear_error_interrupt - invalidate QDMA error interrupt
 * @qdma: handle to QDMA device
 **/
void iwfg_qdma_clear_error_interrupt(unsigned long qdma);

/**
 * iwfg_qdma_ack_and_rearm_error_interrupt - clear all sticky error status bits
 * and rearm the MSI-X error interrupt vector
 * @qdma: handle to QDMA device
 * @vid: MSI-X vector ID used for the error interrupt (typically 0)
 *
 * Must be called from the error thread handler after logging the errors.
 * Clears all W1C error status registers (global, DSC, TRQ, C2H, C2H fatal,
 * H2C), then writes QDMA_OFFSET_GLBL_ERR_INT with the ARM bit set so the
 * hardware will fire a new interrupt on the next error event.
 **/
void iwfg_qdma_ack_and_rearm_error_interrupt(unsigned long qdma, u16 vid);

/**
 * iwfg_qdma_init_tx_queue - initialize a QDMA H2C queue
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @param: pointer to QDMA H2C queue parameters
 *
 * Return 0 on success, negative on failure
 **/
int iwfg_qdma_init_tx_queue(unsigned long qdma, u16 qid,
			    const struct iwfg_qdma_h2c_param *param);

/**
 * iwfg_qdma_init_rx_queue - initialize a QDMA C2H queue
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @param: pointer to QDMA C2H queue parameters
 *
 * Return 0 on success, negative on failure
 **/
int iwfg_qdma_init_rx_queue(unsigned long qdma, u16 qid,
			    const struct iwfg_qdma_c2h_param *param);

/**
 * iwfg_qdma_clear_tx_queue - clear a QDMA H2C queue
 * @qdma: handle to QDMA device
 * @qid: queue ID
 **/
void iwfg_qdma_clear_tx_queue(unsigned long qdma, u16 qid);

/**
 * iwfg_qdma_clear_rx_queue - clear a QDMA C2H queue
 * @qdma: handle to QDMA device
 * @qid: queue ID
 **/
void iwfg_qdma_clear_rx_queue(unsigned long qdma, u16 qid);

/**
 * iwfg_set_tx_head - set TX ring head pointer
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @head: head pointer of the TX ring, i.e., next_to_use
 **/
void iwfg_set_tx_head(unsigned long qdma, u16 qid, u16 head);

/**
 * iwfg_set_rx_head - set RX ring head pointer
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @head: head pointer of the RX ring, i.e., next_to_use
 **/
void iwfg_set_rx_head(unsigned long qdma, u16 qid, u16 head);

/**
 * iwfg_set_completion_tail - set RX completion ring tail pointer
 * @qdma: handle to QDMA device
 * @qid: queue ID
 * @tail: tail pointer of the RX completion ring, i.e., next_to_clean
 **/
void iwfg_set_completion_tail(unsigned long qdma, u16 qid, u16 tail, u8 irq_arm);

/**
 * iwfg_process_qdma_errors - Process and decode QDMA errors
 * @priv: pointer to driver private data
 * @err_info: buffer for error information (can be NULL)
 * @buffer_size: size of the error info buffer
 *
 * Return: 0 if no errors found, non-zero if errors were detected
 **/
int iwfg_process_qdma_errors(struct iwfg_private *priv, char *err_info, size_t buffer_size);

/**
 * iwfg_print_qdma_debug_registers - Print QDMA debug fabric registers
 * @priv: pointer to driver private data
 *
 * This function reads and prints the QDMA Global Debug Fabric registers
 * for debugging purposes including H2C/C2H data paths, async FIFOs, and credits.
 **/
void iwfg_print_qdma_debug_registers(struct iwfg_private *priv);

#endif
