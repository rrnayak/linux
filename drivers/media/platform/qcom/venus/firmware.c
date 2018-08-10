/*
 * Copyright (C) 2017 Linaro Ltd.
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

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/qcom_scm.h>
#include <linux/sizes.h>
#include <linux/soc/qcom/mdt_loader.h>

#include "core.h"
#include "firmware.h"
#include "hfi_venus_io.h"

#define VENUS_PAS_ID			9
#define VENUS_FW_MEM_SIZE		(5 * SZ_1M)
#define VENUS_FW_START_ADDR		0x0

static void venus_reset_cpu(struct venus_core *core)
{
	void __iomem *base = core->base;

	writel(0, base + WRAPPER_FW_START_ADDR);
	writel(VENUS_FW_MEM_SIZE, base + WRAPPER_FW_END_ADDR);
	writel(0, base + WRAPPER_CPA_START_ADDR);
	writel(VENUS_FW_MEM_SIZE, base + WRAPPER_CPA_END_ADDR);
	writel(0x0, base + WRAPPER_CPU_CGC_DIS);
	writel(0x0, base + WRAPPER_CPU_CLOCK_CONFIG);

	/* Bring ARM9 out of reset */
	writel(0, base + WRAPPER_A9SS_SW_RESET);
}

int venus_set_hw_state(struct venus_core *core, bool resume)
{
	if (!core->no_tz)
		return qcom_scm_set_remote_state(resume, 0);

	if (resume)
		venus_reset_cpu(core);
	else
		writel(1, core->base + WRAPPER_A9SS_SW_RESET);

	return 0;
}

static int venus_load_fw(struct venus_core *core, const char *fwname,
			phys_addr_t *mem_phys, size_t *mem_size)
{
	const struct firmware *mdt;
	struct device_node *node;
	struct device *dev;
	struct resource r;
	ssize_t fw_size;
	void *mem_va;
	int ret;

	dev = core->dev;
	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(dev, "no memory-region specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);
	if (ret)
		return ret;

	*mem_phys = r.start;
	*mem_size = resource_size(&r);

	if (*mem_size < VENUS_FW_MEM_SIZE)
		return -EINVAL;

	mem_va = memremap(r.start, *mem_size, MEMREMAP_WC);
	if (!mem_va) {
		dev_err(dev, "unable to map memory region: %pa+%zx\n",
			&r.start, *mem_size);
		return -ENOMEM;
	}

	ret = request_firmware(&mdt, fwname, dev);
	if (ret < 0)
		goto err_unmap;

	fw_size = qcom_mdt_get_size(mdt);
	if (fw_size < 0) {
		ret = fw_size;
		release_firmware(mdt);
		goto err_unmap;
	}

	if (core->no_tz)
		ret = qcom_mdt_load_no_init(dev, mdt, fwname, VENUS_PAS_ID,
					mem_va, *mem_phys, *mem_size, NULL);
	else
		ret = qcom_mdt_load(dev, mdt, fwname, VENUS_PAS_ID,
				mem_va, *mem_phys, *mem_size, NULL);

	release_firmware(mdt);

err_unmap:
	memunmap(mem_va);
	return ret;
}

static int venus_boot_no_tz(struct venus_core *core, phys_addr_t mem_phys,
			size_t mem_size)
{
	struct iommu_domain *iommu_dom;
	struct device *dev;
	int ret;

	dev = core->fw.dev;
	if (!dev)
		return -EPROBE_DEFER;

	iommu_dom = iommu_domain_alloc(&platform_bus_type);
	if (!iommu_dom) {
		dev_err(dev, "Failed to allocate iommu domain\n");
		return -ENOMEM;
	}

	ret = iommu_attach_device(iommu_dom, dev);
	if (ret) {
		dev_err(dev, "could not attach device\n");
		goto err_attach;
	}

	ret = iommu_map(iommu_dom, VENUS_FW_START_ADDR, mem_phys, mem_size,
			IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV);
	if (ret) {
		dev_err(dev, "could not map video firmware region\n");
		goto err_map;
	}

	core->fw.iommu_domain = iommu_dom;
	venus_reset_cpu(core);

	return 0;

err_map:
	iommu_detach_device(iommu_dom, dev);
err_attach:
	iommu_domain_free(iommu_dom);
	return ret;
}

static int venus_shutdown_no_tz(struct venus_core *core)
{
	struct iommu_domain *iommu;
	size_t unmapped = 0;
	u32 reg;
	struct device *dev = core->fw.dev;
	void __iomem *reg_base = core->base;

	/* Assert the reset to ARM9 */
	reg = readl_relaxed(reg_base + WRAPPER_A9SS_SW_RESET);
	reg |= WRAPPER_A9SS_SW_RESET_BIT;
	writel_relaxed(reg, reg_base + WRAPPER_A9SS_SW_RESET);

	/* Make sure reset is asserted before the mapping is removed */
	mb();

	iommu = core->fw.iommu_domain;

	unmapped = iommu_unmap(iommu, VENUS_FW_START_ADDR, VENUS_FW_MEM_SIZE);
	if (unmapped != VENUS_FW_MEM_SIZE)
		dev_err(dev, "failed to unmap firmware\n");

	iommu_detach_device(iommu, dev);
	iommu_domain_free(iommu);

	return 0;
}

int venus_boot(struct venus_core *core)
{
	phys_addr_t mem_phys;
	struct device *dev = core->dev;
	size_t mem_size;
	int ret;

	if (!IS_ENABLED(CONFIG_QCOM_MDT_LOADER) ||
		(!core->no_tz && !qcom_scm_is_available()))
		return -EPROBE_DEFER;

	ret = venus_load_fw(core, core->res->fwname, &mem_phys, &mem_size);
	if (ret) {
		dev_err(dev, "fail to load video firmware\n");
		return -EINVAL;
	}

	if (core->no_tz)
		ret = venus_boot_no_tz(core, mem_phys, mem_size);
	else
		ret = qcom_scm_pas_auth_and_reset(VENUS_PAS_ID);

	return ret;
}

int venus_shutdown(struct venus_core *core)
{
	int ret;

	if (core->no_tz)
		ret = venus_shutdown_no_tz(core);
	else
		ret = qcom_scm_pas_shutdown(VENUS_PAS_ID);

	return ret;
}

static const struct of_device_id firmware_dt_match[] = {
	{ .compatible = "qcom,venus-firmware" },
	{ }
};
MODULE_DEVICE_TABLE(of, firmware_dt_match);

struct platform_driver qcom_video_firmware_driver = {
	.driver = {
			.name = "qcom-video-firmware",
			.of_match_table = firmware_dt_match,
	},
};

MODULE_ALIAS("platform:qcom-video-firmware");
MODULE_DESCRIPTION("Qualcomm Venus firmware driver");
MODULE_LICENSE("GPL v2");
