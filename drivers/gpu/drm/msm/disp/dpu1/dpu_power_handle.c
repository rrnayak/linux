/* Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
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

#define pr_fmt(fmt)	"[drm:%s:%d]: " fmt, __func__, __LINE__

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>

#include "dpu_power_handle.h"
#include "dpu_trace.h"

static int dpu_power_set_data_bus_icc_path(struct dpu_power_handle *phandle)
{
	struct dpu_power_data_bus_handle *pdbus = &phandle->data_bus_handle;
	struct icc_path *path0 = of_icc_get(phandle->dev, "port0");
	struct icc_path *path1 = of_icc_get(phandle->dev, "port1");
	int total_num_paths = 0;

	if (IS_ERR(path0))
		return PTR_ERR(path0);

	total_num_paths = 1;
	pdbus->path[0] = path0;

	if (!IS_ERR(path1)) {
		pdbus->path[1] = path1;
		total_num_paths++;
	}

	if (total_num_paths > MAX_AXI_PORT_COUNT)
		return -EINVAL;

	pdbus->num_paths = total_num_paths;
	pdbus->nrt_num_paths = 0;

	return 0;
}

static void dpu_power_put_data_bus_icc_path(
	struct dpu_power_data_bus_handle *pdbus)
{
	int i;

	for (i = 0; i < pdbus->num_paths; i++) {
		if (pdbus->path[i])
			icc_put(pdbus->path[i]);
	}
}

static int _dpu_power_data_bus_set_quota(
	struct dpu_power_data_bus_handle *pdbus,
	u64 ab_quota_rt, u64 ab_quota_nrt,
	u64 ib_quota_rt, u64 ib_quota_nrt)
{
	u64 ab_quota[MAX_AXI_PORT_COUNT] = {0, 0};
	u64 ib_quota[MAX_AXI_PORT_COUNT] = {0, 0};
	u32 nrt_num_paths = pdbus->nrt_num_paths;
	u32 total_num_paths = pdbus->num_paths;
	u32 rt_num_paths = total_num_paths - nrt_num_paths;
	int i, rc = 0;

	pdbus->ab_rt = ab_quota_rt;
	pdbus->ib_rt = ib_quota_rt;
	pdbus->ab_nrt = ab_quota_nrt;
	pdbus->ib_nrt = ib_quota_nrt;

	if (pdbus->enable) {
		ab_quota_rt = max_t(u64, ab_quota_rt,
				DPU_POWER_HANDLE_ENABLE_BUS_AB_QUOTA);
		ib_quota_rt = max_t(u64, ib_quota_rt,
				DPU_POWER_HANDLE_ENABLE_BUS_IB_QUOTA);
		ab_quota_nrt = max_t(u64, ab_quota_nrt,
				DPU_POWER_HANDLE_ENABLE_BUS_AB_QUOTA);
		ib_quota_nrt = max_t(u64, ib_quota_nrt,
				DPU_POWER_HANDLE_ENABLE_BUS_IB_QUOTA);
	} else {
		ab_quota_rt = min_t(u64, ab_quota_rt,
				DPU_POWER_HANDLE_DISABLE_BUS_AB_QUOTA);
		ib_quota_rt = min_t(u64, ib_quota_rt,
				DPU_POWER_HANDLE_DISABLE_BUS_IB_QUOTA);
		ab_quota_nrt = min_t(u64, ab_quota_nrt,
				DPU_POWER_HANDLE_DISABLE_BUS_AB_QUOTA);
		ib_quota_nrt = min_t(u64, ib_quota_nrt,
				DPU_POWER_HANDLE_DISABLE_BUS_IB_QUOTA);
	}

	if (nrt_num_paths) {
		ab_quota_rt = div_u64(ab_quota_rt, rt_num_paths);
		ab_quota_nrt = div_u64(ab_quota_nrt, nrt_num_paths);

		for (i = 0; i < total_num_paths; i++) {
			if (i < rt_num_paths) {
				ab_quota[i] = ab_quota_rt;
				ib_quota[i] = ib_quota_rt;
			} else {
				ab_quota[i] = ab_quota_nrt;
				ib_quota[i] = ib_quota_nrt;
			}
		}
	} else {
		ab_quota[0] = div_u64(ab_quota_rt + ab_quota_nrt,
					total_num_paths);
		ib_quota[0] = ib_quota_rt + ib_quota_nrt;

		for (i = 1; i < total_num_paths; i++) {
			ab_quota[i] = ab_quota[0];
			ib_quota[i] = ib_quota[0];
		}
	}

	for (i = 0; i < total_num_paths; i++) {
		if (pdbus->path[i]) {
			rc = icc_set(pdbus->path[i], ab_quota[i], ib_quota[i]);
			if (rc) {
				DPU_ERROR("failed to set on path %d\n", i);
				break;
			}
		}

	}

	return rc;
}

int dpu_power_data_bus_set_quota(struct dpu_power_handle *phandle,
		struct dpu_power_client *pclient, int bus_client,
		u64 ab_quota, u64 ib_quota)
{
	int rc = 0;
	int i;
	u64 total_ab_rt = 0, total_ib_rt = 0;
	u64 total_ab_nrt = 0, total_ib_nrt = 0;
	struct dpu_power_client *client;

	if (!phandle || !pclient ||
			bus_client >= DPU_POWER_HANDLE_DATA_BUS_CLIENT_MAX) {
		pr_err("invalid parameters\n");
		return -EINVAL;
	}

	mutex_lock(&phandle->phandle_lock);

	pclient->ab[bus_client] = ab_quota;
	pclient->ib[bus_client] = ib_quota;
	trace_dpu_perf_update_bus(bus_client, ab_quota, ib_quota);

	list_for_each_entry(client, &phandle->power_client_clist, list) {
		for (i = 0; i < DPU_POWER_HANDLE_DATA_BUS_CLIENT_MAX; i++) {
			if (i == DPU_POWER_HANDLE_DATA_BUS_CLIENT_NRT) {
				total_ab_nrt += client->ab[i];
				total_ib_nrt += client->ib[i];
			} else {
				total_ab_rt += client->ab[i];
				total_ib_rt = max(total_ib_rt, client->ib[i]);
			}
		}
	}

	rc = _dpu_power_data_bus_set_quota(&phandle->data_bus_handle,
			total_ab_rt, total_ab_nrt, total_ib_rt, total_ib_nrt);

	mutex_unlock(&phandle->phandle_lock);

	return rc;
}


static int dpu_power_data_bus_update(
	struct dpu_power_data_bus_handle *pdbus, bool enable)
{
	int rc = 0;

	pdbus->enable = enable;
	rc = _dpu_power_data_bus_set_quota(pdbus, pdbus->ab_rt,
			pdbus->ab_nrt, pdbus->ib_rt, pdbus->ib_nrt);

	if (rc)
		pr_err("failed to set data bus vote rc=%d enable:%d\n",
			rc, enable);

	return rc;
}

static void dpu_power_event_trigger_locked(struct dpu_power_handle *phandle,
		u32 event_type)
{
	struct dpu_power_event *event;

	list_for_each_entry(event, &phandle->event_list, list) {
		if (event->event_type & event_type)
			event->cb_fnc(event_type, event->usr);
	}
}

struct dpu_power_client *dpu_power_client_create(
	struct dpu_power_handle *phandle, char *client_name)
{
	struct dpu_power_client *client;
	static u32 id;

	if (!client_name || !phandle) {
		pr_err("client name is null or invalid power data\n");
		return ERR_PTR(-EINVAL);
	}

	client = kzalloc(sizeof(struct dpu_power_client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&phandle->phandle_lock);
	strlcpy(client->name, client_name, MAX_CLIENT_NAME_LEN);
	client->usecase_ndx = VOTE_INDEX_DISABLE;
	client->id = id;
	client->active = true;
	pr_debug("client %s created:%pK id :%d\n", client_name,
		client, id);
	id++;
	list_add(&client->list, &phandle->power_client_clist);
	mutex_unlock(&phandle->phandle_lock);

	return client;
}

void dpu_power_client_destroy(struct dpu_power_handle *phandle,
	struct dpu_power_client *client)
{
	if (!client  || !phandle) {
		pr_err("reg bus vote: invalid client handle\n");
	} else if (!client->active) {
		pr_err("dpu power deinit already done\n");
		kfree(client);
	} else {
		pr_debug("bus vote client %s destroyed:%pK id:%u\n",
			client->name, client, client->id);
		mutex_lock(&phandle->phandle_lock);
		list_del_init(&client->list);
		mutex_unlock(&phandle->phandle_lock);
		kfree(client);
	}
}

int dpu_power_resource_init(struct platform_device *pdev,
	struct dpu_power_handle *phandle)
{
	int rc = 0;

	phandle->dev = &pdev->dev;
	rc = dpu_power_set_data_bus_icc_path(phandle);
	if (rc)
		goto end;

	INIT_LIST_HEAD(&phandle->power_client_clist);
	INIT_LIST_HEAD(&phandle->event_list);

	mutex_init(&phandle->phandle_lock);

end:
	return rc;
}

void dpu_power_resource_deinit(struct platform_device *pdev,
	struct dpu_power_handle *phandle)
{
	struct dpu_power_client *curr_client, *next_client;
	struct dpu_power_event *curr_event, *next_event;

	if (!phandle || !pdev) {
		pr_err("invalid input param\n");
		return;
	}

	mutex_lock(&phandle->phandle_lock);
	list_for_each_entry_safe(curr_client, next_client,
			&phandle->power_client_clist, list) {
		pr_err("cliend:%s-%d still registered with refcount:%d\n",
				curr_client->name, curr_client->id,
				curr_client->refcount);
		curr_client->active = false;
		list_del(&curr_client->list);
	}

	list_for_each_entry_safe(curr_event, next_event,
			&phandle->event_list, list) {
		pr_err("event:%d, client:%s still registered\n",
				curr_event->event_type,
				curr_event->client_name);
		curr_event->active = false;
		list_del(&curr_event->list);
	}

	dpu_power_put_data_bus_icc_path(&phandle->data_bus_handle);
	mutex_unlock(&phandle->phandle_lock);
}

int dpu_power_resource_enable(struct dpu_power_handle *phandle,
	struct dpu_power_client *pclient, bool enable)
{
	bool changed = false;
	u32 max_usecase_ndx = VOTE_INDEX_DISABLE, prev_usecase_ndx;
	int rc = 0;
	struct dpu_power_client *client;

	if (!phandle || !pclient) {
		pr_err("invalid input argument\n");
		return -EINVAL;
	}

	mutex_lock(&phandle->phandle_lock);
	if (enable)
		pclient->refcount++;
	else if (pclient->refcount)
		pclient->refcount--;

	if (pclient->refcount)
		pclient->usecase_ndx = VOTE_INDEX_LOW;
	else
		pclient->usecase_ndx = VOTE_INDEX_DISABLE;

	list_for_each_entry(client, &phandle->power_client_clist, list) {
		if (client->usecase_ndx < VOTE_INDEX_MAX &&
		    client->usecase_ndx > max_usecase_ndx)
			max_usecase_ndx = client->usecase_ndx;
	}

	if (phandle->current_usecase_ndx != max_usecase_ndx) {
		changed = true;
		prev_usecase_ndx = phandle->current_usecase_ndx;
		phandle->current_usecase_ndx = max_usecase_ndx;
	}

	pr_debug("%pS: changed=%d current idx=%d request client %s id:%u enable:%d refcount:%d\n",
		__builtin_return_address(0), changed, max_usecase_ndx,
		pclient->name, pclient->id, enable, pclient->refcount);

	if (!changed)
		goto end;

	if (enable) {
		dpu_power_event_trigger_locked(phandle,
				DPU_POWER_EVENT_PRE_ENABLE);

		rc = dpu_power_data_bus_update(&phandle->data_bus_handle,
						enable);
		if (rc) {
			pr_err("failed to set bus vote rc=%d\n", rc);
			goto bus_update_err;
		}

		dpu_power_event_trigger_locked(phandle,
				DPU_POWER_EVENT_POST_ENABLE);

	} else {
		dpu_power_event_trigger_locked(phandle,
				DPU_POWER_EVENT_PRE_DISABLE);

		dpu_power_data_bus_update(&phandle->data_bus_handle,
					enable);

		dpu_power_event_trigger_locked(phandle,
				DPU_POWER_EVENT_POST_DISABLE);
	}

end:
	mutex_unlock(&phandle->phandle_lock);
	return rc;

bus_update_err:
	phandle->current_usecase_ndx = prev_usecase_ndx;
	mutex_unlock(&phandle->phandle_lock);
	return rc;
}

struct dpu_power_event *dpu_power_handle_register_event(
		struct dpu_power_handle *phandle,
		u32 event_type, void (*cb_fnc)(u32 event_type, void *usr),
		void *usr, char *client_name)
{
	struct dpu_power_event *event;

	if (!phandle) {
		pr_err("invalid power handle\n");
		return ERR_PTR(-EINVAL);
	} else if (!cb_fnc || !event_type) {
		pr_err("no callback fnc or event type\n");
		return ERR_PTR(-EINVAL);
	}

	event = kzalloc(sizeof(struct dpu_power_event), GFP_KERNEL);
	if (!event)
		return ERR_PTR(-ENOMEM);

	event->event_type = event_type;
	event->cb_fnc = cb_fnc;
	event->usr = usr;
	strlcpy(event->client_name, client_name, MAX_CLIENT_NAME_LEN);
	event->active = true;

	mutex_lock(&phandle->phandle_lock);
	list_add(&event->list, &phandle->event_list);
	mutex_unlock(&phandle->phandle_lock);

	return event;
}

void dpu_power_handle_unregister_event(
		struct dpu_power_handle *phandle,
		struct dpu_power_event *event)
{
	if (!phandle || !event) {
		pr_err("invalid phandle or event\n");
	} else if (!event->active) {
		pr_err("power handle deinit already done\n");
		kfree(event);
	} else {
		mutex_lock(&phandle->phandle_lock);
		list_del_init(&event->list);
		mutex_unlock(&phandle->phandle_lock);
		kfree(event);
	}
}
