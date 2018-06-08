// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#ifndef _IPAHAL_FLTRT_I_H_
#define _IPAHAL_FLTRT_I_H_

/* enum ipa_fltrt_equations - RULE equations
 *  These are names values to the equations that can be used
 *  The HAL layer holds mapping between these names and H/W
 *  presentation.
 */
enum ipa_fltrt_equations {
	IPA_TOS_EQ,
	IPA_PROTOCOL_EQ,
	IPA_TC_EQ,
	IPA_OFFSET_MEQ128_0,
	IPA_OFFSET_MEQ128_1,
	IPA_OFFSET_MEQ32_0,
	IPA_OFFSET_MEQ32_1,
	IPA_IHL_OFFSET_MEQ32_0,
	IPA_IHL_OFFSET_MEQ32_1,
	IPA_METADATA_COMPARE,
	IPA_IHL_OFFSET_RANGE16_0,
	IPA_IHL_OFFSET_RANGE16_1,
	IPA_IHL_OFFSET_EQ_32,
	IPA_IHL_OFFSET_EQ_16,
	IPA_FL_EQ,
	IPA_IS_FRAG,
	IPA_EQ_MAX,
};

void ipahal_fltrt_init(enum ipa_hw_version hw_version);
int ipahal_empty_fltrt_init(void);
void ipahal_empty_fltrt_destroy(void);

#endif /* _IPAHAL_FLTRT_I_H_ */
