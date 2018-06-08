// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#define pr_fmt(fmt)    "ipa %s:%d " fmt, __func__, __LINE__

#include <linux/interrupt.h>
#include "ipa_i.h"

#define INTERRUPT_WORKQUEUE_NAME "ipa_interrupt_wq"
#define DIS_SUSPEND_INTERRUPT_TIMEOUT 5
#define IPA_IRQ_NUM_MAX 32

struct ipa_interrupt_info {
	ipa_irq_handler_t handler;
	enum ipa_irq_type interrupt;
	void *private_data;
	bool deferred_flag;
};

struct ipa_interrupt_work_wrap {
	struct work_struct interrupt_work;
	ipa_irq_handler_t handler;
	enum ipa_irq_type interrupt;
	void *private_data;
	void *interrupt_data;
};

static struct ipa_interrupt_info ipa_interrupt_to_cb[IPA_IRQ_NUM_MAX];
static struct workqueue_struct *ipa_interrupt_wq;
static u32 ipa_ee;

static void ipa_tx_suspend_interrupt_wa(void);
static void ipa_enable_tx_suspend_wa(struct work_struct *work);
static DECLARE_DELAYED_WORK(dwork_en_suspend_int, ipa_enable_tx_suspend_wa);
static spinlock_t suspend_wa_lock;
static void ipa_process_interrupts(bool isr_context);

static int ipa_irq_mapping[IPA_IRQ_MAX] = {
	[IPA_UC_TX_CMD_Q_NOT_FULL_IRQ]		= -1,
	[IPA_UC_TO_PROC_ACK_Q_NOT_FULL_IRQ]	= -1,
	[IPA_BAD_SNOC_ACCESS_IRQ]		= 0,
	[IPA_EOT_COAL_IRQ]			= -1,
	[IPA_UC_IRQ_0]				= 2,
	[IPA_UC_IRQ_1]				= 3,
	[IPA_UC_IRQ_2]				= 4,
	[IPA_UC_IRQ_3]				= 5,
	[IPA_UC_IN_Q_NOT_EMPTY_IRQ]		= 6,
	[IPA_UC_RX_CMD_Q_NOT_FULL_IRQ]		= 7,
	[IPA_PROC_TO_UC_ACK_Q_NOT_EMPTY_IRQ]	= 8,
	[IPA_RX_ERR_IRQ]			= 9,
	[IPA_DEAGGR_ERR_IRQ]			= 10,
	[IPA_TX_ERR_IRQ]			= 11,
	[IPA_STEP_MODE_IRQ]			= 12,
	[IPA_PROC_ERR_IRQ]			= 13,
	[IPA_TX_SUSPEND_IRQ]			= 14,
	[IPA_TX_HOLB_DROP_IRQ]			= 15,
	[IPA_GSI_IDLE_IRQ]			= 16,
};

static void ipa_interrupt_defer(struct work_struct *work);
static DECLARE_WORK(ipa_interrupt_defer_work, ipa_interrupt_defer);

static void ipa_deferred_interrupt_work(struct work_struct *work)
{
	struct ipa_interrupt_work_wrap *work_data =
			container_of(work, struct ipa_interrupt_work_wrap,
				     interrupt_work);
	ipa_debug("call handler from workq...\n");
	work_data->handler(work_data->interrupt, work_data->private_data,
			   work_data->interrupt_data);
	kfree(work_data->interrupt_data);
	kfree(work_data);
}

static bool ipa_is_valid_ep(u32 ep_suspend_data)
{
	u32 bmsk = 1;
	u32 i = 0;

	for (i = 0; i < ipa_ctx->ipa_num_pipes; i++) {
		if ((ep_suspend_data & bmsk) && ipa_ctx->ep[i].valid)
			return true;
		bmsk = bmsk << 1;
	}
	return false;
}

static int ipa_handle_interrupt(int irq_num, bool isr_context)
{
	struct ipa_interrupt_info interrupt_info;
	struct ipa_interrupt_work_wrap *work_data;
	u32 suspend_data;
	void *interrupt_data = NULL;
	struct ipa_tx_suspend_irq_data *suspend_interrupt_data = NULL;
	int res;

	interrupt_info = ipa_interrupt_to_cb[irq_num];
	if (!interrupt_info.handler) {
		ipa_err("A callback function wasn't set for interrupt num %d\n",
			irq_num);
		return -EINVAL;
	}

	switch (interrupt_info.interrupt) {
	case IPA_TX_SUSPEND_IRQ:
		ipa_debug_low("processing TX_SUSPEND interrupt work-around\n");
		ipa_tx_suspend_interrupt_wa();
		suspend_data = ipahal_read_reg_n(IPA_IRQ_SUSPEND_INFO_EE_n,
						 ipa_ee);
		ipa_debug_low("get interrupt %d\n", suspend_data);

		/* Clear L2 interrupts status.  Note the following
		 * must not be executed for IPA hardware versions
		 * prior to 3.1.
		 */
		ipahal_write_reg_n(IPA_SUSPEND_IRQ_CLR_EE_n,
				   ipa_ee, suspend_data);
		if (!ipa_is_valid_ep(suspend_data))
			return 0;

		suspend_interrupt_data =
			kzalloc(sizeof(*suspend_interrupt_data), GFP_ATOMIC);
		if (!suspend_interrupt_data) {
			ipa_err("failed allocating suspend_interrupt_data\n");
			return -ENOMEM;
		}
		suspend_interrupt_data->endpoints = suspend_data;
		interrupt_data = suspend_interrupt_data;
		break;
	case IPA_UC_IRQ_0:
		break;
	default:
		break;
	}

	/* Force defer processing if in ISR context. */
	if (interrupt_info.deferred_flag || isr_context) {
		work_data = kzalloc(sizeof(*work_data), GFP_ATOMIC);
		if (!work_data) {
			res = -ENOMEM;
			goto fail_alloc_work;
		}
		INIT_WORK(&work_data->interrupt_work,
			  ipa_deferred_interrupt_work);
		work_data->handler = interrupt_info.handler;
		work_data->interrupt = interrupt_info.interrupt;
		work_data->private_data = interrupt_info.private_data;
		work_data->interrupt_data = interrupt_data;
		queue_work(ipa_interrupt_wq, &work_data->interrupt_work);

	} else {
		interrupt_info.handler(interrupt_info.interrupt,
				       interrupt_info.private_data,
				       interrupt_data);
		kfree(interrupt_data);
	}

	return 0;

fail_alloc_work:
	kfree(interrupt_data);
	return res;
}

static void ipa_enable_tx_suspend_wa(struct work_struct *work)
{
	u32 en;
	u32 suspend_bmask;
	int irq_num;

	ipa_debug_low("Enter\n");

	irq_num = ipa_irq_mapping[IPA_TX_SUSPEND_IRQ];
	ipa_assert(irq_num != -1);

	/* make sure ipa hw is clocked on*/
	ipa_client_add(__func__, false);

	en = ipahal_read_reg_n(IPA_IRQ_EN_EE_n, ipa_ee);
	suspend_bmask = 1 << irq_num;
	/*enable  TX_SUSPEND_IRQ*/
	en |= suspend_bmask;
	ipa_debug("enable TX_SUSPEND_IRQ, IPA_IRQ_EN_EE reg, write val = %u\n"
		, en);
	ipahal_write_reg_n(IPA_IRQ_EN_EE_n, ipa_ee, en);
	ipa_process_interrupts(false);
	ipa_client_remove(__func__, false);

	ipa_debug_low("Exit\n");
}

static void ipa_tx_suspend_interrupt_wa(void)
{
	u32 val;
	u32 suspend_bmask;
	int irq_num;

	ipa_debug_low("Enter\n");
	irq_num = ipa_irq_mapping[IPA_TX_SUSPEND_IRQ];
	ipa_assert(irq_num != -1);

	/*disable TX_SUSPEND_IRQ*/
	val = ipahal_read_reg_n(IPA_IRQ_EN_EE_n, ipa_ee);
	suspend_bmask = 1 << irq_num;
	val &= ~suspend_bmask;
	ipa_debug("Disable TX_SUSPEND_IRQ write %u to IPA_IRQ_EN_EE\n", val);
	ipahal_write_reg_n(IPA_IRQ_EN_EE_n, ipa_ee, val);

	ipa_debug_low("processing suspend interrupt WA delayed work\n");
	queue_delayed_work(ipa_interrupt_wq, &dwork_en_suspend_int,
			   msecs_to_jiffies(DIS_SUSPEND_INTERRUPT_TIMEOUT));

	ipa_debug_low("Exit\n");
}

static inline bool is_uc_irq(int irq_num)
{
	if (ipa_interrupt_to_cb[irq_num].interrupt >= IPA_UC_IRQ_0 &&
	    ipa_interrupt_to_cb[irq_num].interrupt <= IPA_UC_IRQ_3)
		return true;
	else
		return false;
}

static void ipa_process_interrupts(bool isr_context)
{
	u32 reg;
	u32 bmsk;
	u32 i = 0;
	u32 en;
	unsigned long flags;
	bool uc_irq;

	ipa_debug_low("Enter\n");

	spin_lock_irqsave(&suspend_wa_lock, flags);
	en = ipahal_read_reg_n(IPA_IRQ_EN_EE_n, ipa_ee);
	reg = ipahal_read_reg_n(IPA_IRQ_STTS_EE_n, ipa_ee);
	while (en & reg) {
		bmsk = 1;
		for (i = 0; i < IPA_IRQ_NUM_MAX; i++) {
			if (en & reg & bmsk) {
				uc_irq = is_uc_irq(i);

				/* Clear uC interrupt before processing to avoid
				 * clearing unhandled interrupts
				 */
				if (uc_irq)
					ipahal_write_reg_n(IPA_IRQ_CLR_EE_n,
							   ipa_ee, bmsk);

				/* handle the interrupt with spin_lock
				 * unlocked to avoid calling client in atomic
				 * context. mutual exclusion still preserved
				 * as the read/clr is done with spin_lock
				 * locked.
				 */
				spin_unlock_irqrestore(&suspend_wa_lock, flags);
				ipa_handle_interrupt(i, isr_context);
				spin_lock_irqsave(&suspend_wa_lock, flags);

				/* Clear non uC interrupt after processing
				 * to avoid clearing interrupt data
				 */
				if (!uc_irq)
					ipahal_write_reg_n(IPA_IRQ_CLR_EE_n,
							   ipa_ee, bmsk);
			}
			bmsk = bmsk << 1;
		}
		reg = ipahal_read_reg_n(IPA_IRQ_STTS_EE_n, ipa_ee);
		/* since the suspend interrupt HW bug we must
		 * read again the EN register, otherwise the while is endless
		 */
		en = ipahal_read_reg_n(IPA_IRQ_EN_EE_n, ipa_ee);
	}

	spin_unlock_irqrestore(&suspend_wa_lock, flags);
	ipa_debug_low("Exit\n");
}

static void ipa_interrupt_defer(struct work_struct *work)
{
	ipa_debug("processing interrupts in wq\n");
	ipa_client_add(__func__, false);
	ipa_process_interrupts(false);
	ipa_client_remove(__func__, false);
	ipa_debug("Done\n");
}

static irqreturn_t ipa_isr(int irq, void *ctxt)
{
	ipa_debug_low("Enter\n");
	/* defer interrupt handling in case IPA is not clocked on */
	if (!ipa_client_add_additional(__func__, false)) {
		ipa_debug("defer interrupt processing\n");
		queue_work(ipa_ctx->power_mgmt_wq, &ipa_interrupt_defer_work);
		return IRQ_HANDLED;
	}

	ipa_process_interrupts(true);
	ipa_debug_low("Exit\n");

	ipa_client_remove(__func__, false);

	return IRQ_HANDLED;
}

/** ipa_add_interrupt_handler() - Adds handler to an interrupt type
 * @interrupt:		Interrupt type
 * @handler:		The handler to be added
 * @deferred_flag:	whether the handler processing should be deferred in
 *			a workqueue
 * @private_data:	the client's private data
 *
 * Adds handler to an interrupt type and enable the specific bit
 * in IRQ_EN register, associated interrupt in IRQ_STTS register will be enabled
 */
int ipa_add_interrupt_handler(enum ipa_irq_type interrupt,
			      ipa_irq_handler_t handler, bool deferred_flag,
			      void *private_data)
{
	u32 val;
	u32 bmsk;
	int irq_num;
	int client_idx, ep_idx;

	ipa_debug("%s: interrupt_enum %d\n", __func__, interrupt);
	if (interrupt < IPA_BAD_SNOC_ACCESS_IRQ || interrupt >= IPA_IRQ_MAX) {
		ipa_err("invalid interrupt number %d\n", interrupt);
		return -EINVAL;
	}

	irq_num = ipa_irq_mapping[interrupt];
	if (irq_num < 0 || irq_num >= IPA_IRQ_NUM_MAX) {
		ipa_err("interrupt %d not supported\n", interrupt);
		WARN_ON(1);
		return -EFAULT;
	}
	ipa_debug("ipa_interrupt_to_cb irq_num(%d)\n", irq_num);

	ipa_interrupt_to_cb[irq_num].deferred_flag = deferred_flag;
	ipa_interrupt_to_cb[irq_num].handler = handler;
	ipa_interrupt_to_cb[irq_num].private_data = private_data;
	ipa_interrupt_to_cb[irq_num].interrupt = interrupt;

	val = ipahal_read_reg_n(IPA_IRQ_EN_EE_n, ipa_ee);
	ipa_debug("read IPA_IRQ_EN_EE_n register. reg = %d\n", val);
	bmsk = 1 << irq_num;
	val |= bmsk;
	ipahal_write_reg_n(IPA_IRQ_EN_EE_n, ipa_ee, val);
	ipa_debug("wrote IPA_IRQ_EN_EE_n register. reg = %d\n", val);

	/* Register SUSPEND_IRQ_EN_EE_N_ADDR for L2 interrupt.
	 * Note the following must not be executed for IPA hardware
	 * versions prior to 3.1.
	 */
	if (interrupt == IPA_TX_SUSPEND_IRQ) {
		val = ~0;
		for (client_idx = 0; client_idx < IPA_CLIENT_MAX; client_idx++)
			if (IPA_CLIENT_IS_Q6_CONS(client_idx) ||
			    IPA_CLIENT_IS_Q6_PROD(client_idx)) {
				ep_idx = ipa_get_ep_mapping(client_idx);
				ipa_debug("modem ep_idx(%d) client_idx = %d\n",
					  ep_idx, client_idx);
			if (ep_idx < 0)
				ipa_debug("Invalid IPA client\n");
			else
				val &= ~(1 << ep_idx);
		}

		ipahal_write_reg_n(IPA_SUSPEND_IRQ_EN_EE_n, ipa_ee, val);
		ipa_debug("wrote IPA_SUSPEND_IRQ_EN_EE_n reg = %d\n", val);
	}
	return 0;
}

/** ipa_remove_interrupt_handler() - Removes handler to an interrupt type
 * @interrupt:		Interrupt type
 *
 * Removes the handler and disable the specific bit in IRQ_EN register
 */
int ipa_remove_interrupt_handler(enum ipa_irq_type interrupt)
{
	u32 val;
	u32 bmsk;
	int irq_num;

	if (interrupt < IPA_BAD_SNOC_ACCESS_IRQ || interrupt >= IPA_IRQ_MAX) {
		ipa_err("invalid interrupt number %d\n", interrupt);
		return -EINVAL;
	}

	irq_num = ipa_irq_mapping[interrupt];
	if (irq_num < 0 || irq_num >= IPA_IRQ_NUM_MAX) {
		ipa_err("interrupt %d not supported\n", interrupt);
		WARN_ON(1);
		return -EFAULT;
	}

	kfree(ipa_interrupt_to_cb[irq_num].private_data);
	ipa_interrupt_to_cb[irq_num].deferred_flag = false;
	ipa_interrupt_to_cb[irq_num].handler = NULL;
	ipa_interrupt_to_cb[irq_num].private_data = NULL;
	ipa_interrupt_to_cb[irq_num].interrupt = -1;

	/* Unregister SUSPEND_IRQ_EN_EE_N_ADDR for L2 interrupt.
	 * Note the following must not be executed for IPA hardware
	 * versions prior to 3.1.
	 */
	if (interrupt == IPA_TX_SUSPEND_IRQ) {
		ipahal_write_reg_n(IPA_SUSPEND_IRQ_EN_EE_n, ipa_ee, 0);
		ipa_debug("wrote IPA_SUSPEND_IRQ_EN_EE_n reg = %d\n", 0);
	}

	val = ipahal_read_reg_n(IPA_IRQ_EN_EE_n, ipa_ee);
	bmsk = 1 << irq_num;
	val &= ~bmsk;
	ipahal_write_reg_n(IPA_IRQ_EN_EE_n, ipa_ee, val);

	return 0;
}

/** ipa_interrupts_init() - Initialize the IPA interrupts framework
 * @ipa_irq:	The interrupt number to allocate
 * @ee:		Execution environment
 * @ipa_dev:	The basic device structure representing the IPA driver
 *
 * - Initialize the ipa_interrupt_to_cb array
 * - Clear interrupts status
 * - Register the ipa interrupt handler - ipa_isr
 * - Enable apps processor wakeup by IPA interrupts
 */
int ipa_interrupts_init(u32 ipa_irq, u32 ee, struct device *ipa_dev)
{
	int idx;
	int res = 0;

	ipa_ee = ee;
	for (idx = 0; idx < IPA_IRQ_NUM_MAX; idx++) {
		ipa_interrupt_to_cb[idx].deferred_flag = false;
		ipa_interrupt_to_cb[idx].handler = NULL;
		ipa_interrupt_to_cb[idx].private_data = NULL;
		ipa_interrupt_to_cb[idx].interrupt = -1;
	}

	ipa_interrupt_wq = create_singlethread_workqueue(
			INTERRUPT_WORKQUEUE_NAME);
	if (!ipa_interrupt_wq) {
		ipa_err("workqueue creation failed\n");
		return -ENOMEM;
	}

	res = request_irq(ipa_irq, ipa_isr, IRQF_TRIGGER_RISING, "ipa",
			  ipa_dev);
	if (res) {
		ipa_err("fail to register IPA IRQ handler irq=%d\n", ipa_irq);
		return -ENODEV;
	}
	ipa_debug("IPA IRQ handler irq=%d registered\n", ipa_irq);

	spin_lock_init(&suspend_wa_lock);
	return 0;
}

/** ipa_suspend_active_aggr_wa() - Emulate suspend IRQ
 * @clnt_hndl:		suspended client handle, IRQ is emulated for this pipe
 *
 *  Emulate suspend IRQ to unsuspend client which was suspended with an open
 *  aggregation frame in order to bypass HW bug of IRQ not generated when
 *  endpoint is suspended during an open aggregation.
 */
void ipa_suspend_active_aggr_wa(u32 clnt_hdl)
{
	struct ipa_interrupt_info interrupt_info;
	struct ipa_interrupt_work_wrap *work_data;
	struct ipa_tx_suspend_irq_data *suspend_interrupt_data;
	int irq_num;
	int aggr_active_bitmap = ipahal_read_reg(IPA_STATE_AGGR_ACTIVE);

	if (aggr_active_bitmap & (1 << clnt_hdl)) {
		/* force close aggregation */
		ipahal_write_reg(IPA_AGGR_FORCE_CLOSE, (1 << clnt_hdl));

		/* simulate suspend IRQ */
		irq_num = ipa_irq_mapping[IPA_TX_SUSPEND_IRQ];
		interrupt_info = ipa_interrupt_to_cb[irq_num];
		if (!interrupt_info.handler) {
			ipa_err("no CB function for IPA_TX_SUSPEND_IRQ!\n");
			return;
		}
		suspend_interrupt_data =
				kzalloc(sizeof(*suspend_interrupt_data),
					GFP_ATOMIC);
		if (!suspend_interrupt_data) {
			ipa_err("failed allocating suspend_interrupt_data\n");
			return;
		}
		suspend_interrupt_data->endpoints = 1 << clnt_hdl;

		work_data = kzalloc(sizeof(*work_data), GFP_ATOMIC);
		if (!work_data)
			goto fail_alloc_work;

		INIT_WORK(&work_data->interrupt_work,
			  ipa_deferred_interrupt_work);
		work_data->handler = interrupt_info.handler;
		work_data->interrupt = IPA_TX_SUSPEND_IRQ;
		work_data->private_data = interrupt_info.private_data;
		work_data->interrupt_data = (void *)suspend_interrupt_data;
		queue_work(ipa_interrupt_wq, &work_data->interrupt_work);
		return;
fail_alloc_work:
		kfree(suspend_interrupt_data);
	}
}
