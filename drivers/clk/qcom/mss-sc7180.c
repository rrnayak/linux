// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,mss-sc7180.h>

#include "clk-regmap.h"
#include "clk-branch.h"
#include "common.h"

static struct clk_branch mss_axi_nav_clk = {
	.halt_reg = 0xbc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xbc,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "mss_axi_nav_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch mss_axi_crypto_clk = {
	.halt_reg = 0xcc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xcc,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "mss_axi_crypto_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct regmap_config mss_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.fast_io	= true,
};

static struct clk_regmap *mss_sc7180_clocks[] = {
	[MSS_AXI_CRYPTO_CLK] = &mss_axi_crypto_clk.clkr,
	[MSS_AXI_NAV_CLK] = &mss_axi_nav_clk.clkr,
};

static const struct qcom_cc_desc mss_sc7180_desc = {
	.config = &mss_regmap_config,
	.clks = mss_sc7180_clocks,
	.num_clks = ARRAY_SIZE(mss_sc7180_clocks),
};

static int mss_sc7180_probe(struct platform_device *pdev)
{
	return qcom_cc_probe(pdev, &mss_sc7180_desc);
}

static const struct of_device_id mss_sc7180_match_table[] = {
	{ .compatible = "qcom,sc7180-mss" },
	{ }
};
MODULE_DEVICE_TABLE(of, mss_sc7180_match_table);

static struct platform_driver mss_sc7180_driver = {
	.probe		= mss_sc7180_probe,
	.driver		= {
		.name		= "sc7180-mss",
		.of_match_table = mss_sc7180_match_table,
	},
};

static int __init mss_sc7180_init(void)
{
	return platform_driver_register(&mss_sc7180_driver);
}
subsys_initcall(mss_sc7180_init);

static void __exit mss_sc7180_exit(void)
{
	platform_driver_unregister(&mss_sc7180_driver);
}
module_exit(mss_sc7180_exit);

MODULE_DESCRIPTION("QTI MSS SC7180 Driver");
MODULE_LICENSE("GPL v2");
