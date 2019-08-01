/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2017-2019, The Linux Foundation. All rights reserved. */

#ifndef __QCOM_WATCHDOG_H__
#define __QCOM_WATCHDOG_H__

#ifdef CONFIG_QCOM_WDT
void qcom_wdt_bite(void);
#else
static inline void qcom_wdt_bite(void) { }
#endif

#endif
