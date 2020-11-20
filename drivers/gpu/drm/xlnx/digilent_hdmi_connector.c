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

#include <drm/drm_atomic_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "digilent_hdmi_drv.h"

static inline struct digilent_hdmi_private *to_private(struct drm_connector *connector)
{
	return container_of(connector, struct digilent_hdmi_private, connector);
}

static int digilent_hdmi_connector_get_modes(struct drm_connector *connector)
{
	struct digilent_hdmi_private *private = to_private(connector);
	struct edid *edid;
	int count = 0;

	if (private->i2c_bus) {
		edid = drm_get_edid(connector, private->i2c_bus);
		if (!edid) {
			DRM_ERROR("failed to get EDIT data from I2C bus\n");
			return 0;
		}

		/*
		 * Other drivers tend to call update edid property after the call to
		 * drm_add_edid_modes. If problems with modesetting, this could be why.
		 */
		drm_connector_update_edid_property(connector, edid);
		count = drm_add_edid_modes(connector, edid);
		kfree(edid);
	} else {
		count = drm_add_modes_noedid(connector, private->hmax, private->vmax);
		drm_set_preferred_mode(connector, private->hpref, private->vpref);
	}

	return count;
}

static int digilent_hdmi_connector_mode_valid(struct drm_connector *connector,
		struct drm_display_mode *mode)
{
	struct digilent_hdmi_private *private = to_private(connector);

	if (mode && !(mode->flags & (DRM_MODE_FLAG_INTERLACE |
					DRM_MODE_FLAG_DBLCLK | DRM_MODE_FLAG_3D_MASK))
			&& mode->clock <= private->fmax
			&& mode->hdisplay <= private->hmax
			&& mode->vdisplay <= private->vmax) {
		return MODE_OK;
	}

	return MODE_BAD;
}

static struct drm_encoder *digilent_hdmi_best_encoder(struct drm_connector *connector)
{
	return &to_private(connector)->encoder;
}

static struct drm_connector_helper_funcs digilent_hdmi_connector_helper_funcs = {
	.get_modes	= digilent_hdmi_connector_get_modes,
	.mode_valid	= digilent_hdmi_connector_mode_valid,
	.best_encoder	= digilent_hdmi_best_encoder,
};

static enum drm_connector_status digilent_hdmi_connector_detect(
		struct drm_connector *connector, bool force)
{
	struct digilent_hdmi_private *private = to_private(connector);

	if (private->i2c_bus) {
		if (drm_probe_ddc(private->i2c_bus))
			return connector_status_connected;

		return connector_status_disconnected;
	}

	return connector_status_unknown;
}

static void digilent_hdmi_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static struct drm_connector_funcs digilent_hdmi_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = digilent_hdmi_connector_detect,
	.destroy = digilent_hdmi_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

int digilent_hdmi_connector_create(struct drm_device *ddev) {
	struct digilent_hdmi_private *private = ddev->dev_private;
	struct drm_connector *connector = &private->connector;
	struct drm_encoder *encoder = &private->encoder;
	int ret;

	connector->polled = DRM_CONNECTOR_POLL_CONNECT |
				DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_connector_init(ddev, connector,
			&digilent_hdmi_connector_funcs,
			DRM_MODE_CONNECTOR_HDMIA);
	if (ret) {
		DRM_ERROR("failed to init connector\n");
		goto register_connector_init_fail;
	}
	drm_connector_helper_add(connector, &digilent_hdmi_connector_helper_funcs);

	ret = drm_connector_register(connector);
	if (ret) {
		DRM_ERROR("failed to register connector\n");
		goto register_connector_register_fail;
	}

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret) {
		DRM_ERROR("failed to attach encoder to connector\n");
		goto attach_encoder_fail;
	}

	return 0;

attach_encoder_fail:
	drm_connector_unregister(connector);
register_connector_register_fail:
	drm_connector_cleanup(connector);
register_connector_init_fail:
	return ret;
}
