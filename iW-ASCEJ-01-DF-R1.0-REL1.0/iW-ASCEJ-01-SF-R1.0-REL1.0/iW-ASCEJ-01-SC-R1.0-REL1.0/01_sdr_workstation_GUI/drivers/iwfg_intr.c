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
#include <linux/pci.h>
#include <linux/slab.h>  /* for kmalloc */

#include "iwfg_intr.h"
#include "iwfg.h"
#include "iwfg_dma.h"
#include "qdma_access/qdma_register.h"

#define IWFG_MAX_IRQ_NAME 32

// extern int iwfg_poll(struct napi_struct *napi, int budget);

static irqreturn_t iwfg_q_handler(int irq, void *dev_id)
{
	struct iwfg_q_vector *vec = dev_id;
	struct iwfg_private *priv = vec->priv;
	u16 qid = vec->vid - 2;  // Queue vectors start at vid=2, so queue 0 is vid=2
	struct iwfg_qdma_queue *q = priv->queue[qid];
	bool debug = 0;

	if (debug) dev_info(&priv->pdev->dev, "queue irq");

	// if (q->direction == DMA_TO_DEVICE) {
	// 	return IRQ_HANDLED;
	// }

	// Safety check for q
	if (!q) {
		dev_warn(&priv->pdev->dev, "queue %d not initialized", qid);
		return IRQ_HANDLED;
	}

	/*
	 * Do NOT drop TX completion interrupts just because q->user_buf is
	 * (transiently) NULL: iwfg_tx_clean()/iwfg_tx_reclaim() safely
	 * handles a NULL user_buf (it still reconciles next_to_clean with
	 * the writeback and re-arms the IRQ; it only skips the wake-up).
	 * Silently returning IRQ_HANDLED here left the ring accounting
	 * stale and could strand the next transfer. The RX work item does
	 * dereference user_buf, so keep the guard for C2H only.
	 */
	if (!q->user_buf && q->direction != DMA_TO_DEVICE) {
		dev_warn(&priv->pdev->dev, "queue %d user buffer not initialized", qid);
		return IRQ_HANDLED;
	}

	schedule_work_on(smp_processor_id(), &q->intr_work);

	return IRQ_HANDLED;
}

static irqreturn_t iwfg_user_handler(int irq, void *dev_id)
{
	// struct iwfg_private *priv = dev_id;
	return IRQ_WAKE_THREAD;
}

static irqreturn_t iwfg_user_thread_fn(int irq, void *dev_id)
{
	struct iwfg_private *priv = dev_id;

	// dev_info(&priv->pdev->dev,
	// 	"User IRQ (BH) fired on Funtion#%05x: vector=%d\n",
	// 	PCI_FUNC(priv->pdev->devfn), irq);

	volatile u32 reg_val;
	struct iwfg_qdma_queue *q = NULL;
	u32 offset = 0;

	reg_val = iwfg_read_reg(&priv->hw, IWFG_C2H_INTR_REG_OFFSET);
	
	if (reg_val & IWFG_C2H_INTR_BIT_MASK) {
		/**
		 * currently not using, but firmware interrupts when hdmi rx resolution is updated
		 */
		iwfg_write_reg(&priv->hw, IWFG_C2H_INTR_REG_OFFSET, 0x0);
	} else {
		/**
		 * traverse through all the availble interrupt status registers to 
		 * identify the source of interrupt.
		 */
		for (int i = 0; i < priv->num_streams; i++) {
			offset = IWFG_USER_INTR_STATUS_REG_OFFSET + i * IWFG_VID_STREAM_BRAM_LEN;
			reg_val = iwfg_read_reg(&priv->hw, offset);
			if (reg_val == 0x1) {
				q = priv->queue[i];
				iwfg_write_reg(&priv->hw, offset, 0x0); // Clear the interrupt status bit
				break;
			}
		}
	}

	if (q == NULL) {
		// dev_warn(&priv->pdev->dev, "User IRQ received but no queue is associated");
		return IRQ_HANDLED;
	}
	
	
	// vsync signal
	// reg_val = iwfg_read_reg(&priv->hw, 0x08);
	// if((reg_val & 0x1) != 0 ) {
		// dev_warn(&priv->pdev->dev, "vsync interrupt without clearing the bit");
		// atomic_set(&q->vsync_event, 0);
	// } else {
		atomic_set(&q->vsync_event, 1);
	// }
	wake_up_interruptible(&q->vsync_waitq);

	return IRQ_HANDLED;
}

static irqreturn_t iwfg_error_handler(int irq, void *dev_id)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t iwfg_error_thread_fn(int irq, void *dev_id)
{
	struct iwfg_private *priv = dev_id;
	struct pci_dev *pdev = priv->pdev;
	struct qdma_dev *qdev = (struct qdma_dev *)priv->hw.qdma;
	// char *err_info;
	// int result;
	u32 error_val;

	// /* Allocate memory for error information - use kmalloc for large buffer */
	// err_info = kmalloc(2048, GFP_KERNEL);
	// if (!err_info) {
	// 	dev_err(&pdev->dev, "Failed to allocate memory for error information\n");
	// 	/* Even if allocation fails, continue with error handling but skip detailed logging */
	// 	err_info = NULL;
	// } else {
	// 	memset(err_info, 0, 2048);
	// }

	dev_err(&pdev->dev,
		"Error IRQ (BH) fired on Function#%05x: vector=%d\n",
		PCI_FUNC(pdev->devfn), irq);

	/* Log all relevant error status registers before clearing them */
	error_val = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_ERR_STAT);
	dev_err(&pdev->dev, "QDMA Global Error Status:          0x%08x\n", error_val);

	error_val = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_DSC_ERR_STS);
	dev_err(&pdev->dev, "QDMA Global Descriptor Error:      0x%08x\n", error_val);

	error_val = qdma_read_reg(qdev, QDMA_OFFSET_GLBL_TRQ_ERR_STAT);
	dev_err(&pdev->dev, "QDMA Global TRQ Error:             0x%08x\n", error_val);

	error_val = qdma_read_reg(qdev, QDMA_OFFSET_C2H_ERR_STAT);
	dev_err(&pdev->dev, "QDMA C2H Error Status:             0x%08x\n", error_val);

	error_val = qdma_read_reg(qdev, QDMA_OFFSET_C2H_FATAL_ERR_STAT);
	dev_err(&pdev->dev, "QDMA C2H Fatal Error Status:       0x%08x\n", error_val);

	error_val = qdma_read_reg(qdev, QDMA_OFFSET_H2C_ERR_STAT);
	dev_err(&pdev->dev, "QDMA H2C Error Status:             0x%08x\n", error_val);

	/*
	 * Clear all sticky W1C error bits and rearm the MSI-X error vector
	 * (vid = 0) so the interrupt fires again on the next error event.
	 */
	iwfg_qdma_ack_and_rearm_error_interrupt(priv->hw.qdma, 0);

	return IRQ_HANDLED;
}

/**
 * iwfg_clear_q_vector - clear a queue vector
 * @priv: pointer to driver private data
 * @vid: MSI-X vector ID
 **/
static void iwfg_clear_q_vector(struct iwfg_private *priv, u16 vid)
{
	struct iwfg_q_vector *vec = priv->q_vector[vid - 2];  // Convert MSI-X vector ID to queue index

	if (!vec)
		return;
	free_irq(pci_irq_vector(priv->pdev, vid), vec);
	priv->q_vector[vid - 2] = NULL;  // Clear the pointer
	kfree(vec);
}

/**
 * iwfg_init_q_vector - initialize a queue vector
 * @priv: pointer to driver private data
 * @vid: vector ID
 *
 * This function does the following: allocate coherent DMA region for interrupt
 * aggregation ring, register NAPI instances, and initialize relevant QDMA
 * interrupt registers.  Return 0 on success, negative on failure
 **/
static int iwfg_init_q_vector(struct iwfg_private *priv, u16 vid)
{
	struct pci_dev *pdev = priv->pdev;
	struct iwfg_q_vector *vec;
	char* name = (char*)kzalloc(sizeof(char)*IWFG_MAX_IRQ_NAME, GFP_KERNEL);
	int rv;

	vec = kzalloc(sizeof(struct iwfg_q_vector), GFP_KERNEL);
	if (!vec)
		return -ENOMEM;
	vec->priv = priv;
	vec->vid = vid;

	snprintf(name, IWFG_MAX_IRQ_NAME, "%s-%d", IWFG_DRIVER_NAME, vid);
	rv = request_irq(pci_irq_vector(pdev, vid), iwfg_q_handler,
			 0, name, vec);
	if (rv < 0) {
		dev_err(&pdev->dev, "Failed to setup queue vector %s", name);
		return rv;
	}

	/* setup affinity mask and node */
	/* cpu = vid % num_online_cpus(); */
	/* cpumask_set_cpu(cpu, &vec->affinity_mask); */
	/* vec->numa_node = node; */

	dev_info(&pdev->dev, "Setup IRQ vector %d with name %s",
		 pci_irq_vector(pdev, vid), name);
	priv->q_vector[vid - 2] = vec;  // Store using queue index (0-based), not MSI-X vector ID

	return 0;
}

/**
 * iwfg_acquire_msix_vectors - acquire MSI-X vectors
 * @priv: pointer to driver private data
 *
 * Attempt to acquire a suitable range of MSI-X vector interrupts.  Return 0 on
 * success, and negative on error.
 *
 * For every PF, a minimum of 2 vectors are required for proper operation, one
 * for queue interrupt and one for user interreupt.  The master PF requires one
 * additional vector for global error interrupt.
 **/
static int iwfg_acquire_msix_vectors(struct iwfg_private *priv)
{
	int vectors, non_q_vectors;

	vectors = IWFG_MAX_QUEUES;
	non_q_vectors = 1;
	if (test_bit(IWFG_FLAG_MASTER_PF, priv->flags))
		non_q_vectors++;
	vectors += non_q_vectors;

	vectors = pci_alloc_irq_vectors(priv->pdev, non_q_vectors + 1, vectors,
					PCI_IRQ_MSIX);
	if (vectors < 0) {
		dev_err(&priv->pdev->dev,
			"Failed to allocate vectors in the range [%d, %d]",
			non_q_vectors + 1, vectors);
		return vectors;
	}

	vectors -= non_q_vectors;
	priv->num_q_vectors = vectors;

	dev_info(&priv->pdev->dev, "Allocated %d queue vectors\n",
		 priv->num_q_vectors);
	return 0;
}

/**
 * iwfg_set_num_queues - calculate the number of active queues
 * @priv: pointer to driver private data
 *
 * The number of active queues equals to either the number of queue vectors, or
 * the real number of queues in the associated net device, whichever is smaller.
 **/
static void iwfg_set_num_queues(struct iwfg_private *priv)
{
	priv->num_tx_queues =
		min_t(u16, priv->num_q_vectors, IWFG_MAX_QUEUES);
	priv->num_rx_queues =
		min_t(u16, priv->num_q_vectors, IWFG_MAX_QUEUES);
}

int iwfg_init_capacity(struct iwfg_private *priv)
{
	int rv;

	rv = iwfg_acquire_msix_vectors(priv);
	if (rv < 0)
		return rv;
	iwfg_set_num_queues(priv);
	return 0;
}

void iwfg_clear_capacity(struct iwfg_private *priv)
{
	priv->num_tx_queues = 0;
	priv->num_rx_queues = 0;
	priv->num_q_vectors = 0;
	pci_free_irq_vectors(priv->pdev);
}

int iwfg_init_interrupt(struct iwfg_private *priv)
{
	struct pci_dev *pdev = priv->pdev;
	int vid = 0, rv, qvid;

	if (!test_bit(IWFG_FLAG_MASTER_PF, priv->flags))
		return 0;

	/* Vector assignment scheme:
	 * vid = 0: Error interrupt (master PF only)
	 * vid = 1: User interrupt
	 * vid = 2+: Queue interrupts (queue 0 = vid 2, queue 1 = vid 3, etc.)
	 */

	/* Setup error interrupt (vid = 0) */
	rv = request_threaded_irq(pci_irq_vector(pdev, vid),
				  iwfg_error_handler, iwfg_error_thread_fn,
				  0, "iwfg-error", priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "Failed to setup error interrupt");
		goto clear_interrupt;
	}
	iwfg_qdma_init_error_interrupt(priv->hw.qdma, vid);
	set_bit(IWFG_ERROR_INTR, priv->state);

	vid++;

	/* Setup user interrupt (vid = 1) */
	rv = request_threaded_irq(pci_irq_vector(pdev, vid),
				  iwfg_user_handler, iwfg_user_thread_fn,
				  0, "iwfg-user", priv);
	if (rv < 0) {
		dev_err(&pdev->dev, "Failed to setup user interrupt");
		goto clear_interrupt;
	}
	set_bit(IWFG_USER_INTR, priv->state);

	qvid = ++vid;

	/* Setup queue interrupts (vid = 2 and above) */
	for (vid = qvid; vid < qvid + priv->num_q_vectors; ++vid) {
		rv = iwfg_init_q_vector(priv, vid);
		if (rv < 0)
			goto clear_interrupt;
	}

	return 0;

clear_interrupt:
	iwfg_clear_interrupt(priv);
	return rv;
}

void iwfg_clear_interrupt(struct iwfg_private *priv)
{
	u8 master_pf = test_bit(IWFG_FLAG_MASTER_PF, priv->flags);
	int vid;

	/* Clear queue vectors first (starting from vid=2) */
	for (vid = 2; vid < 2 + priv->num_q_vectors; vid++) {
		iwfg_clear_q_vector(priv, vid);
	}

	/* Clear user interrupt (vid=1) */
	if (test_bit(IWFG_USER_INTR, priv->state)) {
		free_irq(pci_irq_vector(priv->pdev, 1), priv);
		clear_bit(IWFG_USER_INTR, priv->state);
	}

	/* Clear error interrupt (vid=0) - only for master PF */
	if (master_pf && test_bit(IWFG_ERROR_INTR, priv->state)) {
		free_irq(pci_irq_vector(priv->pdev, 0), priv);
		iwfg_qdma_clear_error_interrupt(priv->hw.qdma);
		clear_bit(IWFG_ERROR_INTR, priv->state);
	}
}
