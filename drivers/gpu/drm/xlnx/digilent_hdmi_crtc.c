/*
 * Digilent HDMI DRM driver
 *
 * Copyright 2012 Analog Devices Inc.
 * Copyright 2020 Digilent Inc.
 *
 * Authors:
 *  Lars-Peter Clausen <lars@metafoo.de>
 *  Cosmin Tanislav <demonsingur@gmail.com>
 *
 * Licensed under the GPL-2.
 */

#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/slab.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_vblank.h>
#include <video/videomode.h>

#include "digilent_hdmi_drv.h"

static struct digilent_hdmi_private *plane_to_private(struct drm_plane *plane)
{
	return container_of(plane, struct digilent_hdmi_private, plane);
}

static struct digilent_hdmi_private *to_private(struct drm_crtc *crtc)
{
	return container_of(crtc, struct digilent_hdmi_private, crtc);
}

static struct dma_async_tx_descriptor *digilent_hdmi_vdma_prep_interleaved_desc(
		struct drm_plane *plane)
{
	struct digilent_hdmi_private *private = plane_to_private(plane);
	struct drm_framebuffer *fb = plane->state->fb;
	size_t offset, hw_row_size;
	struct drm_gem_cma_object *obj;

	obj = drm_fb_cma_get_gem_obj(plane->state->fb, 0);

	offset = plane->state->crtc_x * fb->format->cpp[0] +
		plane->state->crtc_y * fb->pitches[0];

	/* Interleaved DMA is used that way:
	 * Each interleaved frame is a row (hsize) implemented in ONE
	 * chunk (sgl has len 1).
	 * The number of interleaved frames is the number of rows (vsize).
	 * The icg in used to pack data to the HW, so that the buffer len
	 * is fb->piches[0], but the actual size for the hw is somewhat less
	 */
	private->dma_template->dir = DMA_MEM_TO_DEV;
	private->dma_template->src_start = obj->paddr + offset;
	/* sgl list have just one entry (each interleaved frame have 1 chunk) */
	private->dma_template->frame_size = 1;
	/* the number of interleaved frame, each has the size specified in sgl */
	private->dma_template->numf = plane->state->crtc_h;
	private->dma_template->src_sgl = 1;
	private->dma_template->src_inc = 1;

	/* vdma IP does not provide any addr to the hdmi IP, so dst_inc
	 * and dst_sgl should make no any difference.
	 */
	private->dma_template->dst_inc = 0;
	private->dma_template->dst_sgl = 0;

	hw_row_size = plane->state->crtc_w * fb->format->cpp[0];
	private->dma_template->sgl[0].size = hw_row_size;

	/* the vdma driver seems to look at icg, and not src_icg */
	private->dma_template->sgl[0].icg =
		fb->pitches[0] - hw_row_size;

	return dmaengine_prep_interleaved_dma(private->dma,
			private->dma_template, DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
}

static void digilent_hdmi_plane_atomic_update(struct drm_plane *plane,
		struct drm_plane_state *old_state)
{
	struct digilent_hdmi_private *private = plane_to_private(plane);
	struct dma_async_tx_descriptor *desc;

	if (!plane->state->crtc || !plane->state->fb)
		return;

	dmaengine_terminate_all(private->dma);

	desc = digilent_hdmi_vdma_prep_interleaved_desc(plane);
	if (!desc) {
		DRM_ERROR("failed to prepare dma descriptor\n");
		return;
	}

	dmaengine_submit(desc);
	dma_async_issue_pending(private->dma);
}

static void digilent_hdmi_crtc_enable(struct drm_crtc *crtc,
		struct drm_crtc_state *old_state)
{
	struct drm_display_mode *adjusted_mode = &crtc->state->adjusted_mode;
	struct digilent_hdmi_private *private = to_private(crtc);
	struct videomode vm;
	int vrefresh;

	if (private->vtc_bridge) {
		/* set video timing */
		drm_display_mode_to_videomode(adjusted_mode, &vm);
		xlnx_bridge_set_timing(private->vtc_bridge, &vm);
		xlnx_bridge_enable(private->vtc_bridge);
	}

	/* Delay of 1 vblank interval for timing gen to be stable */
	vrefresh = (adjusted_mode->clock * 1000) /
		   (adjusted_mode->vtotal * adjusted_mode->htotal);
	msleep(1 * 1000 / vrefresh);
}

static void digilent_hdmi_crtc_disable(struct drm_crtc *crtc,
		struct drm_crtc_state *old_state)
{
	struct digilent_hdmi_private *private = to_private(crtc);

	xlnx_bridge_disable(private->vtc_bridge);

	dmaengine_terminate_all(private->dma);
}

static void digilent_hdmi_crtc_atomic_begin(struct drm_crtc *crtc,
	struct drm_crtc_state *state)
{
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (event) {
		crtc->state->event = NULL;

		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static const struct drm_crtc_helper_funcs digilent_hdmi_crtc_helper_funcs = {
	.atomic_enable = digilent_hdmi_crtc_enable,
	.atomic_disable = digilent_hdmi_crtc_disable,
	.atomic_begin = digilent_hdmi_crtc_atomic_begin,
};

static void digilent_hdmi_crtc_destroy(struct drm_crtc *crtc)
{
	struct digilent_hdmi_private *private = to_private(crtc);

	drm_crtc_cleanup(crtc);
	kfree(private->dma_template);
}

static const struct drm_crtc_funcs digilent_hdmi_crtc_funcs = {
	.destroy = digilent_hdmi_crtc_destroy,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_plane_helper_funcs digilent_hdmi_plane_helper_funcs = {
	.atomic_update = digilent_hdmi_plane_atomic_update,
};

static const struct drm_plane_funcs digilent_hdmi_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const u32 digilent_hdmi_supported_formats[] = {
	DRM_FORMAT_XRGB8888,
};

int digilent_hdmi_crtc_create(struct drm_device *ddev)
{
	struct digilent_hdmi_private *private = ddev->dev_private;
	struct drm_plane *plane = &private->plane;
	struct drm_crtc *crtc = &private->crtc;
	int ret;

	/* we know we'll always use only one data chunk */
	private->dma_template = kzalloc(
			sizeof(struct dma_interleaved_template) +
			sizeof(struct data_chunk), GFP_KERNEL);
	if (!private->dma_template) {
		DRM_ERROR("failed to allocate memory for DMA template\n");
		ret = -ENOMEM;
		goto dma_template_alloc_fail;
	}

	ret = drm_universal_plane_init(ddev, plane, 0xff,
			&digilent_hdmi_plane_funcs,
			digilent_hdmi_supported_formats,
			ARRAY_SIZE(digilent_hdmi_supported_formats), NULL,
			DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		DRM_ERROR("failed to initialize plane\n");
		goto plane_init_fail;
	}
	drm_plane_helper_add(plane, &digilent_hdmi_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(ddev, crtc, plane, NULL,
			&digilent_hdmi_crtc_funcs, NULL);
	if (ret) {
		DRM_ERROR("failed to initialize crtc\n");
		goto crtc_init_fail;
	}
	drm_crtc_helper_add(crtc, &digilent_hdmi_crtc_helper_funcs);

	return 0;

crtc_init_fail:
	drm_plane_cleanup(plane);
plane_init_fail:
	kfree(private->dma_template);
dma_template_alloc_fail:
	return ret;
}
