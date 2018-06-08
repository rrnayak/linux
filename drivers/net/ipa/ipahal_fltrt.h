// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#ifndef _IPAHAL_FLTRT_H_
#define _IPAHAL_FLTRT_H_

#include "ipa_common_i.h"

/* struct ipahal_fltrt_alloc_imgs_params - Params for tbls imgs allocations
 *  The allocation logic will allocate DMA memory representing the header.
 *  If the bodies are local (SRAM) the allocation will allocate
 *  a DMA buffers that would contain the content of these local tables in raw
 * @ipt: IP version type
 * @tbls_num: Number of tables to represent by the header
 * @num_lcl_hash_tbls: Number of local (sram) hashable tables
 * @num_lcl_nhash_tbls: Number of local (sram) non-hashable tables
 * @total_sz_lcl_hash_tbls: Total size of local hashable tables
 * @total_sz_lcl_nhash_tbls: Total size of local non-hashable tables
 * @hash_hdr/nhash_hdr: OUT params for the header structures
 * @hash_bdy/nhash_bdy: OUT params for the local body structures
 */
struct ipahal_fltrt_alloc_imgs_params {
	enum ipa_ip_type ipt;
	u32 tbls_num;
	u32 num_lcl_hash_tbls;
	u32 num_lcl_nhash_tbls;
	u32 total_sz_lcl_hash_tbls;
	u32 total_sz_lcl_nhash_tbls;

	/* OUT PARAMS */
	struct ipa_mem_buffer hash_hdr;
	struct ipa_mem_buffer nhash_hdr;
	struct ipa_mem_buffer hash_bdy;
	struct ipa_mem_buffer nhash_bdy;
};

/* enum ipahal_rt_rule_hdr_type - Header type used in rt rules
 * @IPAHAL_RT_RULE_HDR_NONE: No header is used
 * @IPAHAL_RT_RULE_HDR_RAW: Raw header is used
 * @IPAHAL_RT_RULE_HDR_PROC_CTX: Header Processing context is used
 */
enum ipahal_rt_rule_hdr_type {
	IPAHAL_RT_RULE_HDR_NONE,
	IPAHAL_RT_RULE_HDR_RAW,
	IPAHAL_RT_RULE_HDR_PROC_CTX,
};

/* struct ipahal_rt_rule_gen_params - Params for generating rt rule
 * @ipt: IP family version
 * @dst_pipe_idx: Destination pipe index
 * @hdr_type: Header type to be used
 * @hdr_lcl: Does header on local or system table?
 * @hdr_ofst: Offset of the header in the header table
 * @priority: Rule priority
 * @id: Rule ID
 * @rule: Rule info
 */
struct ipahal_rt_rule_gen_params {
	enum ipa_ip_type ipt;
	int dst_pipe_idx;
	enum ipahal_rt_rule_hdr_type hdr_type;
	bool hdr_lcl;
	u32 hdr_ofst;
	u32 priority;
	u32 id;
	const struct ipa_rt_rule *rule;
};

/* struct ipahal_rt_rule_entry - Rt rule info parsed from H/W
 * @dst_pipe_idx: Destination pipe index
 * @hdr_lcl: Does the references header located in sram or system mem?
 * @hdr_ofst: Offset of the header in the header table
 * @hdr_type: Header type to be used
 * @priority: Rule priority
 * @retain_hdr: to retain the removed header in header removal
 * @id: Rule ID
 * @rule_size: Rule size in memory
 */
struct ipahal_rt_rule_entry {
	int dst_pipe_idx;
	bool hdr_lcl;
	u32 hdr_ofst;
	enum ipahal_rt_rule_hdr_type hdr_type;
	u32 priority;
	bool retain_hdr;
	u32 id;
	u32 rule_size;
};

/* struct ipahal_flt_rule_gen_params - Params for generating flt rule
 * @ipt: IP family version
 * @rt_tbl_idx: Routing table the rule pointing to
 * @priority: Rule priority
 * @id: Rule ID
 * @rule: Rule info
 */
struct ipahal_flt_rule_gen_params {
	enum ipa_ip_type ipt;
	u32 rt_tbl_idx;
	u32 priority;
	u32 id;
	const struct ipa_flt_rule *rule;
};

/* Get the H/W table (flt/rt) header width */
u32 ipahal_get_hw_tbl_hdr_width(void);

/* Get the H/W local table (SRAM) address alignment
 * Tables headers references to local tables via offsets in SRAM
 * This function return the alignment of the offset that IPA expects
 */
u32 ipahal_get_lcl_tbl_addr_alignment(void);

/* Rule priority is used to distinguish rules order
 * at the integrated table consisting from hashable and
 * non-hashable tables. Max priority are rules that once are
 * scanned by IPA, IPA will not look for further rules and use it.
 */
int ipahal_get_rule_max_priority(void);

/* Given a priority, calc and return the next lower one if it is in
 * legal range.
 */
int ipahal_rule_decrease_priority(int *prio);

/* Does the given ID represents rule miss? */
bool ipahal_is_rule_miss_id(u32 id);

/* Get rule ID with high bit only asserted
 * Used e.g. to create groups of IDs according to this bit
 */
u32 ipahal_get_rule_id_hi_bit(void);

/* Get the low value possible to be used for rule-id */
u32 ipahal_get_low_rule_id(void);

/* ipahal_rt_generate_empty_img() - Generate empty route image
 *  Creates routing header buffer for the given tables number.
 * For each table, make it point to the empty table on DDR.
 * @tbls_num: Number of tables. For each will have an entry in the header
 * @mem: mem object that points to DMA mem representing the hdr structure
 * @atomic: should DMA allocation be executed with atomic flag
 */
int ipahal_rt_generate_empty_img(u32 tbls_num, struct ipa_mem_buffer *mem,
				 gfp_t gfp);

/* ipahal_flt_generate_empty_img() - Generate empty filter image
 *  Creates filter header buffer for the given tables number.
 *  For each table, make it point to the empty table on DDR.
 * @tbls_num: Number of tables. For each will have an entry in the header
 * @ep_bitmap: Bitmap representing the EP that has flt tables. The format
 *  should be: bit0->EP0, bit1->EP1
 * @mem: mem object that points to DMA mem representing the hdr structure
 * @atomic: should DMA allocation be executed with atomic flag
 */
int ipahal_flt_generate_empty_img(u32 tbls_num, u64 ep_bitmap,
				  struct ipa_mem_buffer *mem, gfp_t gfp);

/* ipahal_free_empty_img() - free empty filter or route image
 */
void ipahal_free_empty_img(struct ipa_mem_buffer *mem);

#endif /* _IPAHAL_FLTRT_H_ */
