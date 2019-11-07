// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015-2019, The Linux Foundation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#include <linux/module.h>

static DEFINE_MUTEX(iommu_debug_attachments_lock);
static LIST_HEAD(iommu_debug_attachments);

/*
 * Each group may have more than one domain; but each domain may
 * only have one group.
 * Used by debug tools to display the name of the device(s) associated
 * with a particular domain.
 */
struct iommu_debug_attachment {
	struct iommu_domain *domain;
	struct iommu_group *group;
	struct list_head list;
};

void iommu_debug_attach_device(struct iommu_domain *domain,
			       struct device *dev)
{
	struct iommu_debug_attachment *attach;
	struct iommu_group *group;

	group = dev->iommu_group;
	if (!group)
		return;

	mutex_lock(&iommu_debug_attachments_lock);
	list_for_each_entry(attach, &iommu_debug_attachments, list)
		if (attach->domain == domain && attach->group == group)
			goto out;

	attach = kzalloc(sizeof(*attach), GFP_KERNEL);
	if (!attach)
		goto out;

	attach->domain = domain;
	attach->group = group;
	INIT_LIST_HEAD(&attach->list);

	list_add(&attach->list, &iommu_debug_attachments);
out:
	mutex_unlock(&iommu_debug_attachments_lock);
}

void iommu_debug_domain_remove(struct iommu_domain *domain)
{
	struct iommu_debug_attachment *it, *tmp;

	mutex_lock(&iommu_debug_attachments_lock);
	list_for_each_entry_safe(it, tmp, &iommu_debug_attachments, list) {
		if (it->domain != domain)
			continue;
		list_del(&it->list);
		kfree(it);
	}

	mutex_unlock(&iommu_debug_attachments_lock);
}
