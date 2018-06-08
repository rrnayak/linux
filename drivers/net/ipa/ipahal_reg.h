// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#ifndef _IPAHAL_REG_H_
#define _IPAHAL_REG_H_

/* Registers names
 *
 * NOTE:: Any change to this enum, need to change to ipareg_name_to_str
 *	array as well.
 */
enum ipahal_reg {
	IPA_ROUTE,
	IPA_IRQ_STTS_EE_n,
	IPA_IRQ_EN_EE_n,
	IPA_IRQ_CLR_EE_n,
	IPA_IRQ_SUSPEND_INFO_EE_n,
	IPA_SUSPEND_IRQ_EN_EE_n,
	IPA_SUSPEND_IRQ_CLR_EE_n,
	IPA_BCR,
	IPA_ENABLED_PIPES,
	IPA_VERSION,
	IPA_TAG_TIMER,
	IPA_COMP_HW_VERSION,
	IPA_SPARE_REG_1,
	IPA_STATE_AGGR_ACTIVE,
	IPA_ENDP_INIT_HDR_n,
	IPA_ENDP_INIT_HDR_EXT_n,
	IPA_ENDP_INIT_AGGR_n,
	IPA_AGGR_FORCE_CLOSE,
	IPA_ENDP_INIT_ROUTE_n,
	IPA_ENDP_INIT_MODE_n,
	IPA_ENDP_INIT_NAT_n,
	IPA_ENDP_INIT_CTRL_n,
	IPA_ENDP_INIT_HOL_BLOCK_EN_n,
	IPA_ENDP_INIT_HOL_BLOCK_TIMER_n,
	IPA_ENDP_INIT_DEAGGR_n,
	IPA_ENDP_INIT_SEQ_n,
	IPA_DEBUG_CNT_REG_n,
	IPA_ENDP_INIT_CFG_n,
	IPA_IRQ_EE_UC_n,
	IPA_ENDP_INIT_HDR_METADATA_MASK_n,
	IPA_SHARED_MEM_SIZE,
	IPA_SRAM_DIRECT_ACCESS_n,
	IPA_DEBUG_CNT_CTRL_n,
	IPA_LOCAL_PKT_PROC_CNTXT_BASE,
	IPA_ENDP_STATUS_n,
	IPA_ENDP_FILTER_ROUTER_HSH_CFG_n,
	IPA_SRC_RSRC_GRP_01_RSRC_TYPE_n,
	IPA_SRC_RSRC_GRP_23_RSRC_TYPE_n,
	IPA_DST_RSRC_GRP_01_RSRC_TYPE_n,
	IPA_DST_RSRC_GRP_23_RSRC_TYPE_n,
	IPA_QSB_MAX_WRITES,
	IPA_QSB_MAX_READS,
	IPA_IDLE_INDICATION_CFG,
	IPA_DPS_SEQUENCER_FIRST,
	IPA_HPS_SEQUENCER_FIRST,
	IPA_ENABLE_GSI,
	IPA_ENDP_GSI_CFG_TLV_n,
	IPA_ENDP_GSI_CFG_AOS_n,
	IPA_ENDP_GSI_CFG1_n,
	IPA_ENDP_GSI_CFG2_n,
	IPA_REG_MAX,
};

/* struct ipahal_reg_route - IPA route register
 * @route_dis: route disable
 * @route_def_pipe: route default pipe
 * @route_def_hdr_table: route default header table
 * @route_def_hdr_ofst: route default header offset table
 * @route_frag_def_pipe: Default pipe to route fragmented exception
 *    packets and frag new rule statues, if source pipe does not have
 *    a notification status pipe defined.
 * @route_def_retain_hdr: default value of retain header. It is used
 *    when no rule was hit
 */
struct ipahal_reg_route {
	u32 route_dis;
	u32 route_def_pipe;
	u32 route_def_hdr_table;
	u32 route_def_hdr_ofst;
	u8  route_frag_def_pipe;
	u32 route_def_retain_hdr;
};

/* struct ipahal_reg_endp_init_route - IPA ENDP_INIT_ROUTE_n register
 * @route_table_index: Default index of routing table (IPA Consumer).
 */
struct ipahal_reg_endp_init_route {
	u32 route_table_index;
};

/* struct ipahal_reg_endp_init_mode - IPA ENDP_INIT_MODE_n register
 * @dst_pipe_number: This parameter specifies destination output-pipe-packets
 *	will be routed to. Valid for DMA mode only and for Input
 *	Pipes only (IPA Consumer)
 */
struct ipahal_reg_endp_init_mode {
	u32 dst_pipe_number;
	struct ipa_ep_cfg_mode ep_mode;
};

/* struct ipahal_reg_shared_mem_size - IPA SHARED_MEM_SIZE register
 * @shared_mem_sz: Available size [in 8Bytes] of SW partition within
 *	IPA shared memory.
 * @shared_mem_baddr: Offset of SW partition within IPA
 *	shared memory[in 8Bytes]. To get absolute address of SW partition,
 *	add this offset to IPA_SRAM_DIRECT_ACCESS_n baddr.
 */
struct ipahal_reg_shared_mem_size {
	u32 shared_mem_sz;
	u32 shared_mem_baddr;
};

/* struct ipahal_reg_ep_cfg_status - status configuration in IPA end-point
 * @status_en: Determines if end point supports Status Indications. SW should
 *	set this bit in order to enable Statuses. Output Pipe - send
 *	Status indications only if bit is set. Input Pipe - forward Status
 *	indication to STATUS_ENDP only if bit is set. Valid for Input
 *	and Output Pipes (IPA Consumer and Producer)
 * @status_ep: Statuses generated for this endpoint will be forwarded to the
 *	specified Status End Point. Status endpoint needs to be
 *	configured with STATUS_EN=1 Valid only for Input Pipes (IPA
 *	Consumer)
 * @status_location: Location of PKT-STATUS on destination pipe.
 *	If set to 0 (default), PKT-STATUS will be appended before the packet
 *	for this endpoint. If set to 1, PKT-STATUS will be appended after the
 *	packet for this endpoint. Valid only for Output Pipes (IPA Producer)
 * @status_pkt_suppress:
 */
struct ipahal_reg_ep_cfg_status {
	bool status_en;
	u8 status_ep;
	bool status_location;
	u8 status_pkt_suppress;
};

/* struct ipa_hash_tuple - Hash tuple members for flt and rt
 *  the fields tells if to be masked or not
 * @src_id: pipe number for flt, table index for rt
 * @src_ip_addr: IP source address
 * @dst_ip_addr: IP destination address
 * @src_port: L4 source port
 * @dst_port: L4 destination port
 * @protocol: IP protocol field
 * @meta_data: packet meta-data
 *
 */
struct ipahal_reg_hash_tuple {
	/* src_id: pipe in flt, tbl index in rt */
	bool src_id;
	bool src_ip_addr;
	bool dst_ip_addr;
	bool src_port;
	bool dst_port;
	bool protocol;
	bool meta_data;
};

/* struct ipahal_reg_fltrt_hash_tuple - IPA hash tuple register
 * @flt: Hash tuple info for filtering
 * @rt: Hash tuple info for routing
 * @undefinedX: Undefined/Unused bit fields set of the register
 */
struct ipahal_reg_fltrt_hash_tuple {
	struct ipahal_reg_hash_tuple flt;
	struct ipahal_reg_hash_tuple rt;
	u32 undefined1;
	u32 undefined2;
};

/* enum ipahal_reg_dbg_cnt_type - Debug Counter Type
 * DBG_CNT_TYPE_IPV4_FLTR - Count IPv4 filtering rules
 * DBG_CNT_TYPE_IPV4_ROUT - Count IPv4 routing rules
 * DBG_CNT_TYPE_GENERAL - General counter
 * DBG_CNT_TYPE_IPV6_FLTR - Count IPv6 filtering rules
 * DBG_CNT_TYPE_IPV4_ROUT - Count IPv6 routing rules
 */
enum ipahal_reg_dbg_cnt_type {
	DBG_CNT_TYPE_IPV4_FLTR	= 0,
	DBG_CNT_TYPE_IPV4_ROUT	= 1,
	DBG_CNT_TYPE_GENERAL	= 2,
	DBG_CNT_TYPE_IPV6_FLTR	= 4,
	DBG_CNT_TYPE_IPV6_ROUT	= 5,
};

/* struct ipahal_reg_debug_cnt_ctrl - IPA_DEBUG_CNT_CTRL_n register
 * @en - Enable debug counter
 * @type - Type of debugging couting
 * @product - False->Count Bytes . True->Count #packets
 * @src_pipe - Specific Pipe to match. If FF, no need to match
 *	specific pipe
 * @rule_idx_pipe_rule - Global Rule or Pipe Rule. If pipe, then indicated by
 *	src_pipe. Starting at IPA V3_5,
 *	no support on Global Rule. This field will be ignored.
 * @rule_idx - Rule index. Irrelevant for type General
 */
struct ipahal_reg_debug_cnt_ctrl {
	bool en;
	enum ipahal_reg_dbg_cnt_type type;
	bool product;
	u8 src_pipe;
	bool rule_idx_pipe_rule;
	u16 rule_idx;
};

/* struct ipahal_reg_rsrc_grp_cfg - Mix/Max values for two rsrc groups
 * @x_min - first group min value
 * @x_max - first group max value
 * @y_min - second group min value
 * @y_max - second group max value
 */
struct ipahal_reg_rsrc_grp_cfg {
	u32 x_min;
	u32 x_max;
	u32 y_min;
	u32 y_max;
};

/* struct ipahal_reg_qsb_max_writes - IPA QSB Max Writes register
 * @qmb_0_max_writes: Max number of outstanding writes for GEN_QMB_0
 * @qmb_1_max_writes: Max number of outstanding writes for GEN_QMB_1
 */
struct ipahal_reg_qsb_max_writes {
	u32 qmb_0_max_writes;
	u32 qmb_1_max_writes;
};

/* struct ipahal_reg_qsb_max_reads - IPA QSB Max Reads register
 * @qmb_0_max_reads: Max number of outstanding reads for GEN_QMB_0
 * @qmb_1_max_reads: Max number of outstanding reads for GEN_QMB_1
 * @qmb_0_max_read_beats: Max number of outstanding read beats for GEN_QMB_0
 * @qmb_1_max_read_beats: Max number of outstanding read beats for GEN_QMB_1
 */
struct ipahal_reg_qsb_max_reads {
	u32 qmb_0_max_reads;
	u32 qmb_1_max_reads;
	u32 qmb_0_max_read_beats;
	u32 qmb_1_max_read_beats;
};

/* struct ipahal_reg_idle_indication_cfg - IPA IDLE_INDICATION_CFG register
 * @const_non_idle_enable: enable the asserting of the IDLE value and DCD
 * @enter_idle_debounce_thresh:	 configure the debounce threshold
 */
struct ipahal_reg_idle_indication_cfg {
	u16 enter_idle_debounce_thresh;
	bool const_non_idle_enable;
};

/* Get the offset of a n parameterized register */
u32 ipahal_reg_n_offset(enum ipahal_reg reg, u32 n);

/* Get the offset of a register */
static inline u32 ipahal_reg_offset(enum ipahal_reg reg)
{
	return ipahal_reg_n_offset(reg, 0);
}

/* ipahal_read_reg_n() - Get the raw value of n parameterized reg */
u32 ipahal_read_reg_n(enum ipahal_reg reg, u32 n);

/* ipahal_write_reg_n() - Write to n parameterized reg a raw value */
void ipahal_write_reg_n(enum ipahal_reg reg, u32 n, u32 val);

/* ipahal_read_reg_n_fields() - Get the parsed value of n parameterized reg */
void ipahal_read_reg_n_fields(enum ipahal_reg reg, u32 n, void *fields);

/* ipahal_write_reg_n_fields() - Write to n parameterized reg a parsed value */
void ipahal_write_reg_n_fields(enum ipahal_reg reg, u32 n, const void *fields);

/* ipahal_read_reg() - Get the raw value of a reg */
static inline u32 ipahal_read_reg(enum ipahal_reg reg)
{
	return ipahal_read_reg_n(reg, 0);
}

/* ipahal_write_reg() - Write to reg a raw value */
static inline void ipahal_write_reg(enum ipahal_reg reg, u32 val)
{
	ipahal_write_reg_n(reg, 0, val);
}

/* ipahal_read_reg_fields() - Get the parsed value of a reg */
static inline void
ipahal_read_reg_fields(enum ipahal_reg reg, void *fields)
{
	ipahal_read_reg_n_fields(reg, 0, fields);
}

/* ipahal_write_reg_fields() - Write to reg a parsed value */
static inline void ipahal_write_reg_fields(enum ipahal_reg reg,
					   const void *fields)
{
	ipahal_write_reg_n_fields(reg, 0, fields);
}

u32 ipahal_aggr_get_max_byte_limit(void);
u32 ipahal_aggr_get_max_pkt_limit(void);

#endif /* _IPAHAL_REG_H_ */
