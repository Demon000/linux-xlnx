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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>
#include <linux/clk.h>

#include <drm/drmP.h>
#include <drm/drm.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>

#include "digilent_hdmi_drv.h"

#define DRIVER_NAME	"digilent_hdmi_drm"
#define DRIVER_DESC	"DIGILENT HDMI DRM"
#define DRIVER_DATE	"20120930"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

#define DIGILENT_ENC_MAX_FREQ 150000
#define DIGILENT_ENC_MAX_H 1920
#define DIGILENT_ENC_MAX_V 1080
#define DIGILENT_ENC_PREF_H 1280
#define DIGILENT_ENC_PREF_V 720

static struct drm_mode_config_funcs digilent_hdmi_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.output_poll_changed = drm_fb_helper_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void digilent_hdmi_unload(struct drm_device *dev)
{
	drm_kms_helper_poll_fini(dev);
	drm_mode_config_cleanup(dev);
}

static const struct file_operations digilent_hdmi_driver_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.mmap		= drm_gem_cma_mmap,
	.poll		= drm_poll,
	.read		= drm_read,
	.unlocked_ioctl	= drm_ioctl,
	.release	= drm_release,
};

static struct drm_driver digilent_hdmi_drm_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.unload			= digilent_hdmi_unload,
	.lastclose		= drm_fb_helper_lastclose,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap		= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,
	.dumb_create		= drm_gem_cma_dumb_create,
	.gem_free_object	= drm_gem_cma_free_object,
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	.dumb_create		= drm_gem_cma_dumb_create,
	.fops			= &digilent_hdmi_driver_fops,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.date			= DRIVER_DATE,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
};

static int digilent_hdmi_parse_of(struct platform_device *pdev)
{
	struct digilent_hdmi_private *private = platform_get_drvdata(pdev);
	struct device_node *np = pdev->dev.of_node;
	struct device_node *sub_node;
	int ret;

	private->hdmi_clock = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(private->hdmi_clock)) {
		ret = PTR_ERR(private->hdmi_clock);
		DRM_ERROR("failed to find HDMI clock: %d\n", ret);
		goto clk_get_fail;
	}

	private->dma = dma_request_slave_channel_reason(&pdev->dev, "video");
	if (IS_ERR(private->dma)) {
		ret = PTR_ERR(private->dma);
		DRM_ERROR("DMA channel not ready: %d\n", ret);
		goto dma_request_fail;
	}

	sub_node = of_parse_phandle(np, "vtc", 0);
	if (sub_node) {
		private->vtc_bridge = of_xlnx_bridge_get(sub_node);
		of_node_put(sub_node);

		if (!private->vtc_bridge) {
			ret = -EPROBE_DEFER;
			DRM_ERROR("VTC bridge instance not found: %d\n", ret);
			goto vtc_bridge_get_fail;
		}
	} else {
		DRM_ERROR("VTC property not found\n");
	}

	sub_node = of_parse_phandle(np, "edid-i2c", 0);
	if (sub_node) {
		private->i2c_bus = of_find_i2c_adapter_by_node(sub_node);
		of_node_put(sub_node);

		if (!private->i2c_bus) {
			DRM_ERROR("EDID I2C adapter not found: %d\n", ret);
			ret = -EPROBE_DEFER;
			goto i2c_adapter_find_fail;
		}
	} else {
		DRM_ERROR("EDID I2C property not found\n");
	}

	ret = of_property_read_u32(np, "fmax", &private->fmax);
	if (ret < 0)
		private->fmax = DIGILENT_ENC_MAX_FREQ;

	ret = of_property_read_u32(np, "hmax", &private->hmax);
	if (ret < 0)
		private->hmax = DIGILENT_ENC_MAX_H;

	ret = of_property_read_u32(np, "vmax", &private->vmax);
	if (ret < 0)
		private->vmax = DIGILENT_ENC_MAX_V;

	ret = of_property_read_u32(np, "hpref", &private->hpref);
	if (ret < 0)
		private->hpref = DIGILENT_ENC_PREF_H;

	ret = of_property_read_u32(np, "vpref", &private->vpref);
	if (ret < 0)
		private->vpref = DIGILENT_ENC_PREF_V;

	return 0;

i2c_adapter_find_fail:
	if (private->vtc_bridge)
		of_xlnx_bridge_put(private->vtc_bridge);
vtc_bridge_get_fail:
	dma_release_channel(private->dma);
dma_request_fail:
clk_get_fail:
	return ret;
}

static void digilent_hdmi_of_release(struct platform_device *pdev)
{
	struct digilent_hdmi_private *private = platform_get_drvdata(pdev);

	if (private->i2c_bus)
		i2c_put_adapter(private->i2c_bus);

	if (private->vtc_bridge)
		of_xlnx_bridge_put(private->vtc_bridge);

	dma_release_channel(private->dma);
}

static int digilent_hdmi_platform_probe(struct platform_device *pdev)
{
	struct digilent_hdmi_private *private;
	struct drm_device *ddev;
	int ret;

	private = devm_kzalloc(&pdev->dev, sizeof(*private), GFP_KERNEL);
	if (!private) {
		ret = -ENOMEM;
		DRM_ERROR("failed to allocate memory for private data %d\n", ret);
		goto private_alloc_fail;
	}
	platform_set_drvdata(pdev, private);

	ret = digilent_hdmi_parse_of(pdev);
	if (ret) {
		DRM_ERROR("failed to parse OF: %d\n", ret);
		goto parse_of_fail;
	}

	ddev = drm_dev_alloc(&digilent_hdmi_drm_driver, &pdev->dev);
	if (IS_ERR(ddev)) {
		ret = PTR_ERR(ddev);
		DRM_ERROR("failed to allocate DRM device: %d\n", ret);
		goto drm_dev_alloc_fail;
	}

	private->drm_dev = ddev;
	ddev->dev_private = private;

	drm_mode_config_init(ddev);

	ddev->mode_config.funcs = &digilent_hdmi_mode_config_funcs;
	ddev->mode_config.min_width = 0;
	ddev->mode_config.min_height = 0;
	ddev->mode_config.max_width = private->hmax;
	ddev->mode_config.max_height = private->vmax;

	drm_kms_helper_poll_init(ddev);

	ret = digilent_hdmi_crtc_create(ddev);
	if (ret) {
		DRM_ERROR("failed to create crtc: %d\n", ret);
		goto crtc_create_fail;
	}

	ret = digilent_hdmi_encoder_create(ddev);
	if (ret) {
		DRM_ERROR("failed to create encoder: %d\n", ret);
		goto encoder_create_fail;
	}

	ret = digilent_hdmi_connector_create(ddev);
	if (ret) {
		DRM_ERROR("failed to create connector: %d\n", ret);
		goto connector_create_fail;
	}

	drm_mode_config_reset(ddev);

	ret = drm_dev_register(ddev, 0);
	if (ret) {
		DRM_ERROR("failed to initialize fbdev: %d\n", ret);
		goto fbdev_setup_fail;
	}

	drm_fbdev_generic_setup(ddev, 32);

	return 0;

fbdev_setup_fail:
connector_create_fail:
encoder_create_fail:
crtc_create_fail:
	drm_mode_config_cleanup(ddev);
drm_dev_alloc_fail:
	digilent_hdmi_of_release(pdev);
parse_of_fail:
private_alloc_fail:
	return ret;
}

static int digilent_hdmi_platform_remove(struct platform_device *pdev)
{
	struct digilent_hdmi_private *private = platform_get_drvdata(pdev);

	digilent_hdmi_of_release(pdev);

	drm_atomic_helper_shutdown(private->drm_dev);

	drm_put_dev(private->drm_dev);

	return 0;
}

static const struct of_device_id digilent_hdmi_driver_of_match[] = {
	{ .compatible = "digilent,hdmi-tx", },
	{ },
};
MODULE_DEVICE_TABLE(of, digilent_hdmi_driver_of_match);

static struct platform_driver digilent_hdmi_driver = {
	.driver = {
		.name = "digilent-hdmi-tx",
		.owner = THIS_MODULE,
		.of_match_table = digilent_hdmi_driver_of_match,
	},
	.probe = digilent_hdmi_platform_probe,
	.remove = digilent_hdmi_platform_remove,
};
module_platform_driver(digilent_hdmi_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Cosmin Tanislav <demonsingur@gmail.com>");
MODULE_DESCRIPTION("Digilent HDMI DRM driver");
