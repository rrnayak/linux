// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#define pr_fmt(fmt)    "gsi %s:%d " fmt, __func__, __LINE__

#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>

#include "ipa_i.h"
#include "gsi.h"
#include "gsi_reg.h"

#define GSI_CHAN_MAX	  31
#define GSI_EVT_RING_MAX  23

#define GSI_CMD_TIMEOUT		msecs_to_jiffies(5000)
#define GSI_STOP_CMD_TIMEOUT	msecs_to_jiffies(20)

#define GSI_MAX_CH_LOW_WEIGHT	15
#define GSI_MHI_ER_START	10
#define GSI_MHI_ER_END		16

#define GSI_RESET_WA_MIN_SLEEP	1000
#define GSI_RESET_WA_MAX_SLEEP	2000

#define GSI_MAX_PREFETCH	0	/* 0 means 1 segment; 1 means 2 */

enum gsi_err_code {
	GSI_INVALID_TRE_ERR = 0x1,
	GSI_OUT_OF_BUFFERS_ERR = 0x2,
	GSI_OUT_OF_RESOURCES_ERR = 0x3,
	GSI_UNSUPPORTED_INTER_EE_OP_ERR = 0x4,
	GSI_EVT_RING_EMPTY_ERR = 0x5,
	GSI_NON_ALLOCATED_EVT_ACCESS_ERR = 0x6,
	GSI_HWO_1_ERR = 0x8
};

enum gsi_evt_chtype {
	GSI_EVT_CHTYPE_MHI_EV = 0x0,
	GSI_EVT_CHTYPE_XHCI_EV = 0x1,
	GSI_EVT_CHTYPE_GPI_EV = 0x2,
	GSI_EVT_CHTYPE_XDCI_EV = 0x3
};

enum gsi_chan_prot {
	GSI_CHAN_PROT_MHI = 0x0,
	GSI_CHAN_PROT_XHCI = 0x1,
	GSI_CHAN_PROT_GPI = 0x2,
	GSI_CHAN_PROT_XDCI = 0x3
};

enum gsi_chan_evt {
	GSI_CHAN_EVT_INVALID = 0x0,
	GSI_CHAN_EVT_SUCCESS = 0x1,
	GSI_CHAN_EVT_EOT = 0x2,
	GSI_CHAN_EVT_OVERFLOW = 0x3,
	GSI_CHAN_EVT_EOB = 0x4,
	GSI_CHAN_EVT_OOB = 0x5,
	GSI_CHAN_EVT_DB_MODE = 0x6,
	GSI_CHAN_EVT_UNDEFINED = 0x10,
	GSI_CHAN_EVT_RE_ERROR = 0x11,
};

enum gsi_evt_ring_state {
	GSI_EVT_RING_STATE_NOT_ALLOCATED = 0x0,
	GSI_EVT_RING_STATE_ALLOCATED = 0x1,
	GSI_EVT_RING_STATE_ERROR = 0xf
};

enum gsi_chan_state {
	GSI_CHAN_STATE_NOT_ALLOCATED = 0x0,
	GSI_CHAN_STATE_ALLOCATED = 0x1,
	GSI_CHAN_STATE_STARTED = 0x2,
	GSI_CHAN_STATE_STOPPED = 0x3,
	GSI_CHAN_STATE_STOP_IN_PROC = 0x4,
	GSI_CHAN_STATE_ERROR = 0xf
};

struct gsi_ring_ctx {
	spinlock_t slock;	/* XXX comment this */
	struct ipa_mem_buffer mem;
	u64 wp;
	u64 rp;
	u64 wp_local;
	u64 rp_local;
	u8 elem_sz;
	u16 max_num_elem;
	u64 end;
};

struct gsi_chan_ctx {
	struct gsi_chan_props props;
	enum gsi_chan_state state;
	struct gsi_ring_ctx ring;
	void **user_data;
	struct gsi_evt_ctx *evtr;
	struct mutex mlock;	/* XXX comment this */
	struct completion compl;
	bool allocated;
	atomic_t poll_mode;
	u32 tlv_size;		/* slots in TLV */
};

struct gsi_evt_ctx {
	struct ipa_mem_buffer mem;
	u16 int_modt;
	enum gsi_evt_ring_state state;
	u8 id;
	struct gsi_ring_ctx ring;
	struct mutex mlock;	/* XXX comment this */
	struct completion compl;
	struct gsi_chan_ctx *chan;
	atomic_t chan_ref_cnt;
};

struct ch_debug_stats {
	unsigned long ch_allocate;
	unsigned long ch_start;
	unsigned long ch_stop;
	unsigned long ch_reset;
	unsigned long ch_de_alloc;
	unsigned long ch_db_stop;
	unsigned long cmd_completed;
};

struct gsi_ctx {
	void __iomem *base;
	struct device *dev;
	u32 ee;
	u32 phys_base;
	unsigned int irq;
	bool per_registered;
	struct gsi_chan_ctx chan[GSI_CHAN_MAX];
	struct ch_debug_stats ch_dbg[GSI_CHAN_MAX];
	struct gsi_evt_ctx evtr[GSI_EVT_RING_MAX];
	struct mutex mlock;	/* XXX comment this */
	spinlock_t slock;	/* XXX comment this */
	unsigned long evt_bmap;
	atomic_t num_chan;
	atomic_t num_evt_ring;
	u32 max_ch;
	u32 max_ev;
};

static struct gsi_ctx *gsi_ctx;

enum gsi_re_type {
	GSI_RE_XFER = 0x2,
	GSI_RE_IMMD_CMD = 0x3,
	GSI_RE_NOP = 0x4,
};

struct gsi_tre {
	u64 buffer_ptr;
	u16 buf_len;
	u16 resvd1;
	u16 chain:1;
	u16 resvd4:7;
	u16 ieob:1;
	u16 ieot:1;
	u16 bei:1;
	u16 resvd3:5;
	u8 re_type;
	u8 resvd2;
} __packed;

struct gsi_xfer_compl_evt {
	u64 xfer_ptr;
	u16 len;
	u8 resvd1;
	u8 code;  /* see gsi_chan_evt */
	u16 resvd;
	u8 type;
	u8 chid;
} __packed;

enum gsi_err_type {
	GSI_ERR_TYPE_GLOB = 0x1,
	GSI_ERR_TYPE_CHAN = 0x2,
	GSI_ERR_TYPE_EVT = 0x3,
};

struct gsi_log_err {
	u32 arg3:4;
	u32 arg2:4;
	u32 arg1:4;
	u32 code:4;
	u32 resvd:3;
	u32 virt_idx:5;
	u32 err_type:4;
	u32 ee:4;
} __packed;

enum gsi_ch_cmd_opcode {
	GSI_CH_ALLOCATE = 0x0,
	GSI_CH_START = 0x1,
	GSI_CH_STOP = 0x2,
	GSI_CH_RESET = 0x9,
	GSI_CH_DE_ALLOC = 0xa,
	GSI_CH_DB_STOP = 0xb,
};

enum gsi_evt_ch_cmd_opcode {
	GSI_EVT_ALLOCATE = 0x0,
	GSI_EVT_RESET = 0x9,  /* TODO: is this valid? */
	GSI_EVT_DE_ALLOC = 0xa,
};

/** gsi_gpi_channel_scratch - GPI protocol SW config area of channel scratch
 *
 * @max_outstanding_tre: Used for the prefetch management sequence by the
 *			 sequencer. Defines the maximum number of allowed
 *			 outstanding TREs in IPA/GSI (in Bytes). RE engine
 *			 prefetch will be limited by this configuration. It
 *			 is suggested to configure this value to IPA_IF
 *			 channel TLV queue size times element size. To disable
 *			 the feature in doorbell mode (DB Mode=1). Maximum
 *			 outstanding TREs should be set to 64KB
 *			 (or any value larger or equal to ring length . RLEN)
 * @outstanding_threshold: Used for the prefetch management sequence by the
 *			 sequencer. Defines the threshold (in Bytes) as to when
 *			 to update the channel doorbell. Should be smaller than
 *			 Maximum outstanding TREs. value. It is suggested to
 *			 configure this value to 2 * element size.
 */
struct gsi_gpi_channel_scratch {
	u64 resvd1;
	u32 resvd2:16;
	u32 max_outstanding_tre:16;
	u32 resvd3:16;
	u32 outstanding_threshold:16;
} __packed;

/** gsi_channel_scratch - channel scratch SW config area */
union gsi_channel_scratch {
	struct gsi_gpi_channel_scratch gpi;
	struct {
		u32 word1;
		u32 word2;
		u32 word3;
		u32 word4;
	} data __packed;
} __packed;

/* Read a value from the given offset into the I/O space defined in
 * the GSI context.
 */
static u32 gsi_readl(u32 offset)
{
	return readl(gsi_ctx->base + offset);
}

/* Write the provided value to the given offset into the I/O space
 * defined in the GSI context.
 */
static void gsi_writel(u32 v, u32 offset)
{
	writel(v, gsi_ctx->base + offset);
}

static void gsi_irq_set(u32 offset, u32 val)
{
	gsi_writel(val, offset);
}

static void gsi_irq_update(u32 offset, u32 mask, u32 val)
{
	u32 curr;

	curr = gsi_readl(offset);
	val = (curr & ~mask) | (val & mask);
	gsi_writel(val, offset);
}

static void gsi_irq_control_event(u32 ee, u8 evt_id, bool enable)
{
	u32 mask = BIT(evt_id);
	u32 val = enable ? ~0 : 0;

	gsi_irq_update(GSI_EE_N_CNTXT_SRC_IEOB_IRQ_MSK_OFFS(ee), mask, val);
}

static void gsi_irq_control_all(u32 ee, bool enable)
{
	u32 val = enable ? ~0 : 0;

	/* Inter EE commands / interrupt are no supported. */
	gsi_irq_set(GSI_EE_N_CNTXT_TYPE_IRQ_MSK_OFFS(ee), val);
	gsi_irq_set(GSI_EE_N_CNTXT_SRC_GSI_CH_IRQ_MSK_OFFS(ee), val);
	gsi_irq_set(GSI_EE_N_CNTXT_SRC_EV_CH_IRQ_MSK_OFFS(ee), val);
	gsi_irq_set(GSI_EE_N_CNTXT_SRC_IEOB_IRQ_MSK_OFFS(ee), val);
	gsi_irq_set(GSI_EE_N_CNTXT_GLOB_IRQ_EN_OFFS(ee), val);
	/* Never enable GSI_BREAK_POINT */
	val &= ~field_gen(1, EN_GSI_BREAK_POINT_BMSK);
	gsi_irq_set(GSI_EE_N_CNTXT_GSI_IRQ_EN_OFFS(ee), val);
}

static void gsi_handle_chan_ctrl(void)
{
	u32 ee = gsi_ctx->ee;
	u32 valid_mask = GENMASK(gsi_ctx->max_ch - 1, 0);
	u32 chan_mask;

	chan_mask = gsi_readl(GSI_EE_N_CNTXT_SRC_GSI_CH_IRQ_OFFS(ee));
	gsi_writel(chan_mask, GSI_EE_N_CNTXT_SRC_GSI_CH_IRQ_CLR_OFFS(ee));

	ipa_debug("chan_mask %x\n", chan_mask);
	if (chan_mask & ~valid_mask) {
		ipa_err("invalid channels (> %u)\n", gsi_ctx->max_ch);
		chan_mask &= valid_mask;
	}

	while (chan_mask) {
		int i = __ffs(chan_mask);
		struct gsi_chan_ctx *ctx = &gsi_ctx->chan[i];
		u32 val;

		val = gsi_readl(GSI_EE_N_GSI_CH_K_CNTXT_0_OFFS(i, ee));
		ctx->state = field_val(val, CHSTATE_BMSK);
		ipa_debug("ch %d state updated to %u\n", i, ctx->state);

		complete(&ctx->compl);

		chan_mask ^= BIT(i);
	}
}

static void gsi_handle_evt_ctrl(void)
{
	u32 ee = gsi_ctx->ee;
	u32 valid_mask = GENMASK(gsi_ctx->max_ev - 1, 0);
	u32 evt_mask;

	evt_mask = gsi_readl(GSI_EE_N_CNTXT_SRC_EV_CH_IRQ_OFFS(ee));
	gsi_writel(evt_mask, GSI_EE_N_CNTXT_SRC_EV_CH_IRQ_CLR_OFFS(ee));

	ipa_debug("evt_mask %x\n", evt_mask);
	if (evt_mask & ~valid_mask) {
		ipa_err("invalid events (> %u)\n", gsi_ctx->max_ev);
		evt_mask &= valid_mask;
	}

	while (evt_mask) {
		int i = __ffs(evt_mask);
		struct gsi_evt_ctx *ctx = &gsi_ctx->evtr[i];
		u32 val;

		val = gsi_readl(GSI_EE_N_EV_CH_K_CNTXT_0_OFFS(i, ee));
		ctx->state = field_val(val, EV_CHSTATE_BMSK);
		ipa_debug("evt %d state updated to %u\n", i, ctx->state);

		complete(&ctx->compl);

		evt_mask ^= BIT(i);
	}
}

static void
handle_glob_chan_err(u32 err_ee, u32 chan_id, u32 code)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];
	u32 ee = gsi_ctx->ee;
	u32 val;

	ipa_bug_on(err_ee != ee && code != GSI_UNSUPPORTED_INTER_EE_OP_ERR);

	if (WARN_ON(chan_id >= gsi_ctx->max_ch)) {
		ipa_err("unexpected chan_id %u\n", chan_id);
		return;
	}

	switch (code) {
	case GSI_INVALID_TRE_ERR:
		ipa_err("got INVALID_TRE_ERR\n");
		val = gsi_readl(GSI_EE_N_GSI_CH_K_CNTXT_0_OFFS(chan_id, ee));
		ctx->state = field_val(val, CHSTATE_BMSK);
		ipa_debug("chan_id %u state updated to %u\n", chan_id,
			  ctx->state);
		ipa_bug_on(ctx->state != GSI_CHAN_STATE_ERROR);
		break;
	case GSI_OUT_OF_BUFFERS_ERR:
		ipa_err("got OUT_OF_BUFFERS_ERR\n");
		break;
	case GSI_OUT_OF_RESOURCES_ERR:
		ipa_err("got OUT_OF_RESOURCES_ERR\n");
		complete(&ctx->compl);
		break;
	case GSI_UNSUPPORTED_INTER_EE_OP_ERR:
		ipa_err("got UNSUPPORTED_INTER_EE_OP_ERR\n");
		break;
	case GSI_NON_ALLOCATED_EVT_ACCESS_ERR:
		ipa_err("got NON_ALLOCATED_EVT_ACCESS_ERR\n");
		break;
	case GSI_HWO_1_ERR:
		ipa_err("got HWO_1_ERR\n");
		break;
	default:
		ipa_err("unexpected channel error code %u\n", code);
		ipa_bug();
	}
	ipa_assert(ctx->props.chan_user_data);
}

static void
handle_glob_evt_err(u32 err_ee, u32 evt_id, u32 code)
{
	struct gsi_evt_ctx *ctx = &gsi_ctx->evtr[evt_id];
	u32 ee = gsi_ctx->ee;

	ipa_bug_on(err_ee != ee && code != GSI_UNSUPPORTED_INTER_EE_OP_ERR);

	if (WARN_ON(evt_id >= gsi_ctx->max_ev)) {
		ipa_err("unexpected evt_id %u\n", evt_id);
		return;
	}

	switch (code) {
	case GSI_OUT_OF_BUFFERS_ERR:
		ipa_err("got OUT_OF_BUFFERS_ERR\n");
		break;
	case GSI_OUT_OF_RESOURCES_ERR:
		ipa_err("got OUT_OF_RESOURCES_ERR\n");
		complete(&ctx->compl);
		break;
	case GSI_UNSUPPORTED_INTER_EE_OP_ERR:
		ipa_err("got UNSUPPORTED_INTER_EE_OP_ERR\n");
		break;
	case GSI_EVT_RING_EMPTY_ERR:
		ipa_err("got EVT_RING_EMPTY_ERR\n");
		break;
	default:
		ipa_err("unexpected event error code %u\n", code);
		ipa_bug();
	}
}

static void gsi_handle_glob_err(u32 err)
{
	struct gsi_log_err *log = (struct gsi_log_err *)&err;

	ipa_err("log err_type %u ee %u idx %u\n", log->err_type, log->ee,
		log->virt_idx);
	ipa_err("log code 0x%1x arg1 0x%1x arg2 0x%1x arg3 0x%1x\n", log->code,
		log->arg1, log->arg2, log->arg3);

	ipa_bug_on(log->err_type == GSI_ERR_TYPE_GLOB);

	switch (log->err_type) {
	case GSI_ERR_TYPE_CHAN:
		handle_glob_chan_err(log->ee, log->virt_idx, log->code);
		break;
	case GSI_ERR_TYPE_EVT:
		handle_glob_evt_err(log->ee, log->virt_idx, log->code);
		break;
	default:
		WARN_ON(1);
	}
}

static void gsi_handle_glob_ee(void)
{
	u32 ee = gsi_ctx->ee;
	u32 val;

	val = gsi_readl(GSI_EE_N_CNTXT_GLOB_IRQ_STTS_OFFS(ee));

	if (val & ERROR_INT_BMSK) {
		u32 err = gsi_readl(GSI_EE_N_ERROR_LOG_OFFS(ee));

		gsi_writel(0, GSI_EE_N_ERROR_LOG_OFFS(ee));
		gsi_writel(~0, GSI_EE_N_ERROR_LOG_CLR_OFFS(ee));

		gsi_handle_glob_err(err);
	}

	if (val & EN_GP_INT1_BMSK)
		ipa_err("unexpected GP INT1 received\n");

	ipa_bug_on(val & EN_GP_INT2_BMSK);
	ipa_bug_on(val & EN_GP_INT3_BMSK);

	gsi_writel(val, GSI_EE_N_CNTXT_GLOB_IRQ_CLR_OFFS(ee));
}

static void ring_wp_local_inc(struct gsi_ring_ctx *ctx)
{
	ctx->wp_local += ctx->elem_sz;
	if (ctx->wp_local == ctx->end)
		ctx->wp_local = ctx->mem.phys_base;
}

static void ring_rp_local_inc(struct gsi_ring_ctx *ctx)
{
	ctx->rp_local += ctx->elem_sz;
	if (ctx->rp_local == ctx->end)
		ctx->rp_local = ctx->mem.phys_base;
}

static u16 ring_rp_local_index(struct gsi_ring_ctx *ctx)
{
	return (u16)(ctx->rp_local - ctx->mem.phys_base) / ctx->elem_sz;
}

static u16 ring_wp_local_index(struct gsi_ring_ctx *ctx)
{
	return (u16)(ctx->wp_local - ctx->mem.phys_base) / ctx->elem_sz;
}

static void chan_xfer_cb(struct gsi_chan_ctx *ctx, u8 evt_id, u16 count)
{
	void *chan_data;
	void *xfer_data;

	if (evt_id != GSI_CHAN_EVT_EOT) {
		ipa_err("ch %hhu unexpected %sX event id %hhu\n",
			ctx->props.ch_id, ctx->props.from_gsi ? "R" : "T",
			evt_id);
		return;
	}

	chan_data = ctx->props.chan_user_data;
	xfer_data = ctx->user_data[ring_rp_local_index(&ctx->ring)];
	if (ctx->props.from_gsi)
		ipa_gsi_irq_rx_notify_cb(chan_data, xfer_data, count);
	else
		ipa_gsi_irq_tx_notify_cb(chan_data, xfer_data, count);
}

static u16 gsi_process_chan(struct gsi_xfer_compl_evt *evt, bool callback)
{
	struct gsi_chan_ctx *ctx;
	u32 chan_id = evt->chid;

	if (WARN_ON(chan_id >= gsi_ctx->max_ch)) {
		ipa_err("unexpected chan_id %u\n", chan_id);
		return 0;
	}

	ctx = &gsi_ctx->chan[chan_id];
	ctx->ring.rp_local = evt->xfer_ptr;

	if (callback)
		chan_xfer_cb(ctx, evt->code, evt->len);

	/* Record that we've processed this channel ring element. */
	ring_rp_local_inc(&ctx->ring);
	ctx->ring.rp = ctx->ring.rp_local;

	return evt->len;
}

static u16 gsi_process_evt_re(struct gsi_evt_ctx *ctx, bool callback)
{
	struct gsi_xfer_compl_evt *evt;
	u16 idx = ring_rp_local_index(&ctx->ring);
	u16 size;

	evt = ctx->ring.mem.base + idx * ctx->ring.elem_sz;
	size = gsi_process_chan(evt, callback);
	ring_rp_local_inc(&ctx->ring);

	/* recycle this element */
	ring_wp_local_inc(&ctx->ring);

	return size;
}

static void gsi_ring_evt_doorbell(struct gsi_evt_ctx *ctx)
{
	u32 val;

	/* The doorbell 0 and 1 registers store the low-order and
	 * high-order 32 bits of the event ring doorbell register,
	 * respectively.  LSB (doorbell 0) must be written last.
	 */
	val = ctx->ring.wp_local >> 32;
	gsi_writel(val, GSI_EE_N_EV_CH_K_DOORBELL_1_OFFS(ctx->id, gsi_ctx->ee));

	val = ctx->ring.wp_local & GENMASK(31, 0);
	gsi_writel(val, GSI_EE_N_EV_CH_K_DOORBELL_0_OFFS(ctx->id, gsi_ctx->ee));
}

static void gsi_ring_chan_doorbell(struct gsi_chan_ctx *ctx)
{
	u32 val;

	/* allocate new events for this channel first
	 * before submitting the new TREs.
	 * for TO_GSI channels the event ring doorbell is rang as part of
	 * interrupt handling.
	 */
	if (ctx->props.from_gsi)
		gsi_ring_evt_doorbell(ctx->evtr);
	ctx->ring.wp = ctx->ring.wp_local;

	/* The doorbell 0 and 1 registers store the low-order and
	 * high-order 32 bits of the channel ring doorbell register,
	 * respectively.  LSB (doorbell 0) must be written last.
	 */
	val = ctx->ring.wp_local >> 32;
	gsi_writel(val, GSI_EE_N_GSI_CH_K_DOORBELL_1_OFFS(ctx->props.ch_id,
							  gsi_ctx->ee));
	val = ctx->ring.wp_local & GENMASK(31, 0);
	gsi_writel(val, GSI_EE_N_GSI_CH_K_DOORBELL_0_OFFS(ctx->props.ch_id,
							  gsi_ctx->ee));
}

static void handle_event(int evt_id)
{
	struct gsi_evt_ctx *ctx = &gsi_ctx->evtr[evt_id];
	u32 ee = gsi_ctx->ee;
	unsigned long flags;
	bool check_again;

	spin_lock_irqsave(&ctx->ring.slock, flags);

	do {
		u32 val;

		val = gsi_readl(GSI_EE_N_EV_CH_K_CNTXT_4_OFFS(evt_id, ee));
		ctx->ring.rp = (ctx->ring.rp & GENMASK_ULL(63, 32)) | val;

		check_again = false;
		while (ctx->ring.rp_local != ctx->ring.rp) {
			if (atomic_read(&ctx->chan->poll_mode)) {
				check_again = false;
				break;
			}
			check_again = true;
			(void)gsi_process_evt_re(ctx, true);
		}

		gsi_ring_evt_doorbell(ctx);
	} while (check_again);

	spin_unlock_irqrestore(&ctx->ring.slock, flags);
}

static void gsi_handle_ieob(void)
{
	u32 ee = gsi_ctx->ee;
	u32 valid_mask = GENMASK(gsi_ctx->max_ev - 1, 0);
	u32 evt_mask;

	evt_mask = gsi_readl(GSI_EE_N_CNTXT_SRC_IEOB_IRQ_OFFS(ee));
	evt_mask &= gsi_readl(GSI_EE_N_CNTXT_SRC_IEOB_IRQ_MSK_OFFS(ee));
	gsi_writel(evt_mask, GSI_EE_N_CNTXT_SRC_IEOB_IRQ_CLR_OFFS(ee));

	if (evt_mask & ~valid_mask) {
		ipa_err("invalid events (> %u)\n", gsi_ctx->max_ev);
		evt_mask &= valid_mask;
	}

	while (evt_mask) {
		int i = __ffs(evt_mask);

		handle_event(i);

		evt_mask ^= BIT(i);
	}
}

static void gsi_handle_inter_ee_chan_ctrl(void)
{
	u32 ee = gsi_ctx->ee;
	u32 valid_mask = GENMASK(gsi_ctx->max_ch - 1, 0);
	u32 chan_mask;

	chan_mask = gsi_readl(GSI_INTER_EE_N_SRC_GSI_CH_IRQ_OFFS(ee));
	gsi_writel(chan_mask, GSI_INTER_EE_N_SRC_GSI_CH_IRQ_CLR_OFFS(ee));

	if (chan_mask & ~valid_mask) {
		ipa_err("invalid channels (> %u)\n", gsi_ctx->max_ch);
		chan_mask &= valid_mask;
	}

	while (chan_mask) {
		int i = __ffs(chan_mask);

		/* not currently expected */
		ipa_err("ch %d was inter-EE changed\n", i);
		chan_mask ^= BIT(i);
	}
}

static void gsi_handle_inter_ee_evt_ctrl(void)
{
	u32 ee = gsi_ctx->ee;
	u32 valid_mask = GENMASK(gsi_ctx->max_ev - 1, 0);
	u32 evt_mask;

	evt_mask = gsi_readl(GSI_INTER_EE_N_SRC_EV_CH_IRQ_OFFS(ee));
	gsi_writel(evt_mask, GSI_INTER_EE_N_SRC_EV_CH_IRQ_CLR_OFFS(ee));

	if (evt_mask & ~valid_mask) {
		ipa_err("invalid events (> %u)\n", gsi_ctx->max_ev);
		evt_mask &= valid_mask;
	}

	while (evt_mask) {
		int i = __ffs(evt_mask);

		/* not currently expected */
		ipa_err("evt %d was inter-EE changed\n", i);
		evt_mask ^= BIT(i);
	}
}

static void gsi_handle_general(void)
{
	u32 ee = gsi_ctx->ee;
	u32 val;

	val = gsi_readl(GSI_EE_N_CNTXT_GSI_IRQ_STTS_OFFS(ee));

	ipa_bug_on(val & CLR_GSI_MCS_STACK_OVRFLOW_BMSK);
	ipa_bug_on(val & CLR_GSI_CMD_FIFO_OVRFLOW_BMSK);
	ipa_bug_on(val & CLR_GSI_BUS_ERROR_BMSK);

	if (val & CLR_GSI_BREAK_POINT_BMSK)
		ipa_err("got breakpoint\n");

	gsi_writel(val, GSI_EE_N_CNTXT_GSI_IRQ_CLR_OFFS(ee));
}

#define GSI_ISR_MAX_ITER 50

static irqreturn_t gsi_isr(int irq, void *ctxt)
{
	u32 ee = gsi_ctx->ee;
	u32 cnt = 0;
	u32 type;

	ipa_assert(ctxt == gsi_ctx);

	while ((type = gsi_readl(GSI_EE_N_CNTXT_TYPE_IRQ_OFFS(ee)))) {
		ipa_debug_low("type %x\n", type);

		do {
			u32 single = BIT(__ffs(type));

			switch (single) {
			case CH_CTRL_BMSK:
				gsi_handle_chan_ctrl();
				break;
			case EV_CTRL_BMSK:
				gsi_handle_evt_ctrl();
				break;
			case GLOB_EE_BMSK:
				gsi_handle_glob_ee();
				break;
			case IEOB_BMSK:
				gsi_handle_ieob();
				break;
			case INTER_EE_CH_CTRL_BMSK:
				gsi_handle_inter_ee_chan_ctrl();
				break;
			case INTER_EE_EV_CTRL_BMSK:
				gsi_handle_inter_ee_evt_ctrl();
				break;
			case GENERAL_BMSK:
				gsi_handle_general();
				break;
			default:
				WARN(true, "%s: unrecognized type 0x%08x\n",
				     __func__, single);
				break;
			}
			type ^= single;
		} while (type);

		ipa_bug_on(++cnt > GSI_ISR_MAX_ITER);
	}

	return IRQ_HANDLED;
}

static u32 gsi_get_max_channels(void)
{
	u32 max_channels;
	u32 val;

	/* SDM845 uses GSI hardware version 1.3.0 */
	val = gsi_readl(GSI_EE_N_GSI_HW_PARAM_2_OFFS(gsi_ctx->ee));
	max_channels = field_val(val, GSI_NUM_CH_PER_EE_BMSK);

	if (WARN_ON(max_channels > GSI_CHAN_MAX)) {
		ipa_err("bad GSI max channels %u\n", max_channels);

		return 0;
	}
	ipa_debug("max channels %d\n", max_channels);

	return max_channels;
}

static u32 gsi_get_max_event_rings(void)
{
	u32 max_event_rings;
	u32 val;

	/* SDM845 uses GSI hardware version 1.3.0 */
	val = gsi_readl(GSI_EE_N_GSI_HW_PARAM_2_OFFS(gsi_ctx->ee));
	max_event_rings = field_val(val, GSI_NUM_EV_PER_EE_BMSK);

	if (WARN_ON(max_event_rings > GSI_EVT_RING_MAX)) {
		ipa_err("bad GSI max event rings %u\n", max_event_rings);

		return 0;
	}
	ipa_debug("max event rings %d\n", max_event_rings);

	return max_event_rings;
}

int gsi_register_device(void)
{
	struct platform_device *ipa_pdev = to_platform_device(gsi_ctx->dev);
	u32 val;
	int ret;

	if (gsi_ctx->per_registered) {
		ipa_err("per already registered\n");
		return -ENOTSUPP;
	}

	/* Get IPA GSI IRQ number */
	ret = platform_get_irq_byname(ipa_pdev, "gsi-irq");
	if (ret < 0) {
		ipa_err("failed to get gsi-irq!\n");
		return -ENODEV;
	}
	gsi_ctx->irq = ret;
	ipa_debug("GSI irq %u\n", gsi_ctx->irq);

	spin_lock_init(&gsi_ctx->slock);
	ret = devm_request_irq(gsi_ctx->dev, gsi_ctx->irq, gsi_isr,
			       IRQF_TRIGGER_HIGH, "gsi", gsi_ctx);
	if (ret) {
		ipa_err("failed to register isr for %u\n", gsi_ctx->irq);
		return -EIO;
	}

	ret = enable_irq_wake(gsi_ctx->irq);
	if (ret)
		ipa_err("failed to enable wake irq %u\n", gsi_ctx->irq);
	else
		ipa_err("GSI irq is wake enabled %u\n", gsi_ctx->irq);

	val = gsi_readl(GSI_EE_N_GSI_STATUS_OFFS(gsi_ctx->ee));
	if (!(val & ENABLED_BMSK)) {
		ipa_err("manager EE has not enabled GSI, GSI un-usable\n");
		return -EIO;
	}

	gsi_ctx->per_registered = true;
	mutex_init(&gsi_ctx->mlock);
	atomic_set(&gsi_ctx->num_chan, 0);
	atomic_set(&gsi_ctx->num_evt_ring, 0);

	gsi_ctx->max_ch = gsi_get_max_channels();
	if (!gsi_ctx->max_ch) {
		ipa_err("failed to get max channels\n");
		return -EIO;
	}
	gsi_ctx->max_ev = gsi_get_max_event_rings();
	if (!gsi_ctx->max_ev) {
		ipa_err("failed to get max event rings\n");
		return -EIO;
	}

	/* bitmap is max events excludes reserved events */
	gsi_ctx->evt_bmap = ~((1 << gsi_ctx->max_ev) - 1);
	gsi_ctx->evt_bmap |= ((1 << (GSI_MHI_ER_END + 1)) - 1) ^
		((1 << GSI_MHI_ER_START) - 1);

	/* Enable all interrupts */
	gsi_irq_control_all(gsi_ctx->ee, true);

	/* Writing 1 indicates IRQ interrupts; 0 would be MSI */
	gsi_writel(1, GSI_EE_N_CNTXT_INTSET_OFFS(gsi_ctx->ee));

	gsi_writel(0, GSI_EE_N_ERROR_LOG_OFFS(gsi_ctx->ee));

	return 0;
}

int gsi_deregister_device(void)
{
	if (atomic_read(&gsi_ctx->num_chan)) {
		ipa_err("%u channels are allocated\n",
			atomic_read(&gsi_ctx->num_chan));
		return -ENOTSUPP;
	}

	if (atomic_read(&gsi_ctx->num_evt_ring)) {
		ipa_err("%u evt rings are allocated\n",
			atomic_read(&gsi_ctx->num_evt_ring));
		return -ENOTSUPP;
	}

	/* Don't bother clearing the error log again (ERROR_LOG) or
	 * setting the interrupt type again (INTSET).  Disable all
	 * interrupts.
	 */
	gsi_irq_control_all(gsi_ctx->ee, false);

	/* Clean up everything else set up by gsi_register_device() */
	gsi_ctx->evt_bmap = 0;
	gsi_ctx->max_ev = 0;
	gsi_ctx->max_ch = 0;
	gsi_ctx->per_registered = false;
	/* XXX We don't know whether enabling this succeeded */
	/* (void)disable_irq_wake(gsi_ctx->irq); */

	return 0;
}

/* Determine whether the given GSI firmware address range is acceptable. */
bool gsi_firmware_size_ok(u32 base, u32 size)
{
	u32 load_address;

	/* The base has to match the first instruction address.  And
	 * the size can't exceed the number of 4-byte instructions
	 * we have room for.
	 */
	load_address = gsi_ctx->phys_base + GSI_INST_RAM_N_OFFS(0);

	return base == load_address && size <= 4 * (GSI_INST_RAM_N_MAX_N + 1);
}

/* Return the endpoint config info for the given pipe, or NULL */
static const struct ipa_gsi_ep_config *ipa_pipe_ep_config(u32 pipe_idx)
{
	enum ipa_client_type client;

	for (client = 0; client < IPA_CLIENT_MAX; client++) {
		const struct ipa_gsi_ep_config *ep_config;

		ep_config = ipa_get_gsi_ep_info(client);
		if (ep_config && ep_config->ipa_ep_num == pipe_idx)
			return ep_config;
	}

	return NULL;
}

/* Enable configuration of pipes */
static void gsi_tz_pipe_config_enable(bool enable)
{
	u32 val = 0x80000805;		/* Config enabled value */

	if (!enable)
		val |= 0x40000000;	/* Set the config locked bit */
	ipahal_write_reg(IPA_SPARE_REG_1, val);
}

/* Perform initialization normally done by TZ */
static void gsi_tz_pipe_setup(void)
{
	u32 tlv_offset = 0;	/* TLV FIFO (GSI->IPA) offset */
	u32 aos_offset = 0;	/* AOS FIFO (IPA->GSI) offset */
	u32 val;
	u32 i;

	ipahal_write_reg(IPA_ENABLE_GSI, 1);

	gsi_tz_pipe_config_enable(true);

	for (i = 0; i < IPA_MAX_NUM_PIPES; i++) {
		const struct ipa_gsi_ep_config *ep_config;

		ep_config = ipa_pipe_ep_config(i);
		if (!ep_config)
			continue;

		/* Write TLV, AOS, and CFG1 for enabled pipes only */
		if (i < ipa_ctx->ipa_num_pipes) {
			val = tlv_offset;
			val |= ep_config->ipa_if_tlv << 16;
			ipahal_write_reg_n(IPA_ENDP_GSI_CFG_TLV_n, i, val);
			tlv_offset += ep_config->ipa_if_tlv;

			val = aos_offset;
			val |= ep_config->ipa_if_aos << 16;
			ipahal_write_reg_n(IPA_ENDP_GSI_CFG_AOS_n, i, val);
			aos_offset += ep_config->ipa_if_aos;

			val = ep_config->ee;
			val |= ep_config->ipa_gsi_chan_num << 8;
			val |= 1 << 16;
			ipahal_write_reg_n(IPA_ENDP_GSI_CFG1_n, i, val);
		}

		/* Toggle the high bit on and off on CFG2 for all pipes */
		ipahal_write_reg_n(IPA_ENDP_GSI_CFG2_n, i, 1 << 31);
		ipahal_write_reg_n(IPA_ENDP_GSI_CFG2_n, i, 0);
	}

	gsi_tz_pipe_config_enable(false);
}

static void gsi_write_ieps(void)
{
	gsi_writel(1, GSI_IRAM_PTR_CH_CMD_OFFS);
	gsi_writel(2, GSI_IRAM_PTR_CH_DB_OFFS);
	gsi_writel(3, GSI_IRAM_PTR_CH_DIS_COMP_OFFS);
	gsi_writel(4, GSI_IRAM_PTR_CH_EMPTY_OFFS);
	gsi_writel(5, GSI_IRAM_PTR_EE_GENERIC_CMD_OFFS);
	gsi_writel(6, GSI_IRAM_PTR_EVENT_GEN_COMP_OFFS);
	gsi_writel(7, GSI_IRAM_PTR_INT_MOD_STOPPED_OFFS);
	gsi_writel(8, GSI_IRAM_PTR_PERIPH_IF_TLV_IN_0_OFFS);
	gsi_writel(9, GSI_IRAM_PTR_PERIPH_IF_TLV_IN_2_OFFS);
	gsi_writel(10, GSI_IRAM_PTR_PERIPH_IF_TLV_IN_1_OFFS);
	gsi_writel(11, GSI_IRAM_PTR_NEW_RE_OFFS);
	gsi_writel(12, GSI_IRAM_PTR_READ_ENG_COMP_OFFS);
	gsi_writel(13, GSI_IRAM_PTR_TIMER_EXPIRED_OFFS);
	gsi_writel(14, GSI_IRAM_PTR_EV_DB_OFFS);
	gsi_writel(15, GSI_IRAM_PTR_UC_GP_INT_OFFS);
	gsi_writel(16, GSI_IRAM_PTR_WRITE_ENG_COMP_OFFS);
}

static void gsi_config_backpressure(void)
{
	gsi_writel(0xfffffffe, GSI_IC_DISABLE_CHNL_BCK_PRS_LSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_DISABLE_CHNL_BCK_PRS_MSB_OFFS);
	gsi_writel(0xffffffbf, GSI_IC_GEN_EVNT_BCK_PRS_LSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_GEN_EVNT_BCK_PRS_MSB_OFFS);
	gsi_writel(0xffffefff, GSI_IC_GEN_INT_BCK_PRS_LSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_GEN_INT_BCK_PRS_MSB_OFFS);
	gsi_writel(0xffffefff, GSI_IC_STOP_INT_MOD_BCK_PRS_LSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_STOP_INT_MOD_BCK_PRS_MSB_OFFS);
	gsi_writel(0x00000000, GSI_IC_PROCESS_DESC_BCK_PRS_LSB_OFFS);
	gsi_writel(0x00000000, GSI_IC_PROCESS_DESC_BCK_PRS_MSB_OFFS);
	gsi_writel(0x00ffffff, GSI_IC_TLV_STOP_BCK_PRS_LSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_TLV_STOP_BCK_PRS_MSB_OFFS);
	gsi_writel(0xfdffffff, GSI_IC_TLV_RESET_BCK_PRS_LSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_TLV_RESET_BCK_PRS_MSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_RGSTR_TIMER_BCK_PRS_LSB_OFFS);
	gsi_writel(0xfffffffe, GSI_IC_RGSTR_TIMER_BCK_PRS_MSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_READ_BCK_PRS_LSB_OFFS);
	gsi_writel(0xffffefff, GSI_IC_READ_BCK_PRS_MSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_WRITE_BCK_PRS_LSB_OFFS);
	gsi_writel(0xffffdfff, GSI_IC_WRITE_BCK_PRS_MSB_OFFS);
	gsi_writel(0xffffffff, GSI_IC_UCONTROLLER_GPR_BCK_PRS_LSB_OFFS);
	gsi_writel(0xff03ffff, GSI_IC_UCONTROLLER_GPR_BCK_PRS_MSB_OFFS);
}

void gsi_firmware_enable(void)
{
	u32 val;

	/* ------- TZ code ------- */
	gsi_tz_pipe_config_enable(false);	/* Shouldn't be needed */
	gsi_tz_pipe_setup();

	val = gsi_readl(GSI_EE_N_GSI_SW_VERSION_OFFS(IPA_EE_UC));
	ipa_debug("GSI SW version 0x%08x\n", val);

	gsi_writel(0, GSI_PERIPH_BASE_ADDR_MSB_OFFS);
	gsi_writel(ipa_ctx->ipa_wrapper_base, GSI_PERIPH_BASE_ADDR_LSB_OFFS);

	gsi_write_ieps();
	/* ------- */

	val = field_gen(1, MCS_CFG_ENABLE_BMSK);
	gsi_writel(val, GSI_MCS_CFG_OFFS);

	val = field_gen(1, GSI_ENABLE_BMSK);
	val |= field_gen(1, MCS_ENABLE_BMSK);		/* TZ code */
	val |= field_gen(1, DOUBLE_MCS_CLK_FREQ_BMSK);
	gsi_writel(val, GSI_CFG_OFFS);

	/* ------- TZ code ------- */
	val = gsi_readl(GSI_EE_N_GSI_MCS_CODE_VER_OFFS(IPA_EE_UC));
	ipa_debug("GSI MCS CODE version 0x%08x\n", val);

	gsi_config_backpressure();
	/* ------- */
}

/* Compute the value to write to the event ring context 0 register */
static u32
evt_ring_ctx_0_val(enum gsi_evt_chtype chtype, bool intr_irq, u32 re_size)
{
	u32 val;

	val = field_gen((u32)chtype, EV_CHTYPE_BMSK);
	val |= field_gen(intr_irq ? 1 : 0, EV_INTYPE_BMSK);
	val |= field_gen(re_size, EV_ELEMENT_SIZE_BMSK);

	return val;
}

/* Compute the value to write to the event ring context 8 register */
static u32 evt_ring_ctx_8_val(u32 int_modt, u32 int_modc)
{
	u32 val;

	val = field_gen(int_modt, MODT_BMSK);
	val |= field_gen(int_modc, MODC_BMSK);

	return val;
}

static void
gsi_program_evt_ring_ctx(struct ipa_mem_buffer *mem, u8 evt_id, u16 int_modt)
{
	u32 ee = gsi_ctx->ee;
	u32 int_modc = 1;	/* moderation always comes from channel*/
	u32 val;

	ipa_debug("intf GPI intr IRQ RE size %u\n", GSI_EVT_RING_ELEMENT_SIZE);

	val = evt_ring_ctx_0_val(GSI_EVT_CHTYPE_GPI_EV, true,
				 GSI_EVT_RING_ELEMENT_SIZE);
	gsi_writel(val, GSI_EE_N_EV_CH_K_CNTXT_0_OFFS(evt_id, ee));

	val = field_gen(mem->size, EV_R_LENGTH_BMSK);
	gsi_writel(val, GSI_EE_N_EV_CH_K_CNTXT_1_OFFS(evt_id, ee));

	/* The context 2 and 3 registers store the low-order and
	 * high-order 32 bits of the address of the event ring,
	 * respectively.
	 */
	val = mem->phys_base & GENMASK(31, 0);
	gsi_writel(val, GSI_EE_N_EV_CH_K_CNTXT_2_OFFS(evt_id, ee));

	val = mem->phys_base >> 32;
	gsi_writel(val, GSI_EE_N_EV_CH_K_CNTXT_3_OFFS(evt_id, ee));

	val = evt_ring_ctx_8_val(int_modt, int_modc);
	gsi_writel(val, GSI_EE_N_EV_CH_K_CNTXT_8_OFFS(evt_id, ee));

	/* No MSI write data, and MSI address high and low address is 0 */
	gsi_writel(0, GSI_EE_N_EV_CH_K_CNTXT_9_OFFS(evt_id, ee));
	gsi_writel(0, GSI_EE_N_EV_CH_K_CNTXT_10_OFFS(evt_id, ee));
	gsi_writel(0, GSI_EE_N_EV_CH_K_CNTXT_11_OFFS(evt_id, ee));

	/* We don't need to get event read pointer updates */
	gsi_writel(0, GSI_EE_N_EV_CH_K_CNTXT_12_OFFS(evt_id, ee));
	gsi_writel(0, GSI_EE_N_EV_CH_K_CNTXT_13_OFFS(evt_id, ee));
}

static void gsi_init_ring(struct gsi_ring_ctx *ctx, struct ipa_mem_buffer *mem)
{
	spin_lock_init(&ctx->slock);
	ctx->mem = *mem;
	ctx->wp = mem->phys_base;
	ctx->rp = mem->phys_base;
	ctx->wp_local = mem->phys_base;
	ctx->rp_local = mem->phys_base;
	ctx->elem_sz = GSI_EVT_RING_ELEMENT_SIZE;
	ctx->max_num_elem = mem->size / ctx->elem_sz - 1;
	ctx->end = mem->phys_base + (ctx->max_num_elem + 1) * ctx->elem_sz;
}

static void gsi_prime_evt_ring(struct gsi_evt_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->ring.slock, flags);
	memset(ctx->ring.mem.base, 0, ctx->ring.mem.size);
	ctx->ring.wp_local = ctx->ring.mem.phys_base +
		ctx->ring.max_num_elem * ctx->ring.elem_sz;
	gsi_ring_evt_doorbell(ctx);
	spin_unlock_irqrestore(&ctx->ring.slock, flags);
}

/* Issue a GSI command by writing a value to a register, then wait
 * for completion to be signaled.  Returns 0 if a timeout occurred,
 * non-zero (positive) othwerwise.  Note that the register offset
 * is first, value to write is second (reverse of writel() order).
 */
static u32 command(u32 reg, u32 val, struct completion *compl)
{
	// BUILD_BUG_ON(GSI_CMD_TIMEOUT > (unsigned long)U32_MAX);

	gsi_writel(val, reg);

	return (u32)wait_for_completion_timeout(compl, GSI_CMD_TIMEOUT);
}

/* Issue an event ring command and wait for it to complete */
static u32 evt_ring_command(unsigned long evt_id, enum gsi_evt_ch_cmd_opcode op)
{
	struct completion *compl = &gsi_ctx->evtr[evt_id].compl;
	u32 ee = gsi_ctx->ee;
	u32 val;

	val = field_gen((u32)evt_id, EV_CHID_BMSK);
	val |= field_gen((u32)op, EV_OPCODE_BMSK);

	val = command(GSI_EE_N_EV_CH_CMD_OFFS(ee), val, compl);
	if (!val)
		ipa_err("evt_id %lu timed out\n", evt_id);

	return val;
}

/* Issue a channel command and wait for it to complete */
static u32 channel_command(unsigned long chan_id, enum gsi_ch_cmd_opcode op)
{
	struct completion *compl = &gsi_ctx->chan[chan_id].compl;
	u32 ee = gsi_ctx->ee;
	u32 val;

	val = field_gen((u32)chan_id, CH_CHID_BMSK);
	val |= field_gen((u32)op, CH_OPCODE_BMSK);

	val = command(GSI_EE_N_GSI_CH_CMD_OFFS(ee), val, compl);
	if (!val)
		ipa_err("chan_id %lu timed out\n", chan_id);

	return val;
}

/* Note: only GPI interfaces, IRQ interrupts are currently supported */
long gsi_alloc_evt_ring(u32 size, u16 int_modt)
{
	unsigned long required_alignment = roundup_pow_of_two(size);
	u32 ee = gsi_ctx->ee;
	unsigned long evt_id;
	struct gsi_evt_ctx *ctx;
	unsigned long flags;
	u32 completed;
	u32 val;
	int ret;

	ipa_assert(!(size % GSI_EVT_RING_ELEMENT_SIZE));

	/* Start by allocating the event id to use */
	mutex_lock(&gsi_ctx->mlock);
	evt_id = find_first_zero_bit(&gsi_ctx->evt_bmap, GSI_EVT_RING_MAX);
	if (evt_id == GSI_EVT_RING_MAX) {
		ipa_err("failed to alloc event ID\n");
		mutex_unlock(&gsi_ctx->mlock);
		return -ENOMEM;
	}
	set_bit(evt_id, &gsi_ctx->evt_bmap);
	mutex_unlock(&gsi_ctx->mlock);	/* acquired again below */

	ipa_debug("Using %lu as virt evt id\n", evt_id);

	ctx = &gsi_ctx->evtr[evt_id];
	memset(ctx, 0, sizeof(*ctx));
	ctx->id = evt_id;

	if (ipahal_dma_alloc(&ctx->mem, size, GFP_KERNEL)) {
		ipa_err("fail to dma alloc %u bytes\n", size);
		ret = -ENOMEM;
		goto err_clear_bit;
	}

	/* Verify the result meets our alignment requirements */
	if (ctx->mem.phys_base % required_alignment) {
		ipa_err("ring base %pad not aligned to 0x%lx\n",
			&ctx->mem.phys_base, required_alignment);
		ret = -EINVAL;
		goto err_free_dma;
	}

	ctx->int_modt = int_modt;
	mutex_init(&ctx->mlock);
	init_completion(&ctx->compl);
	atomic_set(&ctx->chan_ref_cnt, 0);

	mutex_lock(&gsi_ctx->mlock);

	completed = evt_ring_command(evt_id, GSI_EVT_ALLOCATE);
	if (!completed) {
		ret = -ETIMEDOUT;
		goto err_unlock;
	}

	if (ctx->state != GSI_EVT_RING_STATE_ALLOCATED) {
		ipa_err("evt_id %lu allocation failed state %u\n",
			evt_id, ctx->state);
		ret = -ENOMEM;
		goto err_unlock;
	}

	gsi_program_evt_ring_ctx(&ctx->mem, evt_id, int_modt);
	gsi_init_ring(&ctx->ring, &ctx->mem);

	atomic_inc(&gsi_ctx->num_evt_ring);
	gsi_prime_evt_ring(ctx);
	mutex_unlock(&gsi_ctx->mlock);

	spin_lock_irqsave(&gsi_ctx->slock, flags);
	val = BIT(evt_id);
	gsi_writel(val, GSI_EE_N_CNTXT_SRC_IEOB_IRQ_CLR_OFFS(ee));

	/* enable ieob interrupts */
	gsi_irq_control_event(gsi_ctx->ee, ctx->id, true);
	spin_unlock_irqrestore(&gsi_ctx->slock, flags);

	return evt_id;

err_unlock:
	mutex_unlock(&gsi_ctx->mlock);
err_free_dma:
	ipahal_dma_free(&ctx->mem);
err_clear_bit:
	smp_mb__before_atomic();	/* XXX comment this */
	clear_bit(evt_id, &gsi_ctx->evt_bmap);
	smp_mb__after_atomic();	/* XXX comment this */

	return ret;
}

static void __gsi_zero_evt_ring_scratch(unsigned long evt_id)
{
	u32 ee = gsi_ctx->ee;

	gsi_writel(0, GSI_EE_N_EV_CH_K_SCRATCH_0_OFFS(evt_id, ee));
	gsi_writel(0, GSI_EE_N_EV_CH_K_SCRATCH_1_OFFS(evt_id, ee));
}

void gsi_dealloc_evt_ring(unsigned long evt_id)
{
	struct gsi_evt_ctx *ctx;
	u32 completed;

	ipa_bug_on(evt_id >= gsi_ctx->max_ev);

	ctx = &gsi_ctx->evtr[evt_id];

	ipa_bug_on(atomic_read(&ctx->chan_ref_cnt));

	/* TODO: add check for ERROR state */
	ipa_bug_on(ctx->state != GSI_EVT_RING_STATE_ALLOCATED);

	mutex_lock(&gsi_ctx->mlock);
	reinit_completion(&ctx->compl);

	completed = evt_ring_command(evt_id, GSI_EVT_DE_ALLOC);
	ipa_bug_on(!completed);

	ipa_bug_on(ctx->state != GSI_EVT_RING_STATE_NOT_ALLOCATED);

	clear_bit(evt_id, &gsi_ctx->evt_bmap);
	mutex_unlock(&gsi_ctx->mlock);

	ctx->int_modt = 0;
	ipahal_dma_free(&ctx->mem);

	atomic_dec(&gsi_ctx->num_evt_ring);
}

void gsi_reset_evt_ring(unsigned long evt_id)
{
	struct gsi_evt_ctx *ctx;
	u32 completed;

	ipa_bug_on(evt_id >= gsi_ctx->max_ev);

	ctx = &gsi_ctx->evtr[evt_id];

	ipa_bug_on(ctx->state != GSI_EVT_RING_STATE_ALLOCATED);

	mutex_lock(&gsi_ctx->mlock);
	reinit_completion(&ctx->compl);

	completed = evt_ring_command(evt_id, GSI_EVT_RESET);
	ipa_bug_on(!completed);

	ipa_bug_on(ctx->state != GSI_EVT_RING_STATE_ALLOCATED);

	gsi_program_evt_ring_ctx(&ctx->mem, evt_id, ctx->int_modt);
	gsi_init_ring(&ctx->ring, &ctx->mem);

	__gsi_zero_evt_ring_scratch(evt_id);

	gsi_prime_evt_ring(ctx);
	mutex_unlock(&gsi_ctx->mlock);
}

static void
gsi_program_chan_ctx(struct gsi_chan_props *props, u32 ee, u8 erindex)
{
	u32 val;

	val = field_gen(GSI_CHAN_PROT_GPI, CHTYPE_PROTOCOL_BMSK);
	val |= field_gen(props->from_gsi ? 0 : 1, CHTYPE_DIR_BMSK);
	val |= field_gen(erindex, ERINDEX_BMSK);
	val |= field_gen(GSI_CHAN_RING_ELEMENT_SIZE, ELEMENT_SIZE_BMSK);
	gsi_writel(val, GSI_EE_N_GSI_CH_K_CNTXT_0_OFFS(props->ch_id, ee));

	val = field_gen(props->mem.size, R_LENGTH_BMSK);
	gsi_writel(val, GSI_EE_N_GSI_CH_K_CNTXT_1_OFFS(props->ch_id, ee));

	/* The context 2 and 3 registers store the low-order and
	 * high-order 32 bits of the address of the channel ring,
	 * respectively.
	 */
	val = props->mem.phys_base & GENMASK(31, 0);
	gsi_writel(val, GSI_EE_N_GSI_CH_K_CNTXT_2_OFFS(props->ch_id, ee));

	val = props->mem.phys_base >> 32;
	gsi_writel(val, GSI_EE_N_GSI_CH_K_CNTXT_3_OFFS(props->ch_id, ee));

	val = field_gen(props->low_weight, WRR_WEIGHT_BMSK);
	val |= field_gen(GSI_MAX_PREFETCH, MAX_PREFETCH_BMSK);
	val |= field_gen(props->use_db_engine ? 1 : 0, USE_DB_ENG_BMSK);
	gsi_writel(val, GSI_EE_N_GSI_CH_K_QOS_OFFS(props->ch_id, ee));
}

static int gsi_validate_channel_props(struct gsi_chan_props *props)
{
	dma_addr_t phys_base;
	dma_addr_t last;

	if (props->mem.size % 16) {
		ipa_err("bad params mem.size %u not a multiple of re size %u\n",
			props->mem.size, GSI_CHAN_RING_ELEMENT_SIZE);
		return -EINVAL;
	}

	phys_base = props->mem.phys_base;
	if (phys_base % roundup_pow_of_two(props->mem.size)) {
		ipa_err("bad params ring base not aligned 0x%llx align 0x%lx\n",
			phys_base, roundup_pow_of_two(props->mem.size));
		return -EINVAL;
	}

	last = phys_base + props->mem.size - GSI_CHAN_RING_ELEMENT_SIZE;

	/* MSB should stay same within the ring */
	if ((phys_base & GENMASK_ULL(63, 32)) != (last & GENMASK_ULL(63, 32))) {
		ipa_err("MSB is not fixed on ring base 0x%llx size 0x%x\n",
			phys_base, props->mem.size);
		return -EINVAL;
	}

	if (!props->mem.base) {
		ipa_err("GPI protocol requires ring base VA\n");
		return -EINVAL;
	}

	if (props->low_weight > GSI_MAX_CH_LOW_WEIGHT) {
		ipa_err("invalid channel low weight %u\n", props->low_weight);
		return -EINVAL;
	}

	return 0;
}

long gsi_alloc_channel(struct gsi_chan_props *props)
{
	struct gsi_chan_ctx *ctx;
	struct gsi_evt_ctx *evtr;
	size_t size;
	u8 evt_id;
	void **user_data;
	long chan_id;
	u32 completed;

	if (ipahal_dma_alloc(&props->mem, props->ring_size, GFP_KERNEL)) {
		ipa_err("fail to dma alloc %u bytes\n", props->ring_size);
		return -ENOMEM;
	}

	if (props->ch_id >= gsi_ctx->max_ch) {
		ipa_err("chan_id %hhu too large (must be < %u)\n",
			props->ch_id, gsi_ctx->max_ch);
		return -EINVAL;
	}

	if (gsi_validate_channel_props(props)) {
		ipa_err("bad params\n");
		ipahal_dma_free(&props->mem);
		return -EINVAL;
	}

	evt_id = props->evt_ring_hdl;
	evtr = &gsi_ctx->evtr[evt_id];
	if (atomic_read(&evtr->chan_ref_cnt)) {
		ipa_err("evt ring %hhu in use\n", evt_id);
		ipahal_dma_free(&props->mem);
		return -ENOTSUPP;
	}

	chan_id = (long)props->ch_id;
	ctx = &gsi_ctx->chan[chan_id];
	if (ctx->allocated) {
		ipa_err("chan %ld already allocated\n", chan_id);
		ipahal_dma_free(&props->mem);
		return -ENODEV;
	}

	memset(ctx, 0, sizeof(*ctx));
	size = props->mem.size / GSI_CHAN_RING_ELEMENT_SIZE * sizeof(void *);
	user_data = kzalloc(size, GFP_KERNEL);
	if (!user_data) {
		ipa_err("error allocating user pointer array\n");
		ipahal_dma_free(&props->mem);
		return -ENOMEM;
	}

	mutex_init(&ctx->mlock);
	init_completion(&ctx->compl);
	atomic_set(&ctx->poll_mode, 0);	/* Initially in callback mode */
	ctx->props = *props;

	mutex_lock(&gsi_ctx->mlock);

	completed = channel_command(chan_id, GSI_CH_ALLOCATE);
	if (!completed) {
		chan_id = -ETIMEDOUT;
		goto err_mutex_unlock;
	}
	if (ctx->state != GSI_CHAN_STATE_ALLOCATED) {
		ipa_err("chan_id %ld allocation failed state %d\n",
			chan_id, ctx->state);
		chan_id = -ENOMEM;
		goto err_mutex_unlock;
	}

	gsi_ctx->ch_dbg[chan_id].ch_allocate++;

	mutex_unlock(&gsi_ctx->mlock);

	ctx->evtr = evtr;
	atomic_inc(&evtr->chan_ref_cnt);
	evtr->chan = ctx;

	gsi_program_chan_ctx(props, gsi_ctx->ee, evt_id);
	gsi_init_ring(&ctx->ring, &props->mem);

	ctx->user_data = user_data;
	ctx->allocated = true;
	atomic_inc(&gsi_ctx->num_chan);

	return chan_id;

err_mutex_unlock:
	mutex_unlock(&gsi_ctx->mlock);
	kfree(user_data);
	ipahal_dma_free(&ctx->props.mem);

	return chan_id;
}

static void __gsi_write_channel_scratch(unsigned long chan_id)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];
	union gsi_channel_scratch scr = { };
	struct gsi_gpi_channel_scratch *gpi = &scr.gpi;
	u32 ee = gsi_ctx->ee;
	u32 val;

	/* See comments above definition of gsi_gpi_channel_scratch */
	gpi->max_outstanding_tre = ctx->tlv_size * GSI_CHAN_RING_ELEMENT_SIZE;
	gpi->outstanding_threshold = 2 * GSI_CHAN_RING_ELEMENT_SIZE;

	val = scr.data.word1;
	gsi_writel(val, GSI_EE_N_GSI_CH_K_SCRATCH_0_OFFS(chan_id, ee));

	val = scr.data.word2;
	gsi_writel(val, GSI_EE_N_GSI_CH_K_SCRATCH_1_OFFS(chan_id, ee));

	val = scr.data.word3;
	gsi_writel(val, GSI_EE_N_GSI_CH_K_SCRATCH_2_OFFS(chan_id, ee));

	/* We must preserve the upper 16 bits of the last scratch
	 * register.  The next sequence assumes those bits remain
	 * unchanged between the read and the write.
	 */
	val = gsi_readl(GSI_EE_N_GSI_CH_K_SCRATCH_3_OFFS(chan_id, ee));
	val = (scr.data.word4 & GENMASK(31, 16)) | (val & GENMASK(15, 0));
	gsi_writel(val, GSI_EE_N_GSI_CH_K_SCRATCH_3_OFFS(chan_id, ee));
}

int gsi_write_channel_scratch(unsigned long chan_id, u32 tlv_size)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];

	ctx->tlv_size = tlv_size;

	mutex_lock(&ctx->mlock);

	__gsi_write_channel_scratch(chan_id);

	mutex_unlock(&ctx->mlock);

	return 0;
}

int gsi_start_channel(unsigned long chan_id)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];
	u32 completed;

	if (ctx->state != GSI_CHAN_STATE_ALLOCATED &&
	    ctx->state != GSI_CHAN_STATE_STOP_IN_PROC &&
	    ctx->state != GSI_CHAN_STATE_STOPPED) {
		ipa_err("bad state %d\n", ctx->state);
		return -ENOTSUPP;
	}

	mutex_lock(&gsi_ctx->mlock);
	reinit_completion(&ctx->compl);

	gsi_ctx->ch_dbg[chan_id].ch_start++;

	completed = channel_command(chan_id, GSI_CH_START);
	if (!completed) {
		mutex_unlock(&gsi_ctx->mlock);
		return -ETIMEDOUT;
	}
	if (ctx->state != GSI_CHAN_STATE_STARTED) {
		ipa_err("chan %lu unexpected state %u\n", chan_id, ctx->state);
		ipa_bug();
	}

	mutex_unlock(&gsi_ctx->mlock);

	return 0;
}

int gsi_stop_channel(unsigned long chan_id)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];
	u32 completed;
	u32 val;
	int ret;

	if (ctx->state == GSI_CHAN_STATE_STOPPED) {
		ipa_debug("chan_id %lu already stopped\n", chan_id);
		return 0;
	}

	if (ctx->state != GSI_CHAN_STATE_STARTED &&
	    ctx->state != GSI_CHAN_STATE_STOP_IN_PROC &&
	    ctx->state != GSI_CHAN_STATE_ERROR) {
		ipa_err("bad state %d\n", ctx->state);
		return -ENOTSUPP;
	}

	mutex_lock(&gsi_ctx->mlock);
	reinit_completion(&ctx->compl);

	gsi_ctx->ch_dbg[chan_id].ch_stop++;

	completed = channel_command(chan_id, GSI_CH_STOP);
	if (!completed) {
		u32 ee = gsi_ctx->ee;

		/* check channel state here in case the channel is stopped but
		 * the interrupt was not handled yet.
		 */
		val = gsi_readl(GSI_EE_N_GSI_CH_K_CNTXT_0_OFFS(chan_id, ee));
		ctx->state = field_val(val, CHSTATE_BMSK);
		if (ctx->state == GSI_CHAN_STATE_STOPPED) {
			ret = 0;
			goto free_lock;
		}
		ret = -ETIMEDOUT;
		goto free_lock;
	}

	if (ctx->state != GSI_CHAN_STATE_STOPPED &&
	    ctx->state != GSI_CHAN_STATE_STOP_IN_PROC) {
		ipa_err("chan %lu unexpected state %u\n", chan_id, ctx->state);
		ret = -EBUSY;
		goto free_lock;
	}

	if (ctx->state == GSI_CHAN_STATE_STOP_IN_PROC) {
		ipa_err("chan %lu busy try again\n", chan_id);
		ret = -EAGAIN;
		goto free_lock;
	}

	ret = 0;

free_lock:
	mutex_unlock(&gsi_ctx->mlock);

	return ret;
}

int gsi_reset_channel(unsigned long chan_id)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];
	bool reset_done = false;
	u32 completed;

	if (ctx->state != GSI_CHAN_STATE_STOPPED) {
		ipa_err("bad state %d\n", ctx->state);
		return -ENOTSUPP;
	}

	mutex_lock(&gsi_ctx->mlock);
reset:
	reinit_completion(&ctx->compl);

	gsi_ctx->ch_dbg[chan_id].ch_reset++;

	completed = channel_command(chan_id, GSI_CH_RESET);
	if (!completed) {
		ipa_err("chan_id %lu timed out\n", chan_id);
		mutex_unlock(&gsi_ctx->mlock);
		return -ETIMEDOUT;
	}

	if (ctx->state != GSI_CHAN_STATE_ALLOCATED) {
		ipa_err("chan_id %lu unexpected state %u\n", chan_id,
			ctx->state);
		ipa_bug();
	}

	/* workaround: reset GSI producers again */
	if (ctx->props.from_gsi && !reset_done) {
		usleep_range(GSI_RESET_WA_MIN_SLEEP, GSI_RESET_WA_MAX_SLEEP);
		reset_done = true;
		goto reset;
	}

	gsi_program_chan_ctx(&ctx->props, gsi_ctx->ee, ctx->evtr->id);
	gsi_init_ring(&ctx->ring, &ctx->props.mem);

	/* restore scratch */
	__gsi_write_channel_scratch(chan_id);

	mutex_unlock(&gsi_ctx->mlock);

	return 0;
}

void gsi_dealloc_channel(unsigned long chan_id)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];
	u32 completed;

	ipa_bug_on(ctx->state != GSI_CHAN_STATE_ALLOCATED);

	mutex_lock(&gsi_ctx->mlock);
	reinit_completion(&ctx->compl);

	gsi_ctx->ch_dbg[chan_id].ch_de_alloc++;

	completed = channel_command(chan_id, GSI_CH_DE_ALLOC);
	ipa_bug_on(!completed);

	ipa_bug_on(ctx->state != GSI_CHAN_STATE_NOT_ALLOCATED);

	mutex_unlock(&gsi_ctx->mlock);

	kfree(ctx->user_data);
	ipahal_dma_free(&ctx->props.mem);
	ctx->allocated = false;
	atomic_dec(&ctx->evtr->chan_ref_cnt);
	atomic_dec(&gsi_ctx->num_chan);
}

static u16 __gsi_query_channel_free_re(struct gsi_chan_ctx *ctx)
{
	u16 start;
	u16 end;
	u16 used;

	start = ring_rp_local_index(&ctx->ring);
	end = ring_wp_local_index(&ctx->ring);

	if (end >= start)
		used = end - start;
	else
		used = ctx->ring.max_num_elem + 1 - (start - end);

	return ctx->ring.max_num_elem - used;
}

bool gsi_is_channel_empty(unsigned long chan_id)
{
	struct gsi_chan_ctx *ctx;
	unsigned long flags;
	u32 ee = gsi_ctx->ee;
	bool empty;
	u32 val;

	ctx = &gsi_ctx->chan[chan_id];

	spin_lock_irqsave(&ctx->evtr->ring.slock, flags);

	val = gsi_readl(GSI_EE_N_GSI_CH_K_CNTXT_4_OFFS(ctx->props.ch_id, ee));
	ctx->ring.rp = (ctx->ring.rp & GENMASK_ULL(63, 32)) | val;

	val = gsi_readl(GSI_EE_N_GSI_CH_K_CNTXT_6_OFFS(ctx->props.ch_id, ee));
	ctx->ring.wp = (ctx->ring.wp & GENMASK_ULL(63, 32)) | val;

	if (ctx->props.from_gsi)
		empty = ctx->ring.rp_local == ctx->ring.rp;
	else
		empty = ctx->ring.wp == ctx->ring.rp;

	spin_unlock_irqrestore(&ctx->evtr->ring.slock, flags);

	ipa_debug("chan_id %lu RP 0x%llx WP 0x%llx RP_LOCAL 0x%llx\n", chan_id,
		  ctx->ring.rp, ctx->ring.wp, ctx->ring.rp_local);

	return empty;
}

int gsi_queue_xfer(unsigned long chan_id, u16 num_xfers,
		   struct gsi_xfer_elem *xfer, bool ring_db)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];
	u16 free;
	struct gsi_tre tre;
	struct gsi_tre *tre_ptr;
	u16 idx;
	u64 wp_rollback;
	u32 i;
	unsigned long flags;
	int ret;

	if (!num_xfers || !xfer) {
		ipa_err("bad params chan_id %lu num_xfers %u xfer %p\n",
			chan_id, num_xfers, xfer);
		return -EINVAL;
	}

	spin_lock_irqsave(&ctx->evtr->ring.slock, flags);

	free = __gsi_query_channel_free_re(ctx);

	if (num_xfers > free) {
		ipa_err("chan_id %lu num_xfers %u free %u\n", chan_id,
			num_xfers, free);
		ret = -ENOSPC;
		goto out_unlock;
	}

	wp_rollback = ctx->ring.wp_local;
	for (i = 0; i < num_xfers; i++) {
		memset(&tre, 0, sizeof(tre));
		tre.buffer_ptr = xfer[i].addr;
		tre.buf_len = xfer[i].len;
		if (xfer[i].type == GSI_XFER_ELEM_DATA) {
			tre.re_type = GSI_RE_XFER;
		} else if (xfer[i].type == GSI_XFER_ELEM_IMME_CMD) {
			tre.re_type = GSI_RE_IMMD_CMD;
		} else if (xfer[i].type == GSI_XFER_ELEM_NOP) {
			tre.re_type = GSI_RE_NOP;
		} else {
			ipa_err("chan_id %lu bad RE type %u\n", chan_id,
				xfer[i].type);
			break;
		}
		tre.bei = (xfer[i].flags & GSI_XFER_FLAG_BEI) ? 1 : 0;
		tre.ieot = (xfer[i].flags & GSI_XFER_FLAG_EOT) ? 1 : 0;
		tre.ieob = (xfer[i].flags & GSI_XFER_FLAG_EOB) ? 1 : 0;
		tre.chain = (xfer[i].flags & GSI_XFER_FLAG_CHAIN) ? 1 : 0;

		idx = ring_wp_local_index(&ctx->ring);
		tre_ptr = ctx->ring.mem.base + idx * ctx->ring.elem_sz;

		/* write the TRE to ring */
		*tre_ptr = tre;
		ctx->user_data[idx] = xfer[i].xfer_user_data;
		ring_wp_local_inc(&ctx->ring);
	}

	if (i != num_xfers) {
		/* reject all the xfers */
		ctx->ring.wp_local = wp_rollback;
		ret = -EINVAL;
		goto out_unlock;
	}

	/* ensure TRE is set before ringing doorbell */
	wmb();

	if (ring_db)
		gsi_ring_chan_doorbell(ctx);

	ret = 0;
out_unlock:
	spin_unlock_irqrestore(&ctx->evtr->ring.slock, flags);

	return ret;
}

int gsi_start_xfer(unsigned long chan_id)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];

	if (ctx->state != GSI_CHAN_STATE_STARTED) {
		ipa_err("bad state %d\n", ctx->state);
		return -ENOTSUPP;
	}

	if (ctx->ring.wp == ctx->ring.wp_local)
		return 0;

	gsi_ring_chan_doorbell(ctx);

	return 0;
}

int gsi_poll_channel(unsigned long chan_id)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];
	struct gsi_evt_ctx *evtr = ctx->evtr;
	u32 ee = gsi_ctx->ee;
	unsigned long flags;
	bool empty;
	int size = 0;

	spin_lock_irqsave(&evtr->ring.slock, flags);

	/* update rp to see of we have anything new to process */
	if (evtr->ring.rp == evtr->ring.rp_local) {
		u32 val;

		val = gsi_readl(GSI_EE_N_EV_CH_K_CNTXT_4_OFFS(evtr->id, ee));
		evtr->ring.rp = (ctx->ring.rp & GENMASK_ULL(63, 32)) | val;
	}

	empty = evtr->ring.rp == evtr->ring.rp_local;
	if (!empty)
		size = gsi_process_evt_re(evtr, false);

	spin_unlock_irqrestore(&evtr->ring.slock, flags);

	return empty ? -ENOENT : size;
}

static void gsi_config_channel_mode(unsigned long chan_id, bool polling)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];
	unsigned long flags;

	spin_lock_irqsave(&gsi_ctx->slock, flags);
	if (polling)
		gsi_irq_control_event(gsi_ctx->ee, ctx->evtr->id, false);
	else
		gsi_irq_control_event(gsi_ctx->ee, ctx->evtr->id, true);
	atomic_set(&ctx->poll_mode, polling ? 1 : 0);
	spin_unlock_irqrestore(&gsi_ctx->slock, flags);
}

void gsi_channel_intr_enable(unsigned long chan_id)
{
	gsi_config_channel_mode(chan_id, false);
}

void gsi_channel_intr_disable(unsigned long chan_id)
{
	gsi_config_channel_mode(chan_id, true);
}

int gsi_get_channel_cfg(unsigned long chan_id, struct gsi_chan_props *props)
{
	struct gsi_chan_ctx *ctx = &gsi_ctx->chan[chan_id];

	if (ctx->state == GSI_CHAN_STATE_NOT_ALLOCATED) {
		ipa_err("bad state %d\n", ctx->state);
		return -ENOTSUPP;
	}

	mutex_lock(&ctx->mlock);
	*props = ctx->props;
	mutex_unlock(&ctx->mlock);

	return 0;
}

int gsi_set_channel_cfg(unsigned long chan_id, struct gsi_chan_props *props)
{
	struct gsi_chan_ctx *ctx;

	if (gsi_validate_channel_props(props)) {
		ipa_err("bad params props %p\n", props);
		return -EINVAL;
	}

	ctx = &gsi_ctx->chan[chan_id];
	if (ctx->state != GSI_CHAN_STATE_ALLOCATED) {
		ipa_err("bad state %d\n", ctx->state);
		return -ENOTSUPP;
	}

	if (ctx->props.ch_id != props->ch_id ||
	    ctx->props.evt_ring_hdl != props->evt_ring_hdl) {
		ipa_err("changing immutable fields not supported\n");
		return -ENOTSUPP;
	}

	mutex_lock(&ctx->mlock);
	ctx->props = *props;

	gsi_program_chan_ctx(&ctx->props, gsi_ctx->ee, ctx->evtr->id);
	gsi_init_ring(&ctx->ring, &ctx->props.mem);

	/* restore scratch */
	__gsi_write_channel_scratch(chan_id);
	mutex_unlock(&ctx->mlock);

	return 0;
}

/* Initialize GSI driver */
struct gsi_ctx *gsi_init(struct platform_device *pdev, u32 ee)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	resource_size_t size;

	gsi_ctx = devm_kzalloc(dev, sizeof(*gsi_ctx), GFP_KERNEL);
	if (!gsi_ctx)
		return ERR_PTR(-ENOMEM);

	/* Get GSI memory range and map it */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gsi-base");
	if (!res) {
		ipa_err("missing \"gsi-base\" property in DTB\n");
		return ERR_PTR(-EINVAL);
	}

	size = resource_size(res);
	gsi_ctx->base = devm_ioremap_nocache(dev, res->start, size);
	if (!gsi_ctx->base) {
		ipa_err("failed to remap GSI memory\n");
		return ERR_PTR(-ENOMEM);
	}

	gsi_ctx->dev = dev;
	gsi_ctx->ee = ee;
	ipa_assert(res->start <= (resource_size_t)U32_MAX);
	gsi_ctx->phys_base = (u32)res->start;

	return gsi_ctx;
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Generic Software Interface (GSI)");
