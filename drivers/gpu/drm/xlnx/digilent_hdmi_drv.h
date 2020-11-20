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

#ifndef _DIGILENT_HDMI_DRV_H_
#define _DIGILENT_HDMI_DRV_H_

#include <drm/drm.h>
#include "xlnx_bridge.h"

struct digilent_hdmi_private {
	struct drm_device *drm_dev;

	struct drm_crtc crtc;
	struct drm_plane plane;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct dma_chan *dma;
	struct dma_interleaved_template *dma_template;

	struct i2c_adapter *i2c_bus;
	u32 fmax;
	u32 hmax;
	u32 vmax;
	u32 hpref;
	u32 vpref;

	struct xlnx_bridge *vtc_bridge;

	struct clk *hdmi_clock;
	bool clk_enabled;
};

int digilent_hdmi_crtc_create(struct drm_device *dev);
int digilent_hdmi_encoder_create(struct drm_device *dev);
int digilent_hdmi_connector_create(struct drm_device *ddev);

#endif
