#ifndef IWFG_DMA_H
#define IWFG_DMA_H

#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include "iwfg_hardware.h"
#include "iwfg.h"
#include "iwfg_register.h"

/* Buffer DMA state flags for concurrent access detection */
// #define IWFG_BUF_DMA_IDLE    0  /* Buffer is not in use for DMA */
// #define IWFG_BUF_DMA_C2H     1  /* Buffer is being used for C2H (receive) */
// #define IWFG_BUF_DMA_H2C     2  /* Buffer is being used for H2C (transmit) */

struct iwfg_tx_buffer {
	dma_addr_t dma_addr;
	u32 len;
};

struct iwfg_tx_desc {
	struct list_head list;
	struct qdma_h2c_st_desc desc;
};

struct iwfg_rx_desc {
	struct list_head list;
	struct qdma_c2h_st_desc desc;
};

struct iwfg_user_page_buf {
	struct list_head list;
	void __user *user_addr;
	u32 num_pages;
	u32 sg_nents;
	u32 sg_mapped_count;  // Number of DMA-mapped segments (can be < sg_nents due to coalescing)
	u32 tx_desc_count;
	u32 rx_desc_count;
	u32 tx_desc_served; // number of descriptors served count (for chunked buffers h2c only)
	// u32 rx_desc_served; // number of descriptors served count (for chunked buffers c2h only)
	struct scatterlist *sgl;
	struct page **pgs;
	size_t usr_buf_size; // total size of the user buffer
	size_t size_to_fill; // size of the current chunk to fill
	size_t cur_filled_size;
	size_t total_filled_size;
	u32 compl_count; // number of chunked completions
	u32 req_compls; // number of completions requested by user
	bool is_synced;
	bool is_chunked;
	// int direction;
	bool locked;
	bool sg_mapped;
	bool sg_host; // true if the scatterlist is mapped to host memory
	// bool qdma_mapped; // true if the buffer is mapped to QDMA
	atomic_t ready_to_use; // ready to use by userspace
	wait_queue_head_t waitq;
	spinlock_t lock; // protects buffer state (cur_filled_size, total_filled_size, compl_count)

	bool fifo_overflow; // flag to indicate if fifo overflow (qdma random tready deassertion workaround)

	// new things here,
	struct list_head tx_desc_list; // dma descriptors list
	struct list_head rx_desc_list; // dma descriptors list

	// DMA-BUF export support
	struct dma_buf *dmabuf;       // Exported DMA-BUF object
	bool exported;                // True if buffer has been exported as DMA-BUF
	struct dma_resv resv;         // Reservation object for synchronization
	struct iwfg_private *priv;    // Back-reference to device

	// Buffer type and CMA/DMA allocation support
	u32 buf_type;                 // enum iwfg_buf_type
	void *coherent_buf;           // Kernel virtual address for DMA_ALLOC buffers
	dma_addr_t coherent_dma_addr; // DMA address for DMA_ALLOC buffers
	size_t coherent_size;         // Size of allocation for cleanup
	struct page *cma_pages;       // Base page from dma_alloc_from_contiguous
	struct sg_table *coherent_sgt; // SG table from sg_alloc_table_from_pages
	
	// DMA-BUF import support
	struct dma_buf *import_dmabuf;       // Imported DMA-BUF object
	struct dma_buf_attachment *import_attach; // Attachment for imported DMA-BUF
	struct sg_table *import_sgt;         // Scatter-gather table from imported DMA-BUF

	bool is_audio;
};

/**
 * struct iwfg_ring - generic ring structure
 **/
struct iwfg_ring {
	u16 count;		/* number of descriptors */
	u8 *desc;		/* base address for descriptors */
	u8 *wb;			/* descriptor writeback */
	dma_addr_t dma_addr;	/* DMA address for descriptors */

	u16 next_to_use;	/* pidx*/
	u16 next_to_clean;	/* cidx */
	u16 head;		/* software head index, for RX -->  */
	u8 color;
};

struct iwfg_qdma_queue {
	u16 qid;
	int direction;
	DECLARE_BITMAP(state, 32);

	u16 desc_buf_size;

	struct iwfg_ring desc_ring;
	struct iwfg_ring cmpl_ring;
	struct iwfg_q_vector *vector;

	struct iwfg_user_page_buf *user_buf; // to attach/bind buffer to the queue
	struct iwfg_user_page_buf *prearm_user_buf; // next buffer to arm at boundary
	bool prearm_posted; // true once prearm_user_buf descriptors are in QDMA ring
	int stream_id; // stream id used for C2H roll
	size_t rx_pending_carry; // overshoot bytes to apply to next active RX buffer
	
	struct work_struct intr_work;
	struct iwfg_private *priv;

	wait_queue_head_t vsync_waitq; // for rx queue vsync wait
	atomic_t vsync_event;  // 1 = vsync event occurred, waiting thread hasto clear it
};

struct iwfg_q_vector {
	u16 vid;
	struct iwfg_private *priv;
	struct cpumask affinity_mask;
	int numa_node;
};

// void iwfg_rx_refill(struct iwfg_qdma_queue *q);
int iwfg_xmit_data(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf, u32 qid);
void iwfg_rx_roll(struct iwfg_private *priv, u16 qid, int stream_id);
int iwfg_recv_data(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf, u32 qid, int stream_id, struct iwfg_user_page_buf *next_user_buf);
int iwfg_wait_for_data(struct iwfg_user_page_buf *dma_buf);

u32 iwfg_update_rx_bufsz_idx(struct iwfg_private *priv, u32 dma_len, bool is_audio, int stream_id);
int iwfg_map_sg_to_tx_descs(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf_hndl, u32 desc_len);
void iwfg_release_tx_descs(struct iwfg_user_page_buf *user_buf_hndl);
int iwfg_map_sg_to_rx_descs(struct iwfg_private *priv, struct iwfg_user_page_buf *user_buf_hndl, u32 desc_len);
void iwfg_release_rx_descs(struct iwfg_user_page_buf *user_buf_hndl);

// int iwfg_init_queue_resource(struct iwfg_private *priv);
// int iwfg_clear_queue_resource(struct iwfg_private *priv);

int iwfg_init_rx_queue(struct iwfg_private *priv, u16 qid);
void iwfg_clear_rx_queue(struct iwfg_private *priv, u16 qid);
int iwfg_init_tx_queue(struct iwfg_private *priv, u16 qid);
void iwfg_clear_tx_queue(struct iwfg_private *priv, u16 qid);

struct iwfg_user_page_buf* iwfg_get_user_buf_by_addr(struct iwfg_private *priv, struct iwfg_cdev *icdev, void *user_buf, size_t size);
struct iwfg_user_page_buf* iwfg_get_user_buf_by_index(struct iwfg_private *priv, struct iwfg_cdev *icdev, u32 index);
struct iwfg_user_page_buf* iwfg_add_user_buf(struct iwfg_private *priv, struct iwfg_cdev *icdev, void *user_buf, 
	size_t size, bool is_chunked, size_t chunk_size, bool is_audio);
void iwfg_delete_user_buf(struct iwfg_private *priv, struct iwfg_cdev *icdev);

void iwfg_qdma_c2h_start(struct iwfg_private *priv, struct iwfg_cdev *icdev);
void iwfg_qdma_c2h_stop(struct iwfg_private *priv, struct iwfg_cdev *icdev);

/* Enable/disable the PL-side H2C datapath (VID_OUT_SEL). Called from
 * iwfg_open()/iwfg_release() for H2C nodes - replaces the register write
 * lost when the STREAM_START/STREAM_STOP ioctls were removed. */
void iwfg_qdma_h2c_start(struct iwfg_private *priv, struct iwfg_cdev *icdev);
void iwfg_qdma_h2c_stop(struct iwfg_private *priv, struct iwfg_cdev *icdev);

void iwfg_qdma_audio_start(struct iwfg_private *priv, struct iwfg_cdev *icdev);
void iwfg_qdma_audio_stop(struct iwfg_private *priv, struct iwfg_cdev *icdev);

// DMA-BUF buffer allocation/import (ARM-only, CMA-based)
#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)

/**
 * iwfg_add_dma_alloc_buf - Allocate a coherent DMA buffer
 * @priv: device private data
 * @user_buf_req: buffer request parameters
 *
 * Allocates a driver-managed coherent DMA buffer that can be
 * exported via DMA-BUF for sharing with other devices (e.g., GPU).
 *
 * Returns: 0 on success, negative error code on failure
 */
int iwfg_add_dma_alloc_buf(struct iwfg_private *priv, struct iwfg_user_buf *user_buf_req);

/**
 * iwfg_add_dma_import_buf - Import an external DMA-BUF
 * @priv: device private data
 * @user_buf_req: buffer request parameters (contains import_fd)
 *
 * Imports a DMA-BUF from another driver (e.g., GPU) for use with
 * the QDMA engine. The imported buffer cannot be re-exported.
 *
 * Returns: 0 on success, negative error code on failure
 */
int iwfg_add_dma_import_buf(struct iwfg_private *priv, struct iwfg_user_buf *user_buf_req);

#else /* !CONFIG_ARM && !CONFIG_ARM64 */

static inline int iwfg_add_dma_alloc_buf(struct iwfg_private *priv, struct iwfg_user_buf *user_buf_req)
{
	pr_err("iwfg: DMA-BUF alloc (CMA) is only supported on ARM platforms\n");
	return -EOPNOTSUPP;
}

static inline int iwfg_add_dma_import_buf(struct iwfg_private *priv, struct iwfg_user_buf *user_buf_req)
{
	pr_err("iwfg: DMA-BUF import is only supported on ARM platforms\n");
	return -EOPNOTSUPP;
}

#endif /* CONFIG_ARM || CONFIG_ARM64 */

// DMA-BUF export/fd support (only on ARM platforms)
#include "iwfg_dmabuf.h"

#endif // IWFG_DMA_H
