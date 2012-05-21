/*
 *  linux/arch/arm/mach-omap2/clock.c
 *
 *  Copyright (C) 2005-2008 Texas Instruments, Inc.
 *  Copyright (C) 2004-2010 Nokia Corporation
 *
 *  Contacts:
 *  Richard Woodruff <r-woodruff2@ti.com>
 *  Paul Walmsley
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#undef DEBUG

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <trace/events/power.h>

#include <asm/cpu.h>
#include "clockdomain.h"
#include <plat/cpu.h>
#include <plat/prcm.h>

#include "clock.h"
#include "cm2xxx_3xxx.h"
#include "cm-regbits-24xx.h"
#include "cm-regbits-34xx.h"

u16 cpu_mask;

/*
 * clkdm_control: if true, then when a clock is enabled in the
 * hardware, its clockdomain will first be enabled; and when a clock
 * is disabled in the hardware, its clockdomain will be disabled
 * afterwards.
 */
static bool clkdm_control = true;

LIST_HEAD(omap_clocks);
DEFINE_MUTEX(omap_clocks_mutex);

/*
 * Used for clocks that have the same value as the parent clock,
 * divided by some factor
 */
unsigned long omap_fixed_divisor_recalc(struct clk_hw *hw,
		unsigned long parent_rate)
{
        struct clk_hw_omap *oclk;

        if (!hw) {
                pr_warning("%s: hw is NULL\n", __func__);
                return -EINVAL;
        }

        oclk = to_clk_hw_omap(hw);

        WARN_ON(!oclk->fixed_div);

        return parent_rate / oclk->fixed_div;
}

/*
 * OMAP2+ specific clock functions
 */

/* Private functions */

/**
 * _omap2_module_wait_ready - wait for an OMAP module to leave IDLE
 * @clk: struct clk * belonging to the module
 *
 * If the necessary clocks for the OMAP hardware IP block that
 * corresponds to clock @clk are enabled, then wait for the module to
 * indicate readiness (i.e., to leave IDLE).  This code does not
 * belong in the clock code and will be moved in the medium term to
 * module-dependent code.  No return value.
 */
static void _omap2_module_wait_ready(struct clk_hw_omap *clk)
{
	void __iomem *companion_reg, *idlest_reg;
	u8 other_bit, idlest_bit, idlest_val;

	/* Not all modules have multiple clocks that their IDLEST depends on */
	if (clk->find_companion) {
		clk->find_companion(clk, &companion_reg, &other_bit);
		if (!(__raw_readl(companion_reg) & (1 << other_bit)))
			return;
	}

	clk->find_idlest(clk, &idlest_reg, &idlest_bit, &idlest_val);
	omap2_cm_wait_idlest(idlest_reg, (1 << idlest_bit), idlest_val,
			     __clk_get_name(clk->hw.clk));
}

/* Public functions */

/**
 * omap2_init_clk_clkdm - look up a clockdomain name, store pointer in clk
 * @clk: OMAP clock struct ptr to use
 *
 * Convert a clockdomain name stored in a struct clk 'clk' into a
 * clockdomain pointer, and save it into the struct clk.  Intended to be
 * called during clk_register().  No return value.
 */
void omap2_init_clk_clkdm(struct clk_hw *hw)
{
	struct clk_hw_omap *clk = to_clk_hw_omap(hw);
	struct clockdomain *clkdm;
	const char *clk_name;

	if (!clk->clkdm_name)
		return;

	clk_name = __clk_get_name(hw->clk);

	clkdm = clkdm_lookup(clk->clkdm_name);
	if (clkdm) {
		pr_debug("clock: associated clk %s to clkdm %s\n",
			 clk_name, clk->clkdm_name);
		clk->clkdm = clkdm;
	} else {
		pr_debug("clock: could not associate clk %s to "
			 "clkdm %s\n", clk_name, clk->clkdm_name);
	}
}

/**
 * omap2_clk_disable_clkdm_control - disable clkdm control on clk enable/disable
 *
 * Prevent the OMAP clock code from calling into the clockdomain code
 * when a hardware clock in that clockdomain is enabled or disabled.
 * Intended to be called at init time from omap*_clk_init().  No
 * return value.
 */
void __init omap2_clk_disable_clkdm_control(void)
{
	clkdm_control = false;
}

/**
 * omap2_clk_dflt_find_companion - find companion clock to @clk
 * @clk: struct clk * to find the companion clock of
 * @other_reg: void __iomem ** to return the companion clock CM_*CLKEN va in
 * @other_bit: u8 ** to return the companion clock bit shift in
 *
 * Note: We don't need special code here for INVERT_ENABLE for the
 * time being since INVERT_ENABLE only applies to clocks enabled by
 * CM_CLKEN_PLL
 *
 * Convert CM_ICLKEN* <-> CM_FCLKEN*.  This conversion assumes it's
 * just a matter of XORing the bits.
 *
 * Some clocks don't have companion clocks.  For example, modules with
 * only an interface clock (such as MAILBOXES) don't have a companion
 * clock.  Right now, this code relies on the hardware exporting a bit
 * in the correct companion register that indicates that the
 * nonexistent 'companion clock' is active.  Future patches will
 * associate this type of code with per-module data structures to
 * avoid this issue, and remove the casts.  No return value.
 */
void omap2_clk_dflt_find_companion(struct clk_hw_omap *clk,
	 		void __iomem **other_reg, u8 *other_bit)
{
	u32 r;

	/*
	 * Convert CM_ICLKEN* <-> CM_FCLKEN*.  This conversion assumes
	 * it's just a matter of XORing the bits.
	 */
	r = ((__force u32)clk->enable_reg ^ (CM_FCLKEN ^ CM_ICLKEN));

	*other_reg = (__force void __iomem *)r;
	*other_bit = clk->enable_bit;
}

/**
 * omap2_clk_dflt_find_idlest - find CM_IDLEST reg va, bit shift for @clk
 * @clk: struct clk * to find IDLEST info for
 * @idlest_reg: void __iomem ** to return the CM_IDLEST va in
 * @idlest_bit: u8 * to return the CM_IDLEST bit shift in
 * @idlest_val: u8 * to return the idle status indicator
 *
 * Return the CM_IDLEST register address and bit shift corresponding
 * to the module that "owns" this clock.  This default code assumes
 * that the CM_IDLEST bit shift is the CM_*CLKEN bit shift, and that
 * the IDLEST register address ID corresponds to the CM_*CLKEN
 * register address ID (e.g., that CM_FCLKEN2 corresponds to
 * CM_IDLEST2).  This is not true for all modules.  No return value.
 */
void omap2_clk_dflt_find_idlest(struct clk_hw_omap *clk,
		void __iomem **idlest_reg, u8 *idlest_bit, u8 *idlest_val)
{
	u32 r;

	r = (((__force u32)clk->enable_reg & ~0xf0) | 0x20);
	*idlest_reg = (__force void __iomem *)r;
	*idlest_bit = clk->enable_bit;

	/*
	 * 24xx uses 0 to indicate not ready, and 1 to indicate ready.
	 * 34xx reverses this, just to keep us on our toes
	 * AM35xx uses both, depending on the module.
	 */
	if (cpu_is_omap24xx())
		*idlest_val = OMAP24XX_CM_IDLEST_VAL;
	else if (cpu_is_omap34xx())
		*idlest_val = OMAP34XX_CM_IDLEST_VAL;
	else
		BUG();

}

int omap2_dflt_clk_enable(struct clk_hw *hw)
{
	struct clk_hw_omap *clk;
 	u32 v;
	int ret = 0;

	clk = to_clk_hw_omap(hw);

	if (clkdm_control && clk->clkdm) {
		ret = clkdm_clk_enable(clk->clkdm, hw->clk);
		if (ret) {
			WARN(1, "%s: could not enable %s's clockdomain %s: %d\n",
					__func__, __clk_get_name(hw->clk),
					clk->clkdm->name, ret);
			return ret;
		}
	}

	if (unlikely(clk->enable_reg == NULL)) {
		pr_err("%s: %s missing enable_reg\n", __func__,
				__clk_get_name(hw->clk));
		ret = -EINVAL;
		goto err;
	}

	/* FIXME should not have INVERT_ENABLE bit here */
	v = __raw_readl(clk->enable_reg);
	if (clk->flags & INVERT_ENABLE)
		v &= ~(1 << clk->enable_bit);
	else
		v |= (1 << clk->enable_bit);
	__raw_writel(v, clk->enable_reg);
	v = __raw_readl(clk->enable_reg); /* OCP barrier */

	if (clk->find_idlest)
		_omap2_module_wait_ready(clk);

	return 0;

err:
	if (clkdm_control && clk->clkdm)
		clkdm_clk_disable(clk->clkdm, hw->clk);
 	return ret;
}

void omap2_dflt_clk_disable(struct clk_hw *hw)
{
	struct clk_hw_omap *clk;
	u32 v;

	clk = to_clk_hw_omap(hw);
	if (!clk->enable_reg) {
		/*
		 * 'independent' here refers to a clock which is not
		 * controlled by its parent.
		 */
		pr_err("%s: independent clock %s has no enable_reg\n",
				__func__, __clk_get_name(hw->clk));
 		return;
 	}

	v = __raw_readl(clk->enable_reg);
	if (clk->flags & INVERT_ENABLE)
		v |= (1 << clk->enable_bit);
	else
		v &= ~(1 << clk->enable_bit);
	__raw_writel(v, clk->enable_reg);
	/* No OCP barrier needed here since it is a disable operation */

	if (clkdm_control && clk->clkdm)
		clkdm_clk_disable(clk->clkdm, hw->clk);
}

/* TODO: Need to handle these for the COMMON clk case */
int __init omap2_clk_switch_mpurate_at_boot(const char *mpurate_ck_name)
{
	return 0;
}

void __init omap2_clk_print_new_rates(const char *hfclkin_ck_name,
				      const char *core_ck_name,
				      const char *mpu_ck_name)
{}
