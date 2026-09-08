/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dc_nohw DMA-BUF exporter.
 *
 * Exports the contiguous CMA swapchain back buffers as DMA-BUF FDs through a
 * small miscdevice. The buffers are allocated by dc_nohw at device init and
 * live for the module's lifetime, so a live export only needs to pin the
 * module (dma_buf owner = THIS_MODULE) to satisfy the ownership invariant --
 * no per-buffer deferred-free machinery is required.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/err.h>

#include "dc_nohw_export.h"

MODULE_IMPORT_NS("DMA_BUF");

/* Plain-typed accessors into dc_nohw internals (kept free of IMG headers). */
int DCNohwExportInit(void);
void DCNohwExportDeinit(void);
struct device *DCNohwGetDev(void);
int DCNohwGetGeometry(unsigned int *width, unsigned int *height,
		      unsigned int *stride, unsigned int *count,
		      unsigned int *buffer_size);
int DCNohwGetBufferInfo(unsigned int index, void **cpu_vaddr,
			unsigned int *dma_addr, unsigned int *size);

struct dc_nohw_dmabuf {
	struct device *dev;
	void *cpu_vaddr;
	dma_addr_t dma_addr;
	size_t size;
};

static struct sg_table *dc_nohw_map(struct dma_buf_attachment *attach,
				    enum dma_data_direction dir)
{
	struct dc_nohw_dmabuf *b = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(b->dev, sgt, b->cpu_vaddr, b->dma_addr, b->size);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	ret = dma_map_sgtable(attach->dev, sgt, dir, 0);
	if (ret) {
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(ret);
	}

	return sgt;
}

static void dc_nohw_unmap(struct dma_buf_attachment *attach,
			  struct sg_table *sgt, enum dma_data_direction dir)
{
	dma_unmap_sgtable(attach->dev, sgt, dir, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static int dc_nohw_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct dc_nohw_dmabuf *b = dmabuf->priv;

	return dma_mmap_coherent(b->dev, vma, b->cpu_vaddr, b->dma_addr, b->size);
}

/* Backing is non-cached coherent memory: no CPU cache maintenance needed. */
static int dc_nohw_begin_cpu(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	return 0;
}

static int dc_nohw_end_cpu(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	return 0;
}

static void dc_nohw_dmabuf_release(struct dma_buf *dmabuf)
{
	kfree(dmabuf->priv);
}

static const struct dma_buf_ops dc_nohw_dmabuf_ops = {
	.map_dma_buf = dc_nohw_map,
	.unmap_dma_buf = dc_nohw_unmap,
	.mmap = dc_nohw_mmap,
	.begin_cpu_access = dc_nohw_begin_cpu,
	.end_cpu_access = dc_nohw_end_cpu,
	.release = dc_nohw_dmabuf_release,
};

static int dc_nohw_export_one(unsigned int index)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dc_nohw_dmabuf *b;
	struct dma_buf *dmabuf;
	struct device *dev;
	void *cpu_vaddr;
	unsigned int dma_addr = 0, size = 0;
	int fd, ret;

	dev = DCNohwGetDev();
	if (!dev)
		return -ENODEV;

	ret = DCNohwGetBufferInfo(index, &cpu_vaddr, &dma_addr, &size);
	if (ret)
		return ret;
	if (!cpu_vaddr || !size)
		return -ENOENT;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	b->cpu_vaddr = cpu_vaddr;
	b->dma_addr = (dma_addr_t)dma_addr;
	b->size = size;

	exp_info.exp_name = "dc_nohw";
	exp_info.owner = THIS_MODULE;
	exp_info.ops = &dc_nohw_dmabuf_ops;
	exp_info.size = size;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv = b;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		kfree(b);
		return PTR_ERR(dmabuf);
	}

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);

	return fd;
}

static long dc_nohw_export_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case DC_NOHW_EXPORT_QUERY_ABI: {
		struct dc_nohw_export_abi abi;
		unsigned int w = 0, h = 0, stride = 0, count = 0, bufsize = 0;
		int ret;

		ret = DCNohwGetGeometry(&w, &h, &stride, &count, &bufsize);
		if (ret)
			return ret;

		memset(&abi, 0, sizeof(abi));
		abi.abi_version = DC_NOHW_EXPORT_ABI_VERSION;
		abi.width = w;
		abi.height = h;
		abi.stride = stride;
		abi.fourcc = DC_NOHW_EXPORT_FOURCC_ARGB8888;
		abi.buffer_count = count;
		abi.buffer_size = bufsize;

		if (copy_to_user(uarg, &abi, sizeof(abi)))
			return -EFAULT;
		return 0;
	}
	case DC_NOHW_EXPORT_BUFFER: {
		struct dc_nohw_export_buffer req;
		int fd;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		if (req.flags)
			return -EINVAL;

		fd = dc_nohw_export_one(req.index);
		if (fd < 0)
			return fd;

		req.fd = fd;
		req.reserved = 0;
		if (copy_to_user(uarg, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations dc_nohw_export_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = dc_nohw_export_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice dc_nohw_export_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "dc_nohw_export",
	.fops = &dc_nohw_export_fops,
};

int DCNohwExportInit(void)
{
	return misc_register(&dc_nohw_export_misc);
}

void DCNohwExportDeinit(void)
{
	misc_deregister(&dc_nohw_export_misc);
}
