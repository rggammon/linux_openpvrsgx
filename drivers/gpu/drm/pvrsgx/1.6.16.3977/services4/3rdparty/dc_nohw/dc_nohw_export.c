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
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

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
void DCNohwNotifySwap(unsigned int index);
void DCNohwNotifySwapchain(int create, unsigned int buffer_count);

/* ---- Swap-notify: a pollable per-open swap event stream (ABI v2) ---- */

#define DC_NOHW_EV_RING 64u

struct dc_nohw_sub {
        struct list_head list;
        wait_queue_head_t wq;
        spinlock_t lock;
        unsigned int head;      /* free-running write counter */
        unsigned int tail;      /* free-running read counter */
        struct dc_nohw_export_event ev[DC_NOHW_EV_RING];
};

static LIST_HEAD(dc_nohw_subs);
static DEFINE_SPINLOCK(dc_nohw_subs_lock);
static atomic_t dc_nohw_swap_seq = ATOMIC_INIT(0);

static void dc_nohw_broadcast(const struct dc_nohw_export_event *e)
{
        struct dc_nohw_sub *s;
        unsigned long f0, f1;

        spin_lock_irqsave(&dc_nohw_subs_lock, f0);
        list_for_each_entry(s, &dc_nohw_subs, list) {
                spin_lock_irqsave(&s->lock, f1);
                if (s->head - s->tail >= DC_NOHW_EV_RING)
                        s->tail++;              /* drop oldest (mailbox) */
                s->ev[s->head % DC_NOHW_EV_RING] = *e;
                s->head++;
                spin_unlock_irqrestore(&s->lock, f1);
                wake_up_interruptible(&s->wq);
        }
        spin_unlock_irqrestore(&dc_nohw_subs_lock, f0);
}

void DCNohwNotifySwap(unsigned int index)
{
        struct dc_nohw_export_event e;

        memset(&e, 0, sizeof(e));
        e.type = DC_NOHW_EVENT_SWAP;
        e.index = index;
        e.seq = (__u32)atomic_inc_return(&dc_nohw_swap_seq);
        dc_nohw_broadcast(&e);
}

void DCNohwNotifySwapchain(int create, unsigned int buffer_count)
{
        struct dc_nohw_export_event e;

        memset(&e, 0, sizeof(e));
        e.type = create ? DC_NOHW_EVENT_SWAPCHAIN_CREATE
                        : DC_NOHW_EVENT_SWAPCHAIN_DESTROY;
        e.index = buffer_count;
        dc_nohw_broadcast(&e);
}

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

struct dma_buf *dc_nohw_make_dmabuf(unsigned int index)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dc_nohw_dmabuf *b;
	struct dma_buf *dmabuf;
	struct device *dev;
	void *cpu_vaddr;
	unsigned int dma_addr = 0, size = 0;
	int ret;

	dev = DCNohwGetDev();
	if (!dev)
		return ERR_PTR(-ENODEV);

	ret = DCNohwGetBufferInfo(index, &cpu_vaddr, &dma_addr, &size);
	if (ret)
		return ERR_PTR(ret);
	if (!cpu_vaddr || !size)
		return ERR_PTR(-ENOENT);

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return ERR_PTR(-ENOMEM);
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
		return dmabuf;
	}
	return dmabuf;
}

static int dc_nohw_export_one(unsigned int index)
{
	struct dma_buf *dmabuf;
	int fd;

	dmabuf = dc_nohw_make_dmabuf(index);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

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
        case DC_NOHW_EXPORT_SUBSCRIBE: {
                struct dc_nohw_sub *s = file->private_data;
                unsigned long f;

                if (s)
                        return 0;               /* already subscribed */
                s = kzalloc(sizeof(*s), GFP_KERNEL);
                if (!s)
                        return -ENOMEM;
                init_waitqueue_head(&s->wq);
                spin_lock_init(&s->lock);
                spin_lock_irqsave(&dc_nohw_subs_lock, f);
                list_add(&s->list, &dc_nohw_subs);
                spin_unlock_irqrestore(&dc_nohw_subs_lock, f);
                file->private_data = s;
                return 0;
        }
        default:
                return -ENOTTY;
        }
}

static int dc_nohw_export_open(struct inode *inode, struct file *file)
{
        file->private_data = NULL;
        return 0;
}

static __poll_t dc_nohw_export_poll(struct file *file, poll_table *wait)
{
        struct dc_nohw_sub *s = file->private_data;
        __poll_t mask = 0;
        unsigned long f;

        if (!s)
                return 0;
        poll_wait(file, &s->wq, wait);
        spin_lock_irqsave(&s->lock, f);
        if (s->head != s->tail)
                mask |= EPOLLIN | EPOLLRDNORM;
        spin_unlock_irqrestore(&s->lock, f);
        return mask;
}

static ssize_t dc_nohw_export_read(struct file *file, char __user *buf,
                                   size_t count, loff_t *ppos)
{
        struct dc_nohw_sub *s = file->private_data;
        unsigned long f;
        size_t done = 0;

        if (!s)
                return -EINVAL;
        if (count < sizeof(struct dc_nohw_export_event))
                return -EINVAL;

        spin_lock_irqsave(&s->lock, f);
        while (s->head == s->tail) {
                spin_unlock_irqrestore(&s->lock, f);
                if (file->f_flags & O_NONBLOCK)
                        return -EAGAIN;
                if (wait_event_interruptible(s->wq, s->head != s->tail))
                        return -ERESTARTSYS;
                spin_lock_irqsave(&s->lock, f);
        }
        while (s->head != s->tail &&
               done + sizeof(struct dc_nohw_export_event) <= count) {
                struct dc_nohw_export_event e = s->ev[s->tail % DC_NOHW_EV_RING];

                s->tail++;
                spin_unlock_irqrestore(&s->lock, f);
                if (copy_to_user(buf + done, &e, sizeof(e)))
                        return -EFAULT;
                done += sizeof(e);
                spin_lock_irqsave(&s->lock, f);
        }
        spin_unlock_irqrestore(&s->lock, f);
        return (ssize_t)done;
}

static int dc_nohw_export_release(struct inode *inode, struct file *file)
{
        struct dc_nohw_sub *s = file->private_data;
        unsigned long f;

        if (s) {
                spin_lock_irqsave(&dc_nohw_subs_lock, f);
                list_del(&s->list);
                spin_unlock_irqrestore(&dc_nohw_subs_lock, f);
                kfree(s);
                file->private_data = NULL;
        }
        return 0;
}

static const struct file_operations dc_nohw_export_fops = {
        .owner = THIS_MODULE,
        .open = dc_nohw_export_open,
        .release = dc_nohw_export_release,
        .unlocked_ioctl = dc_nohw_export_ioctl,
        .poll = dc_nohw_export_poll,
        .read = dc_nohw_export_read,
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
