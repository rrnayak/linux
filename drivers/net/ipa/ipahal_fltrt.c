// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#define pr_fmt(fmt)	"ipahal %s:%d " fmt, __func__, __LINE__

#include <linux/debugfs.h>
#include "ipahal.h"
#include "ipahal_fltrt.h"
#include "ipahal_fltrt_i.h"
#include "ipahal_i.h"

/* Width and alignment values for H/W structures.  Values could
 * differ for different versions of IPA hardware.
 */
#define IPA_HW_TBL_WIDTH		8
#define IPA_HW_TBL_SYSADDR_ALIGN	128
#define IPA_HW_TBL_LCLADDR_ALIGN	8
#define IPA_HW_TBL_BLK_SIZE_ALIGN	128
#define IPA_HW_RULE_START_ALIGN	8
#define IPA_HW_TBL_HDR_WIDTH		8
#define IPA_HW_TBL_ADDR_MASK		127
#define IPA_HW_RULE_BUF_SIZE		256

/* Rules Priority.
 * Needed due to rules classification to hashable and non-hashable.
 * Higher priority is lower in number. i.e. 0 is highest priority
 */
#define IPA_RULE_MAX_PRIORITY	0
#define IPA_RULE_MIN_PRIORITY	1023

/* RULE ID, bit length (e.g. 10 bits).  */
#define IPA_RULE_ID_BIT_LEN		10
#define IPA_LOW_RULE_ID		1

/* struct ipahal_fltrt_obj - Flt/Rt H/W information for specific IPA version
 * @tbl_width: Width of table in bytes
 * @sysaddr_align: System table address alignment
 * @lcladdr_align: Local table offset alignment
 * @blk_sz_align: Rules block size alignment
 * @rule_start_align: Rule start address alignment
 * @tbl_hdr_width: Width of the header structure in bytes
 * @tbl_addr_mask: Masking for Table address
 * @rule_max_prio: Max possible priority of a rule
 * @rule_min_prio: Min possible priority of a rule
 * @low_rule_id: Low value of Rule ID that can be used
 * @rule_id_bit_len: Rule is high (MSB) bit len
 * @rule_buf_size: Max size rule may utilize.
 * @write_val_to_hdr: Write address or offset to header entry
 * @create_flt_bitmap: Create bitmap in H/W format using given bitmap
 * @create_tbl_addr: Given raw table address, create H/W formated one
 * @parse_tbl_addr: Parse the given H/W address (hdr format)
 * @rt_generate_hw_rule: Generate RT rule in H/W format
 * @flt_generate_hw_rule: Generate FLT rule in H/W format
 * @flt_generate_eq: Generate flt equation attributes from rule attributes
 * @rt_parse_hw_rule: Parse rt rule read from H/W
 * @flt_parse_hw_rule: Parse flt rule read from H/W
 * @eq_bitfield: Array of the bit fields of the support equations
 */
struct ipahal_fltrt_obj {
	u32 tbl_width;
	u32 sysaddr_align;
	u32 lcladdr_align;
	u32 blk_sz_align;
	u32 rule_start_align;
	u32 tbl_hdr_width;
	u32 tbl_addr_mask;
	int rule_max_prio;
	int rule_min_prio;
	u32 low_rule_id;
	u32 rule_id_bit_len;
	u32 rule_buf_size;
	void (*write_val_to_hdr)(u64 val, u8 *hdr);
	u64 (*create_flt_bitmap)(u64 ep_bitmap);
	u64 (*create_tbl_addr)(u64 addr);
	u64 (*parse_tbl_addr)(u64 hwaddr);
	u8 eq_bitfield[IPA_EQ_MAX];
};

static struct ipahal_fltrt_obj ipahal_fltrt;

static u64 ipa_fltrt_create_flt_bitmap(u64 ep_bitmap)
{
	/* At IPA3, global configuration is possible but not used */
	return ep_bitmap << 1;
}

static u64 ipa_fltrt_create_tbl_addr(u64 addr)
{
	ipa_assert(!(addr % ipahal_fltrt.sysaddr_align));

	return addr;
}

static u64 ipa_fltrt_parse_tbl_addr(u64 hwaddr)
{
	ipa_debug_low("Parsing hwaddr 0x%llx\n", hwaddr);

	ipa_assert(!(hwaddr & 0x1));
	ipa_assert(!(hwaddr % ipahal_fltrt.sysaddr_align));

	return hwaddr;
}

/* The IPA implements offloaded packet filtering and routing
 * capabilities.  This is managed by programming IPA-resident
 * tables of rules that define the processing that should be
 * performed by the IPA and the conditions under which they
 * should be applied.  Aspects of these rules are constrained
 * by things like table entry sizes and alignment requirements.
 * As IPA technology evolves, some of these constraints may
 * change, and the following table specifies the parameters
 * that should be used for particular versions of IPA hardware.
 *
 * The table consists of a set of "filter/route objects", each of
 * which is a structure that defines the constraints that must be
 * used for a particular version of IPA hardware.  There are also a
 * few functions that format data related to these tables to be sent
 * to the IPA, or parse an address coming from it.  The first
 * version of IPA hardware supported by the "ipahal" layer is 3.0.
 *
 * A version of IPA hardware newer than 3.0 does not need to
 * provide an entry in the following array if the constraints for
 * that version are the same as was defined by an older version;
 * it only requires an entry if one or more parameters differ from
 * what's used in the previous version.  If a slot below is empty
 * (indicated by a 0 tbl_width field) the corresponding hardware
 * version's properties are taken from an older hardware version.
 *
 * The entries in this table have the following constraints.  Much
 * of this will be dictated by the hardware; the following statements
 * document assumptions of the code:
 * - 0 is not a valid table width; a 0 tbl_width value in an
 *   entry indicates the entry contains no definitions, and
 *   the definitions for that corresponding hardware version
 *   are inherited from an earlier version's entry.
 * - sysaddr_align is non-zero, and is a power of 2
 * - lcladdr_align is non-zero, and is a power of 2.
 * - blk_sz_align is non-zero, and is a power of 2.
 * - rule_start_align is non-zero, and is a power of 2.
 * - tbl_hdr_width is non-zero
 * - tbl_addr_mask is non-zero and is one less than a power of 2
 * - rule_min_prio is not less than rule_max_prio (0 is max prio)
 * - rule_id_bit_len is 2 or more
 * - write_val_to_hdr, create_flt_bitmap, create_tbl_addr, and
 *   parse_tbl_addr must be non-null function pointers
 */
static const struct ipahal_fltrt_obj ipahal_fltrt_objs[] = {
	/* IPAv3.5.1 */
	[IPA_HW_v3_5_1] = {
		.tbl_width		= IPA_HW_TBL_WIDTH,
		.sysaddr_align		= IPA_HW_TBL_SYSADDR_ALIGN,
		.lcladdr_align		= IPA_HW_TBL_LCLADDR_ALIGN,
		.blk_sz_align		= IPA_HW_TBL_BLK_SIZE_ALIGN,
		.rule_start_align	= IPA_HW_RULE_START_ALIGN,
		.tbl_hdr_width		= IPA_HW_TBL_HDR_WIDTH,
		.tbl_addr_mask		= IPA_HW_TBL_ADDR_MASK,
		.rule_max_prio		= IPA_RULE_MAX_PRIORITY,
		.rule_min_prio		= IPA_RULE_MIN_PRIORITY,
		.low_rule_id		= IPA_LOW_RULE_ID,
		.rule_id_bit_len	= IPA_RULE_ID_BIT_LEN,
		.rule_buf_size		= IPA_HW_RULE_BUF_SIZE,
		.write_val_to_hdr	= ipa_write_64,
		.create_flt_bitmap	= ipa_fltrt_create_flt_bitmap,
		.create_tbl_addr	= ipa_fltrt_create_tbl_addr,
		.parse_tbl_addr		= ipa_fltrt_parse_tbl_addr,
		.eq_bitfield = {
			[IPA_TOS_EQ]			= 0,
			[IPA_PROTOCOL_EQ]		= 1,
			[IPA_TC_EQ]			= 2,
			[IPA_OFFSET_MEQ128_0]		= 3,
			[IPA_OFFSET_MEQ128_1]		= 4,
			[IPA_OFFSET_MEQ32_0]		= 5,
			[IPA_OFFSET_MEQ32_1]		= 6,
			[IPA_IHL_OFFSET_MEQ32_0]	= 7,
			[IPA_IHL_OFFSET_MEQ32_1]	= 8,
			[IPA_METADATA_COMPARE]		= 9,
			[IPA_IHL_OFFSET_RANGE16_0]	= 10,
			[IPA_IHL_OFFSET_RANGE16_1]	= 11,
			[IPA_IHL_OFFSET_EQ_32]		= 12,
			[IPA_IHL_OFFSET_EQ_16]		= 13,
			[IPA_FL_EQ]			= 14,
			[IPA_IS_FRAG]			= 15,
		},
	},
};

/* Set up an empty table in system memory.  This will be used, for
 * example, to delete a route table safely.  If successful, record
 * the table and also the dev pointer in the IPA HAL context.
 */
int ipahal_empty_fltrt_init(void)
{
	struct ipa_mem_buffer *mem = &ipahal_ctx->empty_fltrt_tbl;
	u32 size = ipahal_fltrt.tbl_width;

	if (ipahal_dma_alloc(mem, size, GFP_KERNEL)) {
		ipa_err("DMA buff alloc fail %u bytes for empty tbl\n", size);
		return -ENOMEM;
	}

	if (mem->phys_base % ipahal_fltrt.sysaddr_align) {
		ipa_err("Empty table buf is not address aligned 0x%pad\n",
			&mem->phys_base);
		ipahal_dma_free(mem);

		return -EFAULT;
	}
	ipahal_ctx->empty_fltrt_tbl_addr =
			ipahal_fltrt.create_tbl_addr(mem->phys_base);

	ipa_debug("empty table allocated in system memory");

	return 0;
}

void ipahal_empty_fltrt_destroy(void)
{
	ipahal_ctx->empty_fltrt_tbl_addr = 0;
	ipahal_dma_free(&ipahal_ctx->empty_fltrt_tbl);
}

/* ipahal_fltrt_init() - Build the FLT/RT information table
 *  See ipahal_fltrt_objs[] comments
 */
void ipahal_fltrt_init(enum ipa_hw_version hw_version)
{
	int i;

	ipa_assert(hw_version < ARRAY_SIZE(ipahal_fltrt_objs));

	ipa_debug("Entry - HW_TYPE=%d\n", hw_version);

	/* Build up a the filter/route table descriptions we'll use */
	for (i = hw_version; i >= 0; i--) {
		const struct ipahal_fltrt_obj *fltrt;

		fltrt = &ipahal_fltrt_objs[i];
		if (fltrt->tbl_width) {
			ipahal_fltrt = *fltrt;
			break;
		}
	}
}

/* Get the H/W table (flt/rt) header width */
u32 ipahal_get_hw_tbl_hdr_width(void)
{
	return ipahal_fltrt.tbl_hdr_width;
}

/* Get the H/W local table (SRAM) address alignment
 * Tables headers references to local tables via offsets in SRAM
 * This function return the alignment of the offset that IPA expects
 */
u32 ipahal_get_lcl_tbl_addr_alignment(void)
{
	return ipahal_fltrt.lcladdr_align - 1;
}

/* Rule priority is used to distinguish rules order
 * at the integrated table consisting from hashable and
 * non-hashable tables. Max priority are rules that once are
 * scanned by IPA, IPA will not look for further rules and use it.
 */
int ipahal_get_rule_max_priority(void)
{
	return ipahal_fltrt.rule_max_prio;
}

/* Given a priority, calc and return the next lower one if it is in
 * legal range.
 */
int ipahal_rule_decrease_priority(int *prio)
{
	if (!prio) {
		ipa_err("Invalid Input\n");
		return -EINVAL;
	}

	/* Priority logic is reverse. 0 priority considred max priority */
	if (*prio > ipahal_fltrt.rule_min_prio ||
	    *prio < ipahal_fltrt.rule_max_prio) {
		ipa_err("Invalid given priority %d\n", *prio);
		return -EINVAL;
	}

	*prio += 1;

	if (*prio > ipahal_fltrt.rule_min_prio) {
		ipa_err("Cannot decrease priority. Already on min\n");
		*prio -= 1;
		return -EFAULT;
	}

	return 0;
}

/* Does the given ID represents rule miss?
 * Rule miss ID, is always the max ID possible in the bit-pattern
 */
bool ipahal_is_rule_miss_id(u32 id)
{
	return id == ((1U << ipahal_fltrt.rule_id_bit_len) - 1);
}

/* Get rule ID with high bit only asserted
 * Used e.g. to create groups of IDs according to this bit
 */
u32 ipahal_get_rule_id_hi_bit(void)
{
	return BIT(ipahal_fltrt.rule_id_bit_len - 1);
}

/* Get the low value possible to be used for rule-id */
u32 ipahal_get_low_rule_id(void)
{
	return ipahal_fltrt.low_rule_id;
}

void ipahal_free_empty_img(struct ipa_mem_buffer *mem)
{
	ipahal_dma_free(mem);
}

/* ipahal_rt_generate_empty_img() - Generate empty route image
 *  Creates routing header buffer for the given tables number.
 *  For each table, make it point to the empty table on DDR.
 * @tbls_num: Number of tables. For each will have an entry in the header
 * @mem: mem object that points to DMA mem representing the hdr structure
 * @gfp: GFP flag to supply with DMA allocation request
 */
int ipahal_rt_generate_empty_img(u32 tbls_num, struct ipa_mem_buffer *mem,
				 gfp_t gfp)
{
	u32 width = ipahal_fltrt.tbl_hdr_width;
	int i = 0;
	u64 addr;

	ipa_debug("Entry\n");

	if (ipahal_dma_alloc(mem, tbls_num * width, gfp))
		return -ENOMEM;

	addr = ipahal_ctx->empty_fltrt_tbl_addr;
	while (i < tbls_num)
		ipahal_fltrt.write_val_to_hdr(addr, mem->base + i++ * width);

	return 0;
}

/* ipahal_flt_generate_empty_img() - Generate empty filter image
 *  Creates filter header buffer for the given tables number.
 *  For each table, make it point to the empty table on DDR.
 * @tbls_num: Number of tables. For each will have an entry in the header
 * @ep_bitmap: Bitmap representing the EP that has flt tables. The format
 *  should be: bit0->EP0, bit1->EP1
 *  If bitmap is zero -> create tbl without bitmap entry
 * @mem: mem object that points to DMA mem representing the hdr structure
 * @gfp: GFP flag to supply with DMA allocation request
 */
int ipahal_flt_generate_empty_img(u32 tbls_num, u64 ep_bitmap,
				  struct ipa_mem_buffer *mem, gfp_t gfp)
{
	u32 width = ipahal_fltrt.tbl_hdr_width;
	int i = 0;
	u64 addr;

	ipa_debug("Entry - ep_bitmap 0x%llx\n", ep_bitmap);

	if (ep_bitmap)
		tbls_num++;

	if (ipahal_dma_alloc(mem, tbls_num * width, gfp))
		return -ENOMEM;

	if (ep_bitmap) {
		u64 flt_bitmap = ipahal_fltrt.create_flt_bitmap(ep_bitmap);

		ipa_debug("flt bitmap 0x%llx\n", flt_bitmap);
		ipahal_fltrt.write_val_to_hdr(flt_bitmap, mem->base);
		i++;
	}

	addr = ipahal_ctx->empty_fltrt_tbl_addr;
	while (i < tbls_num)
		ipahal_fltrt.write_val_to_hdr(addr, mem->base + i++ * width);

	return 0;
}
