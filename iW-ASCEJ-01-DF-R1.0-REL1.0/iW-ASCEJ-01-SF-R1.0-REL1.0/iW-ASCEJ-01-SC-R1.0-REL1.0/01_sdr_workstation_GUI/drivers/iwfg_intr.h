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
#ifndef __IWFG_LIB_H__
#define __IWFG_LIB_H__

#include "iwfg.h"

/**
 * iwfg_init_capacity - calculate the number of vectors and queues
 * @priv: pointer to driver private data
 *
 * Return 0 on success, negative on failure
 **/
int iwfg_init_capacity(struct iwfg_private *priv);

/**
 * iwfg_clear_capacity - reset the number of vectors and queues to zero
 * @priv: pointer to driver private data
 **/
void iwfg_clear_capacity(struct iwfg_private *priv);

/**
 * iwfg_init_interrupt - initialize interrupt resource
 * @priv: pointer to driver private data
 *
 * Return 0 on success, negative on failure
 **/
int iwfg_init_interrupt(struct iwfg_private *priv);

/**
 * iwfg_clear_interrupt - clear resource for all vectors
 * @priv: pointer to driver private data
 **/
void iwfg_clear_interrupt(struct iwfg_private *priv);

#endif
