// SPDX-License-Identifier: GPL-2.0
/*
 * PM domains for CPUs via genpd - managed by cpuidle-psci.
 *
 * Copyright (C) 2019 Linaro Ltd.
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 *
 */

#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/psci.h>

#include "cpuidle-psci.h"

struct device *psci_dt_attach_cpu(int cpu)
{
	struct device *dev;

	/* Currently limit the hierarchical topology to be used in OSI mode. */
	if (!psci_has_osi_support())
		return NULL;

	dev = dev_pm_domain_attach_by_name(get_cpu_device(cpu), "psci");
	if (IS_ERR_OR_NULL(dev))
		return dev;

	pm_runtime_irq_safe(dev);
	if (cpu_online(cpu))
		pm_runtime_get_sync(dev);

	return dev;
}
