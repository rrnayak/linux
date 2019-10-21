// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 */

#include <drm/drm_panel.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/backlight.h>
#include <linux/module.h>
#include <linux/delay.h>

#include <video/mipi_display.h>

static const char * const regulator_names[] = {
	"vdda",
	"vdd3p3",
};

static unsigned long const regulator_enable_loads[] = {
	32000,
	13200,
};

static unsigned long const regulator_disable_loads[] = {
	80,
	80,
};

struct cmd_set {
	u8 commands[4];
	u8 size;
};

struct rm69299_config {
	u32 width_mm;
	u32 height_mm;
	const char *panel_name;
	const struct cmd_set *panel_on_cmds;
	u32 num_on_cmds;
	const struct drm_display_mode *dm;
};

struct visionox_rm69299 {
	struct device *dev;
	struct drm_panel panel;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *reset_gpio;

	struct backlight_device *backlight;

	struct mipi_dsi_device *dsi;
	const struct rm69299_config *config;
	bool prepared;
	bool enabled;
};

static inline struct visionox_rm69299 *panel_to_ctx(struct drm_panel *panel)
{
	return container_of(panel, struct visionox_rm69299, panel);
}

static const struct cmd_set qcom_rm69299_1080p_panel_magic_cmds[] = {
	{ { 0xfe, 0x00 }, 2 },
	{ { 0xc2, 0x08 }, 2 },
	{ { 0x35, 0x00 }, 2 },
	{ { 0x51, 0xff }, 2 },
};

static int visionox_dcs_write(struct drm_panel *panel, u32 command)
{
	struct visionox_rm69299 *ctx = panel_to_ctx(panel);
	int i = 0, ret;

	ret = mipi_dsi_dcs_write(ctx->dsi, command, NULL, 0);
	if (ret < 0) {
		DRM_DEV_ERROR(ctx->dev,
			"cmd 0x%x failed for dsi = %d\n",
			command, i);
	}

	return ret;
}

static int visionox_dcs_write_buf(struct drm_panel *panel,
	u32 size, const u8 *buf)
{
	struct visionox_rm69299 *ctx = panel_to_ctx(panel);
	int ret = 0;
	int i = 0;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buf, size);
	if (ret < 0) {
		DRM_DEV_ERROR(ctx->dev,
			"failed to tx cmd [%d], err: %d\n", i, ret);
		return ret;
	}

	return ret;
}

static int visionox_35597_power_on(struct visionox_rm69299 *ctx)
{
	int ret, i;
	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++) {
		ret = regulator_set_load(ctx->supplies[i].consumer,
					regulator_enable_loads[i]);
		if (ret)
			return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	/*
	 * Reset sequence of visionox panel requires the panel to be
	 * out of reset for 10ms, followed by being held in reset
	 * for 10ms and then out again
	 */
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(10000, 20000);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);

	return 0;
}

static int visionox_rm69299_power_off(struct visionox_rm69299 *ctx)
{
	int ret = 0;
	int i;

	gpiod_set_value(ctx->reset_gpio, 0);

	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++) {
		ret = regulator_set_load(ctx->supplies[i].consumer,
				regulator_disable_loads[i]);
		if (ret) {
			DRM_DEV_ERROR(ctx->dev,
				"regulator_set_load failed %d\n", ret);
			return ret;
		}
	}

	ret = regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret) {
		DRM_DEV_ERROR(ctx->dev,
			"regulator_bulk_disable failed %d\n", ret);
	}
	return ret;
}

static int visionox_rm69299_disable(struct drm_panel *panel)
{
	struct visionox_rm69299 *ctx = panel_to_ctx(panel);
	int ret;

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ret = backlight_disable(ctx->backlight);
		if (ret < 0)
			DRM_DEV_ERROR(ctx->dev, "backlight disable failed %d\n",
				ret);
	}

	ctx->enabled = false;
	return 0;
}

static int visionox_rm69299_unprepare(struct drm_panel *panel)
{
	struct visionox_rm69299 *ctx = panel_to_ctx(panel);
	int ret = 0;

	if (!ctx->prepared)
		return 0;

	ctx->dsi->mode_flags = 0;

	ret = visionox_dcs_write(panel, MIPI_DCS_SET_DISPLAY_OFF);
	if (ret < 0) {
		DRM_DEV_ERROR(ctx->dev,
			"set_display_off cmd failed ret = %d\n",
			ret);
	}

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	ret = visionox_dcs_write(panel, MIPI_DCS_ENTER_SLEEP_MODE);
	if (ret < 0) {
		DRM_DEV_ERROR(ctx->dev,
			"enter_sleep cmd failed ret = %d\n", ret);
	}

	ret = visionox_rm69299_power_off(ctx);
	if (ret < 0)
		DRM_DEV_ERROR(ctx->dev, "power_off failed ret = %d\n", ret);

	ctx->prepared = false;
	return ret;
}

static int visionox_rm69299_prepare(struct drm_panel *panel)
{
	struct visionox_rm69299 *ctx = panel_to_ctx(panel);
	int ret;
	int i;
	const struct cmd_set *panel_on_cmds;
	const struct rm69299_config *config;
	u32 num_cmds;

	if (ctx->prepared)
		return 0;

	ret = visionox_35597_power_on(ctx);
	if (ret < 0)
		return ret;

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	config = ctx->config;
	panel_on_cmds = config->panel_on_cmds;
	num_cmds = config->num_on_cmds;

	for (i = 0; i < num_cmds; i++) {
		ret = visionox_dcs_write_buf(panel,
				panel_on_cmds[i].size,
					panel_on_cmds[i].commands);
		if (ret < 0) {
			DRM_DEV_ERROR(ctx->dev,
				"cmd set tx failed i = %d ret = %d\n",
					i, ret);
			goto power_off;
		}
	}

	ret = visionox_dcs_write(panel, MIPI_DCS_EXIT_SLEEP_MODE);
	if (ret < 0) {
		DRM_DEV_ERROR(ctx->dev,
			"exit_sleep_mode cmd failed ret = %d\n",
			ret);
		goto power_off;
	}

	/* Per DSI spec wait 120ms after sending exit sleep DCS command */
	msleep(120);

	ret = visionox_dcs_write(panel, MIPI_DCS_SET_DISPLAY_ON);
	if (ret < 0) {
		DRM_DEV_ERROR(ctx->dev,
			"set_display_on cmd failed ret = %d\n", ret);
		goto power_off;
	}

	/* Per DSI spec wait 120ms after sending set_display_on DCS command */
	msleep(120);

	ctx->prepared = true;

	return 0;

power_off:
	if (visionox_rm69299_power_off(ctx))
		DRM_DEV_ERROR(ctx->dev, "power_off failed\n");
	return ret;
}

static int visionox_rm69299_enable(struct drm_panel *panel)
{
	struct visionox_rm69299 *ctx = panel_to_ctx(panel);
	int ret;

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ret = backlight_enable(ctx->backlight);
		if (ret < 0)
			DRM_DEV_ERROR(ctx->dev, "backlight enable failed %d\n",
						  ret);
	}

	ctx->enabled = true;

	return 0;
}

static int visionox_rm69299_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct visionox_rm69299 *ctx = panel_to_ctx(panel);
	struct drm_display_mode *mode;
	const struct rm69299_config *config;

	config = ctx->config;
	mode = drm_mode_create(connector->dev);
	if (!mode) {
		DRM_DEV_ERROR(ctx->dev,
			"failed to create a new display mode\n");
		return 0;
	}

	connector->display_info.width_mm = config->width_mm;
	connector->display_info.height_mm = config->height_mm;
	drm_mode_copy(mode, config->dm);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs visionox_rm69299_drm_funcs = {
	.disable = visionox_rm69299_disable,
	.unprepare = visionox_rm69299_unprepare,
	.prepare = visionox_rm69299_prepare,
	.enable = visionox_rm69299_enable,
	.get_modes = visionox_rm69299_get_modes,
};

static int visionox_rm69299_panel_add(struct visionox_rm69299 *ctx)
{
	struct device *dev = ctx->dev;
	int ret, i;
	const struct rm69299_config *config;

	config = ctx->config;
	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++)
		ctx->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		DRM_DEV_ERROR(dev, "cannot get reset gpio %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}

	ret = gpiod_direction_output(ctx->reset_gpio, 0);
	if(ret < 0) {
		pr_err("direction output failed \n");
	}

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &visionox_rm69299_drm_funcs;
	drm_panel_add(&ctx->panel);

	return 0;
}

static const struct drm_display_mode qcom_sc7180_mtp_1080p_mode = {
	.name = "1080x2248",
	.clock = 158695,
	.hdisplay = 1080,
	.hsync_start = 1080 + 26,
	.hsync_end = 1080 + 26 + 2,
	.htotal = 1080 + 26 + 2 + 36,
	.vdisplay = 2248,
	.vsync_start = 2248 + 56,
	.vsync_end = 2248 + 56 + 4,
	.vtotal = 2248 + 56 + 4 + 4,
	.vrefresh = 60,
	.flags = 0,
};

static const struct rm69299_config rm69299_dir = {
	.width_mm = 74,
	.height_mm = 131,
	.panel_name = "qcom_sdm845_mtp_2k_panel",
	.dm = &qcom_sc7180_mtp_1080p_mode,
	.panel_on_cmds = qcom_rm69299_1080p_panel_magic_cmds,
	.num_on_cmds = ARRAY_SIZE(qcom_rm69299_1080p_panel_magic_cmds),
};

static int visionox_rm69299_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct visionox_rm69299 *ctx;
	int ret = 0;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;

	ctx->config = of_device_get_match_data(dev);

	if (!ctx->config) {
		dev_err(dev, "missing device configuration\n");
		return -ENODEV;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	ctx->dsi = dsi;

	ret = visionox_rm69299_panel_add(ctx);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to add panel\n");
		goto err_panel_add;
	}

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM |
		MIPI_DSI_CLOCK_NON_CONTINUOUS;
	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		DRM_DEV_ERROR(dev,
			"dsi attach failed ret = %d\n", ret);
		goto err_dsi_attach;
	}

	return 0;

err_dsi_attach:
	drm_panel_remove(&ctx->panel);
err_panel_add:
	mipi_dsi_device_unregister(dsi);
	return ret;
}

static int visionox_rm69299_remove(struct mipi_dsi_device *dsi)
{
	struct visionox_rm69299 *ctx = mipi_dsi_get_drvdata(dsi);

	if (ctx->dsi) {
		mipi_dsi_detach(ctx->dsi);
		mipi_dsi_device_unregister(ctx->dsi);
	}

	drm_panel_remove(&ctx->panel);
	return 0;
}

static const struct of_device_id visionox_rm69299_of_match[] = {
	{
		.compatible = "visionox,rm69299-1080p-display",
		.data = &rm69299_dir,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, visionox_rm69299_of_match);

static struct mipi_dsi_driver visionox_rm69299_driver = {
	.driver = {
		.name = "panel-visionox-rm69299",
		.of_match_table = visionox_rm69299_of_match,
	},
	.probe = visionox_rm69299_probe,
	.remove = visionox_rm69299_remove,
};
module_mipi_dsi_driver(visionox_rm69299_driver);

MODULE_DESCRIPTION("VISIONOX RM69299 DSI Panel Driver");
MODULE_LICENSE("GPL v2");
