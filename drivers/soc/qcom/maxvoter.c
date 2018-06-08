/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018, The Linux Foundation. All rights reserved. */

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/platform_device.h>

const char *pd_names[] = {
	"mx",
	"cx",
	NULL
};

struct device *devs[10];

static int maxvoter_probe(struct platform_device *pdev)
{
	int i = 0, num_pds;

	while (pd_names[i])
		i++;

	num_pds = i;

	if (num_pds <= 1)
		return -EINVAL;

	for (i = 0; i < num_pds; i++) {
		devs[i] = dev_pm_domain_attach_by_id(&pdev->dev, i);
		if (IS_ERR(devs[i]))
			return PTR_ERR(devs[i]);
		if (!device_link_add(&pdev->dev, devs[i], DL_FLAG_STATELESS |
				     DL_FLAG_PM_RUNTIME))
			return -EINVAL;
		dev_pm_genpd_set_performance_state(devs[i], INT_MAX);
	}

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	return 0;
}

static const struct of_device_id maxvoter_match_table[] = {
	{ .compatible = "qcom,maxvoter" },
	{ },
};

static struct platform_driver maxvoter_driver = {
	.probe  = maxvoter_probe,
	.driver = {
		   .name = "maxvoter",
		   .of_match_table = maxvoter_match_table,
	},
};

static int __init maxvoter_init(void)
{
	return platform_driver_register(&maxvoter_driver);
}
arch_initcall(maxvoter_init);
