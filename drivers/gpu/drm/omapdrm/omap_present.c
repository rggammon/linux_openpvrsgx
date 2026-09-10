// SPDX-License-Identifier: GPL-2.0-only
/*
 * omap_present.c - in-kernel present entry points for external DisplayClass
 * providers (e.g. the PowerVR dc_nohw bridge).
 *
 * These let another in-tree/appliance module hand omapdrm a DMA-BUF-backed
 * framebuffer and drive the primary plane directly, without a userspace DRM
 * master in the flip path. Import wraps a foreign DMA-BUF as an omapdrm
 * framebuffer; present commits it to the active CRTC's primary plane.
 */

#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/export.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modeset_lock.h>

#include "omap_drv.h"

/* Set in omapdrm_init(), cleared in omapdrm_cleanup() (omap_drv.c). */
struct drm_device *omapdrm_global_ddev;

/*
 * Wrap a foreign DMA-BUF (e.g. a dc_nohw contiguous CMA back buffer) as an
 * omapdrm framebuffer. On success the returned fb holds a reference on the
 * imported GEM object, which in turn holds a reference on @dbuf, so the caller
 * may drop its own @dbuf reference afterwards.
 */
struct drm_framebuffer *omapdrm_import_dmabuf(struct dma_buf *dbuf,
					      u32 width, u32 height,
					      u32 pitch, u32 fourcc)
{
	struct drm_device *dev = omapdrm_global_ddev;
	const struct drm_format_info *info;
	struct drm_mode_fb_cmd2 cmd = {0};
	struct drm_gem_object *bo;
	struct drm_framebuffer *fb;

	if (!dev || !dbuf)
		return ERR_PTR(-ENODEV);

	info = drm_format_info(fourcc);
	if (!info)
		return ERR_PTR(-EINVAL);

	bo = omap_gem_prime_import(dev, dbuf);
	if (IS_ERR(bo))
		return ERR_CAST(bo);

	cmd.width = width;
	cmd.height = height;
	cmd.pixel_format = fourcc;
	cmd.pitches[0] = pitch;

	fb = omap_framebuffer_init(dev, info, &cmd, &bo);
	if (IS_ERR(fb)) {
		drm_gem_object_put(bo);
		return fb;
	}

	return fb;
}
EXPORT_SYMBOL_GPL(omapdrm_import_dmabuf);

void omapdrm_release_fb(struct drm_framebuffer *fb)
{
	if (fb)
		drm_framebuffer_put(fb);
}
EXPORT_SYMBOL_GPL(omapdrm_release_fb);

static struct drm_crtc *omap_present_active_crtc(struct drm_device *dev)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, dev)
		if (crtc->state && crtc->state->active)
			return crtc;

	drm_for_each_crtc(crtc, dev)
		if (crtc->enabled)
			return crtc;

	return NULL;
}

/*
 * Commit @fb to the active CRTC's primary plane. @flip_done (if non-NULL) is
 * invoked once the commit has been issued; Phase 6a uses a blocking commit and
 * a fire-and-forget completion (mailbox). The commit changes only the primary
 * plane fb, not the mode, so the existing pipeline (set up by fbcon before a
 * parking master suspended it) is reused.
 */
int omapdrm_present(struct drm_framebuffer *fb,
		    void (*flip_done)(void *cookie), void *cookie)
{
	struct drm_device *dev = omapdrm_global_ddev;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_plane_state *pstate;
	struct drm_crtc *crtc;
	int ret;

	if (!dev || !fb)
		return -ENODEV;

	crtc = omap_present_active_crtc(dev);
	if (!crtc)
		return -ENODEV;

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE,
				   ret);

	state = drm_atomic_commit_alloc(dev);
	if (!state) {
		ret = -ENOMEM;
	} else {
		state->acquire_ctx = &ctx;

		pstate = drm_atomic_get_plane_state(state, crtc->primary);
		if (IS_ERR(pstate)) {
			ret = PTR_ERR(pstate);
		} else {
			ret = drm_atomic_set_crtc_for_plane(pstate, crtc);
			if (!ret) {
				drm_atomic_set_fb_for_plane(pstate, fb);
				pstate->crtc_x = 0;
				pstate->crtc_y = 0;
				pstate->crtc_w = crtc->state->mode.hdisplay;
				pstate->crtc_h = crtc->state->mode.vdisplay;
				pstate->src_x = 0;
				pstate->src_y = 0;
				pstate->src_w = fb->width << 16;
				pstate->src_h = fb->height << 16;

				ret = drm_atomic_commit(state);
			}
		}

		drm_atomic_commit_put(state);
	}

	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);

	if (!ret && flip_done)
		flip_done(cookie);

	return ret;
}
EXPORT_SYMBOL_GPL(omapdrm_present);

