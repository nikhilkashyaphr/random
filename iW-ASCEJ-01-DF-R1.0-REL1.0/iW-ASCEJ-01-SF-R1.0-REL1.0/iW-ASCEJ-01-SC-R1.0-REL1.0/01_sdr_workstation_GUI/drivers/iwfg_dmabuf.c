#include <linux/module.h>
#include "iwfg.h"

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)

#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/version.h>
#include "iwfg_dma.h"
#include "iwfg_dmabuf.h"


static int iwfg_dmabuf_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attachment)
{
	struct iwfg_user_page_buf *user_buf_hndl = dmabuf->priv;
	
	dev_dbg(&user_buf_hndl->priv->pdev->dev, "DMA-BUF attach called\n");
	return 0;
}

static void iwfg_dmabuf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attachment)
{
	struct iwfg_user_page_buf *user_buf_hndl = dmabuf->priv;
	
	dev_dbg(&user_buf_hndl->priv->pdev->dev, "DMA-BUF detach called\n");
}

static struct sg_table *iwfg_dmabuf_map_dma_buf(struct dma_buf_attachment *attachment,
						 enum dma_data_direction direction)
{
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct iwfg_user_page_buf *user_buf_hndl = dmabuf->priv;
	struct sg_table *table;
	struct scatterlist *new_sg;
	int i, ret;

	dev_info(&user_buf_hndl->priv->pdev->dev, "DMA-BUF map called for device %s\n", 
		dev_name(attachment->dev));

	if (!user_buf_hndl) {
		pr_err("iwfg: Invalid user buffer handle in DMA-BUF map\n");
		return ERR_PTR(-EINVAL);
	}

	if (!user_buf_hndl->sg_mapped) {
		dev_err(&user_buf_hndl->priv->pdev->dev, "Buffer not DMA mapped\n");
		return ERR_PTR(-EINVAL);
	}

	if (user_buf_hndl->num_pages == 0 || !user_buf_hndl->pgs) {
		dev_err(&user_buf_hndl->priv->pdev->dev, "No pages available for DMA-BUF map\n");
		return ERR_PTR(-EINVAL);
	}

	// Allocate and initialize sg_table structure
	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);

	// Allocate scatterlist with one entry per page
	ret = sg_alloc_table(table, user_buf_hndl->num_pages, GFP_KERNEL);
	if (ret) {
		dev_err(&user_buf_hndl->priv->pdev->dev, 
			"Failed to allocate sg_table with %d entries\n", user_buf_hndl->num_pages);
		kfree(table);
		return ERR_PTR(ret);
	}

	// Build scatterlist from the pages array - this works for both USERPTR and DMA_ALLOC
	// For DMA_ALLOC, we extracted page pointers from the coherent allocation
	new_sg = table->sgl;
	for (i = 0; i < user_buf_hndl->num_pages; i++) {
		if (!user_buf_hndl->pgs[i]) {
			dev_err(&user_buf_hndl->priv->pdev->dev, 
				"NULL page at index %d\n", i);
			sg_free_table(table);
			kfree(table);
			return ERR_PTR(-EINVAL);
		}
		sg_set_page(new_sg, user_buf_hndl->pgs[i], PAGE_SIZE, 0);
		new_sg = sg_next(new_sg);
	}

	// Let the requesting device (GPU) create its own DMA mapping
	ret = dma_map_sgtable(attachment->dev, table, direction, 0);
	if (ret) {
		dev_err(&user_buf_hndl->priv->pdev->dev, 
			"Failed to map sg table for device %s: %d\n",
			dev_name(attachment->dev), ret);
		sg_free_table(table);
		kfree(table);
		return ERR_PTR(ret);
	}

	dev_info(&user_buf_hndl->priv->pdev->dev, 
		"DMA-BUF mapped %d pages for device %s\n", 
		user_buf_hndl->num_pages, dev_name(attachment->dev));
		
	return table;
}

static void iwfg_dmabuf_unmap_dma_buf(struct dma_buf_attachment *attachment,
				      struct sg_table *table,
				      enum dma_data_direction direction)
{
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct iwfg_user_page_buf *user_buf_hndl = dmabuf->priv;

	if (!table) {
		dev_warn(&user_buf_hndl->priv->pdev->dev, "DMA-BUF unmap called with NULL table\n");
		return;
	}

	dev_dbg(&user_buf_hndl->priv->pdev->dev, 
		"DMA-BUF unmap called - unmapping %d entries\n", table->nents);

	// Unmap the scatterlist for this device
	dma_unmap_sgtable(attachment->dev, table, direction, 0);
	
	// Free the allocated sg_table and its scatterlist entries
	sg_free_table(table);
	kfree(table);
	
	dev_dbg(&user_buf_hndl->priv->pdev->dev, "DMA-BUF unmap completed\n");
}

static void iwfg_dmabuf_release(struct dma_buf *dmabuf)
{
	struct iwfg_user_page_buf *user_buf_hndl = dmabuf->priv;

	dev_info(&user_buf_hndl->priv->pdev->dev, 
		"DMA-BUF release called - buffer size: %zu, num_pages: %d\n",
		user_buf_hndl->usr_buf_size, user_buf_hndl->num_pages);

	// Clear the dmabuf reference and exported flag
	user_buf_hndl->dmabuf = NULL;
	user_buf_hndl->exported = false;
	
	// Clean up the reservation object
	dma_resv_fini(&user_buf_hndl->resv);
	
	dev_info(&user_buf_hndl->priv->pdev->dev, "DMA-BUF release completed\n");
	
	// Note: Don't free user_buf_hndl here as it's managed by iwfg_delete_user_buf()
	// This callback is only for DMA-BUF specific cleanup
}

static int iwfg_dmabuf_begin_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	struct iwfg_user_page_buf *user_buf_hndl = dmabuf->priv;

	dev_dbg(&user_buf_hndl->priv->pdev->dev, "DMA-BUF begin CPU access\n");

	// Sync the buffer for CPU access if it's currently mapped for device
	if (user_buf_hndl->sg_mapped && !user_buf_hndl->sg_host) {
		dma_sync_sg_for_cpu(&user_buf_hndl->priv->pdev->dev,
				     user_buf_hndl->sgl,
				     user_buf_hndl->sg_mapped_count,  // Use mapped count
				     DMA_BIDIRECTIONAL);
		user_buf_hndl->sg_host = true;
	}

	return 0;
}

static int iwfg_dmabuf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction direction)
{
	struct iwfg_user_page_buf *user_buf_hndl = dmabuf->priv;

	dev_dbg(&user_buf_hndl->priv->pdev->dev, "DMA-BUF end CPU access\n");

	// Sync the buffer for device access
	if (user_buf_hndl->sg_mapped && user_buf_hndl->sg_host) {
		dma_sync_sg_for_device(&user_buf_hndl->priv->pdev->dev,
				       user_buf_hndl->sgl,
				       user_buf_hndl->sg_mapped_count,  // Use mapped count
				       DMA_BIDIRECTIONAL);
		user_buf_hndl->sg_host = false;
	}

	return 0;
}

static int iwfg_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct iwfg_user_page_buf *user_buf_hndl = dmabuf->priv;
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	size_t size = vma->vm_end - vma->vm_start;
	int i, ret = 0;

	dev_dbg(&user_buf_hndl->priv->pdev->dev, "DMA-BUF mmap called\n");

	if (offset + size > user_buf_hndl->usr_buf_size)
		return -EINVAL;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0))
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
#endif
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	// Map the pages to user space
	for (i = 0; i < user_buf_hndl->num_pages && size > 0; i++) {
		unsigned long pfn = page_to_pfn(user_buf_hndl->pgs[i]);
		size_t page_size = min_t(size_t, PAGE_SIZE, size);

		ret = remap_pfn_range(vma, addr, pfn, page_size, vma->vm_page_prot);
		if (ret)
			break;

		addr += page_size;
		size -= page_size;
	}

	return ret;
}

static const struct dma_buf_ops iwfg_dmabuf_ops = {
	.attach = iwfg_dmabuf_attach,
	.detach = iwfg_dmabuf_detach,
	.map_dma_buf = iwfg_dmabuf_map_dma_buf,
	.unmap_dma_buf = iwfg_dmabuf_unmap_dma_buf,
	.release = iwfg_dmabuf_release,
	.begin_cpu_access = iwfg_dmabuf_begin_cpu_access,
	.end_cpu_access = iwfg_dmabuf_end_cpu_access,
	.mmap = iwfg_dmabuf_mmap,
};

int iwfg_dmabuf_export(struct iwfg_user_page_buf *user_buf_hndl)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	size_t aligned_size;

	if (!user_buf_hndl || !user_buf_hndl->priv) {
		return -EINVAL;
	}

	// USERPTR buffers cannot be exported - they use pinned user pages
	// which are not safe for DMA-BUF sharing across processes
	if (user_buf_hndl->buf_type == IWFG_BUF_TYPE_USERPTR) {
		dev_err(&user_buf_hndl->priv->pdev->dev,
			"USERPTR buffers cannot be exported as DMA-BUF\n");
		return -EINVAL;
	}

	// DMA_IMPORT buffers also cannot be re-exported
	if (user_buf_hndl->buf_type == IWFG_BUF_TYPE_DMA_IMPORT) {
		dev_err(&user_buf_hndl->priv->pdev->dev,
			"Imported DMA-BUF cannot be re-exported\n");
		return -EINVAL;
	}

	if (user_buf_hndl->exported) {
		dev_warn(&user_buf_hndl->priv->pdev->dev,
			 "Buffer already exported as DMA-BUF\n");
		return -EEXIST;
	}

	if (!user_buf_hndl->sg_mapped) {
		dev_err(&user_buf_hndl->priv->pdev->dev,
			"Buffer must be DMA mapped before export\n");
		return -EINVAL;
	}

	// Ensure size is page-aligned for proper GEM object creation
	aligned_size = PAGE_ALIGN(user_buf_hndl->usr_buf_size);
	if (aligned_size != user_buf_hndl->usr_buf_size) {
		dev_warn(&user_buf_hndl->priv->pdev->dev,
			 "Buffer size %zu not page-aligned, using %zu\n",
			 user_buf_hndl->usr_buf_size, aligned_size);
	}

	// Initialize reservation object
	dma_resv_init(&user_buf_hndl->resv);

	// Setup export info
	exp_info.exp_name = "iwfg";
	exp_info.owner = THIS_MODULE;
	exp_info.ops = &iwfg_dmabuf_ops;
	exp_info.size = aligned_size;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.resv = &user_buf_hndl->resv;
	exp_info.priv = user_buf_hndl;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		dma_resv_fini(&user_buf_hndl->resv);
		dev_err(&user_buf_hndl->priv->pdev->dev,
			"Failed to export DMA-BUF: %ld (size=%zu, pages=%d, sg_nents=%d)\n", 
			PTR_ERR(dmabuf), aligned_size, user_buf_hndl->num_pages, 
			user_buf_hndl->sg_nents);
		return PTR_ERR(dmabuf);
	}

	user_buf_hndl->dmabuf = dmabuf;
	user_buf_hndl->exported = true;

	dev_info(&user_buf_hndl->priv->pdev->dev,
		 "DMA-BUF exported for buffer (size: %zu bytes, pages: %d, sg_nents: %d)\n",
		 aligned_size, user_buf_hndl->num_pages, user_buf_hndl->sg_nents);

	return 0;
}

int iwfg_get_dmabuf_fd(struct iwfg_private *priv, u32 buf_index)
{
	struct iwfg_user_page_buf *user_buf_hndl;
	int fd, count = 0;

	// Find the buffer by index
	list_for_each_entry(user_buf_hndl, &priv->dma_buf_list, list) {
		if (count == buf_index) {
			break;
		}
		count++;
	}

	if (count != buf_index || &user_buf_hndl->list == &priv->dma_buf_list) {
		dev_err(&priv->pdev->dev, "Invalid buffer index: %u\n", buf_index);
		return -EINVAL;
	}

	// Only DMA_ALLOC buffers can be exported
	if (user_buf_hndl->buf_type == IWFG_BUF_TYPE_USERPTR) {
		dev_err(&priv->pdev->dev, 
			"USERPTR buffer %u cannot be exported as DMA-BUF\n", buf_index);
		return -EINVAL;
	}

	if (user_buf_hndl->buf_type == IWFG_BUF_TYPE_DMA_IMPORT) {
		dev_err(&priv->pdev->dev, 
			"Imported DMA-BUF %u cannot be re-exported\n", buf_index);
		return -EINVAL;
	}

	// Export the buffer if not already exported
	if (!user_buf_hndl->exported) {
		int ret = iwfg_dmabuf_export(user_buf_hndl);
		if (ret) {
			return ret;
		}
	}

	// Get file descriptor - each call to dma_buf_fd() creates a new fd but uses the same dmabuf
	// The DMA-BUF subsystem handles reference counting automatically
	fd = dma_buf_fd(user_buf_hndl->dmabuf, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		dev_err(&priv->pdev->dev, "Failed to get DMA-BUF fd: %d\n", fd);
		return fd;
	}

	dev_dbg(&priv->pdev->dev, "DMA-BUF fd %d created for buffer %u\n", fd, buf_index);

	return fd;
}

#endif /* CONFIG_ARM || CONFIG_ARM64 */