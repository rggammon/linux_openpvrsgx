/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dc_nohw_present.c - optional in-kernel present path.
 *
 * Module param dc_nohw.present selects the mode:
 *   0 = off (dc_nohw completes each swap inline, as originally).
 *   1 = mailbox (Stage 6a): completion stays inline; a workqueue presents the
 *       latest completed buffer per swap. Cross-buffer ghosting is expected.
 *   2 = paced (Stage 6b): completion is DEFERRED to the present worker, which
 *       does a blocking atomic commit (the flip) and then completes the
 *       PREVIOUS buffer's swap command (off-by-one) — the previously displayed
 *       buffer has stopped scanning, so freeing it is safe. This restores the
 *       FREE->RENDERING->READY->QUEUED->SCANNING->FREE protocol (no ghosting,
 *       vsync back-pressure), matching how omaplfb completes a flip on the
 *       vsync after it is programmed.
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
void DCNohwCompleteFlip(void *cookie);
int DCNohwPresentFlip(unsigned int index, void *cookie);
void DCNohwPresentInit(void);
void DCNohwPresentFlush(void);
void DCNohwPresentTeardown(void);

/* omapdrm exported entry points (drivers/gpu/drm/omapdrm/omap_present.c). */
struct drm_framebuffer;
extern struct drm_framebuffer *omapdrm_import_dmabuf(struct dma_buf *dbuf,
						     u32 width, u32 height,
						     u32 pitch, u32 fourcc);
extern void omapdrm_release_fb(struct drm_framebuffer *fb);
extern int omapdrm_present(struct drm_framebuffer *fb,
			   void (*flip_done)(void *cookie), void *cookie);

static int dc_nohw_present_mode;
module_param_named(present, dc_nohw_present_mode, int, 0644);
MODULE_PARM_DESC(present, "in-kernel present: 0=off, 1=mailbox, 2=paced");

#define DC_NOHW_PRESENT_MAX 3
#define DC_NOHW_PRESENT_QLEN 8u

struct dc_nohw_flip {
	void *cookie;
	unsigned int index;
};

static struct {
	struct mutex lock;
	struct workqueue_struct *wq;
	struct work_struct work;
	struct drm_framebuffer *fb[DC_NOHW_PRESENT_MAX];
	unsigned int count;
	bool imported;
	int latest;			/* mailbox (mode 1) */
	struct dc_nohw_flip q[DC_NOHW_PRESENT_QLEN];	/* paced FIFO (mode 2) */
	unsigned int head, tail;
} P;

/* Import the dc_nohw back buffers as omapdrm framebuffers (once). Caller holds P.lock. */
static int present_import_locked(void)
{
	unsigned int w = 0, h = 0, stride = 0, count = 0, bufsize = 0;
	unsigned int i;
	int ret;

	if (P.imported)
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
		dma_buf_put(dbuf);	/* omapdrm took its own ref on success */
		if (IS_ERR(fb)) {
			ret = PTR_ERR(fb);
			goto fail;
		}
		P.fb[i] = fb;
	}
	P.count = count;
	P.imported = true;
	return 0;
fail:
	while (i--) {
		omapdrm_release_fb(P.fb[i]);
		P.fb[i] = NULL;
	}
	return ret;
}

static void present_paced(void)
{
	for (;;) {
		struct dc_nohw_flip f;
		struct drm_framebuffer *fb = NULL;

		mutex_lock(&P.lock);
		if (P.head == P.tail) {
			mutex_unlock(&P.lock);
			return;
		}
		f = P.q[P.head % DC_NOHW_PRESENT_QLEN];
		P.head++;
		if (present_import_locked() == 0 && f.index < P.count)
			fb = P.fb[f.index];
		mutex_unlock(&P.lock);

		if (fb)
			omapdrm_present(fb, NULL, NULL);	/* blocking: f.index now scanning */

		/*
		 * Completing this swap frees the PREVIOUSLY displayed buffer
		 * (the one f.index just replaced), now off-screen — so completing
		 * here, after the flip, is safe (no reuse-while-scanning) and does
		 * not withhold the swap (no deadlock).
		 */
		DCNohwCompleteFlip(f.cookie);
	}
}

static void present_mailbox(void)
{
	struct drm_framebuffer *fb = NULL;
	int idx;

	mutex_lock(&P.lock);
	if (present_import_locked() == 0) {
		idx = P.latest;
		if (idx >= 0 && (unsigned int)idx < P.count)
			fb = P.fb[idx];
	}
	mutex_unlock(&P.lock);
	if (fb)
		omapdrm_present(fb, NULL, NULL);
}

static void present_work(struct work_struct *w)
{
	if (dc_nohw_present_mode == 2)
		present_paced();
	else
		present_mailbox();
}

void DCNohwPresentInit(void)
{
	mutex_init(&P.lock);
	INIT_WORK(&P.work, present_work);
	P.latest = -1;
	P.head = P.tail = 0;
	P.wq = alloc_ordered_workqueue("dc_nohw_present", 0);
}

/* Returns 1 if completion is deferred to the worker (paced), 0 otherwise. */
int DCNohwPresentFlip(unsigned int index, void *cookie)
{
	if (!P.wq)
		return 0;

	if (dc_nohw_present_mode == 2) {
		int deferred = 1;

		mutex_lock(&P.lock);
		if (P.tail - P.head < DC_NOHW_PRESENT_QLEN) {
			P.q[P.tail % DC_NOHW_PRESENT_QLEN].cookie = cookie;
			P.q[P.tail % DC_NOHW_PRESENT_QLEN].index = index;
			P.tail++;
		} else {
			deferred = 0;	/* queue full: let caller complete inline */
		}
		mutex_unlock(&P.lock);
		if (deferred)
			queue_work(P.wq, &P.work);
		return deferred;
	}

	if (dc_nohw_present_mode == 1) {
		mutex_lock(&P.lock);
		P.latest = (int)index;
		mutex_unlock(&P.lock);
		queue_work(P.wq, &P.work);
	}
	return 0;
}

/* Release the imported framebuffers and reset the queue. Called on swapchain
 * destroy and at teardown. Releasing the fbs here (not at module unload) is
 * essential: each imported fb holds the dc_nohw dma_buf, whose owner=THIS_MODULE
 * pins dcnohw — keeping them until unload would make dcnohw impossible to rmmod.
 * The buffers are module-scope, so the next session re-imports them.
 */
void DCNohwPresentFlush(void)
{
	struct drm_framebuffer *fbs[DC_NOHW_PRESENT_MAX];
	unsigned int i, n;

	if (P.wq)
		flush_workqueue(P.wq);
	mutex_lock(&P.lock);
	P.head = P.tail = 0;
	P.latest = -1;
	n = P.count;
	for (i = 0; i < n; i++) {
		fbs[i] = P.fb[i];
		P.fb[i] = NULL;
	}
	P.count = 0;
	P.imported = false;
	mutex_unlock(&P.lock);

	for (i = 0; i < n; i++)
		omapdrm_release_fb(fbs[i]);
}

void DCNohwPresentTeardown(void)
{
	DCNohwPresentFlush();
	if (P.wq) {
		destroy_workqueue(P.wq);
		P.wq = NULL;
	}
}

