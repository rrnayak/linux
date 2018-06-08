/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
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

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/pm.h>

static bool download_mode = IS_ENABLED(CONFIG_POWER_RESET_MSM_DOWNLOAD_MODE);
module_param(download_mode, bool, 0);

#define QCOM_SET_DLOAD_MODE 0x10
static void __iomem *msm_ps_hold;
static struct regmap *tcsr_regmap;
static unsigned int dload_mode_offset;

static int deassert_pshold(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	writel(0, msm_ps_hold);
	mdelay(10000);

	return NOTIFY_DONE;
}

static struct notifier_block restart_nb = {
	.notifier_call = deassert_pshold,
	.priority = 128,
};

static void do_msm_poweroff(void)
{
	deassert_pshold(&restart_nb, 0, NULL);
}

static int msm_restart_probe(struct platform_device *pdev)
{
	int ret;
	struct of_phandle_args args;
	struct device *dev = &pdev->dev;
	struct resource *mem;

	if (download_mode) {
		ret = of_parse_phandle_with_fixed_args(dev->of_node,
						       "qcom,dload-mode", 1, 0,
						       &args);
		if (ret < 0)
			return ret;

		tcsr_regmap = syscon_node_to_regmap(args.np);
		of_node_put(args.np);
		if (IS_ERR(tcsr_regmap))
			return PTR_ERR(tcsr_regmap);

		dload_mode_offset = args.args[0];

		/* Enable download mode by writing the cookie */
		regmap_write(tcsr_regmap, dload_mode_offset,
			     QCOM_SET_DLOAD_MODE);
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	msm_ps_hold = devm_ioremap_resource(dev, mem);
	if (IS_ERR(msm_ps_hold))
		return PTR_ERR(msm_ps_hold);

	register_restart_handler(&restart_nb);

	pm_power_off = do_msm_poweroff;

	return 0;
}

static void msm_restart_shutdown(struct platform_device *pdev)
{
	/* Clean shutdown, disable download mode to allow normal restart */
	if (download_mode)
		regmap_write(tcsr_regmap, dload_mode_offset, 0x0);
}

static const struct of_device_id of_msm_restart_match[] = {
	{ .compatible = "qcom,pshold", },
	{},
};
MODULE_DEVICE_TABLE(of, of_msm_restart_match);

static struct platform_driver msm_restart_driver = {
	.probe = msm_restart_probe,
	.driver = {
		.name = "msm-restart",
		.of_match_table = of_match_ptr(of_msm_restart_match),
	},
	.shutdown = msm_restart_shutdown,
};

static int __init msm_restart_init(void)
{
	return platform_driver_register(&msm_restart_driver);
}
device_initcall(msm_restart_init);
