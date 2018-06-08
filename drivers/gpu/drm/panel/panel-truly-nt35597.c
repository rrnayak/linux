// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/gpio/consumer.h>
#include <linux/leds.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <drm/drmP.h>
#include <drm/drm_panel.h>
#include <drm/drm_mipi_dsi.h>

#define BL_NODE_NAME_SIZE 32
#define PRIM_DISPLAY_NODE 0

static const char * const regulator_names[] = {
	"vdda",
	"vdispp",
	"vdispn"
};

static unsigned long regulator_enable_loads[] = {
	62000,
	100000,
	100000};

static unsigned long regulator_disable_loads[] = {
	80,
	100,
	100};

struct truly_wqxga {
	struct device *dev;
	struct drm_panel panel;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *reset_gpio;
	struct gpio_desc *mode_gpio;

	struct backlight_device *backlight;
	/* WLED params */
	struct led_trigger *wled;
	struct videomode vm;

	struct mipi_dsi_device *dsi[2];

	bool prepared;
	bool enabled;
};

static inline struct truly_wqxga *panel_to_truly_wqxga(struct drm_panel *panel)
{
	return container_of(panel, struct truly_wqxga, panel);
}

static int truly_wqxga_power_on(struct truly_wqxga *ctx)
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

	msleep(20);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(20000, 20100);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(20000, 20100);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(50000, 50100);

	/* dual port */
	gpiod_set_value(ctx->mode_gpio, 0);

	return 0;
}

static int truly_wqxga_power_off(struct truly_wqxga *ctx)
{
	int ret, i;

	gpiod_set_value(ctx->reset_gpio, 0);

	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++) {
		ret = regulator_set_load(ctx->supplies[i].consumer,
				regulator_disable_loads[i]);
		if (ret)
			return ret;
	}

	return regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
}

static int truly_wqxga_disable(struct drm_panel *panel)
{
	struct truly_wqxga *ctx = panel_to_truly_wqxga(panel);

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;
	return 0;
}

static int truly_wqxga_unprepare(struct drm_panel *panel)
{
	struct truly_wqxga *ctx = panel_to_truly_wqxga(panel);
	struct mipi_dsi_device **dsis = ctx->dsi;
	int ret = 0, i;

	if (!ctx->prepared)
		return 0;

	dsis[0]->mode_flags = 0;
	dsis[1]->mode_flags = 0;

	for (i = 0; i < 2; i++)
		if (mipi_dsi_dcs_set_display_off(dsis[i]) < 0)
			ret = -ECOMM;
	msleep(78);

	for (i = 0; i < 2; i++)
		if (mipi_dsi_dcs_enter_sleep_mode(dsis[i]) < 0)
			ret = -ECOMM;
	msleep(78);

	truly_wqxga_power_off(ctx);

	ctx->prepared = false;
	return ret;
}

#define MAX_LEN	5
struct {
	u8 commands[MAX_LEN];
	int size;
} panel_cmds[] = { /* CMD2_P0 */
		   { { 0xff, 0x20 }, 2 },
		   { { 0xfb, 0x01 }, 2 },
		   { { 0x00, 0x01 }, 2 },
		   { { 0x01, 0x55 }, 2 },
		   { { 0x02, 0x45 }, 2 },
		   { { 0x05, 0x40 }, 2 },
		   { { 0x06, 0x19 }, 2 },
		   { { 0x07, 0x1e }, 2 },
		   { { 0x0b, 0x73 }, 2 },
		   { { 0x0c, 0x73 }, 2 },
		   { { 0x0e, 0xb0 }, 2 },
		   { { 0x0f, 0xae }, 2 },
		   { { 0x11, 0xb8 }, 2 },
		   { { 0x13, 0x00 }, 2 },
		   { { 0x58, 0x80 }, 2 },
		   { { 0x59, 0x01 }, 2 },
		   { { 0x5a, 0x00 }, 2 },
		   { { 0x5b, 0x01 }, 2 },
		   { { 0x5c, 0x80 }, 2 },
		   { { 0x5d, 0x81 }, 2 },
		   { { 0x5e, 0x00 }, 2 },
		   { { 0x5f, 0x01 }, 2 },
		   { { 0x72, 0x11 }, 2 },
		   { { 0x68, 0x03 }, 2 },
		   /* CMD2_P4 */
		   { { 0xFF, 0x24 }, 2 },
		   { { 0xFB, 0x01 }, 2 },
		   { { 0x00, 0x1C }, 2 },
		   { { 0x01, 0x0B }, 2 },
		   { { 0x02, 0x0C }, 2 },
		   { { 0x03, 0x01 }, 2 },
		   { { 0x04, 0x0F }, 2 },
		   { { 0x05, 0x10 }, 2 },
		   { { 0x06, 0x10 }, 2 },
		   { { 0x07, 0x10 }, 2 },
		   { { 0x08, 0x89 }, 2 },
		   { { 0x09, 0x8A }, 2 },
		   { { 0x0A, 0x13 }, 2 },
		   { { 0x0B, 0x13 }, 2 },
		   { { 0x0C, 0x15 }, 2 },
		   { { 0x0D, 0x15 }, 2 },
		   { { 0x0E, 0x17 }, 2 },
		   { { 0x0F, 0x17 }, 2 },
		   { { 0x10, 0x1C }, 2 },
		   { { 0x11, 0x0B }, 2 },
		   { { 0x12, 0x0C }, 2 },
		   { { 0x13, 0x01 }, 2 },
		   { { 0x14, 0x0F }, 2 },
		   { { 0x15, 0x10 }, 2 },
		   { { 0x16, 0x10 }, 2 },
		   { { 0x17, 0x10 }, 2 },
		   { { 0x18, 0x89 }, 2 },
		   { { 0x19, 0x8A }, 2 },
		   { { 0x1A, 0x13 }, 2 },
		   { { 0x1B, 0x13 }, 2 },
		   { { 0x1C, 0x15 }, 2 },
		   { { 0x1D, 0x15 }, 2 },
		   { { 0x1E, 0x17 }, 2 },
		   { { 0x1F, 0x17 }, 2 },
		   /* STV */
		   { { 0x20, 0x40 }, 2 },
		   { { 0x21, 0x01 }, 2 },
		   { { 0x22, 0x00 }, 2 },
		   { { 0x23, 0x40 }, 2 },
		   { { 0x24, 0x40 }, 2 },
		   { { 0x25, 0x6D }, 2 },
		   { { 0x26, 0x40 }, 2 },
		   { { 0x27, 0x40 }, 2 },
		   /* Vend */
		   { { 0xE0, 0x00 }, 2 },
		   { { 0xDC, 0x21 }, 2 },
		   { { 0xDD, 0x22 }, 2 },
		   { { 0xDE, 0x07 }, 2 },
		   { { 0xDF, 0x07 }, 2 },
		   { { 0xE3, 0x6D }, 2 },
		   { { 0xE1, 0x07 }, 2 },
		   { { 0xE2, 0x07 }, 2 },
		   /* UD */
		   { { 0x29, 0xD8 }, 2 },
		   { { 0x2A, 0x2A }, 2 },
		   /* CLK */
		   { { 0x4B, 0x03 }, 2 },
		   { { 0x4C, 0x11 }, 2 },
		   { { 0x4D, 0x10 }, 2 },
		   { { 0x4E, 0x01 }, 2 },
		   { { 0x4F, 0x01 }, 2 },
		   { { 0x50, 0x10 }, 2 },
		   { { 0x51, 0x00 }, 2 },
		   { { 0x52, 0x80 }, 2 },
		   { { 0x53, 0x00 }, 2 },
		   { { 0x56, 0x00 }, 2 },
		   { { 0x54, 0x07 }, 2 },
		   { { 0x58, 0x07 }, 2 },
		   { { 0x55, 0x25 }, 2 },
		   /* Reset XDONB */
		   { { 0x5B, 0x43 }, 2 },
		   { { 0x5C, 0x00 }, 2 },
		   { { 0x5F, 0x73 }, 2 },
		   { { 0x60, 0x73 }, 2 },
		   { { 0x63, 0x22 }, 2 },
		   { { 0x64, 0x00 }, 2 },
		   { { 0x67, 0x08 }, 2 },
		   { { 0x68, 0x04 }, 2 },
		   /* Resolution:1440x2560 */
		   { { 0x72, 0x02 }, 2 },
		   /* mux */
		   { { 0x7A, 0x80 }, 2 },
		   { { 0x7B, 0x91 }, 2 },
		   { { 0x7C, 0xD8 }, 2 },
		   { { 0x7D, 0x60 }, 2 },
		   { { 0x7F, 0x15 }, 2 },
		   { { 0x75, 0x15 }, 2 },
		   /* ABOFF */
		   { { 0xB3, 0xC0 }, 2 },
		   { { 0xB4, 0x00 }, 2 },
		   { { 0xB5, 0x00 }, 2 },
		   /* Source EQ */
		   { { 0x78, 0x00 }, 2 },
		   { { 0x79, 0x00 }, 2 },
		   { { 0x80, 0x00 }, 2 },
		   { { 0x83, 0x00 }, 2 },
		   /* FP BP */
		   { { 0x93, 0x0A }, 2 },
		   { { 0x94, 0x0A }, 2 },
		   /* Inversion Type */
		   { { 0x8A, 0x00 }, 2 },
		   { { 0x9B, 0xFF }, 2 },
		   /* IMGSWAP =1 @PortSwap=1 */
		   { { 0x9D, 0xB0 }, 2 },
		   { { 0x9F, 0x63 }, 2 },
		   { { 0x98, 0x10 }, 2 },
		   /* FRM */
		   { { 0xEC, 0x00 }, 2 },
		   /* CMD1 */
		   { { 0xFF, 0x10 }, 2 },
		    /* VBP+VSA=,VFP = 10H */
		   { { 0x3B, 0x03, 0x0A, 0x0A, }, 4 },
		   /* FTE on */
		   { { 0x35, 0x00 }, 2 },
		   /* EN_BK =1(auto black) */
		   { { 0xE5, 0x01 }, 2 },
		   /* CMD mode(10) VDO mode(03) */
		   { { 0xBB, 0x03 }, 2 },
		   /* Non Reload MTP */
		   { { 0xFB, 0x01 }, 2 },
};

static int truly_wqxga_prepare(struct drm_panel *panel)
{
	struct truly_wqxga *ctx = panel_to_truly_wqxga(panel);
	struct mipi_dsi_device **dsis = ctx->dsi;
	struct mipi_dsi_device *d;
	int ret, i, j;

	if (ctx->prepared)
		return 0;

	ret = truly_wqxga_power_on(ctx);
	if (ret < 0)
		return ret;

	dsis[0]->mode_flags |= MIPI_DSI_MODE_LPM;
	dsis[1]->mode_flags |= MIPI_DSI_MODE_LPM;

	for (j = 0; j < ARRAY_SIZE(panel_cmds); j++) {
		for (i = 0; i < 2; i++) {
			d = dsis[i];
			ret = mipi_dsi_dcs_write_buffer(dsis[i],
					panel_cmds[j].commands,
					panel_cmds[j].size);
			if (ret < 0) {
				dev_err(ctx->dev, "failed to cmd no %d, err: %d\n",
						j, ret);
				return ret;
			}
		}
	}


	for (i = 0; i < 2; i++)
		if (mipi_dsi_dcs_exit_sleep_mode(dsis[i]) < 0) {
			dev_err(ctx->dev, "failed to exit sleep mode\n");
			return -ECOMM;
		}
	msleep(78);

	for (i = 0; i < 2; i++)
		if (mipi_dsi_dcs_set_display_on(dsis[i]) < 0) {
			dev_err(ctx->dev, "failed to send display on\n");
			return -ECOMM;
		}
	msleep(78);

	ctx->prepared = true;

	return 0;
}

static int truly_wqxga_enable(struct drm_panel *panel)
{
	struct truly_wqxga *ctx = panel_to_truly_wqxga(panel);

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}
	ctx->enabled = true;

	return 0;
}

static int truly_wqxga_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct truly_wqxga *ctx = panel_to_truly_wqxga(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode) {
		dev_err(ctx->dev, "failed to create a new display mode\n");
		return 0;
	}

	drm_display_mode_from_videomode(&ctx->vm, mode);
	connector->display_info.width_mm = 74;
	connector->display_info.height_mm = 131;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs truly_wqxga_drm_funcs = {
	.disable = truly_wqxga_disable,
	.unprepare = truly_wqxga_unprepare,
	.prepare = truly_wqxga_prepare,
	.enable = truly_wqxga_enable,
	.get_modes = truly_wqxga_get_modes,
};

static int truly_wqxga_panel_add(struct truly_wqxga *ctx)
{
	struct device *dev = ctx->dev;
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++)
		ctx->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}

	ctx->mode_gpio = devm_gpiod_get(dev, "mode", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->mode_gpio)) {
		dev_err(dev, "cannot get mode gpio %ld\n",
			PTR_ERR(ctx->mode_gpio));
		ctx->mode_gpio = NULL;
		return PTR_ERR(ctx->mode_gpio);
	}

	ret = pinctrl_pm_select_default_state(dev);
	if (ret) {
		dev_err(dev, "%s: failed to set pinctrl default state, %d\n",
			__func__, ret);
		return ret;
	}

	ret = of_get_videomode(dev->of_node, &ctx->vm, 0);
	if (ret < 0)
		return ret;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &truly_wqxga_drm_funcs;
	drm_panel_add(&ctx->panel);

	return 0;
}

static void truly_wqxga_panel_del(struct truly_wqxga *ctx)
{
	if (ctx->panel.dev)
		drm_panel_remove(&ctx->panel);
}

static void truly_wqxga_bkl_unregister(struct truly_wqxga *ctx)
{
	if (ctx->backlight)
		put_device(&ctx->backlight->dev);
}

static int truly_backlight_device_update_status(struct backlight_device *bd)
{
	int brightness;
	int max_brightness;
	int rc = 0;
	struct truly_wqxga *ctx = dev_get_drvdata(&bd->dev);

	brightness = bd->props.brightness;
	max_brightness = bd->props.max_brightness;

	if ((bd->props.power != FB_BLANK_UNBLANK) ||
		(bd->props.state & BL_CORE_FBBLANK) ||
		  (bd->props.state & BL_CORE_SUSPENDED))
		brightness = 0;

	if (brightness > max_brightness)
		brightness = max_brightness;

	if (ctx->wled)
		led_trigger_event(ctx->wled, brightness);

	return rc;
}

static int truly_backlight_device_get_brightness(struct backlight_device *bd)
{
	return bd->props.brightness;
}

static const struct backlight_ops truly_backlight_device_ops = {
	.update_status = truly_backlight_device_update_status,
	.get_brightness = truly_backlight_device_get_brightness,
};

static int truly_backlight_setup(struct truly_wqxga *ctx)
{
	struct backlight_properties props;
	char bl_node_name[BL_NODE_NAME_SIZE];

	if (!ctx->backlight) {
		memset(&props, 0, sizeof(props));
		props.type = BACKLIGHT_RAW;
		props.power = FB_BLANK_UNBLANK;
		props.max_brightness = 4096;

		snprintf(bl_node_name, BL_NODE_NAME_SIZE, "panel%u-backlight",
				 PRIM_DISPLAY_NODE);

		ctx->backlight =  backlight_device_register(bl_node_name,
				ctx->dev, ctx,
				&truly_backlight_device_ops, &props);

		if (IS_ERR_OR_NULL(ctx->backlight)) {
			pr_err("Failed to register backlight\n");
			ctx->backlight = NULL;
			return -ENODEV;
		}

		/* Register with the LED driver interface */
		led_trigger_register_simple("bkl-trigger", &ctx->wled);

		if (!ctx->wled) {
			pr_err("backlight led registration failed\n");
			return -ENODEV;
		}
	}

	return 0;
}

static int truly_wqxga_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct truly_wqxga *ctx;
	struct mipi_dsi_device *secondary = NULL;
	struct device_node *dsi1;
	struct mipi_dsi_host *dsi1_host;
	int ret = 0;
	const struct mipi_dsi_device_info info = {.type = "trulynt35597",
		.channel = 0,
		.node = NULL,
	};

	/* This device represents itself as one with
	 * two input ports which are fed by the output
	 * ports of the two DSI controllers . The DSI0
	 * is the master controller and has most of the
	 * panel related info in its child node.
	 */

	/* configure master DSI device */
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS |
	MIPI_DSI_MODE_LPM;

	/* get the dsi1 output port node */
	dsi1 = of_graph_get_remote_node(dsi->dev.of_node, 1, -1);
	if (!dsi1) {
		dev_err(dev, "failed to get remote node\n");
		return -ENODEV;
	}

	dsi1_host = of_find_mipi_dsi_host_by_node(dsi1);
	if (!dsi1_host) {
		dev_err(dev, "failed to find dsi host\n");
		ret = -EPROBE_DEFER;
		goto err_host;
	}

	/* register the second DSI device */
	secondary = mipi_dsi_device_register_full(dsi1_host,
		&info);

	if (IS_ERR(secondary)) {
		dev_err(dev, "failed to create dsi device\n");
		ret = PTR_ERR(dsi);
		goto err_dsi_device;
	}

	/* configure secondary DSI device */
	secondary->lanes = 4;
	secondary->format = MIPI_DSI_FMT_RGB888;
	secondary->mode_flags = MIPI_DSI_MODE_VIDEO |
		MIPI_DSI_CLOCK_NON_CONTINUOUS |
		MIPI_DSI_MODE_LPM;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);

	if (!ctx) {
		ret = -ENOMEM;
		goto err_dsi_ctx;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	ctx->dsi[0] = dsi;
	ctx->dsi[1] = secondary;

	ret = truly_wqxga_panel_add(ctx);
	if (ret) {
		dev_err(dev, "failed to add panel\n");
		goto err_panel_add;
	}

	ret = truly_backlight_setup(ctx);
	if (ret) {
		dev_err(dev, "backlight setup failed\n");
		goto err_backlight;
	}

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "master dsi attach failed\n");
		goto err_dsi_attach;
	}

	ret = mipi_dsi_attach(secondary);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach on secondary failed\n");
		goto err_dsi_attach_sec;
	}

	of_node_put(dsi1);

	return 0;

err_dsi_attach_sec:
	mipi_dsi_detach(ctx->dsi[0]);
err_dsi_attach:
	truly_wqxga_bkl_unregister(ctx);
err_backlight:
	truly_wqxga_panel_del(ctx);
err_panel_add:
	mipi_dsi_device_unregister(secondary);
err_dsi_ctx:
err_dsi_device:
err_host:
	of_node_put(dsi1);
	return ret;
}

static int truly_wqxga_remove(struct mipi_dsi_device *dsi)
{
	struct truly_wqxga *ctx = mipi_dsi_get_drvdata(dsi);

	if (ctx) {
		if (ctx->dsi[0])
			mipi_dsi_detach(ctx->dsi[0]);
		if (ctx->dsi[1]) {
			mipi_dsi_detach(ctx->dsi[1]);
			mipi_dsi_device_unregister(ctx->dsi[1]);
		}
		truly_wqxga_bkl_unregister(ctx);
		truly_wqxga_panel_del(ctx);
		kfree(ctx);
	}

	return 0;
}

static const struct of_device_id truly_wqxga_of_match[] = {
	{ .compatible = "truly,nt35597", },
	{ }
};
MODULE_DEVICE_TABLE(of, truly_wqxga_of_match);

static struct mipi_dsi_driver truly_wqxga_driver = {
	.driver = {
		.name = "panel_truly_nt35597",
		.of_match_table = truly_wqxga_of_match,
	},
	.probe = truly_wqxga_probe,
	.remove = truly_wqxga_remove,
};
module_mipi_dsi_driver(truly_wqxga_driver);

MODULE_DESCRIPTION("truly nt35597 DSI Panel Driver");
MODULE_LICENSE("GPL v2");
