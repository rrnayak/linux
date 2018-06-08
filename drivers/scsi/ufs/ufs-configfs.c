// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2018, Linux Foundation.

#include <linux/configfs.h>
#include <linux/err.h>
#include <linux/string.h>

#include "ufs.h"
#include "ufshcd.h"

static inline struct ufs_hba *config_item_to_hba(struct config_item *item)
{
	struct config_group *group = to_config_group(item);
	struct configfs_subsystem *subsys = to_configfs_subsystem(group);
	struct ufs_hba *hba = container_of(subsys, struct ufs_hba, subsys);

	return subsys ? hba : NULL;
}

static ssize_t ufs_provision_show(struct config_item *item, char *buf)
{
	u8 desc_buf[QUERY_DESC_CONFIGURATION_DEF_SIZE] = {0};
	int buff_len = QUERY_DESC_CONFIGURATION_DEF_SIZE;
	int i, ret, curr_len = 0;
	struct ufs_hba *hba = config_item_to_hba(item);

	if (!hba)
		return -EINVAL;

	ret = ufshcd_query_descriptor_retry(hba, UPIU_QUERY_OPCODE_READ_DESC,
		QUERY_DESC_IDN_CONFIGURATION, 0, 0, desc_buf, &buff_len);

	if (ret)
		return ret;

	for (i = 0; i < buff_len; i++)
		curr_len += snprintf((buf + curr_len), (PAGE_SIZE - curr_len),
				"0x%x ", desc_buf[i]);

	return curr_len;
}

ssize_t ufshcd_desc_configfs_store(struct ufs_hba *hba,
		const char *buf, size_t count)
{
	char *strbuf;
	char *strbuf_copy;
	u8 desc_buf[QUERY_DESC_CONFIGURATION_DEF_SIZE] = {0};
	int buff_len = QUERY_DESC_CONFIGURATION_DEF_SIZE;
	char *token;
	int i, ret, value;
	u32 bConfigDescrLock = 0;

	/* reserve one byte for null termination */
	strbuf = kmalloc(count + 1, GFP_KERNEL);
	if (!strbuf)
		return -ENOMEM;

	strbuf_copy = strbuf;
	strlcpy(strbuf, buf, count + 1);

	if (!hba)
		goto out;

	/* Just return if bConfigDescrLock is already set */
	ret = ufshcd_query_attr(hba, UPIU_QUERY_OPCODE_READ_ATTR,
		QUERY_ATTR_IDN_CONF_DESC_LOCK, 0, 0, &bConfigDescrLock);
	if (ret)
		goto out;

	if (bConfigDescrLock) {
		dev_err(hba->dev, "%s: bConfigDescrLock already set to %u, cannot re-provision device!\n",
		__func__, bConfigDescrLock);
		goto out;
	}

	for (i = 0; i < QUERY_DESC_CONFIGURATION_DEF_SIZE; i++) {
		token = strsep(&strbuf, " ");
		if (!token && i)
			break;

		ret = kstrtoint(token, 0, &value);
		if (ret) {
			dev_err(hba->dev, "%s: kstrtoint failed %d %s\n",
				__func__, ret, token);
			goto out;
		}
		desc_buf[i] = (u8)value;
	}

	/* Write configuration descriptor to provision ufs */
	ret = ufshcd_query_descriptor_retry(hba, UPIU_QUERY_OPCODE_WRITE_DESC,
		QUERY_DESC_IDN_CONFIGURATION, 0, 0, desc_buf, &buff_len);

	if (!ret)
		dev_err(hba->dev, "%s: UFS Provisioning done, reboot now!\n",
		__func__);

out:
	kfree(strbuf_copy);
	return count;
}

static ssize_t ufs_provision_store(struct config_item *item,
		const char *buf, size_t count)
{
	struct ufs_hba *hba = config_item_to_hba(item);

	return ufshcd_desc_configfs_store(hba, buf, count);
}

static struct configfs_attribute ufshcd_attr_provision = {
	.ca_name	= "ufs_provision",
	.ca_mode	= 0644,
	.ca_owner	= THIS_MODULE,
	.show		= ufs_provision_show,
	.store		= ufs_provision_store,
};

static struct configfs_attribute *ufshcd_attrs[] = {
	&ufshcd_attr_provision,
	NULL,
};

static struct config_item_type ufscfg_type = {
	.ct_attrs	= ufshcd_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem ufscfg_subsys = {
	.su_group = {
		.cg_item = {
			.ci_type = &ufscfg_type,
		},
	},
};

int ufshcd_configfs_init(struct ufs_hba *hba, const char *name)
{
	int ret;
	struct configfs_subsystem *subsys = &hba->subsys;

	strncpy(ufscfg_subsys.su_group.cg_item.ci_namebuf, name, strlen(name));
	subsys->su_group = ufscfg_subsys.su_group;
	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);
	ret = configfs_register_subsystem(subsys);
	if (ret)
		pr_err("Error %d while registering subsystem %s\n",
		       ret,
		       subsys->su_group.cg_item.ci_namebuf);

	return ret;
}

void ufshcd_configfs_exit(void)
{
	configfs_unregister_subsystem(&ufscfg_subsys);
}
