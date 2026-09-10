/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dc_nohw_present.c - optional in-kernel present path (Stage 6a).
 *
 * When enabled (module param dc_nohw.present=1), each DisplayClass swap drives
 * omapdrm's primary plane directly via the exported omapdrm_present(), with no
 * userspace presenter in the flip path. The dc_nohw back buffers are imported
 * once as omapdrm framebuffers. Phase 6a is a mailbox: ProcessFlip completes
 * the swap immediately and the commit is issued asynchronously to the latest
 * requested buffer (expected cross-buffer ghosting, fixed by 6b pacing).
 */

#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "dc_nohw_export.h"

/* Plain-typed accessors into dc_nohw internals (kept free of IMG headers). */
int DCNohwGetGeometry(unsigned int *width, unsigned int *height,
		      unsigned int *stride, unsigned int *count,
		      unsigned int *buffer_size);
struct dma_buf *dc_nohw_make_dmabuf(unsigned int index);
void DCNohwPresentInit(void);
void DCNohwPresentFlip(unsigned int index);
void DCNohwPresentTeardown(void);

/* omapdrm exported entry points (drivers/gpu/drm/omapdrm/omap_present.c). */
struct dma_buf;
struct drm_framebuffer;
extern struct drm_framebuffer *omapdrm_import_dmabuf(struct dma_buf *dbuf,
						     u32 width, u32 height,
						     u32 pitch, u32 fourcc);
extern void omapdrm_release_fb(struct drm_framebuffer *fb);
extern int omapdrm_present(struct drm_framebuffer *fb,
			   void (*flip_done)(void *cookie), void *cookie);

static bool dc_nohw_present_enabled;
module_param_named(present, dc_nohw_present_enabled, bool, 0644);
MODULE_PARM_DESC(present, "Stage 6a: drive omapdrm directly on each swap");

#define DC_NOHW_PRESENT_MAX 3

static struct {
	struct mutex lock;
	struct workqueue_struct *wq;
	struct work_struct work;
	struct drm_framebuffer *fb[DC_NOHW_PRESENT_MAX];
	unsigned int count;
	bool imported;
	int latest;			/* latest requested index, -1 = none */
} dc_nohw_present;

/* Import the dc_nohw back buffers as omapdrm framebuffers (once). */
static int dc_nohw_present_import_locked(void)
{
	unsigned int w = 0, h = 0, stride = 0, count = 0, bufsize = 0;
	unsigned int i;
	int ret;

	if (dc_nohw_present.imported)
		return 0;

	ret = DCNohwGetGeometry(&w, &h, &stride, &count, &bufsize);
	if (ret)
		return ret;
	if (count > DC_NOHW_PRESENT_MAX)
		count = DC_NOHW_PRESENT_MAX;

	for (i = 0; i < count; i++) {
		struct dma_buf *dbuf;
		struct drm_framebuffer *fb;

		dbuf = dc_nohw_make_dmabuf(i);
		if (IS_ERR(dbuf)) {
			ret = PTR_ERR(dbuf);
			goto fail;
		}

		fb = omapdrm_import_dmabuf(dbuf, w, h, stride,
					   DC_NOHW_EXPORT_FOURCC_ARGB8888);
		/* omapdrm took its own ref on the dma_buf on success. */
		dma_buf_put(dbuf);
		if (IS_ERR(fb)) {
			ret = PTR_ERR(fb);
			goto fail;
		}
		dc_nohw_present.fb[i] = fb;
	}

	dc_nohw_present.count = count;
	dc_nohw_present.imported = true;
	return 0;

fail:
	while (i--) {
		omapdrm_release_fb(dc_nohw_present.fb[i]);
		dc_nohw_present.fb[i] = NULL;
	}
	return ret;
}

static void dc_nohw_present_work(struct work_struct *w)
{
	struct drm_framebuffer *fb = NULL;
	int idx;

	mutex_lock(&dc_nohw_present.lock);
	if (dc_nohw_present_import_locked() == 0) {
		idx = dc_nohw_present.latest;
		if (idx >= 0 && (unsigned int)idx < dc_nohw_present.count)
			fb = dc_nohw_present.fb[idx];
	}
	mutex_unlock(&dc_nohw_present.lock);

	if (fb)
		omapdrm_present(fb, NULL, NULL);
}

void DCNohwPresentInit(void)
{
	mutex_init(&dc_nohw_present.lock);
	INIT_WORK(&dc_nohw_present.work, dc_nohw_present_work);
	dc_nohw_present.latest = -1;
	dc_nohw_present.wq = alloc_ordered_workqueue("dc_nohw_present", 0);
}

void DCNohwPresentFlip(unsigned int index)
{
	if (!dc_nohw_present_enabled || !dc_nohw_present.wq)
		return;

	mutex_lock(&dc_nohw_present.lock);
	dc_nohw_present.latest = (int)index;
	mutex_unlock(&dc_nohw_present.lock);

	queue_work(dc_nohw_present.wq, &dc_nohw_present.work);
}

void DCNohwPresentTeardown(void)
{
	unsigned int i;

	if (dc_nohw_present.wq) {
		flush_workqueue(dc_nohw_present.wq);
		destroy_workqueue(dc_nohw_present.wq);
		dc_nohw_present.wq = NULL;
	}

	mutex_lock(&dc_nohw_present.lock);
	for (i = 0; i < dc_nohw_present.count; i++) {
		omapdrm_release_fb(dc_nohw_present.fb[i]);
		dc_nohw_present.fb[i] = NULL;
	}
	dc_nohw_present.count = 0;
	dc_nohw_present.imported = false;
	mutex_unlock(&dc_nohw_present.lock);
}

