// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2017-2019, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "%s: " fmt, KBUILD_MODNAME

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <linux/soc/qcom/smem.h>

enum subsystem_item_id {
	MODEM = 605,
	ADSP,
	CDSP,
	SLPI,
	GPU,
	DISPLAY,
};

enum subsystem_pid {
	PID_APSS = 0,
	PID_MODEM = 1,
	PID_ADSP = 2,
	PID_SLPI = 3,
	PID_CDSP = 5,
	PID_GPU = PID_APSS,
	PID_DISPLAY = PID_APSS,
};

struct subsystem_data {
	char *name;
	enum subsystem_item_id item_id;
	enum subsystem_pid pid;
};

static const struct subsystem_data subsystems[] = {
	{"MODEM", MODEM, PID_MODEM},
	{"ADSP", ADSP, PID_ADSP},
	{"CDSP", CDSP, PID_CDSP},
	{"SLPI", SLPI, PID_SLPI},
	{"GPU", GPU, PID_GPU},
	{"DISPLAY", DISPLAY, PID_DISPLAY},
};

struct subsystem_stats {
	uint32_t version_id;
	uint32_t count;
	uint64_t last_entered;
	uint64_t last_exited;
	uint64_t accumulated_duration;
};

struct subsystem_stats_prv_data {
	struct kobj_attribute ka;
	struct kobject *kobj;
};

static struct subsystem_stats_prv_data *prvdata;

static inline ssize_t subsystem_stats_print(char *prvbuf, ssize_t length,
					    struct subsystem_stats *record,
					    const char *name)
{
	return scnprintf(prvbuf, length, "%s\n\tVersion:0x%x\n"
			"\tSleep Count:0x%x\n"
			"\tSleep Last Entered At:0x%llx\n"
			"\tSleep Last Exited At:0x%llx\n"
			"\tSleep Accumulated Duration:0x%llx\n\n",
			name, record->version_id, record->count,
			record->last_entered, record->last_exited,
			record->accumulated_duration);
}

static ssize_t subsystem_stats_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	ssize_t length = 0;
	int i = 0;
	size_t size = 0;
	struct subsystem_stats *record = NULL;

	/* Read SMEM data written by other subsystems */
	for (i = 0; i < ARRAY_SIZE(subsystems); i++) {
		record = (struct subsystem_stats *) qcom_smem_get(
			  subsystems[i].pid, subsystems[i].item_id, &size);

		if (!IS_ERR_OR_NULL(record) && (PAGE_SIZE - length > 0))
			length += subsystem_stats_print(buf + length,
							PAGE_SIZE - length,
							record,
							subsystems[i].name);
	}

	return length;
}

static int __init subsystem_sleep_stats_init(void)
{
	struct kobject *ss_stats_kobj;
	int ret;

	prvdata = kmalloc(sizeof(*prvdata), GFP_KERNEL);
	if (!prvdata)
		return -ENOMEM;

	ss_stats_kobj = kobject_create_and_add("subsystem_sleep",
					       power_kobj);
	if (!ss_stats_kobj)
		return -ENOMEM;

	prvdata->kobj = ss_stats_kobj;

	sysfs_attr_init(&prvdata->ka.attr);
	prvdata->ka.attr.mode = 0444;
	prvdata->ka.attr.name = "stats";
	prvdata->ka.show = subsystem_stats_show;
	prvdata->ka.store = NULL;

	ret = sysfs_create_file(prvdata->kobj, &prvdata->ka.attr);
	if (ret) {
		pr_err("sysfs_create_file failed\n");
		kobject_put(prvdata->kobj);
		kfree(prvdata);
		return ret;
	}

	return ret;
}

static void __exit subsystem_sleep_stats_exit(void)
{
	sysfs_remove_file(prvdata->kobj, &prvdata->ka.attr);
	kobject_put(prvdata->kobj);
	kfree(prvdata);
}

module_init(subsystem_sleep_stats_init);
module_exit(subsystem_sleep_stats_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc subsystem sleep stats driver");
