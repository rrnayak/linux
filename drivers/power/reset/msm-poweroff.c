// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2013, 2019, The Linux Foundation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/pm.h>

static void __iomem *msm_ps_hold;

void __iomem *msm_reset_debug;
EXPORT_SYMBOL_GPL(msm_reset_debug);

#ifdef CONFIG_RANDOMIZE_BASE
#define KASLR_OFFSET_PROP "qcom,msm-imem-kaslr_offset"
static void __iomem *kaslr_imem_addr;
#endif

#ifdef CONFIG_POWER_RESET_MSM_DOWNLOAD_MODE
void __iomem *dload_imem_addr;
EXPORT_SYMBOL_GPL(dload_imem_addr);
#endif

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
	struct device *dev = &pdev->dev;
	struct resource *mem;
#if defined(CONFIG_RANDOMIZE_BASE) || defined(CONFIG_POWER_RESET_MSM_DOWNLOAD_MODE)
	struct device_node *np;
#endif

#ifdef CONFIG_RANDOMIZE_BASE
#define KASLR_OFFSET_BIT_MASK	0x00000000FFFFFFFF
	np = of_find_compatible_node(NULL, NULL, KASLR_OFFSET_PROP);
	if (!np) {
		pr_err("unable to find DT imem KASLR_OFFSET node\n");
	} else {
		kaslr_imem_addr = of_iomap(np, 0);
		if (!kaslr_imem_addr)
			pr_err("unable to map imem KASLR offset\n");
	}

	if (kaslr_imem_addr) {
		__raw_writel(0xdead4ead, kaslr_imem_addr);
		__raw_writel(KASLR_OFFSET_BIT_MASK &
		(kimage_vaddr - KIMAGE_VADDR), kaslr_imem_addr + 4);
		__raw_writel(KASLR_OFFSET_BIT_MASK &
			((kimage_vaddr - KIMAGE_VADDR) >> 32),
			kaslr_imem_addr + 8);
		iounmap(kaslr_imem_addr);
	}
#endif
#ifdef CONFIG_POWER_RESET_MSM_DOWNLOAD_MODE
	np = of_find_compatible_node(NULL, NULL, "qcom,msm-imem-dload_mode");
	if (!np) {
		dev_err(dev, "unable to find qcom,msm-imem-dload_mode node\n");
	} else {
		dload_imem_addr = of_iomap(np, 0);
		if (!dload_imem_addr)
			dev_err(dev, "unable to map imem dload addr\n");
	}

	if (dload_imem_addr) {
		writel(0xE47B337D, dload_imem_addr);
		iounmap(dload_imem_addr);
	}
#endif
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	msm_ps_hold = devm_ioremap_resource(dev, mem);
	if (IS_ERR(msm_ps_hold))
		return PTR_ERR(msm_ps_hold);

	msm_reset_debug = ioremap(0x01ffa000, 0x4);
	if (!msm_reset_debug)
		return -ENXIO;

	register_restart_handler(&restart_nb);

	pm_power_off = do_msm_poweroff;

	return 0;
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
};

static int __init msm_restart_init(void)
{
	return platform_driver_register(&msm_restart_driver);
}
pure_initcall(msm_restart_init);
