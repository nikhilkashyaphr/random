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
#ifndef __IWFG_REGISTER_H__
#define __IWFG_REGISTER_H__

#include <linux/io.h>
#include "iwfg_hardware.h"

static inline u32 iwfg_read_reg(struct iwfg_hardware *hw, u32 offset)
{
	return ioread32(hw->addr + offset);
}

static inline void iwfg_write_reg(struct iwfg_hardware *hw, u32 offset, u32 val)
{
	iowrite32(val, hw->addr + offset);
}

#define IWFG_VID_STREAM_BRAM_LEN 0x1000

#define IWFG_C2H_CTRL_REG_OFFSET 0x00
#define IWFG_H2C_CTRL_REG_OFFSET 0x04
#define IWFG_C2H_TOT_BUFSZ_OFFSET 0x08
#define IWFG_CARD_VID_STREAM_PRAM_REG_OFFSET 0x0C
#define IWFG_STREAM_SWITCH_INTR_REG_OFFSET 0x10
#define IWFG_C2H_INTR_REG_OFFSET 0x14
#define IWFG_H2C_INTR_REG_OFFSET 0x18
#define IWFG_USER_INTR_STATUS_REG_OFFSET 0x1C
#define IWFG_QDMA_SOFT_RST_REG_OFFSET 0xA0
#define IWFG_VERSION_CTRL_REG_OFFSET 0xA4
#define IWFG_TPG_PATTERN_SEL_REG_OFFSET 0xA8
#define IWFG_CARD_CONFIG_REG_OFFSET 0xAC

#define IWFG_AUD_CTRL_REG_OFFSET 		0xF00
#define IWFG_AUD_BUF_SIZE_REG_OFFSET 	0xF04

#define IWFG_C2H_CTRL_START_MASK 		BIT(0)
#define IWFG_C2H_CTRL_SET_BUFSZ_MASK 	BIT(1)
#define IWFG_C2H_CTRL_QDMA_C2H_QID_MASK 		GENMASK(12, 2)
#define IWFG_C2H_CTRL_QDMA_C2H_BUFSZ_IDX_MASK 	GENMASK(16, 13)
#define IWFG_C2H_CTRL_LEGACY_DROP_MASK 		BIT(17)

#define IWFG_H2C_CTRL_VID_OUT_SEL_MASK 		BIT(0)
#define IWFG_H2C_CTRL_DBG_SW_MASK 			BIT(1)

#define IWFG_CARD_VID_STREAM_PRAM_BPP_MASK  GENMASK(3, 0)
#define IWFG_CARD_VID_STREAM_PRAM_WIDTH_MASK  GENMASK(17, 4)
#define IWFG_CARD_VID_STREAM_PRAM_HEIGHT_MASK  GENMASK(31, 18)

#define IWFG_CARD_CONFIG_NUM_HDMI_INPUTS_MASK  GENMASK(3, 0)
#define IWFG_CARD_CONFIG_NUM_HDMI_OUTPUTS_MASK GENMASK(7, 4)
#define IWFG_CARD_CONFIG_HAS_AUDIO_MASK         BIT(8)

#define IWFG_C2H_INTR_BIT_MASK 			BIT(0)
#define IWFG_C2H_INTR_VID_STREAM_CHANGE_MASK 	BIT(1)

#define IWFG_H2C_INTR_BIT_MASK                  BIT(0)
#define IWFG_H2C_INTR_CARD_STREAM_CHANGE_MASK  	BIT(1)
#define IWFG_H2C_INTR_CARD_CONFIG_QUERY_MASK    BIT(2)

#define IWFG_AUD_CTRL_START_MASK 			BIT(0)
#define IWFG_AUD_CTRL_SET_AUD_BUFSZ_MASK 	BIT(1)
#define IWFG_AUD_CTRL_C2H_QID_MASK 			GENMASK(12, 2)
#define IWFG_AUD_CTRL_QDMA_BUFSZ_IDX_MASK 	GENMASK(16, 13)

/* INDIRECTION TABLE*/
#define INDIRECTION_TABLE_BASE_ADDR			  QDMA_FUNC_OFFSET_INDIR_TABLE(0,0)
#define INDIRECTION_TABLE_SIZE            0x80
#define IWFG_EN_RSS_KEY_SIZE	  			    40


#endif
