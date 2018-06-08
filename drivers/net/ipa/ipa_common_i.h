// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#ifndef _IPA_COMMON_I_H_
#define _IPA_COMMON_I_H_

#include <linux/slab.h>

#include <dt-bindings/soc/qcom,ipa.h>

/* A field_mask is a bitmask that defines both the width and
 * position of a field within in a 32-bit register.  Set bits in a
 * field_mask define which bits are considered part of a field.  At
 * least one bit must be set, and all set bits in a field_mask must
 * be contiguous.
 *
 * The "width" of a field (1-32) can be determined by counting the
 * number of 1 bits in its field_mask.  The "shift" for a field
 * (i.e. the position of its rightmost set bit, 0-31) is the same as
 * the number of low-order 0 bits in the field_mask.  For constant
 * field_mask, these values can be computed at compile time.  (We
 * always inline this, and ensure we're using non-zero constants, to
 * ensure this happens.)
 */
#define field_width(field_mask)	       hweight32(field_mask)
#define field_shift(field_mask)	       __ffs(field_mask)

#define field_gen(val, field_mask)					\
	({								\
		BUILD_BUG_ON(!__builtin_constant_p(field_mask));	\
		BUILD_BUG_ON(!field_mask);				\
		__field_gen(val, field_mask);				\
	})

#define field_val(val, field_mask)					\
	({								\
		BUILD_BUG_ON(!__builtin_constant_p(field_mask));	\
		BUILD_BUG_ON(!field_mask);				\
		__field_val(val, field_mask);				\
	})


/* Generate a field value--the given value shifted into the field's position */
static __always_inline u32 __field_gen(u32 val, u32 field_mask)
{
	WARN_ON(val > field_mask >> field_shift(field_mask));

	return val << field_shift(field_mask) & field_mask;
}

/* Extract the value of a field from the given register */
static __always_inline u32 __field_val(u32 reg, u32 field_mask)
{
	return (reg & field_mask) >> field_shift(field_mask);
}

#define EXTRACT_UC_FEATURE(value) (value >> 5)

/** enum ipa_irq_type - IPA Interrupt Type
 * Used to register handlers for IPA interrupts
 *
 * Below enum is a logical mapping and not the actual interrupt bit in HW
 */
enum ipa_irq_type {
	IPA_BAD_SNOC_ACCESS_IRQ,
	IPA_EOT_COAL_IRQ,
	IPA_UC_IRQ_0,
	IPA_UC_IRQ_1,
	IPA_UC_IRQ_2,
	IPA_UC_IRQ_3,
	IPA_UC_IN_Q_NOT_EMPTY_IRQ,
	IPA_UC_RX_CMD_Q_NOT_FULL_IRQ,
	IPA_UC_TX_CMD_Q_NOT_FULL_IRQ,
	IPA_UC_TO_PROC_ACK_Q_NOT_FULL_IRQ,
	IPA_PROC_TO_UC_ACK_Q_NOT_EMPTY_IRQ,
	IPA_RX_ERR_IRQ,
	IPA_DEAGGR_ERR_IRQ,
	IPA_TX_ERR_IRQ,
	IPA_STEP_MODE_IRQ,
	IPA_PROC_ERR_IRQ,
	IPA_TX_SUSPEND_IRQ,
	IPA_TX_HOLB_DROP_IRQ,
	IPA_BAM_IDLE_IRQ,
	IPA_GSI_IDLE_IRQ = IPA_BAM_IDLE_IRQ,
	IPA_IRQ_MAX
};

/** enum ipa_client_type - names for the various IPA "clients"
 * these are from the perspective of the clients, for e.g.
 * HSIC1_PROD means HSIC client is the producer and IPA is the
 * consumer.
 * PROD clients are always even, and CONS clients are always odd.
 * Add new clients in the end of the list and update IPA_CLIENT_MAX
 */
enum ipa_client_type {
	IPA_CLIENT_HSIC1_PROD                   = 0,
	IPA_CLIENT_HSIC1_CONS                   = 1,

	IPA_CLIENT_HSIC2_PROD                   = 2,
	IPA_CLIENT_HSIC2_CONS                   = 3,

	IPA_CLIENT_HSIC3_PROD                   = 4,
	IPA_CLIENT_HSIC3_CONS                   = 5,

	IPA_CLIENT_HSIC4_PROD                   = 6,
	IPA_CLIENT_HSIC4_CONS                   = 7,

	IPA_CLIENT_HSIC5_PROD                   = 8,
	IPA_CLIENT_HSIC5_CONS                   = 9,

	IPA_CLIENT_WLAN1_PROD                   = 10,
	IPA_CLIENT_WLAN1_CONS                   = 11,

	IPA_CLIENT_A5_WLAN_AMPDU_PROD           = 12,
	IPA_CLIENT_WLAN2_CONS                   = 13,

	/* RESERVERD PROD                       = 14, */
	IPA_CLIENT_WLAN3_CONS                   = 15,

	/* RESERVERD PROD                       = 16, */
	IPA_CLIENT_WLAN4_CONS                   = 17,

	IPA_CLIENT_USB_PROD                     = 18,
	IPA_CLIENT_USB_CONS                     = 19,

	IPA_CLIENT_USB2_PROD                    = 20,
	IPA_CLIENT_USB2_CONS                    = 21,

	IPA_CLIENT_USB3_PROD                    = 22,
	IPA_CLIENT_USB3_CONS                    = 23,

	IPA_CLIENT_USB4_PROD                    = 24,
	IPA_CLIENT_USB4_CONS                    = 25,

	IPA_CLIENT_UC_USB_PROD                  = 26,
	IPA_CLIENT_USB_DPL_CONS                 = 27,

	IPA_CLIENT_A2_EMBEDDED_PROD		= 28,
	IPA_CLIENT_A2_EMBEDDED_CONS		= 29,

	IPA_CLIENT_A2_TETHERED_PROD             = 30,
	IPA_CLIENT_A2_TETHERED_CONS             = 31,

	IPA_CLIENT_APPS_LAN_PROD		= 32,
	IPA_CLIENT_APPS_LAN_CONS		= 33,

	IPA_CLIENT_APPS_WAN_PROD		= 34,
	IPA_CLIENT_APPS_LAN_WAN_PROD = IPA_CLIENT_APPS_WAN_PROD,
	IPA_CLIENT_APPS_WAN_CONS		= 35,

	IPA_CLIENT_APPS_CMD_PROD		= 36,
	IPA_CLIENT_A5_LAN_WAN_CONS		= 37,

	IPA_CLIENT_ODU_PROD                     = 38,
	IPA_CLIENT_ODU_EMB_CONS                 = 39,

	/* RESERVERD PROD                       = 40, */
	IPA_CLIENT_ODU_TETH_CONS                = 41,

	IPA_CLIENT_MHI_PROD                     = 42,
	IPA_CLIENT_MHI_CONS                     = 43,

	IPA_CLIENT_MEMCPY_DMA_SYNC_PROD		= 44,
	IPA_CLIENT_MEMCPY_DMA_SYNC_CONS		= 45,

	IPA_CLIENT_MEMCPY_DMA_ASYNC_PROD	= 46,
	IPA_CLIENT_MEMCPY_DMA_ASYNC_CONS	= 47,

	IPA_CLIENT_ETHERNET_PROD                = 48,
	IPA_CLIENT_ETHERNET_CONS                = 49,

	IPA_CLIENT_Q6_LAN_PROD			= 50,
	IPA_CLIENT_Q6_LAN_CONS			= 51,

	IPA_CLIENT_Q6_WAN_PROD			= 52,
	IPA_CLIENT_Q6_WAN_CONS			= 53,

	IPA_CLIENT_Q6_CMD_PROD			= 54,
	IPA_CLIENT_Q6_DUN_CONS			= 55,

	IPA_CLIENT_Q6_DECOMP_PROD		= 56,
	IPA_CLIENT_Q6_DECOMP_CONS		= 57,

	IPA_CLIENT_Q6_DECOMP2_PROD		= 58,
	IPA_CLIENT_Q6_DECOMP2_CONS		= 59,

	/* RESERVERD PROD			= 60, */
	IPA_CLIENT_Q6_LTE_WIFI_AGGR_CONS	= 61,

	IPA_CLIENT_TEST_PROD                    = 62,
	IPA_CLIENT_TEST_CONS                    = 63,

	IPA_CLIENT_TEST1_PROD                   = 64,
	IPA_CLIENT_TEST1_CONS                   = 65,

	IPA_CLIENT_TEST2_PROD                   = 66,
	IPA_CLIENT_TEST2_CONS                   = 67,

	IPA_CLIENT_TEST3_PROD                   = 68,
	IPA_CLIENT_TEST3_CONS                   = 69,

	IPA_CLIENT_TEST4_PROD                   = 70,
	IPA_CLIENT_TEST4_CONS                   = 71,

	/* RESERVERD PROD			= 72, */
	IPA_CLIENT_DUMMY_CONS			= 73,

	IPA_CLIENT_MAX,
};

#define IPA_CLIENT_IS_PROD(x)	(!((x) & 1))
#define IPA_CLIENT_IS_CONS(x)	(!IPA_CLIENT_IS_PROD(x))

static inline bool IPA_CLIENT_IS_Q6_CONS(enum ipa_client_type client)
{
	return client == IPA_CLIENT_Q6_LAN_CONS ||
		client == IPA_CLIENT_Q6_WAN_CONS ||
		client == IPA_CLIENT_Q6_DUN_CONS ||
		client == IPA_CLIENT_Q6_DECOMP_CONS ||
		client == IPA_CLIENT_Q6_DECOMP2_CONS ||
		client == IPA_CLIENT_Q6_LTE_WIFI_AGGR_CONS;
}

static inline bool IPA_CLIENT_IS_Q6_PROD(enum ipa_client_type client)
{
	return client == IPA_CLIENT_Q6_LAN_PROD ||
		client == IPA_CLIENT_Q6_WAN_PROD ||
		client == IPA_CLIENT_Q6_CMD_PROD ||
		client == IPA_CLIENT_Q6_DECOMP_PROD ||
		client == IPA_CLIENT_Q6_DECOMP2_PROD;
}

static inline bool IPA_CLIENT_IS_APPS_CONS(enum ipa_client_type client)
{
	return client == IPA_CLIENT_APPS_LAN_CONS ||
		client == IPA_CLIENT_APPS_WAN_CONS;
}

struct ipa_active_client_logging_info {
	const char *id_string;
	const char *file;
	int line;
};

/** struct ipa_mem_buffer - IPA memory buffer
 * @base: base
 * @phys_base: physical base address
 * @size: size of memory buffer
 */
struct ipa_mem_buffer {
	void *base;
	dma_addr_t phys_base;
	u32 size;
};

/** max size of the header to be inserted */
#define IPA_HDR_MAX_SIZE 64

/** enum ipa_cs_offload - checksum offload setting */
enum ipa_cs_offload {
	IPA_DISABLE_CS_OFFLOAD		= 0,
	IPA_ENABLE_CS_OFFLOAD_UL	= 1,
	IPA_ENABLE_CS_OFFLOAD_DL	= 2,
	IPA_CS_RSVD
};

/** enum ipa_dp_evt_type - type of event client callback is
 * invoked for on data path
 * @IPA_RECEIVE: data is struct sk_buff
 * @IPA_WRITE_DONE: data is struct sk_buff
 */
enum ipa_dp_evt_type {
	IPA_RECEIVE,
	IPA_WRITE_DONE,
	IPA_CLIENT_START_POLL,
	IPA_CLIENT_COMP_NAPI,
};

/** enum ipa_hw_version - IPA hardware version
 * @IPA_HW_v3_5_1: IPA hardware version 3.5.1
 * @IPA_HW_None: IPA hardware version not defined
 *
 * Device Tree uses symbols like QCOM_IPA_HW_VER_v3_5_1 to represent
 * the IPA hardware version.  We use the values below internally,
 * with ipa_version_get() translating between the two.  Note that
 * order is significant for ipa_hw_type values--higher numeric
 * values imply newer hardware versions.
 */
enum ipa_hw_version {
	IPA_HW_v3_5_1	= 0,
	IPA_HW_NONE	= 0xffffffff,
};

/** enum ipa_mode_type - mode setting type in IPA end-point
 * @BASIC: basic mode
 * @ENABLE_FRAMING_HDLC: not currently supported
 * @ENABLE_DEFRAMING_HDLC: not currently supported
 * @DMA: all data arriving IPA will not go through IPA logic blocks, this
 *  allows IPA to work as DMA for specific pipes.
 */
enum ipa_mode_type {
	IPA_BASIC,
	IPA_ENABLE_FRAMING_HDLC,
	IPA_ENABLE_DEFRAMING_HDLC,
	IPA_DMA,
};

/** enum ipa_nat_en_type - NAT setting type in IPA end-point */
enum ipa_nat_en_type {
	IPA_BYPASS_NAT,
	IPA_SRC_NAT,
	IPA_DST_NAT,
};

/** enum ipa_ipv6ct_en_type - IPv6CT setting type in IPA end-point */
enum ipa_ipv6ct_en_type {
	IPA_BYPASS_IPV6CT,
	IPA_ENABLE_IPV6CT,
};

/** enum ipa_aggr_en_type - aggregation setting type in IPA end-point */
enum ipa_aggr_en_type {
	IPA_BYPASS_AGGR,
	IPA_ENABLE_AGGR,
	IPA_ENABLE_DEAGGR,
};

/** enum ipa_aggr_type - type of aggregation in IPA end-point */
enum ipa_aggr_type {
	IPA_MBIM_16 = 0,
	IPA_HDLC    = 1,
	IPA_TLP	    = 2,
	IPA_RNDIS   = 3,
	IPA_GENERIC = 4,
	IPA_QCMAP   = 6,
};

/** enum hdr_total_len_or_pad_type - type of value held by TOTAL_LEN_OR_PAD
 * field in header configuration register.
 * @IPA_HDR_PAD: field is used as padding length
 * @IPA_HDR_TOTAL_LEN: field is used as total length
 */
enum hdr_total_len_or_pad_type {
	IPA_HDR_PAD = 0,
	IPA_HDR_TOTAL_LEN = 1,
};

/** max size of the name of the resource (routing table, header) */
#define IPA_RESOURCE_NAME_MAX 32

/** enum ipa_ip_type - Address family: IPv4 or IPv6 */
enum ipa_ip_type {
	IPA_IP_v4,
	IPA_IP_v6,
	IPA_IP_MAX
};

/** struct ipa_ep_cfg_nat - NAT configuration in IPA end-point
 * @nat_en:	This defines the default NAT mode for the pipe: in case of
 *		filter miss - the default NAT mode defines the NATing operation
 *		on the packet. Valid for Input Pipes only (IPA consumer)
 */
struct ipa_ep_cfg_nat {
	enum ipa_nat_en_type nat_en;
};

/** struct ipa_ep_cfg_conn_track - IPv6 Connection tracking configuration in
 *	IPA end-point
 * @conn_track_en: Defines speculative conn_track action, means if specific
 *		   pipe needs to have UL/DL IPv6 Connection Tracking or Bybass
 *		   IPv6 Connection Tracking. 0: Bypass IPv6 Connection Tracking
 *					     1: IPv6 UL/DL Connection Tracking.
 *		  Valid for Input Pipes only (IPA consumer)
 */
struct ipa_ep_cfg_conn_track {
	enum ipa_ipv6ct_en_type conn_track_en;
};

/** struct ipa_ep_cfg_hdr - header configuration in IPA end-point
 *
 * @hdr_len:Header length in bytes to be added/removed. Assuming
 *			header len is constant per endpoint. Valid for
 *			both Input and Output Pipes
 * @hdr_ofst_metadata_valid:	0: Metadata_Ofst  value is invalid, i.e., no
 *			metadata within header.
 *			1: Metadata_Ofst  value is valid, i.e., metadata
 *			within header is in offset Metadata_Ofst Valid
 *			for Input Pipes only (IPA Consumer) (for output
 *			pipes, metadata already set within the header)
 * @hdr_ofst_metadata:	Offset within header in which metadata resides
 *			Size of metadata - 4bytes
 *			Example -  Stream ID/SSID/mux ID.
 *			Valid for  Input Pipes only (IPA Consumer) (for output
 *			pipes, metadata already set within the header)
 * @hdr_additional_const_len:	Defines the constant length that should be added
 *			to the payload length in order for IPA to update
 *			correctly the length field within the header
 *			(valid only in case Hdr_Ofst_Pkt_Size_Valid=1)
 *			Valid for Output Pipes (IPA Producer)
 * @hdr_ofst_pkt_size_valid:	0: Hdr_Ofst_Pkt_Size  value is invalid, i.e., no
 *			length field within the inserted header
 *			1: Hdr_Ofst_Pkt_Size  value is valid, i.e., a
 *			packet length field resides within the header
 *			Valid for Output Pipes (IPA Producer)
 * @hdr_ofst_pkt_size:	Offset within header in which packet size reside. Upon
 *			Header Insertion, IPA will update this field within the
 *			header with the packet length . Assumption is that
 *			header length field size is constant and is 2Bytes
 *			Valid for Output Pipes (IPA Producer)
 * @hdr_a5_mux: Determines whether A5 Mux header should be added to the packet.
 *			This bit is valid only when Hdr_En=01(Header Insertion)
 *			SW should set this bit for IPA-to-A5 pipes.
 *			0: Do not insert A5 Mux Header
 *			1: Insert A5 Mux Header
 *			Valid for Output Pipes (IPA Producer)
 * @hdr_remove_additional:	bool switch, remove more of the header
 *			based on the aggregation configuration (register
 *			HDR_LEN_INC_DEAGG_HDR)
 * @hdr_metadata_reg_valid:	bool switch, metadata from
 *			register INIT_HDR_METADATA_n is valid.
 *			(relevant only for IPA Consumer pipes)
 */
struct ipa_ep_cfg_hdr {
	u32  hdr_len;
	u32  hdr_ofst_metadata_valid;
	u32  hdr_ofst_metadata;
	u32  hdr_additional_const_len;
	u32  hdr_ofst_pkt_size_valid;
	u32  hdr_ofst_pkt_size;
	u32  hdr_a5_mux;
	u32  hdr_remove_additional;
	u32  hdr_metadata_reg_valid;
};

/** struct ipa_ep_cfg_hdr_ext - extended header configuration in IPA end-point
 * @hdr_pad_to_alignment: Pad packet to specified alignment
 *	(2^pad to alignment value), i.e. value of 3 means pad to 2^3 = 8 bytes
 *	alignment. Alignment is to 0,2 up to 32 bytes (IPAv2 does not support 64
 *	byte alignment). Valid for Output Pipes only (IPA Producer).
 * @hdr_total_len_or_pad_offset: Offset to length field containing either
 *	total length or pad length, per hdr_total_len_or_pad config
 * @hdr_payload_len_inc_padding: 0-IPA_ENDP_INIT_HDR_n's
 *	HDR_OFST_PKT_SIZE does
 *	not includes padding bytes size, payload_len = packet length,
 *	1-IPA_ENDP_INIT_HDR_n's HDR_OFST_PKT_SIZE includes
 *	padding bytes size, payload_len = packet length + padding
 * @hdr_total_len_or_pad: field is used as PAD length ot as Total length
 *	(header + packet + padding)
 * @hdr_total_len_or_pad_valid: 0-Ignore TOTAL_LEN_OR_PAD field, 1-Process
 *	TOTAL_LEN_OR_PAD field
 * @hdr_little_endian: 0-Big Endian, 1-Little Endian
 */
struct ipa_ep_cfg_hdr_ext {
	u32 hdr_pad_to_alignment;
	u32 hdr_total_len_or_pad_offset;
	bool hdr_payload_len_inc_padding;
	enum hdr_total_len_or_pad_type hdr_total_len_or_pad;
	bool hdr_total_len_or_pad_valid;
	bool hdr_little_endian;
};

/** struct ipa_ep_cfg_mode - mode configuration in IPA end-point
 * @mode:	Valid for Input Pipes only (IPA Consumer)
 * @dst:	This parameter specifies the output pipe to which the packets
 *		will be routed to.
 *		This parameter is valid for Mode=DMA and not valid for
 *		Mode=Basic
 *		Valid for Input Pipes only (IPA Consumer)
 */
struct ipa_ep_cfg_mode {
	enum ipa_mode_type mode;
	enum ipa_client_type dst;
};

/** struct ipa_ep_cfg_aggr - aggregation configuration in IPA end-point
 *
 * @aggr_en:	Valid for both Input and Output Pipes
 * @aggr:	aggregation type (Valid for both Input and Output Pipes)
 * @aggr_byte_limit:	Limit of aggregated packet size in KB (<=32KB) When set
 *			to 0, there is no size limitation on the aggregation.
 *			When both, Aggr_Byte_Limit and Aggr_Time_Limit are set
 *			to 0, there is no aggregation, every packet is sent
 *			independently according to the aggregation structure
 *			Valid for Output Pipes only (IPA Producer )
 * @aggr_time_limit:	Timer to close aggregated packet (<=32ms) When set to 0,
 *			there is no time limitation on the aggregation.  When
 *			both, Aggr_Byte_Limit and Aggr_Time_Limit are set to 0,
 *			there is no aggregation, every packet is sent
 *			independently according to the aggregation structure
 *			Valid for Output Pipes only (IPA Producer)
 * @aggr_pkt_limit: Defines if EOF close aggregation or not. if set to false
 *			HW closes aggregation (sends EOT) only based on its
 *			aggregation config (byte/time limit, etc). if set to
 *			true EOF closes aggregation in addition to HW based
 *			aggregation closure. Valid for Output Pipes only (IPA
 *			Producer). EOF affects only Pipes configured for
 *			generic aggregation.
 * @aggr_hard_byte_limit_en: If set to 1, byte-limit aggregation for this
 *			pipe will apply a hard-limit behavior which will not
 *			allow frames to be closed with more than byte-limit
 *			bytes. If set to 0, previous byte-limit behavior
 *			will apply - frames close once a packet causes the
 *			accumulated byte-count to cross the byte-limit
 *			threshold (closed frame will contain that packet).
 * @aggr_sw_eof_active: 0: EOF does not close aggregation. HW closes aggregation
 *			(sends EOT) only based on its aggregation config
 *			(byte/time limit, etc).
 *			1: EOF closes aggregation in addition to HW based
 *			aggregation closure. Valid for Output Pipes only (IPA
 *			Producer). EOF affects only Pipes configured for generic
 *			aggregation.
 */
struct ipa_ep_cfg_aggr {
	enum ipa_aggr_en_type aggr_en;
	enum ipa_aggr_type aggr;
	u32 aggr_byte_limit;
	u32 aggr_time_limit;
	u32 aggr_pkt_limit;
	u32 aggr_hard_byte_limit_en;
	bool aggr_sw_eof_active;
};

/** struct ipa_ep_cfg_route - route configuration in IPA end-point
 * @rt_tbl_hdl: Defines the default routing table index to be used in case there
 *		is no filter rule matching, valid for Input Pipes only (IPA
 *		Consumer). Clients should set this to 0 which will cause default
 *		v4 and v6 routes setup internally by IPA driver to be used for
 *		this end-point
 */
struct ipa_ep_cfg_route {
	u32 rt_tbl_hdl;
};

/** struct ipa_ep_cfg_holb - head of line blocking config in IPA end-point
 * @en: enable(1 => ok to drop pkt)/disable(0 => never drop pkt)
 * @tmr_val: duration in units of 128 IPA clk clock cyles [0,511], 1 clk=1.28us
 *	     IPAv2.5 support 32 bit HOLB timeout value, previous versions
 *	     supports 16 bit
 */
struct ipa_ep_cfg_holb {
	u16 en;
	u32 tmr_val;
};

/** struct ipa_ep_cfg_deaggr - deaggregation configuration in IPA end-point
 * @deaggr_hdr_len: Deaggregation Header length in bytes. Valid only for Input
 *	Pipes, which are configured for 'Generic' deaggregation.
 * @packet_offset_valid: - 0: PACKET_OFFSET is not used, 1: PACKET_OFFSET is
 *	used.
 * @packet_offset_location: Location of packet offset field, which specifies
 *	the offset to the packet from the start of the packet offset field.
 * @max_packet_len: DEAGGR Max Packet Length in Bytes. A Packet with higher
 *	size wil be treated as an error. 0 - Packet Length is not Bound,
 *	IPA should not check for a Max Packet Length.
 */
struct ipa_ep_cfg_deaggr {
	u32 deaggr_hdr_len;
	bool packet_offset_valid;
	u32 packet_offset_location;
	u32 max_packet_len;
};

/** struct ipa_ep_cfg_cfg - IPA ENDP_INIT Configuration register
 * @frag_offload_en: - 0 - IP packet fragment handling is disabled. IP packet
 *	fragments should be sent to SW. SW is responsible for
 *	configuring filter rules, and IP packet filter exception should be
 *	used to send all fragments to SW. 1 - IP packet fragment
 *	handling is enabled. IPA checks for fragments and uses frag
 *	rules table for processing fragments. Valid only for Input Pipes
 *	(IPA Consumer)
 * @cs_offload_en: Checksum offload enable: 00: Disable checksum offload, 01:
 *	Enable checksum calculation offload (UL) - For output pipe
 *	(IPA producer) specifies that checksum trailer is to be added.
 *	For input pipe (IPA consumer) specifies presence of checksum
 *	header and IPA checksum calculation accordingly. 10: Enable
 *	checksum calculation offload (DL) - For output pipe (IPA
 *	producer) specifies that checksum trailer is to be added. For
 *	input pipe (IPA consumer) specifies IPA checksum calculation.
 *	11: Reserved
 * @cs_metadata_hdr_offset: Offset in Words (4 bytes) within header in which
 *	checksum meta info header (4 bytes) starts (UL). Values are 0-15, which
 *	mean 0 - 60 byte checksum header offset. Valid for input
 *	pipes only (IPA consumer)
 * @gen_qmb_master_sel: Select bit for ENDP GEN-QMB master. This is used to
 *	separate DDR & PCIe transactions in-order to limit them as
 *	a group (using MAX_WRITES/READS limiation). Valid for input and
 *	output pipes (IPA consumer+producer)
 */
struct ipa_ep_cfg_cfg {
	bool frag_offload_en;
	enum ipa_cs_offload cs_offload_en;
	u8 cs_metadata_hdr_offset;
	u8 gen_qmb_master_sel;
};

/** struct ipa_ep_cfg_metadata_mask - Endpoint initialization hdr metadata mask
 * @metadata_mask: Mask specifying which metadata bits to write to
 *	IPA_ENDP_INIT_HDR_n.s HDR_OFST_METADATA. Only
 *	masked metadata bits (set to 1) will be written. Valid for Output
 *	Pipes only (IPA Producer)
 */
struct ipa_ep_cfg_metadata_mask {
	u32 metadata_mask;
};

/** struct ipa_ep_cfg_seq - HPS/DPS sequencer type config in IPA end-point
 * @set_dynamic:  0 - HPS/DPS seq type is configured statically,
 *		   1 - HPS/DPS seq type is set to seq_type
 * @seq_type: HPS/DPS sequencer type configuration
 */
struct ipa_ep_cfg_seq {
	bool set_dynamic;
	int seq_type;
};

/** struct ipa_ep_cfg - configuration of IPA end-point
 * @nat:		NAT parmeters
 * @conn_track:		IPv6CT parmeters
 * @hdr:		Header parameters
 * @hdr_ext:		Extended header parameters
 * @mode:		Mode parameters
 * @aggr:		Aggregation parameters
 * @deaggr:		Deaggregation params
 * @route:		Routing parameters
 * @cfg:		Configuration register data
 * @metadata_mask:	Hdr metadata mask
 * @meta:		Meta Data
 * @seq:		HPS/DPS sequencers configuration
 */
struct ipa_ep_cfg {
	struct ipa_ep_cfg_nat nat;
	struct ipa_ep_cfg_conn_track conn_track;
	struct ipa_ep_cfg_hdr hdr;
	struct ipa_ep_cfg_hdr_ext hdr_ext;
	struct ipa_ep_cfg_mode mode;
	struct ipa_ep_cfg_aggr aggr;
	struct ipa_ep_cfg_deaggr deaggr;
	struct ipa_ep_cfg_route route;
	struct ipa_ep_cfg_cfg cfg;
	struct ipa_ep_cfg_metadata_mask metadata_mask;
	struct ipa_ep_cfg_seq seq;
};

/** struct ipa_ep_cfg_ctrl - Control configuration in IPA end-point
 * @ipa_ep_suspend: 0 - ENDP is enabled, 1 - ENDP is suspended (disabled).
 *			Valid for PROD Endpoints
 * @ipa_ep_delay:   0 - ENDP is free-running, 1 - ENDP is delayed.
 *			SW controls the data flow of an endpoint usind this bit.
 *			Valid for CONS Endpoints
 */
struct ipa_ep_cfg_ctrl {
	bool ipa_ep_suspend;
	bool ipa_ep_delay;
};

#define ipa_debug(fmt, args...) \
		pr_debug(fmt, ## args)

#define ipa_debug_low(fmt, args...) \
		pr_debug(fmt, ## args)

#define ipa_err(fmt, args...) \
		pr_err(fmt, ## args)

#define ipa_info(fmt, args...) \
		pr_info(fmt, ## args)

#define ipa_bug() \
	do {								\
		ipa_err("an unrecoverable error has occurred\n");	\
		BUG();							\
	} while (0)

#define ipa_bug_on(condition)						\
	do {								\
		if (unlikely(condition)) {				\
			ipa_err("ipa_bug_on(%s) failed!\n", #condition); \
			ipa_bug();					\
		}							\
	} while (0)

#ifdef CONFIG_IPA_ASSERT

/* Communicate a condition assumed by the code.  This is intended as
 * an informative statement about something that should always be true.
 *
 * N.B.:  Conditions asserted must not incorporate code with side-effects
 *	  that are necessary for correct execution.  And an assertion
 *	  failure should not be expected to force a crash (because all
 *	  assertion code is optionally compiled out).
 */
#define ipa_assert(cond) \
	do {								\
		if (unlikely(!(cond))) {				\
			ipa_err("ipa_assert(%s) failed!\n", #cond);	\
			ipa_bug();					\
		}							\
	} while (0)
#else	/* !CONFIG_IPA_ASSERT */

#define ipa_assert(expr)	((void)0)

#endif	/* !CONFIG_IPA_ASSERT */

void ipa_write_64(u64 w, u8 *dest);

const char *ipa_client_string(enum ipa_client_type client);

#endif /* _IPA_COMMON_I_H_ */
