// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#define pr_fmt(fmt)    "ipa %s:%d " fmt, __func__, __LINE__

#include <asm/barrier.h>
#include <linux/delay.h>
#include <linux/device.h>
#include "ipa_i.h"

/* These values were determined empirically and shows good E2E bi-
 * directional throughputs
 */
#define IPA_HOLB_TMR_EN 0x1
#define IPA_HOLB_TMR_DIS 0x0
#define IPA_POLL_AGGR_STATE_RETRIES_NUM 3
#define IPA_POLL_AGGR_STATE_SLEEP_MSEC 1

#define IPA_PKT_FLUSH_TO_US 100

static int
ipa_reconfigure_channel_to_gpi(struct ipa_ep_context *ep,
			       struct gsi_chan_props *orig_chan_props,
			       struct ipa_mem_buffer *chan_dma)
{
	struct gsi_chan_props chan_props;

	/* Allocate the DMA space first; it can fail */
	if (ipahal_dma_alloc(chan_dma, 2 * GSI_CHAN_RING_ELEMENT_SIZE,
			     GFP_KERNEL))
		return -ENOMEM;

	/* Set up channel properties */
	memset(&chan_props, 0, sizeof(struct gsi_chan_props));
	chan_props.from_gsi = true;
	chan_props.ch_id = orig_chan_props->ch_id;
	chan_props.evt_ring_hdl = orig_chan_props->evt_ring_hdl;
	chan_props.mem = *chan_dma;
	chan_props.use_db_engine = false;
	chan_props.low_weight = 1;
	chan_props.chan_user_data = NULL;

	if (gsi_set_channel_cfg(ep->gsi_chan_hdl, &chan_props)) {
		ipa_err("Error setting channel properties\n");
		ipahal_dma_free(chan_dma);
		return -EFAULT;
	}

	return 0;
}

static int
ipa_restore_channel_properties(struct ipa_ep_context *ep,
			       struct gsi_chan_props *chan_props)
{
	int gsi_res;

	gsi_res = gsi_set_channel_cfg(ep->gsi_chan_hdl, chan_props);
	if (gsi_res) {
		ipa_err("Error restoring channel properties\n");
		return -EFAULT;
	}

	return 0;
}

static int
ipa_reset_with_open_aggr_frame_wa(u32 clnt_hdl, struct ipa_ep_context *ep)
{
	int result;
	int gsi_res;
	struct gsi_chan_props orig_chan_props;
	struct ipa_mem_buffer chan_dma;
	struct ipa_mem_buffer dma_byte;
	struct gsi_xfer_elem xfer_elem;
	int i;
	int aggr_active_bitmap = 0;
	bool pipe_suspended = false;
	struct ipa_ep_cfg_ctrl ctrl;

	ipa_debug("Applying reset channel with open aggregation frame WA\n");
	ipahal_write_reg(IPA_AGGR_FORCE_CLOSE, (1 << clnt_hdl));

	/* Reset channel */
	gsi_res = gsi_reset_channel(ep->gsi_chan_hdl);
	if (gsi_res) {
		ipa_err("Error resetting channel: %d\n", gsi_res);
		return -EFAULT;
	}

	/* Reconfigure channel to dummy GPI channel */
	memset(&orig_chan_props, 0, sizeof(struct gsi_chan_props));
	gsi_res = gsi_get_channel_cfg(ep->gsi_chan_hdl, &orig_chan_props);
	if (gsi_res) {
		ipa_err("Error getting channel properties: %d\n", gsi_res);
		return -EFAULT;
	}
	result = ipa_reconfigure_channel_to_gpi(ep, &orig_chan_props,
						&chan_dma);
	if (result)
		return -EFAULT;

	ipahal_read_reg_n_fields(IPA_ENDP_INIT_CTRL_n, clnt_hdl, &ctrl);
	if (ctrl.ipa_ep_suspend) {
		ipa_debug("pipe is suspended, remove suspend\n");
		pipe_suspended = true;
		ctrl.ipa_ep_suspend = false;
		ipahal_write_reg_n_fields(IPA_ENDP_INIT_CTRL_n, clnt_hdl,
					  &ctrl);
	}

	/* Start channel and put 1 Byte descriptor on it */
	gsi_res = gsi_start_channel(ep->gsi_chan_hdl);
	if (gsi_res) {
		ipa_err("Error starting channel: %d\n", gsi_res);
		result = -EFAULT;
		goto start_chan_fail;
	}

	if (ipahal_dma_alloc(&dma_byte, 1, GFP_KERNEL)) {
		ipa_err("Error allocating DMA\n");
		result = -ENOMEM;
		goto dma_alloc_fail;
	}

	memset(&xfer_elem, 0, sizeof(struct gsi_xfer_elem));
	xfer_elem.addr = dma_byte.phys_base;
	xfer_elem.len = 1;	/* = dma_byte.size; */
	xfer_elem.flags = GSI_XFER_FLAG_EOT;
	xfer_elem.type = GSI_XFER_ELEM_DATA;

	gsi_res = gsi_queue_xfer(ep->gsi_chan_hdl, 1, &xfer_elem, true);
	if (gsi_res) {
		ipa_err("Error queueing xfer: %d\n", gsi_res);
		result = -EFAULT;
		goto queue_xfer_fail;
	}

	/* Wait for aggregation frame to be closed and stop channel*/
	for (i = 0; i < IPA_POLL_AGGR_STATE_RETRIES_NUM; i++) {
		aggr_active_bitmap = ipahal_read_reg(IPA_STATE_AGGR_ACTIVE);
		if (!(aggr_active_bitmap & (1 << clnt_hdl)))
			break;
		msleep(IPA_POLL_AGGR_STATE_SLEEP_MSEC);
	}

	ipa_bug_on(aggr_active_bitmap & (1 << clnt_hdl));

	ipahal_dma_free(&dma_byte);

	result = ipa_stop_gsi_channel(clnt_hdl);
	if (result) {
		ipa_err("Error stopping channel: %d\n", result);
		goto start_chan_fail;
	}

	/* Reset channel */
	gsi_res = gsi_reset_channel(ep->gsi_chan_hdl);
	if (gsi_res) {
		ipa_err("Error resetting channel: %d\n", gsi_res);
		result = -EFAULT;
		goto start_chan_fail;
	}

	/* Need to sleep for 1ms as required by H/W verified
	 * sequence for resetting GSI channel
	 */
	msleep(IPA_POLL_AGGR_STATE_SLEEP_MSEC);

	if (pipe_suspended) {
		ipa_debug("suspend the pipe again\n");
		ctrl.ipa_ep_suspend = true;
		ipahal_write_reg_n_fields(IPA_ENDP_INIT_CTRL_n, clnt_hdl,
					  &ctrl);
	}

	/* Restore channels properties */
	result = ipa_restore_channel_properties(ep, &orig_chan_props);
	if (result)
		goto restore_props_fail;
	ipahal_dma_free(&chan_dma);

	return 0;

queue_xfer_fail:
	ipahal_dma_free(&dma_byte);
dma_alloc_fail:
	ipa_stop_gsi_channel(clnt_hdl);
start_chan_fail:
	if (pipe_suspended) {
		ipa_debug("suspend the pipe again\n");
		ctrl.ipa_ep_suspend = true;
		ipahal_write_reg_n_fields(IPA_ENDP_INIT_CTRL_n, clnt_hdl,
					  &ctrl);
	}
	ipa_restore_channel_properties(ep, &orig_chan_props);
restore_props_fail:
	ipahal_dma_free(&chan_dma);

	return result;
}

void ipa_reset_gsi_channel(u32 clnt_hdl)
{
	struct ipa_ep_context *ep;
	u32 aggr_active_bitmap;

	ipa_debug("entry\n");

	ipa_bug_on(clnt_hdl >= ipa_ctx->ipa_num_pipes);

	ep = &ipa_ctx->ep[clnt_hdl];
	ipa_bug_on(!ep->valid);

	ipa_client_add(ipa_client_string(ipa_get_client_mapping(clnt_hdl)),
		       true);
	/* Check for open aggregation frame on Consumer EP -
	 * reset with open aggregation frame WA
	 */
	if (IPA_CLIENT_IS_CONS(ep->client))
		aggr_active_bitmap = ipahal_read_reg(IPA_STATE_AGGR_ACTIVE);
	else
		aggr_active_bitmap = 0;

	if (aggr_active_bitmap & (1 << clnt_hdl)) {
		ipa_bug_on(ipa_reset_with_open_aggr_frame_wa(clnt_hdl, ep));
	} else {
		/* If the reset called after stop, need to wait 1ms */
		msleep(IPA_POLL_AGGR_STATE_SLEEP_MSEC);
		ipa_bug_on(gsi_reset_channel(ep->gsi_chan_hdl));
	}

	ipa_client_remove(ipa_client_string(ipa_get_client_mapping(clnt_hdl)),
			  true);

	ipa_debug("exit\n");
}
