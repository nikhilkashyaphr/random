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
 * 
 * Performance Optimizations:
 * - Debug prints controlled by compile-time DEBUG flag (compile with DEBUG=1 to enable)
 * - DMA-mapped buffer path minimized for zero-copy performance
 * - Redundant sync operations removed when DMA coherency is maintained
 */
#include <linux/pci_regs.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#endif
#include <linux/vmalloc.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/dma-map-ops.h>
#include <linux/workqueue.h>
#include <asm/current.h>
#include <linux/delay.h>

#include "iwfg_hardware.h"
#include "qdma_access/qdma_register.h"
#include "iwfg.h"

/**
 * todo: update the dma allocation using 
 * dma_alloc_attrs(dev, size, &dma_handle, GFP_KERNEL, 
 * 	DMA_ATTR_FORCE_CONTIGUOUS);
 * 
 * This will ensure compatibilty between several linux kernel versions and flavors
 */

/* Maximum number of descriptors to post in one batch */
// #define IWFG_MAX_DESC_BATCH 507
#define IWFG_MAX_DESC_BATCH 256

/*
 * H2C completion wait budget (see iwfg_wait_for_tx()):
 *  - SLICE: how often the sleeping ioctl wakes up to reconcile the ring
 *    against the descriptor writeback (recovers a lost/coalesced MSI-X).
 *  - TOTAL: hard ceiling after which the transfer is declared stuck and
 *    -ETIMEDOUT is returned to userspace instead of sleeping forever.
 *    A stuck transfer means the PL is not accepting the AXI-Stream
 *    (tready low): H2C datapath not enabled, DAC/RF path not initialized
 *    by the PS, or clocks/MTS down. 2 s is orders of magnitude above any
 *    legitimate completion time at ~700 MB/s.
 */
#define IWFG_TX_WAIT_SLICE_MS	100
#define IWFG_TX_WAIT_TOTAL_MS	2000

/* TX queue IDs start from this offset to avoid overlap with RX queues */
#define iwfg_qdma_queue_BASE_OFFSET (IWFG_MAX_QUEUES / 2)
#include "iwfg_dma.h"
#include "iwfg_register.h"

#define IWFG_RX_DESC_STEP 256
#define IWFG_RX_PREARM_WATERMARK 1024

/* Forward declarations */
static void iwfg_dma_sync_sg_for_cpu(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf_hndl);

inline static u16 iwfg_ring_get_real_count(struct iwfg_ring *ring)
{
	/* Valid writeback entry means one less count of descriptor entries */
	return (ring->wb) ? (ring->count - 1) : ring->count;
}

inline static bool iwfg_ring_full(struct iwfg_ring *ring)
{
	u16 real_count = iwfg_ring_get_real_count(ring);
	return ((ring->next_to_use + 1) % real_count) == ring->next_to_clean;
}

inline static void iwfg_ring_increment_head(struct iwfg_ring *ring)
{
	u16 real_count = iwfg_ring_get_real_count(ring);
	ring->next_to_use = (ring->next_to_use + 1) % real_count;
}

inline static void iwfg_ring_increment_tail(struct iwfg_ring *ring)
{
	u16 real_count = iwfg_ring_get_real_count(ring);
	ring->next_to_clean = (ring->next_to_clean + 1) % real_count;
}

inline static u16 iwfg_ring_in_flight(struct iwfg_ring *ring)
{
	u16 real_count = iwfg_ring_get_real_count(ring);

	if (ring->next_to_use >= ring->next_to_clean)
		return ring->next_to_use - ring->next_to_clean;

	return real_count - ring->next_to_clean + ring->next_to_use;
}

inline static u16 iwfg_ring_free_count(struct iwfg_ring *ring)
{
	u16 real_count = iwfg_ring_get_real_count(ring);

	return real_count - iwfg_ring_in_flight(ring) - 1;
}

// static int iwfg_pipeline_update_wait(struct iwfg_private *priv)
// {
// 	int ret;

//     ret = wait_event_interruptible(
//         priv->pipeline_update_waitq,
//         (atomic_read(&priv->pipeline_update_event) == 1)
//     );

// 	if (ret == 0)
// 		atomic_set(&priv->pipeline_update_event, 0);

//     return ret; // 0 if woken up, -ERESTARTSYS if interrupted/killed
// }
/*
static int iwfg_vsync_wait(struct iwfg_private *priv, struct iwfg_qdma_queue *q)
{
	int ret;

    ret = wait_event_interruptible(
        q->vsync_waitq,
        (atomic_read(&q->vsync_event) == 1)
    );

	if (ret == 0)
		atomic_set(&q->vsync_event, 0);

    return ret; // 0 if woken up, -ERESTARTSYS if interrupted/killed
}
*/
static bool wait_for_h2c_idle(atomic_t *h2c_busy, unsigned int max_usecs)
{
	unsigned int i;

	for (i = 0; i < max_usecs; ++i) {
		if (atomic_read(h2c_busy) == 0)
			return true;
		cpu_relax();    /* hint to CPU */
		udelay(1);      /* busy-wait 1 us; OK for short usecs */
	}
	return false; /* timed out */
}

/*
 * iwfg_tx_reclaim - reconcile the H2C descriptor ring against the
 * hardware writeback and wake the sleeping DMA_DATA ioctl when the ring
 * has fully drained.
 *
 * Called from two places:
 *   1. iwfg_tx_clean()   - the queue interrupt work item (normal path);
 *   2. iwfg_wait_for_tx() - directly from the waiting ioctl when a wait
 *      slice times out, so a lost/coalesced completion interrupt can no
 *      longer strand the waiter forever (the writeback is re-read and
 *      the ring reconciled synchronously).
 * Safe from both softirq-scheduled work and process context: all ring
 * state is protected by priv->tx_lock (irqsave).
 */
static void iwfg_tx_reclaim(struct iwfg_qdma_queue *q)
{
	struct iwfg_private *priv = q->priv;
	struct iwfg_ring *ring = &q->desc_ring;
	struct qdma_wb_stat wb;
	struct iwfg_user_page_buf *user_buf_hndl = q->user_buf;
	unsigned long flags;
	u16 real_count = iwfg_ring_get_real_count(ring);
	u16 loop_guard;

	qdma_unpack_wb_stat(&wb, ring->wb);

	/*
	 * HARDENING (second-run CPU hang): next_to_clean wraps modulo
	 * real_count, so if the writeback cidx is ever outside
	 * [0, real_count) - e.g. a stale/garbage writeback observed while
	 * the queue is being torn down, or a spurious IRQ right after
	 * re-init - the old "while (wb.cidx != next_to_clean)" could NEVER
	 * terminate, spinning forever inside spin_lock_irqsave (IRQs off)
	 * and hard-locking the CPU. Validate cidx and bound the loop.
	 */
	if (wb.cidx >= real_count) {
		dev_warn_ratelimited(&priv->pdev->dev,
			"tx_clean: bogus wb cidx=%u (ring real_count=%u), qid=%u - ignoring",
			wb.cidx, real_count, q->qid);
		return;
	}

	// dev_dbg(&priv->pdev->dev,
	// 	  "tx_clean: cidx=%u pidx=%u qid=%u next_to_use=%u next_to_clean=%u",
	// 	  wb.cidx, wb.pidx, q->qid, ring->next_to_use, ring->next_to_clean);

	/* Protect ring descriptor operations from race with iwfg_tx_roll */
	spin_lock_irqsave(&priv->tx_lock, flags);

	loop_guard = real_count; /* can never legitimately advance further */
	while (wb.cidx != ring->next_to_clean && loop_guard--) {

		// qdma_unpack_wb_stat(&wb, ring->wb);
		iwfg_ring_increment_tail(ring);
	}
	wmb();
	// to set the IRQ_ARM bit again, because it is cleared by hardware for every few cidx that are cleaned
	iwfg_set_tx_head(priv->hw.qdma, q->qid, ring->next_to_use);

	if (ring->next_to_use == ring->next_to_clean && user_buf_hndl) {
		// dev_info(&priv->pdev->dev,
		// 	  "tx_clean: cidx=%u pidx=%u qid=%u next_to_use=%u next_to_clean=%u",
		// 	  wb.cidx, wb.pidx, q->qid, ring->next_to_use, ring->next_to_clean);
		// dev_info(&priv->pdev->dev, "tx_clean: All descriptors cleaned for qid=%u", q->qid);
		spin_lock(&user_buf_hndl->lock);
		atomic_set(&user_buf_hndl->ready_to_use, 1);
		spin_unlock(&user_buf_hndl->lock);
		wake_up_interruptible(&user_buf_hndl->waitq);
	}

	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

static void iwfg_tx_clean(struct work_struct *work)
{
	struct iwfg_qdma_queue *q =
		container_of(work, struct iwfg_qdma_queue, intr_work);

	iwfg_tx_reclaim(q);
}

/*
 * iwfg_wait_for_tx - bounded wait for H2C ring drain.
 *
 * Replaces the old unbounded iwfg_wait_for_data() on the TX path only
 * (the C2H path keeps its indefinite wait: waiting arbitrarily long for
 * incoming samples is legitimate there). Two failure modes of the old
 * code are closed:
 *
 *  1. LOST COMPLETION INTERRUPT: if the MSI-X for the final descriptors
 *     is coalesced/dropped, nobody ever ran iwfg_tx_clean() and the
 *     waiter slept forever. Now every SLICE we re-read the writeback
 *     and reclaim synchronously.
 *
 *  2. CARD NOT CONSUMING (tready low on the PL AXI-Stream because the
 *     H2C datapath is disabled or the RF chain is not initialized): the
 *     writeback cidx never advances, so after TOTAL ms we log the exact
 *     ring state and return -ETIMEDOUT. Userspace gets a diagnosable
 *     error instead of a silent hang.
 *
 * Returns 0 on drain, -ERESTARTSYS on signal, -ETIMEDOUT when stuck.
 */
static int iwfg_wait_for_tx(struct iwfg_private *priv, struct iwfg_qdma_queue *q)
{
	struct iwfg_user_page_buf *user_buf_hndl = q->user_buf;
	struct iwfg_ring *ring = &q->desc_ring;
	struct qdma_wb_stat wb;
	unsigned int waited_ms = 0;
	long t;

	if (!user_buf_hndl)
		return -EINVAL;

	for (;;) {
		t = wait_event_interruptible_timeout(user_buf_hndl->waitq,
			(atomic_read(&user_buf_hndl->ready_to_use) == 1
			 || user_buf_hndl->compl_count > 0),
			msecs_to_jiffies(IWFG_TX_WAIT_SLICE_MS));

		if (t < 0)
			return t;	/* -ERESTARTSYS: signal (Ctrl+C) */
		if (t > 0)
			break;		/* condition met - ring drained  */

		/*
		 * Slice expired with no wake. Reconcile against the
		 * writeback ourselves in case the completion interrupt
		 * was lost; if that drains the ring the condition is now
		 * true and we exit on the next check.
		 */
		iwfg_tx_reclaim(q);
		if (atomic_read(&user_buf_hndl->ready_to_use) == 1
		    || user_buf_hndl->compl_count > 0)
			break;

		waited_ms += IWFG_TX_WAIT_SLICE_MS;
		if (waited_ms >= IWFG_TX_WAIT_TOTAL_MS) {
			qdma_unpack_wb_stat(&wb, ring->wb);
			dev_err_ratelimited(&priv->pdev->dev,
				"H2C completion timeout (%u ms): qid=%u pidx=%u next_to_clean=%u wb.cidx=%u - card is not consuming the stream (H2C datapath disabled or RF/DAC path not initialized)",
				waited_ms, q->qid, ring->next_to_use,
				ring->next_to_clean, wb.cidx);
			return -ETIMEDOUT;
		}
	}

	/* Consume the completion exactly like iwfg_wait_for_data() */
	spin_lock(&user_buf_hndl->lock);
	atomic_set(&user_buf_hndl->ready_to_use, 0);
	if (user_buf_hndl->compl_count > 0)
		user_buf_hndl->compl_count--;
	spin_unlock(&user_buf_hndl->lock);

	return 0;
}

static int iwfg_tx_roll(struct iwfg_private *priv, u16 qid, u16 start, u16 desc_count)
{
	struct iwfg_qdma_queue *q = priv->queue[qid];
	struct iwfg_ring *desc_ring = &q->desc_ring;
	struct iwfg_user_page_buf *user_buf = q->user_buf;
	struct iwfg_tx_desc *tx_desc;
	u8 *desc_ptr;
	int batch_count = 0;
	int ret;
	unsigned long flags;
	u16 current_index = 0;

	list_for_each_entry(tx_desc, &user_buf->tx_desc_list, list) {
		/* Skip to start position efficiently */
		if (current_index < start) {
			current_index++;
			continue;
		}

		if (desc_count == 0)
			break;

		/* Protect ring descriptor operations from race with iwfg_tx_clean */
		spin_lock_irqsave(&priv->tx_lock, flags);
		
		desc_ptr = desc_ring->desc + QDMA_H2C_ST_DESC_SIZE * desc_ring->next_to_use;
		qdma_pack_h2c_st_desc(desc_ptr, &tx_desc->desc);
		iwfg_ring_increment_head(desc_ring);
		desc_count--;
		batch_count++;

		/* Post descriptors in batches of IWFG_MAX_DESC_BATCH */
		if (batch_count >= IWFG_MAX_DESC_BATCH) {
			wmb();
			iwfg_set_tx_head(priv->hw.qdma, q->qid, desc_ring->next_to_use);
			
			spin_unlock_irqrestore(&priv->tx_lock, flags);
			
			/* Wait (bounded) for batch completion before posting next batch */
			ret = iwfg_wait_for_tx(priv, q);
			if (ret) {
				dev_info(&priv->pdev->dev,
					 "TX roll batch wait ended early (%d)", ret);
				return ret;  /* -ERESTARTSYS or -ETIMEDOUT to userspace */
			}
			
			batch_count = 0;
		} else {
			spin_unlock_irqrestore(&priv->tx_lock, flags);
		}
	}

	/* Post any remaining descriptors */
	if (batch_count > 0) {
		spin_lock_irqsave(&priv->tx_lock, flags);
		wmb();
		iwfg_set_tx_head(priv->hw.qdma, q->qid, desc_ring->next_to_use);
		spin_unlock_irqrestore(&priv->tx_lock, flags);
		
		/* Wait (bounded) for final batch completion */
		ret = iwfg_wait_for_tx(priv, q);
		if (ret) {
			dev_info(&priv->pdev->dev,
				 "TX roll final batch wait ended early (%d)", ret);
			return ret;  /* -ERESTARTSYS or -ETIMEDOUT to userspace */
		}
	}
	
	return 0;  // Success
}

static void iwfg_rx_post_user_descs_locked(struct iwfg_qdma_queue *q,
					   struct iwfg_user_page_buf *user_buf)
{
	struct iwfg_ring *desc_ring = &q->desc_ring;
	struct iwfg_rx_desc *rx_desc;

	list_for_each_entry(rx_desc, &user_buf->rx_desc_list, list) {
		u8 *desc_ptr = desc_ring->desc +
			       QDMA_C2H_ST_DESC_SIZE * desc_ring->next_to_use;

		qdma_pack_c2h_st_desc(desc_ptr, &rx_desc->desc);
		iwfg_ring_increment_head(desc_ring);
	}
}

static void iwfg_rx_maybe_post_prearm(struct iwfg_private *priv,
				      struct iwfg_qdma_queue *q)
{
	struct iwfg_user_page_buf *prearm_buf;
	struct iwfg_ring *desc_ring = &q->desc_ring;
	unsigned long flags;
	u16 in_flight;
	u16 free_count;

	spin_lock_irqsave(&priv->rx_lock, flags);

	prearm_buf = q->prearm_user_buf;
	if (!prearm_buf || q->prearm_posted)
		goto out_unlock;

	in_flight = iwfg_ring_in_flight(desc_ring);
	free_count = iwfg_ring_free_count(desc_ring);
	if (in_flight > IWFG_RX_PREARM_WATERMARK ||
	    free_count < prearm_buf->rx_desc_count)
		goto out_unlock;

	iwfg_rx_post_user_descs_locked(q, prearm_buf);
	q->prearm_posted = true;
	wmb();
	iwfg_set_rx_head(priv->hw.qdma, q->qid, desc_ring->next_to_use);

	dev_dbg(&priv->pdev->dev,
		"RX prearm posted: qid=%u desc=%u in_flight=%u free=%u pidx=%u cidx=%u",
		q->qid, prearm_buf->rx_desc_count, in_flight, free_count,
		desc_ring->next_to_use, desc_ring->next_to_clean);

out_unlock:
	spin_unlock_irqrestore(&priv->rx_lock, flags);
}

void iwfg_rx_roll(struct iwfg_private *priv, u16 qid, int stream_id)
{
	struct iwfg_qdma_queue *q = priv->queue[qid];
	struct iwfg_ring *desc_ring = &q->desc_ring;
	struct iwfg_user_page_buf *user_buf = q->user_buf;
	volatile u32 reg_val;
	unsigned long flags;
	//int ret;

	if (user_buf->is_audio) {
		reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_AUD_CTRL_REG_OFFSET);
		reg_val |= FIELD_SET(IWFG_AUD_CTRL_C2H_QID_MASK, qid);
		iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_AUD_CTRL_REG_OFFSET, reg_val); // write audio qid to audio control reg
	} else {
		reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET);
		reg_val |= FIELD_SET(IWFG_C2H_CTRL_QDMA_C2H_QID_MASK, qid);
		iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET, reg_val);
	}

	/* Sync SGL with CPU before processing descriptors */
	if (q->user_buf && q->user_buf->sgl) {
		iwfg_dma_sync_sg_for_cpu(priv, q->user_buf);
	}

	/* Protect ring descriptor operations from race with iwfg_rx_poll */
	spin_lock_irqsave(&priv->rx_lock, flags);

	iwfg_rx_post_user_descs_locked(q, user_buf);
	wmb();
	iwfg_set_rx_head(priv->hw.qdma, q->qid, desc_ring->next_to_use);

	spin_unlock_irqrestore(&priv->rx_lock, flags);

	/** signal user logic of new descriptors */
	
	if (user_buf->is_audio) {
		reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_AUD_CTRL_REG_OFFSET);
		reg_val |= IWFG_AUD_CTRL_START_MASK | IWFG_AUD_CTRL_SET_AUD_BUFSZ_MASK;
		iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_AUD_CTRL_REG_OFFSET, reg_val);
	} else {
		reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET);

		dev_dbg(&priv->pdev->dev, "RX roll: read C2H_CTRL_REG=0x%08x for stream_id=%d, qid=%d", reg_val, stream_id, qid);
		reg_val |= IWFG_C2H_CTRL_START_MASK;
		reg_val &= ~IWFG_C2H_CTRL_LEGACY_DROP_MASK;
		iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET, reg_val);
		dev_dbg(&priv->pdev->dev, "RX roll: write C2H_CTRL_REG=0x%08x for stream_id=%d, qid=%d", reg_val, stream_id, qid);

	}
	dev_dbg(&priv->pdev->dev, "Rx roll: descriptors posted");
}

/**
 * iwfg_queue_buffer_nowait - Queue a buffer without waiting for completion
 * @priv: device private data
 * @qid: queue ID
 * 
 * This function queues a buffer by calling iwfg_rx_roll() to signal hardware
 * without waiting for data reception. Used for non-blocking buffer queuing.
 */
// void iwfg_queue_buffer_nowait(struct iwfg_private *priv, u16 qid)
// {
// 	struct iwfg_qdma_queue *q = priv->queue[qid];
	
// 	if (q && q->user_buf) {
// 		iwfg_rx_roll(priv, qid);
// 		dev_dbg(&priv->pdev->dev, "Queued buffer %d (non-blocking)", qid);
// 	}
// }

int iwfg_wait_for_data(struct iwfg_user_page_buf *user_buf_hndl)
{
    int ret;
    u32 compl_count;

    // Safety check
    if (!user_buf_hndl) {
        return -EINVAL;
    }

    // Wait until ready_to_use becomes 1 (interruptible)
    // Note: Reading compl_count in wait condition without lock is acceptable for wait_event_interruptible
    // as it's checked again under lock after wakeup
    ret = wait_event_interruptible(
        user_buf_hndl->waitq,
        (atomic_read(&user_buf_hndl->ready_to_use) == 1
		|| user_buf_hndl->compl_count > 0) // Check if any chunked DMA completions are available
    );

    if (ret == 0) {
		// Buffer is ready, reset ready_to_use and decrement appropriate completion counter
		spin_lock(&user_buf_hndl->lock);
		atomic_set(&user_buf_hndl->ready_to_use, 0);
		compl_count = user_buf_hndl->compl_count;
		if (compl_count > 0) {
			user_buf_hndl->compl_count--;
		}
		if (user_buf_hndl->fifo_overflow) {
			ret = -EAGAIN;
			user_buf_hndl->fifo_overflow = 0;
		}
		spin_unlock(&user_buf_hndl->lock);
    }
    // If ret != 0, it means we were interrupted (e.g., by signal/termination)
    // In this case, don't modify any buffer state - cleanup will be done in iwfg_release()

    return ret; // 0 if woken up, -ERESTARTSYS if interrupted/killed
}

int iwfg_xmit_data(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf, u32 qid)
{
	struct iwfg_qdma_queue *q = priv->queue[qid];
	int ret;
	u16 desc_count = 0;
	u16 start;
	int retry_count;
	int busy_state;
	// int prev_direction;
	const int MAX_RETRIES = 1000; // Reasonable limit to prevent infinite loops

	if (!q) {
		dev_err(&priv->pdev->dev, "tx queue %d not initialized", 1);
		return -EINVAL;
	}

	q->user_buf = user_buf;

	// Use atomic compare-and-swap with bounded retry to prevent race condition with reset
	// Wait for h2c_busy to become 0 and atomically set it to 1
	retry_count = 0;
	
	while (retry_count < MAX_RETRIES) {
		if (atomic_cmpxchg(&priv->h2c_busy, 0, 1) == 0) {
			// Successfully acquired H2C
			break;
		}
		
		busy_state = atomic_read(&priv->h2c_busy);
		if (busy_state == 2) {
			// Reset in progress - don't wait, fail immediately
			dev_err(&priv->pdev->dev, "h2c reset in progress, cannot transmit on qid %d", 1);
			return -EBUSY;
		}
		
		// H2C transfer in progress (state 1), wait briefly and retry
		cpu_relax();
		if (retry_count % 100 == 0) {
			// Yield CPU every 100 iterations to be nice to other processes
			cond_resched();
		}
		retry_count++;
	}
	
	if (retry_count >= MAX_RETRIES) {
		dev_err(&priv->pdev->dev, "h2c timeout after %d retries, cannot transmit on qid %d", MAX_RETRIES, 1);
		return -ETIMEDOUT;
	}

	/** Prepare the DMA request */
	// start = dma_req->offset / q->desc_buf_size;
	start = user_buf->tx_desc_served;
	// desc_count = user_buf->size / q->desc_buf_size;
	desc_count = user_buf->tx_desc_count;

	// user_buf->q_desc_served += desc_count;
	// if (dma_req->flags == IWFG_FLAG_LAST_PACKET) {
	// 	desc_count += (user_buf->q_desc_count - user_buf->q_desc_served);
	// 	// desc_count++;
	// 	user_buf->q_desc_served = 0;
	// }
	// dev_alert(&priv->pdev->dev, "qid %d iwfg_xmit_data: start = %u desc_count = %u ||| offset = %u", qid, start, desc_count, dma_req->offset);

	/** Fill the queue with descriptors */
	ret = iwfg_tx_roll(priv, qid, start, desc_count);
	if (ret) {
		dev_info(&priv->pdev->dev, "TX data transmission interrupted");
	}
	atomic_set(&priv->h2c_busy, 0);

	/* Clear buffer DMA direction - H2C complete */

	// dev_alert(&priv->pdev->dev, "TX queue %d, cidx = %d, pidx = %d", qid, q->desc_ring.next_to_clean, q->desc_ring.next_to_use);

	return ret;
}

int iwfg_recv_data(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf,
		   u32 qid, int stream_id, struct iwfg_user_page_buf *next_user_buf)
{
	struct iwfg_qdma_queue *q = priv->queue[qid];
	int ret;
	unsigned long flags;

	if (!q) {
			dev_err(&priv->pdev->dev, "rx queue %d not initialized", qid);
		return -EINVAL;
	}

	dev_dbg(&priv->pdev->dev, "Preparing to receive data on qid %d, stream_id %d", qid, stream_id);

	/*
	 * Record stream_id and pending next buffer before waiting so rx_poll can
	 * post the next descriptors while the current buffer is still active.
	 */
	spin_lock_irqsave(&priv->rx_lock, flags);
	q->stream_id = stream_id;
	q->prearm_user_buf = next_user_buf;
	q->prearm_posted = false;
	spin_unlock_irqrestore(&priv->rx_lock, flags);

	/*
	 * If the incoming buffer was already pre-armed by the previous call,
	 * the ring already has its descriptors and the FPGA is filling it.
	 * Skip iwfg_rx_roll and just wait for the in-progress transfer to finish.
	 */
	if (q->user_buf != user_buf) {
		q->user_buf = user_buf;
		iwfg_rx_roll(priv, qid, stream_id);
	} else {
		dev_dbg(&priv->pdev->dev,
			"Buffer already pre-armed on qid %d, waiting only", qid);
	}

	iwfg_rx_maybe_post_prearm(priv, q);

	ret = iwfg_wait_for_data(q->user_buf);

	if (ret != 0) {
		/* Cancel stale prearm pointer when this wait does not complete cleanly. */
		spin_lock_irqsave(&priv->rx_lock, flags);
		if (q->prearm_user_buf == next_user_buf && !q->prearm_posted)
			q->prearm_user_buf = NULL;
		spin_unlock_irqrestore(&priv->rx_lock, flags);
	}

	return ret;
}

static void iwfg_rx_poll(struct work_struct *work)
{
	struct iwfg_qdma_queue *q =
		container_of(work, struct iwfg_qdma_queue, intr_work);
	struct iwfg_private *priv = q->priv;

	u16 qid = q->qid;
	struct iwfg_ring *desc_ring = &q->desc_ring;
	struct iwfg_ring *cmpl_ring = &q->cmpl_ring;
	struct qdma_c2h_cmpl cmpl;
	struct qdma_c2h_cmpl_stat cmpl_stat;
	u8 *cmpl_ptr;
	u8 *cmpl_stat_ptr;
	u32 color_stat;
	// int i, rv;
	bool flipped = 0;
	// void *res;
	u32 counter = 0;
	struct iwfg_user_page_buf *user_buf_hndl;
	int total_accu_len;
	int cur_accu_len;
	unsigned long flags;
	bool is_usr_ovf = false;
	int expected, actual;
	int retry_count = 0;
	struct iwfg_user_page_buf *prearm_buf = NULL;
	bool prearm_already_posted = false;
	size_t pending_carry = 0;

	cmpl_ptr =
		cmpl_ring->desc + QDMA_C2H_CMPL_SIZE * cmpl_ring->next_to_clean;
	cmpl_stat_ptr =
		cmpl_ring->desc + QDMA_C2H_CMPL_SIZE * (cmpl_ring->count - 1);

	qdma_unpack_c2h_cmpl(&cmpl, cmpl_ptr);
	qdma_unpack_c2h_cmpl_stat(&cmpl_stat, cmpl_stat_ptr);

	color_stat = cmpl_stat.color;

	// dev_alert(&priv->pdev->dev,
	// 	"c2h_cmpl pkt_id %u, pkt_len %u, error %u, color %u cmpl_ring->color:%u",
	// 	cmpl.pkt_id, cmpl.pkt_len, cmpl.err, cmpl.color,
	// 	cmpl_ring->color);

	/* Color of completion entries and completion ring are initialized to 0
	 * and 1 respectively.  When an entry is filled, it has a color bit of
	 * 1, thus making it the same as the completion ring color.  A different
	 * color indicates that we are done with the current batch.  When the
	 * ring index wraps around, the color flips in both software and
	 * hardware.  Therefore, it becomes that completion entries are filled
	 * with a color 0, and completion ring has a color 0 as well.
	 */
	if (cmpl.color != cmpl_ring->color) {
		dev_dbg(&priv->pdev->dev,
			"color mismatch1: cmpl.color %u, cmpl_ring->color %u  cmpl_stat_color %u",
			cmpl.color, cmpl_ring->color, color_stat);
	}

	if (cmpl.err == 1) {
		dev_err(&priv->pdev->dev, "completion error detected in cmpl entry!");
		// todo: need to handle the error ...
		iwfg_qdma_clear_error_interrupt(priv->hw.qdma);
	}

	user_buf_hndl = q->user_buf;
	total_accu_len = user_buf_hndl->total_filled_size;
	cur_accu_len = user_buf_hndl->cur_filled_size;

	/*
	 * Apply stream overshoot captured at the previous buffer boundary to the
	 * current active buffer. Carry is stream-global, not buffer-local.
	 */
	spin_lock_irqsave(&priv->rx_lock, flags);
	if (q->rx_pending_carry) {
		pending_carry = q->rx_pending_carry;
		q->rx_pending_carry = 0;
	}
	spin_unlock_irqrestore(&priv->rx_lock, flags);

	if (pending_carry) {
		total_accu_len += pending_carry;
		cur_accu_len += pending_carry;
	}
	// size_t copied = 0;
	
	// Note: Buffer availability is checked in interrupt handler before scheduling this work
	
	// Reset state when starting a new buffer
	if (user_buf_hndl->cur_filled_size == 0) {
		// dev_dbg(&priv->pdev->dev, "Starting new buffer fill for qid %d", qid);
	}

	int num_iter = 0;

	// main processing loop for rx_poll
	while ((cmpl_ring->next_to_clean != cmpl_stat.pidx)) {
		
		total_accu_len += cmpl.pkt_len;
		cur_accu_len += cmpl.pkt_len;
		counter++;

		if (cmpl.usr_ovf) {
			// Get the corresponding C2H descriptor DMA address
			// u8 *desc_ptr = desc_ring->desc + QDMA_C2H_ST_DESC_SIZE * desc_ring->next_to_clean;
			// struct qdma_c2h_st_desc *current_desc = (struct qdma_c2h_st_desc *)desc_ptr;

			dev_alert(&priv->pdev->dev,
				"qid %u c2h_cmpl pkt_id %u, pkt_len %u, error %u, color %u cmpl_ring->color:%u, sof:%u, usr_overflow:%u",
				qid, cmpl.pkt_id, cmpl.pkt_len, cmpl.err, cmpl.color,
				cmpl_ring->color, cmpl.sof, cmpl.usr_ovf);

			is_usr_ovf = true;

		}

		// dev_dbg(&priv->pdev->dev,
		// 	"cmpl_ring->next_to_clean %u, cmpl_stat.pidx %u, pkt_len %u",
		// 	cmpl_ring->next_to_clean, cmpl_stat.pidx, cmpl.pkt_len);

		/* Protect ring operations from race with iwfg_rx_roll */
		spin_lock_irqsave(&priv->rx_lock, flags);

		iwfg_ring_increment_tail(desc_ring);

		// dev_dbg(&priv->pdev->dev,
		// 	"desc_ring %u next_to_use:%u next_to_clean:%u",
		// 	iwfg_ring_get_real_count(desc_ring),
		// 	desc_ring->next_to_use,
		// 	desc_ring->next_to_clean);
		if (iwfg_ring_full(desc_ring)) {
			dev_dbg(&priv->pdev->dev, "desc_ring full");
		}

		// if (iwfg_rx_high_watermark(q, desc_ring->head)) {
		// 	// dev_alert(&priv->pdev->dev, "Qid %d High watermark: h = %d, t = %d",
		// 	// 	   qid, desc_ring->head,
		// 	// 	   desc_ring->next_to_clean);
		// 	iwfg_rx_refill(q, desc_ring->head);
		// }

		iwfg_ring_increment_tail(cmpl_ring);

		spin_unlock_irqrestore(&priv->rx_lock, flags);

		iwfg_rx_maybe_post_prearm(priv, q);

		// dev_dbg(&priv->pdev->dev,
		// 	"cmpl_ring %u next_to_use:%u next_to_clean:%u, flipped:%s",
		// 	iwfg_ring_get_real_count(cmpl_ring),
		// 	cmpl_ring->next_to_use,
		// 	cmpl_ring->next_to_clean,
		// 	flipped ? "true" : "false");
		if (iwfg_ring_full(cmpl_ring)) {
			dev_dbg(&priv->pdev->dev, "cmpl_ring full");
		}
		if (cmpl.color != cmpl_ring->color) {
			// dev_dbg(&priv->pdev->dev,
			// 	"part 1. cmpl_ring->next_to_clean=%u color *** old fliping *** color[%u]",
			// 	cmpl_ring->next_to_clean,
			// 	cmpl_ring->color);
			cmpl_ring->color = (cmpl_ring->color == 0) ? 1 : 0;
			flipped = 1;
		}
		cmpl_ptr = cmpl_ring->desc +
			   (QDMA_C2H_CMPL_SIZE * cmpl_ring->next_to_clean);

		qdma_unpack_c2h_cmpl(&cmpl, cmpl_ptr);

		// dev_alert(&priv->pdev->dev,
		// 	"c2h_cmpl(b) pkt_id %u, pkt_len %u, error %u, color %u",
		// 	cmpl.pkt_id, cmpl.pkt_len, cmpl.err,
		// 	cmpl.color);

		num_iter++;
	}

	// dev_alert(&priv->pdev->dev,"qid = %u, packet count = %u, total_accu_len = %u, userb_buf_size = %zu", qid,
	// 	counter, total_accu_len, user_buf_hndl->usr_buf_size);

	/* Protect the completion tail update from race with iwfg_rx_roll */
	spin_lock_irqsave(&priv->rx_lock, flags);
	if (cmpl_ring->next_to_clean == cmpl_stat.pidx) {
		// dev_dbg(&priv->pdev->dev,
		// 	"next_to_clean == cmpl_stat.pidx %u",
		// 	cmpl_stat.pidx);
		// napi_cmpl_rval = napi_complete_done(napi, work);
		iwfg_set_completion_tail(priv->hw.qdma, qid,
					 cmpl_ring->next_to_clean, 1);
		// dev_dbg(&priv->pdev->dev, "iwfg_set_completion_tail ");
	}
	spin_unlock_irqrestore(&priv->rx_lock, flags);

	/** signal userspace on dma completion */
	if (is_usr_ovf) { // special case for fpga fifo overflow
		/** if the fifo overflow signal is set, we need to signal the qdma again for new data */
		// TODO: (important) need to revisit workaround logic for dma buffer sharing implementation
		
		spin_lock(&user_buf_hndl->lock);
		atomic_set(&user_buf_hndl->ready_to_use, 1);
		user_buf_hndl->compl_count = 0; // reset completion count on overflow
		user_buf_hndl->req_compls = user_buf_hndl->usr_buf_size / user_buf_hndl->size_to_fill; // reset requested completions
		user_buf_hndl->cur_filled_size = 0; // Reset filled size for next use
		user_buf_hndl->total_filled_size = 0;
		user_buf_hndl->fifo_overflow = 1;
		spin_unlock(&user_buf_hndl->lock);
		
		// wake_up_interruptible(&user_buf_hndl->waitq);
		
		dev_dbg(&priv->pdev->dev,
			"**rx_poll: Fifo Overflow - qid %d, cur_accu_len %d, cur_filled_size reset to %zu, size_to_fill = %zu, total_accu_len = %d, total_filled_size = %zu", 
			qid, cur_accu_len, user_buf_hndl->cur_filled_size, user_buf_hndl->size_to_fill, total_accu_len, user_buf_hndl->total_filled_size);

		// iwfg_write_reg(&priv->hw, 0x08, 0x0);
		
		// Robust reset sequence: wait for any ongoing transfer and atomically set reset state
		wait_for_h2c_idle(&priv->h2c_busy, 1000);
		
		// Atomically transition to reset state, handling any race conditions
		retry_count = 0;
		
		while (retry_count < 100) {
			expected = 0;
			actual = atomic_cmpxchg(&priv->h2c_busy, expected, 2);
			
			if (actual == expected) {
				// Successfully set to reset state
				break;
			} else if (actual == 1) {
				// Transfer in progress, wait and retry
				wait_for_h2c_idle(&priv->h2c_busy, 100);
				retry_count++;
			}
		}
		
		if (retry_count >= 100) {
			dev_warn(&priv->pdev->dev, "Failed to acquire reset lock after %d retries, forcing reset", retry_count);
			atomic_set(&priv->h2c_busy, 2);
		}

		// TODO: do this stuff later
		// iwfg_clear_queue_resource(priv);
		for (int i = 0; i < IWFG_MAX_QUEUES; i++) {
			if (priv->queue[i] && priv->queue[i]->direction == DMA_FROM_DEVICE) {
				iwfg_clear_rx_queue(priv, i);
			} else if (priv->queue[i] && priv->queue[i]->direction == DMA_TO_DEVICE) {
				iwfg_clear_tx_queue(priv, i);
			}
		}

		iwfg_reset_hardware(priv);
		// if (iwfg_init_queue_resource(priv) < 0) {
		// 	dev_err(&priv->pdev->dev, "Failed to re-initialize queue resource %d after overflow", qid);
		// }
		
		struct iwfg_cdev *cdev_entry;
		int ret;

		list_for_each_entry(cdev_entry, &priv->cdev_list, list) {
			if (cdev_entry->direction == DMA_FROM_DEVICE) { // c2h
				ret = iwfg_init_rx_queue(priv, cdev_entry->qid);
			} else if (cdev_entry->direction == DMA_TO_DEVICE) { // h2c
				ret = iwfg_init_tx_queue(priv, cdev_entry->qid);
			} else {
				dev_err(&priv->pdev->dev, "Invalid device direction: 0x%x", cdev_entry->direction);
			}
		}
		
		atomic_set(&priv->h2c_busy, 0);

		iwfg_write_reg(&priv->hw, 0x08, 0x0);
		// iwfg_write_reg(&priv->hw, 0x00, 0x0); // clear resolution lock bit
		// udelay(100);
		// iwfg_write_reg(&priv->hw, 0x0C, 0x10E07804); // config resolution
		udelay(100);
		iwfg_write_reg(&priv->hw, 0x00, 0x3); // set resolution lock
		
		wake_up_interruptible(&user_buf_hndl->waitq);

		// todo: check if invoking iwfg_recv_data is needed after reset
		// iwfg_recv_data(priv, user_buf_hndl);

	} else if (total_accu_len >= user_buf_hndl->usr_buf_size) { // this would be end of the user buffer
	// } else if (total_accu_len >= user_buf_hndl->usr_buf_size || total_accu_len >= user_buf_hndl->usr_buf_size - 4096) { // this would be end of the user buffer
		size_t carry = total_accu_len - user_buf_hndl->usr_buf_size;

		spin_lock(&user_buf_hndl->lock);
		atomic_set(&user_buf_hndl->ready_to_use, 1);
		user_buf_hndl->compl_count += user_buf_hndl->req_compls; // since we are at the end of the buffer, we can signal all requested completions
		user_buf_hndl->req_compls = user_buf_hndl->usr_buf_size / user_buf_hndl->size_to_fill; // reset requested completions
		/*
		 * Carryover is only valid for chunked mode. In full-buffer mode, each
		 * IOCTL expects an entirely fresh buffer; carrying bytes forward can
		 * cause early completion and stale data at deterministic offsets.
		 */
		if (user_buf_hndl->is_chunked) {
			user_buf_hndl->cur_filled_size = carry;
			user_buf_hndl->total_filled_size = carry;
		} else {
			user_buf_hndl->cur_filled_size = 0;
			user_buf_hndl->total_filled_size = 0;
		}
		spin_unlock(&user_buf_hndl->lock);

		if (!user_buf_hndl->is_chunked && carry > 0) {
			spin_lock_irqsave(&priv->rx_lock, flags);
			q->rx_pending_carry = carry;
			spin_unlock_irqrestore(&priv->rx_lock, flags);
		}

		spin_lock_irqsave(&priv->rx_lock, flags);
		if (q->prearm_posted) {
			prearm_buf = q->prearm_user_buf;
			q->prearm_user_buf = NULL;
			prearm_already_posted = true;
			q->prearm_posted = false;
			if (prearm_buf)
				q->user_buf = prearm_buf;
		} else if (desc_ring->next_to_use == desc_ring->next_to_clean) {
			prearm_buf = q->prearm_user_buf;
			q->prearm_user_buf = NULL;
			if (prearm_buf)
				q->user_buf = prearm_buf;
		}
		spin_unlock_irqrestore(&priv->rx_lock, flags);

		if (prearm_buf && !prearm_already_posted)
			iwfg_rx_roll(priv, qid, q->stream_id);

		wake_up_interruptible(&user_buf_hndl->waitq);
		
		// dev_dbg(&priv->pdev->dev,
		// 	"--rx_poll: Buffer complete - qid %d, cur_accu_len %d, cur_filled_size reset to %zu, size_to_fill = %zu, total_accu_len = %d, total_filled_size = %zu", 
		// 	qid, cur_accu_len, user_buf_hndl->cur_filled_size, user_buf_hndl->size_to_fill, total_accu_len, user_buf_hndl->total_filled_size);
		
	} else if (cur_accu_len >= user_buf_hndl->size_to_fill) { // specific for chunked buffers
		
		// Buffer is complete, mark it as ready for userspace
		spin_lock(&user_buf_hndl->lock);
		atomic_set(&user_buf_hndl->ready_to_use, 1);
		user_buf_hndl->compl_count++;
		user_buf_hndl->req_compls--;
		user_buf_hndl->cur_filled_size = cur_accu_len - user_buf_hndl->size_to_fill; // Reset filled size for next use

		if (total_accu_len < user_buf_hndl->usr_buf_size) {
			user_buf_hndl->total_filled_size = total_accu_len;
		}
		spin_unlock(&user_buf_hndl->lock);

		wake_up_interruptible(&user_buf_hndl->waitq);		
		// dev_info(&priv->pdev->dev,
		// 	"rx_poll: Buffer complete - qid %d, cur_accu_len %d, cur_filled_size reset to %zu, size_to_fill = %zu, total_accu_len = %d, total_filled_size = %zu", 
		// 	qid, cur_accu_len, user_buf_hndl->cur_filled_size, user_buf_hndl->size_to_fill, total_accu_len, user_buf_hndl->total_filled_size);
	} else {
		spin_lock(&user_buf_hndl->lock);
		user_buf_hndl->cur_filled_size = cur_accu_len;
		user_buf_hndl->total_filled_size = total_accu_len;
		spin_unlock(&user_buf_hndl->lock);

		// dev_info(&priv->pdev->dev,
		// 	"rx_poll: Buffer complete - qid %d, cur_accu_len %d, cur_filled_size reset to %zu, size_to_fill = %zu, total_accu_len = %d, total_filled_size = %zu, num_iter = %d", 
		// 	qid, cur_accu_len, user_buf_hndl->cur_filled_size, user_buf_hndl->size_to_fill, total_accu_len, user_buf_hndl->total_filled_size, num_iter);
	}

	// dev_dbg(&priv->pdev->dev, "----Interrupt BH return-----");
	return;
}

void iwfg_clear_tx_queue(struct iwfg_private *priv, u16 qid)
{
    struct iwfg_qdma_queue *q = priv->queue[qid];
    struct iwfg_ring *ring;
    u32 size, real_count;

    if (!q)
        return;

    /*
     * TEARDOWN ORDERING FIX (second-run CPU hang):
     * The old order was: invalidate HW ctxt -> free ring -> kfree(q),
     * with NO cancellation of q->intr_work. A final H2C completion IRQ
     * (or one re-armed by iwfg_set_tx_head(irq_arm=1) at the end of
     * iwfg_tx_clean) could leave iwfg_tx_clean queued/running when the
     * ring and q were freed. It then unpacked a writeback stat from
     * freed memory and spun in "while (wb.cidx != next_to_clean)" under
     * spin_lock_irqsave -> infinite loop with IRQs off -> hard lockup
     * on the next open/close cycle. Safe order:
     *   1) invalidate the QDMA contexts (engine stops; no new IRQs for
     *      this qid),
     *   2) unpublish priv->queue[qid] so a stale IRQ that already fired
     *      sees NULL in iwfg_q_handler and bails,
     *   3) cancel_work_sync() any tx_clean already queued/executing
     *      (the queue memory is still valid at this point, so a last
     *      run is harmless),
     *   4) only now free the ring and the queue struct.
     * The current_work() guard exists because the C2H fifo-overflow
     * recovery path in iwfg_rx_poll() calls the clear functions from
     * inside a queue work item; cancel_work_sync() on one's own work
     * item self-deadlocks.
     */
    iwfg_qdma_clear_tx_queue(priv->hw.qdma, qid);

    WRITE_ONCE(priv->queue[qid], NULL);

    if (current_work() != &q->intr_work)
        cancel_work_sync(&q->intr_work);

    /* let any in-flight descriptor fetch the engine issued before the
     * context invalidation land before the ring memory is returned */
    udelay(100);

    // Free TX descriptor ring
    ring = &q->desc_ring;
    real_count = ring->count - 1;
    size = QDMA_H2C_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
    size = ALIGN(size, PAGE_SIZE);

    if (ring->desc)
        dma_free_coherent(&priv->pdev->dev, size, ring->desc, ring->dma_addr);

    // Free TX buffer structure
    kfree(q);
}

/**
 * map_sg_to_tx_descs - Map scatter-gather list to TX descriptors
 * @priv: private device structure
 * @user_buf_hndl: user buffer handle containing the SG list
 * @desc_len: dma length of each descriptor
 * 
 * This function creats the TX descriptor list for a given user buffer's SG-list.
 * 
 * Returns: Number or descriptors on success, 0 on failure
 */
int iwfg_map_sg_to_tx_descs(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf_hndl, u32 desc_len)
{
	struct scatterlist *sg;
	int i;
	int rv = 0;
	u16 tx_desc_count = 0;

	for_each_sg(user_buf_hndl->sgl, sg, user_buf_hndl->sg_mapped_count, i) {
		dma_addr_t dma_addr = sg_dma_address(sg);
		unsigned int len = sg_dma_len(sg);
		
		// dev_info(&priv->pdev->dev, "sg[%d] dma_addr: 0x%llx, len: %u",
		// 	i, (unsigned long long)dma_addr, len);
		
		if (len > desc_len) {
			unsigned int offset = 0;
			while (offset < len) {
				unsigned int this_chunk = min(desc_len, len - offset);
				struct iwfg_tx_desc *tx_desc = kzalloc(sizeof(struct iwfg_tx_desc), GFP_KERNEL);
				if (!tx_desc) {
					dev_err(&priv->pdev->dev, "Failed to allocate TX descriptor");
					rv = -ENOMEM;
					return 0;
				}

				// desc.dst_addr = dma_addr + offset;
				tx_desc->desc.src_addr = dma_addr + offset;
				tx_desc->desc.len = this_chunk;
				tx_desc->desc.metadata_len = this_chunk; // metadata pack length for user logic
				tx_desc->desc.sof = 0;

				list_add_tail(&tx_desc->list, &user_buf_hndl->tx_desc_list);
				tx_desc_count++;
				offset += this_chunk;

				// dev_info(&priv->pdev->dev, "Tx Mapped SG: dma_addr=0x%llx, len=%u",
				// 	(unsigned long long)tx_desc->desc.src_addr,
				// 	this_chunk);
			}
		} else {
			struct iwfg_tx_desc *tx_desc = kzalloc(sizeof(struct iwfg_tx_desc), GFP_KERNEL);
			if (!tx_desc) {
				dev_err(&priv->pdev->dev, "Failed to allocate TX descriptor");
				rv = -ENOMEM;
				return 0;
			}

			// desc.dst_addr = dma_addr ;
			tx_desc->desc.src_addr = dma_addr;
			tx_desc->desc.len = len;
			tx_desc->desc.metadata_len = len; // metadata pack length for user logic
			tx_desc->desc.sof = 0;

			list_add_tail(&tx_desc->list, &user_buf_hndl->tx_desc_list);
			tx_desc_count++;

			// dev_info(&priv->pdev->dev, "Tx Mapped SG: dma_addr=0x%llx, len=%u",
			// 		(unsigned long long)tx_desc->desc.src_addr,
			// 		len);
		}
	}

	if (!list_empty(&user_buf_hndl->tx_desc_list)) {
		struct iwfg_tx_desc *first_entry;
		
		first_entry = list_first_entry(&user_buf_hndl->tx_desc_list,
									struct iwfg_tx_desc,
									list);
		first_entry->desc.sof = 1;
	}

	return tx_desc_count;
}

void iwfg_release_tx_descs(struct iwfg_user_page_buf *user_buf_hndl)
{
	if (!list_empty(&user_buf_hndl->tx_desc_list)) {
		struct iwfg_tx_desc *tx_desc, *tmp;
		list_for_each_entry_safe(tx_desc, tmp, &user_buf_hndl->tx_desc_list, list) {
			list_del(&tx_desc->list);
			kfree(tx_desc);
		}
	}
}

int iwfg_init_tx_queue(struct iwfg_private *priv, u16 qid)
{
	const u8 rngcnt_idx = 15;
	struct iwfg_qdma_queue *q;
	struct iwfg_ring *ring;
	struct iwfg_qdma_h2c_param param;
	u16 vid;
	u32 size, real_count;
	int rv;
	bool debug = 0;

	if (priv->queue[qid]) {
		if (debug)
			dev_dbg(&priv->pdev->dev, "Re-initializing TX queue %d", qid);
		iwfg_clear_tx_queue(priv, qid);
	}

	q = kzalloc(sizeof(struct iwfg_qdma_queue), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	/* evenly assign to TX queues available vectors */
	vid = (qid % priv->num_q_vectors) + 2; // +2 to skip error and user vectors

	q->vector = priv->q_vector[vid - 2]; /* storage is queue-indexed (vid-2), see iwfg_init_q_vector() */
	q->qid = qid;

	ring = &q->desc_ring;
	ring->count = iwfg_ring_count(rngcnt_idx);
	real_count = ring->count - 1;

	/* allocate DMA memory for TX descriptor ring */
	size = QDMA_H2C_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);
	ring->desc = dma_alloc_coherent(&priv->pdev->dev, size, &ring->dma_addr,
					GFP_KERNEL);
	if (!ring->desc) {
		rv = -ENOMEM;
		goto clear_tx_queue;
	}
	memset(ring->desc, 0, size);
	ring->wb = ring->desc + QDMA_H2C_ST_DESC_SIZE * real_count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	ring->color = 0;

	dev_dbg(&priv->pdev->dev, "TX queue %d, ring count %d, ring size %d, real_count %d", 
		    qid, ring->count, size, real_count);

	q->priv = priv;
	q->direction = DMA_TO_DEVICE;

	INIT_WORK(&q->intr_work, iwfg_tx_clean);

	/* initialize QDMA H2C queue */
	param.rngcnt_idx = rngcnt_idx;
	param.dma_addr = ring->dma_addr;
	param.vid = vid;
	rv = iwfg_qdma_init_tx_queue(priv->hw.qdma, qid, &param);
	if (rv < 0)
		goto clear_tx_queue;

	/*
	 * A previous run that was killed mid-transfer (Ctrl-C during
	 * iwfg_tx_roll's wait) can leave h2c_busy stuck at 1, or a fifo
	 * overflow recovery can leave it at 2. Either would make every
	 * transfer on this fresh queue fail with -EBUSY/-ETIMEDOUT. A new
	 * TX queue starts idle by definition.
	 */
	atomic_set(&priv->h2c_busy, 0);

	priv->queue[qid] = q;
	// user_buf_hndl->qdma_mapped = true;
	return 0;

clear_tx_queue:
	iwfg_clear_tx_queue(priv, qid);
	return rv;
}

void iwfg_clear_rx_queue(struct iwfg_private *priv, u16 qid)
{
    struct iwfg_qdma_queue *q = priv->queue[qid];
    struct iwfg_ring *ring;
    u32 size, real_count;

    if (!q)
        return;

    /* Same teardown ordering as iwfg_clear_tx_queue() - see the comment
     * there. iwfg_rx_poll() must never run on a freed queue/ring. */
    iwfg_qdma_clear_rx_queue(priv->hw.qdma, qid);

    WRITE_ONCE(priv->queue[qid], NULL);

    if (current_work() != &q->intr_work)
        cancel_work_sync(&q->intr_work);

    udelay(100);

    // Free RX descriptor ring
    ring = &q->desc_ring;
    real_count = ring->count - 1;
    size = QDMA_C2H_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
    size = ALIGN(size, PAGE_SIZE);

    if (ring->desc)
        dma_free_coherent(&priv->pdev->dev, size, ring->desc, ring->dma_addr);

    // Free completion ring
    ring = &q->cmpl_ring;
    real_count = ring->count - 1;
    size = QDMA_C2H_CMPL_SIZE * real_count + QDMA_C2H_CMPL_STAT_SIZE;
    size = ALIGN(size, PAGE_SIZE);

    if (ring->desc)
        dma_free_coherent(&priv->pdev->dev, size, ring->desc, ring->dma_addr);

    kfree(q);
}

/**
 * iwfg_update_rx_bufsz_idx - Update RX buffer size index in hardware
 * 
 * @
 * 
 * calculate bufsz_index based on the contiguous DMA allotment and inform FPGA logic
 * 
 * Returns: returns c2h buffer size on success, 0 on failure
 */
u32 iwfg_update_rx_bufsz_idx(struct iwfg_private *priv, u32 dma_len, bool is_audio, int stream_id)
{
	volatile u32 reg_val;
	u8 bufsz_idx = 2; // 512 for audio by default 

	/* update bufsz_index based on the contiguous DMA allotment and inform FPGA logic */
	if (is_audio) {
		reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_AUD_CTRL_REG_OFFSET);
		reg_val = FIELD_SET(IWFG_AUD_CTRL_QDMA_BUFSZ_IDX_MASK, bufsz_idx);
		iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_AUD_CTRL_REG_OFFSET, reg_val);
	} else {
		bufsz_idx = iwfg_get_c2h_bufsz_idx(dma_len);
		// bufsz_idx = 4;
		reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET);
		reg_val |= FIELD_SET(IWFG_C2H_CTRL_QDMA_C2H_BUFSZ_IDX_MASK, bufsz_idx);
		iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET, reg_val);
	}

	return iwfg_get_c2h_bufsz(bufsz_idx);
}

/**
 * map_sg_to_rx_descs - Map scatter-gather list to RX descriptors
 * @priv: private device structure
 * @user_buf_hndl: user buffer handle containing the SG list
 * @desc_len: dma length of each descriptor
 * 
 * This function creats the RX descriptor list for a given user buffer's SG-list.
 * 
 * Returns: Number or descriptors on success, 0 on failure
 */
int iwfg_map_sg_to_rx_descs(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf_hndl, u32 desc_len)
{
	struct scatterlist *sg;
	int i;
	int rv = 0;
	u16 rx_desc_count = 0;
	unsigned int len = 0;

	for_each_sg(user_buf_hndl->sgl, sg, user_buf_hndl->sg_mapped_count, i) {
		dma_addr_t dma_addr = sg_dma_address(sg);
		len = sg_dma_len(sg);
		
		// dev_info(&priv->pdev->dev, "sg[%d] dma_addr: 0x%llx, len: %u",
		// 	i, (unsigned long long)dma_addr, len);
		
		if (len > desc_len) {
			unsigned int offset = 0;
			while (offset < len) {
				unsigned int this_chunk = min(desc_len, len - offset);
				struct iwfg_rx_desc *rx_desc = kzalloc(sizeof(struct iwfg_rx_desc), GFP_KERNEL);
				if (!rx_desc) {
					dev_err(&priv->pdev->dev, "Failed to allocate RX descriptor");
					rv = -ENOMEM;
					return 0;
				}

				rx_desc->desc.dst_addr = dma_addr + offset;
				list_add_tail(&rx_desc->list, &user_buf_hndl->rx_desc_list);
				rx_desc_count++;
				offset += this_chunk;

				// dev_info(&priv->pdev->dev, "Rx Mapped SG: dma_addr=0x%llx, len=%u",
				// 	(unsigned long long)rx_desc->desc.dst_addr,
				// 	this_chunk);
			}
		} else {
			struct iwfg_rx_desc *rx_desc = kzalloc(sizeof(struct iwfg_rx_desc), GFP_KERNEL);
			if (!rx_desc) {
				dev_err(&priv->pdev->dev, "Failed to allocate RX descriptor");
				rv = -ENOMEM;
				return 0;
			}

			rx_desc->desc.dst_addr = dma_addr;
			list_add_tail(&rx_desc->list, &user_buf_hndl->rx_desc_list);
			rx_desc_count++;

			// dev_info(&priv->pdev->dev, "Rx Mapped SG: dma_addr=0x%llx, len=%u",
			// 		(unsigned long long)rx_desc->desc.dst_addr,
			// 		len);
		}
	}

	return rx_desc_count;
}

void iwfg_release_rx_descs(struct iwfg_user_page_buf *user_buf_hndl)
{
	// Free RX descriptor list
	if (!list_empty(&user_buf_hndl->rx_desc_list)) {
		struct iwfg_rx_desc *rx_desc, *tmp;
		list_for_each_entry_safe(rx_desc, tmp, &user_buf_hndl->rx_desc_list, list) {
			list_del(&rx_desc->list);
			kfree(rx_desc);
		}
	}
}

int iwfg_init_rx_queue(struct iwfg_private *priv, u16 qid)
{
	u8 bufsz_idx = 15;
	const u8 desc_rngcnt_idx = 15;
	//const u8 cmpl_rngcnt_idx = 15;
	const u8 cmpl_rngcnt_idx = 15;
	// struct net_device *dev = priv->netdev;
	struct iwfg_qdma_queue *q;
	struct iwfg_ring *ring;
	struct iwfg_qdma_c2h_param param;
	// struct scatterlist *sg;
	u16 vid;
	u32 size, real_count;
	int rv;

	if (priv->queue[qid]) {
		dev_dbg(&priv->pdev->dev, "Re-initializing RX queue %d", qid);
		iwfg_clear_rx_queue(priv, qid);
	}

	q = kzalloc(sizeof(struct iwfg_qdma_queue), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	/* evenly assign to RX queues available vectors */
	vid = (qid % priv->num_q_vectors) + 2; // +2 to skip error and user vectors

	// q->netdev = dev;
	q->vector = priv->q_vector[vid - 2]; /* storage is queue-indexed (vid-2), see iwfg_init_q_vector() */
	q->qid = qid;

	// q->xdp_prog = priv->xdp_prog;

	/* allocate DMA memory for RX descriptor ring */
	ring = &q->desc_ring;
	ring->count = iwfg_ring_count(desc_rngcnt_idx);
	real_count = ring->count - 1;

	size = QDMA_C2H_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);
	ring->desc = dma_alloc_coherent(&priv->pdev->dev, size, &ring->dma_addr,
					GFP_KERNEL);
	if (!ring->desc) {
		rv = -ENOMEM;
		goto clear_rx_queue;
	}
	memset(ring->desc, 0, size);
	ring->wb = ring->desc + QDMA_C2H_ST_DESC_SIZE * real_count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	ring->color = 0;

	dev_info(&priv->pdev->dev, "RX queue %d, ring count %d, ring size %d, real_count %d", 
		    qid, ring->count, size, real_count);

	q->priv = priv;
	q->direction = DMA_FROM_DEVICE;
	q->prearm_user_buf = NULL;
	q->prearm_posted = false;
	q->stream_id = 0;
	q->rx_pending_carry = 0;

	/* allocate DMA memory for completion ring */
	ring = &q->cmpl_ring;
	ring->count = iwfg_ring_count(cmpl_rngcnt_idx);
	real_count = ring->count - 1;

	size = QDMA_C2H_CMPL_SIZE * real_count + QDMA_C2H_CMPL_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);
	ring->desc = dma_alloc_coherent(&priv->pdev->dev, size, &ring->dma_addr,
					GFP_KERNEL);
	if (!ring->desc) {
		rv = -ENOMEM;
		goto clear_rx_queue;
	}
	memset(ring->desc, 0, size);
	ring->wb = ring->desc + QDMA_C2H_CMPL_SIZE * real_count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	ring->color = 1;

    INIT_WORK(&q->intr_work, iwfg_rx_poll);

	/* Init wait queue */
	init_waitqueue_head(&q->vsync_waitq);
	atomic_set(&q->vsync_event, 0);

	/* initialize QDMA C2H queue */
	param.bufsz_idx = bufsz_idx;
	param.desc_rngcnt_idx = desc_rngcnt_idx;
	param.cmpl_rngcnt_idx = cmpl_rngcnt_idx;
	param.cmpl_desc_sz = 0;
	param.desc_dma_addr = q->desc_ring.dma_addr;
	param.cmpl_dma_addr = q->cmpl_ring.dma_addr;
	param.vid = vid;
	dev_dbg(&priv->pdev->dev,
		"bufsz_idx %u, desc_rngcnt_idx %u, cmpl_rngcnt_idx %u, desc_dma_addr 0x%llx, cmpl_dma_addr 0x%llx, vid %d",
		bufsz_idx, desc_rngcnt_idx, cmpl_rngcnt_idx,
		q->desc_ring.dma_addr, q->cmpl_ring.dma_addr, vid);

	rv = iwfg_qdma_init_rx_queue(priv->hw.qdma, qid, &param);
	if (rv < 0)
		goto clear_rx_queue;

	/* fill RX descriptor ring with a few descriptors */
	// q->desc_ring.next_to_use = IWFG_RX_DESC_STEP;
	// q->desc_ring.next_to_use = user_buf_hndl->q_desc_count;
	iwfg_set_rx_head(priv->hw.qdma, qid, 0);
	iwfg_set_completion_tail(priv->hw.qdma, qid, 0, 1);

	priv->queue[qid] = q;
	// user_buf_hndl->qdma_mapped = true;
	return 0;

clear_rx_queue:
	iwfg_clear_rx_queue(priv, qid);
	return rv;
}

// int iwfg_init_queue_resource(struct iwfg_private *priv)
// {
// 	int i = 0,rv = 0;

// 	// Iterate over each user DMA buffer in the list
// 	for (i = 0; i < IWFG_MAX_QUEUES; i++) {
// 		if (i % 2 == 0) { // equally divide ques between RX and TX
// 			rv = iwfg_init_rx_queue(priv, i);
// 			priv->num_rx_queues++;
// 		} else {
// 			rv = iwfg_init_tx_queue(priv, i);
// 			priv->num_tx_queues++;
// 		}
// 		if (rv < 0) {
// 			dev_err(&priv->pdev->dev, "iwfg_init_queue_resource %d, err = %d", i, rv);
// 			goto clear_queue_resource;
// 		}
// 	}

// 	return 0;

// clear_queue_resource:
// 	iwfg_clear_queue_resource(priv);
// 	return rv;
// }

// int iwfg_clear_queue_resource(struct iwfg_private *priv)
// {
// 	int i;

// 	for (i = 0; i < IWFG_MAX_QUEUES; i++) {
// 		if (i % 2 == 0)
// 			iwfg_clear_rx_queue(priv, i);
// 		else
// 			iwfg_clear_tx_queue(priv, i);
// 	}

// 	priv->num_rx_queues = 0;
// 	priv->num_tx_queues = 0;

// 	return 0;
// }

static int iwfg_dma_map_sg(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf_hndl)
{
    int count;

    if (!priv || !user_buf_hndl || !user_buf_hndl->sgl || !user_buf_hndl->locked)
        return -EINVAL;

    if (!user_buf_hndl->sg_mapped) {
        count = dma_map_sg(&priv->pdev->dev,
                           user_buf_hndl->sgl,
                           user_buf_hndl->sg_nents,
                           DMA_BIDIRECTIONAL);

        if (count == 0) {
            dev_err(&priv->pdev->dev, "dma_map_sg failed for %d segments\n", user_buf_hndl->sg_nents);
            return -EIO;
        }
        // CRITICAL: Store the mapped count separately - DO NOT overwrite sg_nents!
        // sg_nents must remain the original count for proper cleanup
        user_buf_hndl->sg_mapped_count = count;
        user_buf_hndl->sg_mapped = true;
        user_buf_hndl->sg_host = false;
        dev_info(&priv->pdev->dev, "dma_map_sg: original %d segments, mapped to %d DMA segments\n", 
                 user_buf_hndl->sg_nents, count);
    }

    return 0;
}

static void iwfg_dma_sync_sg_for_cpu(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf_hndl)
{
    if (!priv || !user_buf_hndl || !user_buf_hndl->sgl || !user_buf_hndl->sg_mapped)
        return;

//     if (!user_buf_hndl->sg_host) {
        dma_sync_sg_for_cpu(&priv->pdev->dev,
                            user_buf_hndl->sgl,
                            user_buf_hndl->sg_mapped_count,  // Use mapped count, not original count
                            DMA_BIDIRECTIONAL);
        user_buf_hndl->sg_host = true;
        // Debug print disabled for performance
        // dev_info(&priv->pdev->dev, "dma_sync_sg_for_cpu synced %d segments\n", user_buf_hndl->sg_mapped_count);
//     }
}

static int iwfg_dma_pin_user_pages(struct iwfg_private *priv, struct iwfg_user_page_buf *dma_page_buf,
                            unsigned long user_addr, size_t size, int direction)
{
    int num_pages, num_pinned = 0, page_offset, count, i, ret = 0;
    bool write = (direction == DMA_BIDIRECTIONAL) || (direction == DMA_FROM_DEVICE);

    if (!dma_page_buf || !dma_page_buf->pgs || !dma_page_buf->sgl || size == 0) {
		dev_err(&priv->pdev->dev, "Invalid parameters for iwfg_dma_pin_user_pages");
		return -EINVAL;
	}

    // Calculate number of pages
    page_offset = user_addr & ~PAGE_MASK;
    // num_pages = DIV_ROUND_UP(page_offset + size, PAGE_SIZE);
	num_pages = (int)(((user_addr & ~PAGE_MASK) + size + ~PAGE_MASK) >> PAGE_SHIFT);

    if (num_pages > dma_page_buf->num_pages) {
		dev_err(&priv->pdev->dev, "Number of pages %d exceeds allocated %d\n", num_pages, dma_page_buf->num_pages);
		return -EINVAL;
	}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0))
    mmap_read_lock(current->mm);
#else
    down_read(&current->mm->mmap_sem);
#endif

// #if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,6,0))
//     num_pinned = get_user_pages(user_addr, num_pages,
// #if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0))
//         write ? FOLL_WRITE : 0,
// #else
//         write,
//         0,
// #endif
//         dma_page_buf->pgs
// #if (LINUX_VERSION_CODE < KERNEL_VERSION(6,8,0))
// 	, NULL
// #endif
// );
// #else
//     num_pinned = get_user_pages(
//         current, current->mm, user_addr, num_pages,
//         write, 0, dma_page_buf->pgs, NULL);
// #endif

	num_pinned = get_user_pages_fast(user_addr, num_pages, write ? FOLL_WRITE : 0, dma_page_buf->pgs);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0))
    mmap_read_unlock(current->mm);
#else
    up_read(&current->mm->mmap_sem);
#endif

    if (num_pinned != num_pages) {
		dev_err(&priv->pdev->dev, "Failed to pin user pages: requested %d, pinned %d\n", num_pages, num_pinned);
        ret = -EPERM;
        goto out_unmap;
    }

    // Initialize scatterlist
    sg_init_table(dma_page_buf->sgl, num_pages);

    count = size;
    if (num_pages > 1) {
        unsigned int first_page_len = PAGE_SIZE - page_offset;
        sg_set_page(&dma_page_buf->sgl[0], dma_page_buf->pgs[0], first_page_len, page_offset);
        count -= first_page_len;
		
        for (i = 1; i < num_pages; i++) {
            unsigned int this_page_len = (count < PAGE_SIZE) ? count : PAGE_SIZE;
            sg_set_page(&dma_page_buf->sgl[i], dma_page_buf->pgs[i], this_page_len, 0);
            count -= this_page_len;
        }
    } else {
        sg_set_page(&dma_page_buf->sgl[0], dma_page_buf->pgs[0], count, page_offset);
    }

    dma_page_buf->user_addr = (void *)user_addr;
    dma_page_buf->usr_buf_size = size;
//     dma_page_buf->direction = direction;
    dma_page_buf->num_pages = num_pages;
    dma_page_buf->sg_nents = num_pages;
    dma_page_buf->locked = true;

    return 0;

out_unmap:
    for (i = 0; i < num_pinned; i++) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,5,0))
        put_page(dma_page_buf->pgs[i]);
#else
        page_cache_release(dma_page_buf->pgs[i]);
#endif
    }
    return ret;
}

struct iwfg_user_page_buf* iwfg_get_user_buf_by_addr(struct iwfg_private *priv, struct iwfg_cdev *icdev, void *user_buf, size_t size)
{
	struct iwfg_user_page_buf *user_buf_hndl = NULL;

	if (!priv || !icdev || !user_buf || size <= 0) {
		dev_err(&priv->pdev->dev, "Invalid parameters for iwfg_get_user_buf_by_addr");
		return NULL;
	}

	if (!list_empty(&icdev->dma_buf_list)) {
		list_for_each_entry(user_buf_hndl, &icdev->dma_buf_list, list) {
			if (user_buf_hndl->user_addr == user_buf &&
				user_buf_hndl->usr_buf_size == size) {
				dev_dbg(&priv->pdev->dev, "Found user buffer 0x%p of size %zu", 
						user_buf, size);
				return user_buf_hndl; // Found
			}
		}
	}

	dev_dbg(&priv->pdev->dev, "User buffer 0x%p of size %zu not found", 
			user_buf, size);
	return NULL; // Not found
}

/**
 * iwfg_get_user_buf_by_index - Get user buffer by index
 * @priv: device private data
 * @index: buffer index in the list
 * 
 * Returns: pointer to user buffer handle, or NULL if not found
 * 
 * This function is useful for DMA_ALLOC and DMA_IMPORT buffers
 * which don't have a user virtual address.
 */
struct iwfg_user_page_buf* iwfg_get_user_buf_by_index(struct iwfg_private *priv, struct iwfg_cdev *icdev, u32 index)
{
	struct iwfg_user_page_buf *user_buf_hndl = NULL;
	u32 count = 0;

	if (!priv || !icdev) {
		return NULL;
	}

	list_for_each_entry(user_buf_hndl, &icdev->dma_buf_list, list) {
		if (count == index) {
			return user_buf_hndl;
		}
		count++;
	}

	return NULL; // Not found
}

/**
 * iwfg_free_user_buf_hndl - unwind a single, not-yet-listed buffer handle
 * @priv: device private data
 * @user_buf_hndl: handle created inside iwfg_add_user_buf() that failed
 *                 partway through setup (never added to icdev->dma_buf_list)
 *
 * BUGFIX (H2C bring-up): iwfg_add_user_buf()'s error path used to call
 * iwfg_delete_user_buf(), which walks icdev->dma_buf_list and destroys
 * EVERY registered buffer on the cdev - including perfectly valid ones -
 * and force-clears stream_active. In practice this meant one bad buffer
 * registration (e.g. a garbage prearm pointer from userspace) would
 * unpin/unmap the caller's good buffer mid-transfer ("DMA unmapped 256
 * segments..." in dmesg) and silently stop the stream. This helper
 * releases only the resources owned by the failing handle, in reverse
 * order of acquisition, and touches nothing else.
 */
static void iwfg_free_user_buf_hndl(struct iwfg_private *priv,
				    struct iwfg_user_page_buf *user_buf_hndl)
{
	unsigned int i;

	if (!user_buf_hndl)
		return;

	/* Descriptor lists (safe on empty/partial lists) */
	iwfg_release_tx_descs(user_buf_hndl);
	iwfg_release_rx_descs(user_buf_hndl);

	/* DMA mapping */
	if (user_buf_hndl->sg_mapped) {
		dma_unmap_sg(&priv->pdev->dev,
			     user_buf_hndl->sgl,
			     user_buf_hndl->sg_nents,
			     DMA_BIDIRECTIONAL);
		user_buf_hndl->sg_mapped = false;
	}

	/* Pinned user pages (only if pinning completed; on a pin *failure*
	 * iwfg_dma_pin_user_pages() has already put every page itself) */
	if (user_buf_hndl->locked && user_buf_hndl->pgs) {
		for (i = 0; i < user_buf_hndl->num_pages; i++) {
			if (user_buf_hndl->pgs[i])
				put_page(user_buf_hndl->pgs[i]);
		}
		user_buf_hndl->locked = false;
	}

	vfree(user_buf_hndl->sgl);
	vfree(user_buf_hndl->pgs);
	kfree(user_buf_hndl);
}

struct iwfg_user_page_buf* iwfg_add_user_buf(struct iwfg_private *priv, struct iwfg_cdev *icdev, void *user_buf, 
	size_t size, bool is_chunked, size_t chunk_size, bool is_audio)
{
	struct iwfg_user_page_buf *user_buf_hndl = NULL;
	u32 c2h_buf_size;
	u32 rx_desc_count;
	u32 h2c_buf_size;
	u32 tx_desc_count;
	int stream_id = icdev->stream_id;

	if (!priv || !icdev || !user_buf || size <= 0) {
		dev_err(&priv->pdev->dev, "Invalid parameters for iwfg_add_user_buf");
		return NULL;
	}

	// Check if the user_buf_req is already added
	if (!list_empty(&icdev->dma_buf_list)) {
		list_for_each_entry(user_buf_hndl, &icdev->dma_buf_list, list) {
			if (user_buf_hndl->user_addr == user_buf &&
				user_buf_hndl->usr_buf_size == size) {
				dev_dbg(&priv->pdev->dev, "User buffer 0x%p of size %zu already added", 
						user_buf, size);
				return user_buf_hndl; // Already added
			}
		}
	}

	user_buf_hndl = kzalloc(sizeof(*user_buf_hndl), GFP_KERNEL);
	if (!user_buf_hndl) {
		dev_err(&priv->pdev->dev, "Failed to allocate iwfg_dma_page_buf\n");
		return NULL;
	}

	user_buf_hndl->pgs = vzalloc((size / PAGE_SIZE + 2) * sizeof(struct page *));
	if (!user_buf_hndl->pgs) {
		dev_err(&priv->pdev->dev, "Failed to allocate page pointer array\n");
		kfree(user_buf_hndl);
		return NULL;
	}

	user_buf_hndl->sgl = vzalloc((size / PAGE_SIZE + 2) * sizeof(struct scatterlist));
	if (!user_buf_hndl->sgl) {
		dev_err(&priv->pdev->dev, "Failed to allocate scatterlist\n");
		vfree(user_buf_hndl->pgs);
		kfree(user_buf_hndl);
		return NULL;
	}

	user_buf_hndl->num_pages = size / PAGE_SIZE + 2; // +2 for safety
	user_buf_hndl->sg_nents = 0;
	user_buf_hndl->sg_mapped_count = 0;  // Initialize mapped count
	user_buf_hndl->sg_mapped = false;
	user_buf_hndl->sg_host = false;
	user_buf_hndl->cur_filled_size = 0;
	user_buf_hndl->total_filled_size = 0;
	user_buf_hndl->compl_count = 0;
	user_buf_hndl->is_audio = is_audio;
	user_buf_hndl->priv = priv;  // Set back-reference to device
	
	// Initialize DMA-BUF related fields
	user_buf_hndl->dmabuf = NULL;
	user_buf_hndl->exported = false;
	
	// Set buffer type to USERPTR (user pointer mode)
	user_buf_hndl->buf_type = IWFG_BUF_TYPE_USERPTR;
	user_buf_hndl->coherent_buf = NULL;
	user_buf_hndl->coherent_dma_addr = 0;
	user_buf_hndl->import_dmabuf = NULL;
	user_buf_hndl->import_attach = NULL;
	user_buf_hndl->import_sgt = NULL;

	if (is_chunked) {
		user_buf_hndl->size_to_fill = chunk_size;
		user_buf_hndl->is_chunked = true;
	} else {
		user_buf_hndl->size_to_fill = size;
		user_buf_hndl->is_chunked = false;
	}

	user_buf_hndl->req_compls = size / user_buf_hndl->size_to_fill; // Number of completions expected

	// Initialize atomic variables and spinlock
	atomic_set(&user_buf_hndl->ready_to_use, 0);
	spin_lock_init(&user_buf_hndl->lock);
	INIT_LIST_HEAD(&user_buf_hndl->tx_desc_list);
	INIT_LIST_HEAD(&user_buf_hndl->rx_desc_list);

	// Pin user pages and build scatterlist for this buffer
	// Replace user_buf_req->user_ptrs[i] with the actual user address for this buffer
	if (iwfg_dma_pin_user_pages(priv, user_buf_hndl, (unsigned long)user_buf,
					size, DMA_BIDIRECTIONAL) < 0) {
		dev_err(&priv->pdev->dev, "Failed to pin user pages for buf 0x%p\n", user_buf);
		/* pin failure releases its own pages; free only this handle */
		vfree(user_buf_hndl->sgl);
		vfree(user_buf_hndl->pgs);
		kfree(user_buf_hndl);
		return NULL;
	}

	// Map the user buffer for DMA
	if (iwfg_dma_map_sg(priv, user_buf_hndl) < 0) {
		dev_err(&priv->pdev->dev, "Failed to dma map sg\n");
		/*
		 * Pages are pinned at this point; the old code leaked them
		 * (it freed sgl/pgs without put_page). The helper unpins
		 * them and frees only this handle.
		 */
		iwfg_free_user_buf_hndl(priv, user_buf_hndl);
		return NULL;
	}
		
	// Initial sync to ensure CPU coherency
	iwfg_dma_sync_sg_for_cpu(priv, user_buf_hndl);


	rx_desc_count = 0;
	tx_desc_count = 0;

	/*
	 * NOTE (H2C addition): this function used to build BOTH the RX and TX
	 * descriptor lists, and unconditionally reprogram the C2H video
	 * buffer-size / audio buffer-size registers, for every buffer added
	 * on ANY cdev. That was harmless while only a C2H cdev existed, but
	 * now that an H2C cdev is created too, doing so would:
	 *   1) waste time/memory building an RX descriptor list that a pure
	 *      TX buffer will never use, and
	 *   2) actively corrupt the *live* C2H stream's IWFG_C2H_TOT_BUFSZ /
	 *      IWFG_C2H_CTRL registers with the size of a transmit buffer,
	 *      since those registers are shared per stream_id and are not
	 *      direction-specific.
	 * Both are now gated on icdev->direction, which is fixed per cdev
	 * (set once in iwfg_cdev_create()) and reflects which hardware path
	 * this buffer is actually going to be used on.
	 */
	if (icdev->direction == DMA_FROM_DEVICE) {
		/* ---- C2H (unchanged from the original implementation) ---- */
		if (is_audio) {
			c2h_buf_size = iwfg_update_rx_bufsz_idx(priv,
							      sg_dma_len(user_buf_hndl->sgl),
							      true,
							      stream_id);
		} else {
			/*
			 * QDMA C2H queue is initialized with bufsz_idx=15 (16KB). Use the same
			 * descriptor span for every userspace buffer on this queue; deriving it
			 * from first SG length can differ per buffer and create periodic gaps.
			 */
			c2h_buf_size = iwfg_get_c2h_bufsz(15);
			iwfg_update_rx_bufsz_idx(priv, c2h_buf_size, false, stream_id);
		}

		dev_info(&priv->pdev->dev, "c2h_buf_size: %u", c2h_buf_size);

		rx_desc_count = iwfg_map_sg_to_rx_descs(priv, user_buf_hndl, c2h_buf_size);
		if (rx_desc_count == 0) {
			goto err_cleanup;
		}

		dev_dbg(&priv->pdev->dev,
			"Mapped %u RX descriptors for buffer size %u",
			rx_desc_count, c2h_buf_size);
	} else {
		/* ---- H2C (new) ---- */
		h2c_buf_size = min_t(u32, sg_dma_len(user_buf_hndl->sgl), 16384);

		dev_info(&priv->pdev->dev, "h2c_buf_size: %u", h2c_buf_size);

		tx_desc_count = iwfg_map_sg_to_tx_descs(priv, user_buf_hndl, h2c_buf_size);
		if (tx_desc_count == 0) {
			goto err_cleanup;
		}

		dev_dbg(&priv->pdev->dev,
			"Mapped %u TX descriptors for buffer size %u",
			tx_desc_count, h2c_buf_size);
	}

	// Set the number of descriptors
	user_buf_hndl->rx_desc_count = rx_desc_count;
	user_buf_hndl->tx_desc_count = tx_desc_count;

	init_waitqueue_head(&user_buf_hndl->waitq);
	list_add_tail(&user_buf_hndl->list, &icdev->dma_buf_list);

	atomic_set(&priv->h2c_busy, 0);

	// struct iwfg_rx_desc *rx_desc;
	// list_for_each_entry(rx_desc, &user_buf_hndl->rx_desc_list, list) {
	// 	dev_info(&priv->pdev->dev, "RX Desc: dma_addr=0x%llx",
	// 		(unsigned long long)rx_desc->desc.dst_addr);
	// }

	/* C2H-only stream control-plane registers: never touched by an H2C cdev. */
	if (icdev->direction == DMA_FROM_DEVICE) {
		u32 reg_val = 0;
		// write the dma buffer size to audio reg
		if (is_audio && !icdev->is_audio_locked) {
			iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_AUD_BUF_SIZE_REG_OFFSET, size);
			icdev->is_audio_locked = true;
		} else if (!icdev->is_video_locked) {
			reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET);
			reg_val &= ~IWFG_C2H_CTRL_SET_BUFSZ_MASK;
			iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET, reg_val);
			udelay(100);
			iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_C2H_TOT_BUFSZ_OFFSET, size);
			reg_val |= IWFG_C2H_CTRL_SET_BUFSZ_MASK;
			iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET, reg_val);
			udelay(100);
			icdev->is_video_locked = true;
		}
	}

	return user_buf_hndl;

err_cleanup:
	/*
	 * BUGFIX: was iwfg_delete_user_buf(priv, icdev), which destroyed
	 * every buffer on the cdev (including valid, in-use ones) and
	 * cleared stream_active. Unwind only the handle that failed.
	 */
	iwfg_free_user_buf_hndl(priv, user_buf_hndl);
	return NULL;
}

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)

/**
 * iwfg_add_dma_alloc_buf - Add a driver-allocated DMA buffer
 * @priv: device private data
 * @user_buf_req: buffer request from userspace
 * 
 * Allocates contiguous pages from CMA using dma_alloc_from_contiguous(),
 * then creates scatterlist using sg_alloc_table_from_pages().
 * This gives us valid struct page pointers for DMA-BUF export.
 * 
 * Returns: 0 on success, negative error code on failure
 */
int iwfg_add_dma_alloc_buf(struct iwfg_private *priv, struct iwfg_user_buf *user_buf_req)
{
	struct iwfg_user_page_buf *user_buf_hndl = NULL;
	struct sg_table *sgt = NULL;
	struct page *pages;
	u32 c2h_buf_size;
	u32 rx_desc_count;
	u32 h2c_buf_size;
	u32 tx_desc_count;
	size_t aligned_size;
	int num_pages, i, ret, mapped_nents;
	unsigned int order;

	if (!priv || !user_buf_req || user_buf_req->size <= 0) {
		dev_err(&priv->pdev->dev, "Invalid parameters for iwfg_add_dma_alloc_buf");
		return -EINVAL;
	}

	aligned_size = PAGE_ALIGN(user_buf_req->size);
	num_pages = aligned_size / PAGE_SIZE;
	order = get_order(aligned_size);

	user_buf_hndl = kzalloc(sizeof(*user_buf_hndl), GFP_KERNEL);
	if (!user_buf_hndl) {
		dev_err(&priv->pdev->dev, "Failed to allocate iwfg_user_page_buf\n");
		return -ENOMEM;
	}

	// Allocate contiguous pages from CMA
	// This returns struct page * with valid page structures for DMA-BUF export
	pages = dma_alloc_from_contiguous(&priv->pdev->dev, num_pages, order, GFP_KERNEL);
	if (!pages) {
		dev_err(&priv->pdev->dev, "Failed to allocate %d contiguous pages from CMA\n", num_pages);
		goto err_free_hndl;
	}

	// Store the base page for cleanup
	user_buf_hndl->cma_pages = pages;
	user_buf_hndl->coherent_size = aligned_size;

	// Get virtual address for CPU access
	user_buf_hndl->coherent_buf = page_address(pages);
	if (!user_buf_hndl->coherent_buf) {
		dev_err(&priv->pdev->dev, "Failed to get virtual address for CMA pages\n");
		goto err_free_cma;
	}

	// Zero the buffer
	memset(user_buf_hndl->coherent_buf, 0, aligned_size);

	// Allocate page array for DMA-BUF export
	user_buf_hndl->pgs = kcalloc(num_pages, sizeof(struct page *), GFP_KERNEL);
	if (!user_buf_hndl->pgs) {
		dev_err(&priv->pdev->dev, "Failed to allocate page pointer array\n");
		goto err_free_cma;
	}

	// Fill page array - contiguous pages are sequential from base
	for (i = 0; i < num_pages; i++) {
		user_buf_hndl->pgs[i] = pages + i;
	}

	// Allocate sg_table using sg_alloc_table_from_pages()
	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		dev_err(&priv->pdev->dev, "Failed to allocate sg_table\n");
		goto err_free_pages;
	}

	ret = sg_alloc_table_from_pages(sgt, user_buf_hndl->pgs, num_pages,
					0, aligned_size, GFP_KERNEL);
	if (ret) {
		dev_err(&priv->pdev->dev, "sg_alloc_table_from_pages failed: %d\n", ret);
		goto err_free_sgt_struct;
	}

	// DMA map the scatterlist for QDMA device
	mapped_nents = dma_map_sg(&priv->pdev->dev, sgt->sgl, sgt->nents, DMA_BIDIRECTIONAL);
	if (mapped_nents == 0) {
		dev_err(&priv->pdev->dev, "Failed to DMA map scatterlist\n");
		goto err_free_sgt;
	}

	// Store the first DMA address for reference
	user_buf_hndl->coherent_dma_addr = sg_dma_address(sgt->sgl);

	// Store scatterlist info
	user_buf_hndl->sgl = sgt->sgl;
	user_buf_hndl->coherent_sgt = sgt;
	user_buf_hndl->num_pages = num_pages;
	user_buf_hndl->sg_nents = sgt->nents;
	user_buf_hndl->sg_mapped_count = mapped_nents;
	user_buf_hndl->sg_mapped = true;
	user_buf_hndl->sg_host = true;    // CMA memory is CPU accessible
	user_buf_hndl->usr_buf_size = aligned_size;
	user_buf_hndl->user_addr = NULL;  // No user address for driver-allocated buffer
	user_buf_hndl->cur_filled_size = 0;
	user_buf_hndl->total_filled_size = 0;
	user_buf_hndl->compl_count = 0;
	user_buf_hndl->priv = priv;
	user_buf_hndl->locked = true;  // Buffer is ready for use

	// Initial sync to ensure CPU coherency
	iwfg_dma_sync_sg_for_cpu(priv, user_buf_hndl);
	
	// Set buffer type
	user_buf_hndl->buf_type = IWFG_BUF_TYPE_DMA_ALLOC;
	user_buf_hndl->dmabuf = NULL;
	user_buf_hndl->exported = false;
	user_buf_hndl->import_dmabuf = NULL;
	user_buf_hndl->import_attach = NULL;
	user_buf_hndl->import_sgt = NULL;

	if (user_buf_req->is_chunked) {
		user_buf_hndl->size_to_fill = user_buf_req->chunk_size;
		user_buf_hndl->is_chunked = true;
	} else {
		user_buf_hndl->size_to_fill = aligned_size;
		user_buf_hndl->is_chunked = false;
	}

	user_buf_hndl->req_compls = aligned_size / user_buf_hndl->size_to_fill;

	// Initialize atomic variables and spinlock
	atomic_set(&user_buf_hndl->ready_to_use, 0);
	spin_lock_init(&user_buf_hndl->lock);
	INIT_LIST_HEAD(&user_buf_hndl->tx_desc_list);
	INIT_LIST_HEAD(&user_buf_hndl->rx_desc_list);

	// Create DMA descriptor lists for QDMA
	c2h_buf_size = iwfg_update_rx_bufsz_idx(priv, sg_dma_len(sgt->sgl));
	h2c_buf_size = min_t(u32, sg_dma_len(sgt->sgl), 16384);

	rx_desc_count = iwfg_map_sg_to_rx_descs(priv, user_buf_hndl, c2h_buf_size);
	if (rx_desc_count == 0) {
		goto err_unmap_sg;
	}

	tx_desc_count = iwfg_map_sg_to_tx_descs(priv, user_buf_hndl, h2c_buf_size);
	if (tx_desc_count == 0) {
		goto err_free_rx_descs;
	}

	user_buf_hndl->rx_desc_count = rx_desc_count;
	user_buf_hndl->tx_desc_count = tx_desc_count;

	init_waitqueue_head(&user_buf_hndl->waitq);
	list_add_tail(&user_buf_hndl->list, &priv->dma_buf_list);

	// Auto-export as DMA-BUF for DMA_ALLOC buffers
	if (iwfg_dmabuf_export(user_buf_hndl) < 0) {
		dev_warn(&priv->pdev->dev, "Failed to auto-export DMA-BUF, buffer still usable\n");
	}

	atomic_set(&priv->h2c_busy, 0);
	atomic_set(&priv->stream_active, 0);

	dev_info(&priv->pdev->dev, "Added CMA buffer: size=%zu, dma_addr=0x%llx, pages=%d, sg_nents=%d\n",
		 aligned_size, (unsigned long long)user_buf_hndl->coherent_dma_addr, 
		 num_pages, sgt->nents);

	return 0;

err_free_rx_descs:
	iwfg_release_rx_descs(user_buf_hndl);
err_unmap_sg:
	dma_unmap_sg(&priv->pdev->dev, sgt->sgl, sgt->nents, DMA_BIDIRECTIONAL);
err_free_sgt:
	sg_free_table(sgt);
err_free_sgt_struct:
	kfree(sgt);
err_free_pages:
	kfree(user_buf_hndl->pgs);
err_free_cma:
	dma_release_from_contiguous(&priv->pdev->dev, pages, num_pages);
err_free_hndl:
	kfree(user_buf_hndl);
	return -ENOMEM;
}

/**
 * iwfg_add_dma_import_buf - Import an external DMA-BUF
 * @priv: device private data
 * @user_buf_req: buffer request from userspace (contains import_fd)
 * 
 * Imports an external DMA-BUF and maps it for use with the QDMA engine.
 * 
 * Returns: 0 on success, negative error code on failure
 */
int iwfg_add_dma_import_buf(struct iwfg_private *priv, struct iwfg_user_buf *user_buf_req)
{
	struct iwfg_user_page_buf *user_buf_hndl = NULL;
	struct dma_buf *import_dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct scatterlist *sg;
	u32 c2h_buf_size;
	u32 rx_desc_count;
	u32 h2c_buf_size;
	u32 tx_desc_count;
	int i, num_entries;

	if (!priv || !user_buf_req || user_buf_req->import_fd < 0) {
		dev_err(&priv->pdev->dev, "Invalid parameters for iwfg_add_dma_import_buf");
		return -EINVAL;
	}

	// Get DMA-BUF from file descriptor
	import_dmabuf = dma_buf_get(user_buf_req->import_fd);
	if (IS_ERR(import_dmabuf)) {
		dev_err(&priv->pdev->dev, "Failed to get DMA-BUF from fd %d: %ld\n",
			user_buf_req->import_fd, PTR_ERR(import_dmabuf));
		return PTR_ERR(import_dmabuf);
	}

	// Attach to the DMA-BUF
	attach = dma_buf_attach(import_dmabuf, &priv->pdev->dev);
	if (IS_ERR(attach)) {
		dev_err(&priv->pdev->dev, "Failed to attach to DMA-BUF: %ld\n", PTR_ERR(attach));
		dma_buf_put(import_dmabuf);
		return PTR_ERR(attach);
	}

	// Map the DMA-BUF
	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		dev_err(&priv->pdev->dev, "Failed to map DMA-BUF attachment: %ld\n", PTR_ERR(sgt));
		dma_buf_detach(import_dmabuf, attach);
		dma_buf_put(import_dmabuf);
		return PTR_ERR(sgt);
	}

	// Count entries in the scatter-gather table
	num_entries = 0;
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		num_entries++;
	}

	user_buf_hndl = kzalloc(sizeof(*user_buf_hndl), GFP_KERNEL);
	if (!user_buf_hndl) {
		dev_err(&priv->pdev->dev, "Failed to allocate iwfg_user_page_buf\n");
		goto err_unmap;
	}

	// Store import references
	user_buf_hndl->import_dmabuf = import_dmabuf;
	user_buf_hndl->import_attach = attach;
	user_buf_hndl->import_sgt = sgt;

	// Use the imported scatter-gather table directly
	user_buf_hndl->sgl = sgt->sgl;
	user_buf_hndl->sg_nents = sgt->nents;
	user_buf_hndl->sg_mapped_count = sgt->nents;
	user_buf_hndl->sg_mapped = true;
	user_buf_hndl->sg_host = false;
	user_buf_hndl->pgs = NULL;  // Pages are managed by the exporter
	user_buf_hndl->num_pages = 0;

	user_buf_hndl->usr_buf_size = import_dmabuf->size;
	user_buf_hndl->user_addr = NULL;
	user_buf_hndl->cur_filled_size = 0;
	user_buf_hndl->total_filled_size = 0;
	user_buf_hndl->compl_count = 0;
	user_buf_hndl->priv = priv;
	user_buf_hndl->locked = true;

	// Set buffer type
	user_buf_hndl->buf_type = IWFG_BUF_TYPE_DMA_IMPORT;
	user_buf_hndl->coherent_buf = NULL;
	user_buf_hndl->coherent_dma_addr = 0;
	user_buf_hndl->dmabuf = NULL;
	user_buf_hndl->exported = false;

	if (user_buf_req->is_chunked && user_buf_req->chunk_size > 0) {
		user_buf_hndl->size_to_fill = user_buf_req->chunk_size;
		user_buf_hndl->is_chunked = true;
	} else {
		user_buf_hndl->size_to_fill = import_dmabuf->size;
		user_buf_hndl->is_chunked = false;
	}

	user_buf_hndl->req_compls = import_dmabuf->size / user_buf_hndl->size_to_fill;

	// Initialize atomic variables and spinlock
	atomic_set(&user_buf_hndl->ready_to_use, 0);
	spin_lock_init(&user_buf_hndl->lock);
	INIT_LIST_HEAD(&user_buf_hndl->tx_desc_list);
	INIT_LIST_HEAD(&user_buf_hndl->rx_desc_list);

	// Create DMA descriptor lists
	c2h_buf_size = iwfg_update_rx_bufsz_idx(priv, sg_dma_len(user_buf_hndl->sgl));
	h2c_buf_size = min_t(u32, sg_dma_len(user_buf_hndl->sgl), 16384);

	rx_desc_count = iwfg_map_sg_to_rx_descs(priv, user_buf_hndl, c2h_buf_size);
	if (rx_desc_count == 0) {
		goto err_free_hndl;
	}

	tx_desc_count = iwfg_map_sg_to_tx_descs(priv, user_buf_hndl, h2c_buf_size);
	if (tx_desc_count == 0) {
		goto err_free_rx_descs;
	}

	user_buf_hndl->rx_desc_count = rx_desc_count;
	user_buf_hndl->tx_desc_count = tx_desc_count;

	init_waitqueue_head(&user_buf_hndl->waitq);
	list_add_tail(&user_buf_hndl->list, &priv->dma_buf_list);

	atomic_set(&priv->h2c_busy, 0);
	atomic_set(&priv->stream_active, 0);

	dev_info(&priv->pdev->dev, "Imported DMA-BUF: fd=%d, size=%zu, sg_nents=%d\n",
		 user_buf_req->import_fd, import_dmabuf->size, sgt->nents);

	return 0;

err_free_rx_descs:
	iwfg_release_rx_descs(user_buf_hndl);
err_free_hndl:
	kfree(user_buf_hndl);
err_unmap:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(import_dmabuf, attach);
	dma_buf_put(import_dmabuf);
	return -ENOMEM;
}

#endif /* CONFIG_ARM || CONFIG_ARM64 */

void iwfg_delete_user_buf(struct iwfg_private *priv, struct iwfg_cdev *icdev) {
	struct iwfg_user_page_buf *user_buf_hndl, *tmp;

	if (!priv)
		return;

	// Ensure streaming is deactivated (should already be done by caller)
	atomic_set(&icdev->stream_active, 0);

	// Give any running work threads a chance to see the stream_active change and exit
	msleep(10);

	list_for_each_entry_safe(user_buf_hndl, tmp, &icdev->dma_buf_list, list) {

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
		// Clean up DMA-BUF export if it exists, (In userspace close dma-buf fd before this routine is called)
		if (user_buf_hndl->exported && user_buf_hndl->dmabuf) {
			dev_info(&priv->pdev->dev, "Cleaning up exported DMA-BUF for buffer type %u", 
				user_buf_hndl->buf_type);
			// The dmabuf release callback will handle cleanup when the last reference is dropped
			// Just clear our references to avoid double cleanup
			user_buf_hndl->dmabuf = NULL;
			user_buf_hndl->exported = false;
		}
#endif

		// Handle cleanup based on buffer type
		switch (user_buf_hndl->buf_type) {
		case IWFG_BUF_TYPE_USERPTR:
			// USERPTR: Unmap DMA and release pinned pages
			if (user_buf_hndl->sg_mapped) {
				dma_unmap_sg(&priv->pdev->dev,
					user_buf_hndl->sgl,
					user_buf_hndl->sg_nents,
					DMA_BIDIRECTIONAL);
				user_buf_hndl->sg_mapped = false;
				dev_info(&priv->pdev->dev, "DMA unmapped %d segments for USERPTR buffer 0x%p", 
					user_buf_hndl->sg_nents, user_buf_hndl->user_addr);
			}

			if (user_buf_hndl->pgs && user_buf_hndl->num_pages) {
				for (unsigned int i = 0; i < user_buf_hndl->num_pages; i++) {
					if (user_buf_hndl->pgs[i])
						put_page(user_buf_hndl->pgs[i]);
				}
			}
			vfree(user_buf_hndl->sgl);
			vfree(user_buf_hndl->pgs);
			break;

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
		case IWFG_BUF_TYPE_DMA_ALLOC:
			// DMA_ALLOC: Free CMA allocated buffer
			// First unmap DMA
			if (user_buf_hndl->sg_mapped && user_buf_hndl->coherent_sgt) {
				dma_unmap_sg(&priv->pdev->dev,
					user_buf_hndl->coherent_sgt->sgl,
					user_buf_hndl->coherent_sgt->nents,
					DMA_BIDIRECTIONAL);
				user_buf_hndl->sg_mapped = false;
			}
			
			// Free sg_table from sg_alloc_table_from_pages
			if (user_buf_hndl->coherent_sgt) {
				sg_free_table(user_buf_hndl->coherent_sgt);
				kfree(user_buf_hndl->coherent_sgt);
			}
			
			// Free the page pointer array
			kfree(user_buf_hndl->pgs);
			
			// Release CMA pages
			if (user_buf_hndl->cma_pages) {
				int num_pages = user_buf_hndl->coherent_size / PAGE_SIZE;
				dma_release_from_contiguous(&priv->pdev->dev,
							    user_buf_hndl->cma_pages,
							    num_pages);
				dev_info(&priv->pdev->dev, "Released %d CMA pages",
					num_pages);
			}
			break;

		case IWFG_BUF_TYPE_DMA_IMPORT:
			// DMA_IMPORT: Unmap and detach imported DMA-BUF
			if (user_buf_hndl->import_sgt && user_buf_hndl->import_attach) {
				dma_buf_unmap_attachment(user_buf_hndl->import_attach,
							 user_buf_hndl->import_sgt,
							 DMA_BIDIRECTIONAL);
				dev_info(&priv->pdev->dev, "Unmapped imported DMA-BUF attachment");
			}
			if (user_buf_hndl->import_attach && user_buf_hndl->import_dmabuf) {
				dma_buf_detach(user_buf_hndl->import_dmabuf,
					       user_buf_hndl->import_attach);
				dev_info(&priv->pdev->dev, "Detached imported DMA-BUF");
			}
			if (user_buf_hndl->import_dmabuf) {
				dma_buf_put(user_buf_hndl->import_dmabuf);
				dev_info(&priv->pdev->dev, "Released imported DMA-BUF reference");
			}
			// Note: sgl is owned by import_sgt, don't free it
			// pgs is NULL for imported buffers
			break;
#endif /* CONFIG_ARM || CONFIG_ARM64 */

		default:
			dev_warn(&priv->pdev->dev, "Unknown buffer type %u during cleanup",
				 user_buf_hndl->buf_type);
			break;
		}

		iwfg_release_tx_descs(user_buf_hndl);
		iwfg_release_rx_descs(user_buf_hndl);

		list_del(&user_buf_hndl->list);
		kfree(user_buf_hndl);
	}
	INIT_LIST_HEAD(&icdev->dma_buf_list);
}

void iwfg_qdma_c2h_start(struct iwfg_private *priv, struct iwfg_cdev *icdev)
{
	// Set stream as active
	// int rv;
	u32 reg_val = 0;
	int stream_id = icdev->stream_id;
	atomic_set(&icdev->stream_active, 1);

	dev_info(&priv->pdev->dev, "stream_active %d", atomic_read(&icdev->stream_active));

	iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET, reg_val);

	reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_H2C_CTRL_REG_OFFSET);
	reg_val |= IWFG_H2C_CTRL_VID_OUT_SEL_MASK;
	iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_H2C_CTRL_REG_OFFSET, reg_val);
	
	dev_info(&priv->pdev->dev, "C2H streaming started");
}

/*
 * iwfg_qdma_h2c_start - enable the PL-side H2C datapath.
 *
 * ROOT-CAUSE FIX for "H2C stalls after the first chunk":
 * IWFG_H2C_CTRL_VID_OUT_SEL routes the PL video/IQ output to the QDMA
 * H2C AXI-Stream. Historically it was set by the STREAM_START ioctl;
 * when that ioctl was removed, the only remaining place that set it was
 * iwfg_qdma_c2h_start() - which runs ONLY when the C2H node is opened.
 * In an H2C-only session the bit stayed clear, nothing in the PL
 * consumed the H2C stream, the PL FIFO absorbed the first chunk, tready
 * deasserted, the QDMA stalled before writing back completion, and the
 * blocking DMA_DATA ioctl slept forever. This function is now called
 * from iwfg_open() for every H2C node, restoring the lost register
 * write at the point the v7 lifecycle model expects it (open == start).
 */
void iwfg_qdma_h2c_start(struct iwfg_private *priv, struct iwfg_cdev *icdev)
{
	u32 reg_val;
	int stream_id = icdev->stream_id;

	atomic_set(&icdev->stream_active, 1);

	reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_H2C_CTRL_REG_OFFSET);
	reg_val |= IWFG_H2C_CTRL_VID_OUT_SEL_MASK;
	iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_H2C_CTRL_REG_OFFSET, reg_val);

	dev_info(&priv->pdev->dev,
		 "H2C streaming enabled (stream %d, H2C_CTRL=0x%08x)",
		 stream_id, reg_val);
}

/*
 * iwfg_qdma_h2c_stop - mark the H2C stream inactive on close.
 *
 * VID_OUT_SEL is deliberately left SET unless no other stream on this
 * stream_id is active: iwfg_qdma_c2h_start() also depends on it for the
 * loopback capture path, and clearing it here would break a C2H session
 * that outlives the H2C node. This mirrors the pre-existing behavior
 * where the bit, once set by c2h_start, was never cleared.
 */
void iwfg_qdma_h2c_stop(struct iwfg_private *priv, struct iwfg_cdev *icdev)
{
	struct iwfg_cdev *cdev_entry;
	bool other_active = false;
	u32 reg_val;
	int stream_id = icdev->stream_id;

	atomic_set(&icdev->stream_active, 0);

	list_for_each_entry(cdev_entry, &priv->cdev_list, list) {
		if (cdev_entry != icdev &&
		    cdev_entry->stream_id == stream_id &&
		    atomic_read(&cdev_entry->stream_active) == 1) {
			other_active = true;
			break;
		}
	}

	if (!other_active) {
		reg_val = iwfg_stream_read_reg(&priv->hw, stream_id,
					       IWFG_H2C_CTRL_REG_OFFSET);
		reg_val &= ~IWFG_H2C_CTRL_VID_OUT_SEL_MASK;
		iwfg_stream_write_reg(&priv->hw, stream_id,
				      IWFG_H2C_CTRL_REG_OFFSET, reg_val);
	}

	dev_info(&priv->pdev->dev,
		 "H2C streaming stopped (stream %d, out_sel %s)",
		 stream_id, other_active ? "kept" : "cleared");
}

void iwfg_qdma_c2h_stop(struct iwfg_private *priv, struct iwfg_cdev *icdev)
{
	u32 reg_val = 0;
	int stream_id = icdev->stream_id;
	dev_info(&priv->pdev->dev, "Stopping C2H streaming");
	
	// First, deactivate streaming to prevent further auto-refill scheduling
	atomic_set(&icdev->stream_active, 0);

	// Stop the hardware stream
	reg_val = iwfg_stream_read_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET);
	reg_val &= ~IWFG_C2H_CTRL_START_MASK; // Clear stream enable bit
	iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_C2H_CTRL_REG_OFFSET, reg_val);

	icdev->is_video_locked = false;
	
	dev_info(&priv->pdev->dev, "Hardware stream stopped");
}

void iwfg_qdma_audio_start(struct iwfg_private *priv, struct iwfg_cdev *icdev)
{
	int stream_id = icdev->stream_id;

	// Set stream as active
	atomic_set(&icdev->stream_active, 1);
	dev_info(&priv->pdev->dev, "Audio stream_active %d", atomic_read(&icdev->stream_active));

	iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_AUD_CTRL_REG_OFFSET, 0x0);

	dev_info(&priv->pdev->dev, "Audio streaming started");
}

void iwfg_qdma_audio_stop(struct iwfg_private *priv, struct iwfg_cdev *icdev)
{
	int stream_id = icdev->stream_id;
	dev_info(&priv->pdev->dev, "Stopping audio streaming");

	atomic_set(&icdev->stream_active, 0);

	iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_AUD_CTRL_REG_OFFSET, 0x0);
	iwfg_stream_write_reg(&priv->hw, stream_id, IWFG_AUD_BUF_SIZE_REG_OFFSET, 0x0);

	icdev->is_audio_locked = false;

	dev_info(&priv->pdev->dev, "Audio streaming stop completed");
}
