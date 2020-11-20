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

#include <linux/clk.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_print.h>

#include "digilent_hdmi_drv.h"

static inline struct digilent_hdmi_private *to_private(struct drm_encoder *encoder)
{
	return container_of(encoder, struct digilent_hdmi_private, encoder);
}

static void digilent_hdmi_encoder_enable(struct drm_encoder *encoder)
{
	struct digilent_hdmi_private *private = to_private(encoder);

	if (!private->clk_enabled) {
		clk_prepare_enable(private->hdmi_clock);
		private->clk_enabled = true;
	}
}

static void digilent_hdmi_encoder_disable(struct drm_encoder *encoder)
{
	struct digilent_hdmi_private *private = to_private(encoder);

	if (private->clk_enabled) {
		clk_disable_unprepare(private->hdmi_clock);
		private->clk_enabled = false;
	}
}

static void digilent_hdmi_encoder_mode_set(struct drm_encoder *encoder,
		struct drm_crtc_state *crtc_state,
		struct drm_connector_state *conn_state)
{
	struct digilent_hdmi_private *private = to_private(encoder);
	struct drm_display_mode *mode = &crtc_state->mode;

	clk_set_rate(private->hdmi_clock, mode->clock * 1000);
}

static const struct drm_encoder_helper_funcs digilent_hdmi_encoder_helper_funcs = {
	.enable = digilent_hdmi_encoder_enable,
	.disable = digilent_hdmi_encoder_disable,
	.atomic_mode_set = digilent_hdmi_encoder_mode_set,
};

static const struct drm_encoder_funcs digilent_hdmi_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int digilent_hdmi_encoder_create(struct drm_device *ddev)
{
	struct digilent_hdmi_private *private = ddev->dev_private;
	struct drm_encoder *encoder = &private->encoder;
	int ret;

	encoder->possible_crtcs = 1;

	ret = drm_encoder_init(ddev, encoder, &digilent_hdmi_encoder_funcs,
			DRM_MODE_ENCODER_TMDS, NULL);
	if (ret) {
		DRM_ERROR("failed to initialize DRM encoder\n");
		return ret;
	}
	drm_encoder_helper_add(encoder, &digilent_hdmi_encoder_helper_funcs);

	return 0;
}
