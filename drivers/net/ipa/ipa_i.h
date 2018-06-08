// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#ifndef _IPA_I_H_
#define _IPA_I_H_

#include <linux/bitops.h>
#include <linux/cdev.h>
#include <linux/export.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>

#include "ipa_common_i.h"
#include "ipahal_reg.h"
#include "ipahal.h"
#include "gsi.h"

#define DRV_NAME "ipa"
#define IPA_COOKIE 0x57831603

/* Offset past base of IPA "wrapper" space for register access */
#define IPA_REG_BASE_OFFSET	0x00040000

#define IPA_MAX_NUM_PIPES 31
#define IPA_SYS_DESC_FIFO_SZ 0x800
#define IPA_SYS_TX_DATA_DESC_FIFO_SZ 0x1000
#define IPA_LAN_RX_HEADER_LENGTH (2)
#define IPA_QMAP_HEADER_LENGTH (4)
#define IPA_DL_CHECKSUM_LENGTH (8)
#define IPA_GENERIC_RX_POOL_SZ 192
/* The transport descriptor size was changed to GSI_CHAN_RE_SIZE_16B, but
 * IPA users still use sps_iovec size as FIFO element size.
 */
#define IPA_FIFO_ELEMENT_SIZE 8

#define IPA_MAX_STATUS_STAT_NUM 30

#define IPA_IPC_LOG_PAGES 100

#define IPA_MEM_CANARY_VAL 0xdeadbeef

#define IPA_STATS

#ifdef IPA_STATS
#define IPA_STATS_INC_CNT(val) (++val)
#define IPA_STATS_DEC_CNT(val) (--val)
#define IPA_STATS_EXCP_CNT(__excp, __base) do {				\
		enum ipahal_pkt_status_exception _e = (__excp);		\
		if (_e < 0 || _e >= IPAHAL_PKT_STATUS_EXCEPTION_MAX)	\
			break;						\
		++__base[_e];						\
	} while (0)
#else
#define IPA_STATS_INC_CNT(x) do { } while (0)
#define IPA_STATS_DEC_CNT(x)
#define IPA_STATS_EXCP_CNT(__excp, __base) do { } while (0)
#endif

#define IPA_HDR_BIN0 0
#define IPA_HDR_BIN1 1
#define IPA_HDR_BIN2 2
#define IPA_HDR_BIN3 3
#define IPA_HDR_BIN4 4
#define IPA_HDR_BIN_MAX 5

#define IPA_HDR_PROC_CTX_BIN0 0
#define IPA_HDR_PROC_CTX_BIN1 1
#define IPA_HDR_PROC_CTX_BIN_MAX 2

#define IPA_RX_POOL_CEIL 32
#define IPA_RX_SKB_SIZE 1792

#define IPA_A5_MUX_HDR_NAME "ipa_excp_hdr"
#define IPA_LAN_RX_HDR_NAME "ipa_lan_hdr"
#define IPA_INVALID_L4_PROTOCOL 0xFF

#define IPA_HDR_PROC_CTX_TABLE_ALIGNMENT_BYTE 8
#define IPA_HDR_PROC_CTX_TABLE_ALIGNMENT(start_ofst) \
	(((start_ofst) + IPA_HDR_PROC_CTX_TABLE_ALIGNMENT_BYTE - 1) & \
	~(IPA_HDR_PROC_CTX_TABLE_ALIGNMENT_BYTE - 1))

#define IPA_GSI_CHANNEL_STOP_MAX_RETRY 10
#define IPA_GSI_CHANNEL_STOP_PKT_SIZE 1

#define IPA_GSI_CHANNEL_EMPTY_MAX_RETRY 15

#define IPA_ACTIVE_CLIENTS_LOG_BUFFER_SIZE_LINES 120
#define IPA_ACTIVE_CLIENTS_LOG_LINE_LEN 96
#define FEATURE_ENUM_VAL(feature, opcode) ((feature << 5) | opcode)
#define IPA_HW_NUM_FEATURES 0x8
#define IPA_WAN_MSG_IPV6_ADDR_GW_LEN 4

enum ipa_ees {
	IPA_EE_AP = 0,
	IPA_EE_Q6 = 1,
	IPA_EE_UC = 2,
};

/** struct ipa_tx_suspend_irq_data - interrupt data for IPA_TX_SUSPEND_IRQ
 * @endpoints: bitmask of endpoints which case IPA_TX_SUSPEND_IRQ interrupt
 * @dma_addr: DMA address of this Rx packet
 */
struct ipa_tx_suspend_irq_data {
	u32 endpoints;
};

typedef void (*ipa_notify_cb)(void *priv, enum ipa_dp_evt_type evt,
		       unsigned long data);

/** typedef ipa_irq_handler_t - irq handler/callback type
 * @param ipa_irq_type - [in] interrupt type
 * @param private_data - [in, out] the client private data
 * @param interrupt_data - [out] interrupt information data
 *
 * callback registered by ipa_add_interrupt_handler function to
 * handle a specific interrupt type
 *
 * No return value
 */
typedef void (*ipa_irq_handler_t)(enum ipa_irq_type interrupt,
				void *private_data,
				void *interrupt_data);

/** struct ipa_sys_connect_params - information needed to setup an IPA end-point
 * in system-BAM mode
 * @ipa_ep_cfg: IPA EP configuration
 * @client:	the type of client who "owns" the EP
 * @desc_fifo_sz: size of desc FIFO. This number is used to allocate the desc
 *		fifo for BAM. For GSI, this size is used by IPA driver as a
 *		baseline to calculate the GSI ring size in the following way:
 *		For PROD pipes, GSI ring is 4 * desc_fifo_sz.
		For PROD pipes, GSI ring is 2 * desc_fifo_sz.
 * @priv:	callback cookie
 * @notify:	callback
 *		priv - callback cookie
 *		evt - type of event
 *		data - data relevant to event.  May not be valid. See event_type
 *		enum for valid cases.
 * @napi_enabled: when true, IPA call client callback to start polling
 */
struct ipa_sys_connect_params {
	struct ipa_ep_cfg ipa_ep_cfg;
	enum ipa_client_type client;
	u32 desc_fifo_sz;
	void *priv;
	ipa_notify_cb notify;
	bool napi_enabled;
};

struct ipa_active_client {
	struct list_head links;
	int count;
	const char *id_string;
};

struct ipa_active_clients_log_ctx {
	spinlock_t lock;	/* XXX comment this */
	char *log_buffer[IPA_ACTIVE_CLIENTS_LOG_BUFFER_SIZE_LINES];
	int log_head;
	int log_tail;
	bool log_rdy;
	struct list_head active;
};

struct ipa_status_stats {
	struct ipahal_pkt_status status[IPA_MAX_STATUS_STAT_NUM];
	unsigned int curr;
};

/** struct ipa_ep_context - IPA end point context
 * @valid: flag indicating id EP context is valid
 * @client: EP client type
 * @gsi_chan_hdl: EP's GSI channel handle
 * @gsi_evt_ring_hdl: EP's GSI channel event ring handle
 * @chan_scratch: EP's GSI channel scratch info
 * @cfg: EP cionfiguration
 * @dst_pipe_index: destination pipe index
 * @rt_tbl_idx: routing table index
 * @priv: user provided information which will forwarded once the user is
 *	  notified for new data avail
 * @client_notify: user provided CB for EP events notification, the event is
 *		   data revived.
 * @disconnect_in_progress: Indicates client disconnect in progress.
 * @qmi_request_sent: Indicates whether QMI request to enable clear data path
 *					request is sent or not.
 * @napi_enabled: when true, IPA call client callback to start polling
 */
struct ipa_ep_context {
	int valid;
	enum ipa_client_type client;
	unsigned long gsi_chan_hdl;
	unsigned long gsi_evt_ring_hdl;
	bool bytes_xfered_valid;
	u16 bytes_xfered;
	struct ipa_ep_cfg cfg;
	struct ipa_ep_cfg_holb holb;
	struct ipahal_reg_ep_cfg_status status;
	u32 dst_pipe_index;
	u32 rt_tbl_idx;
	void *priv;
	void (*client_notify)(void *priv, enum ipa_dp_evt_type evt,
			      unsigned long data);
	atomic_t avail_fifo_desc;
	u32 dflt_flt4_rule_hdl;
	u32 dflt_flt6_rule_hdl;
	u32 uc_offload_state;
	bool disconnect_in_progress;
	u32 qmi_request_sent;
	bool napi_enabled;
	u32 eot_in_poll_err;

	/* sys MUST be the last element of this struct */
	struct ipa_sys_context *sys;
};

#define IPA_HW_NUM_FEATURES 0x8
#define FEATURE_ENUM_VAL(feature, opcode) ((feature << 5) | opcode)

/** enum ipa_hw_features - Values that represent the features supported
 * in IPA HW
 * @IPA_HW_FEATURE_COMMON : Feature related to common operation of IPA HW
 *
 */
enum ipa_hw_features {
	IPA_HW_FEATURE_COMMON		=	0x0,
	IPA_HW_FEATURE_MAX		=	IPA_HW_NUM_FEATURES
};

/** enum ipa_hw_2_cpu_events - Values that represent HW event to be sent to CPU.
 * @IPA_HW_2_CPU_EVENT_NO_OP : No event present
 * @IPA_HW_2_CPU_EVENT_ERROR : Event specify a system error is detected by the
 *  device
 * @IPA_HW_2_CPU_EVENT_LOG_INFO : Event providing logging specific information
 */
enum ipa_hw_2_cpu_events {
	IPA_HW_2_CPU_EVENT_NO_OP     =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 0),
	IPA_HW_2_CPU_EVENT_ERROR     =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 1),
	IPA_HW_2_CPU_EVENT_LOG_INFO  =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 2),
};

/** enum ipa_hw_errors - Common error types.
 * @IPA_HW_ERROR_NONE : No error persists
 * @IPA_HW_INVALID_DOORBELL_ERROR : Invalid data read from doorbell
 * @IPA_HW_DMA_ERROR : Unexpected DMA error
 * @IPA_HW_FATAL_SYSTEM_ERROR : HW has crashed and requires reset.
 * @IPA_HW_INVALID_OPCODE : Invalid opcode sent
 * @IPA_HW_INVALID_PARAMS : Invalid params for the requested command
 * @IPA_HW_GSI_CH_NOT_EMPTY_FAILURE : GSI channel emptiness validation failed
 */
enum ipa_hw_errors {
	IPA_HW_ERROR_NONE	       =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 0),
	IPA_HW_INVALID_DOORBELL_ERROR  =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 1),
	IPA_HW_DMA_ERROR	       =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 2),
	IPA_HW_FATAL_SYSTEM_ERROR      =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 3),
	IPA_HW_INVALID_OPCODE	       =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 4),
	IPA_HW_INVALID_PARAMS	     =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 5),
	IPA_HW_CONS_DISABLE_CMD_GSI_STOP_FAILURE =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 6),
	IPA_HW_PROD_DISABLE_CMD_GSI_STOP_FAILURE =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 7),
	IPA_HW_GSI_CH_NOT_EMPTY_FAILURE =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_COMMON, 8)
};

struct ipa_repl_ctx {
	struct ipa_rx_pkt_wrapper **cache;
	atomic_t head_idx;
	atomic_t tail_idx;
	u32 capacity;
};

/** struct ipa_sys_context - IPA GPI pipes context
 * @head_desc_list: header descriptors list
 * @len: the size of the above list
 * @spinlock: protects the list and its size
 * @ep: IPA EP context
 *
 * IPA context specific to the GPI pipes a.k.a LAN IN/OUT and WAN
 */
struct ipa_sys_context {
	u32 len;
	u32 len_pending_xfer;
	atomic_t curr_polling_state;
	struct delayed_work switch_to_intr_work;
	int (*pyld_hdlr)(struct sk_buff *skb, struct ipa_sys_context *sys);
	struct sk_buff * (*get_skb)(unsigned int len, gfp_t flags);
	void (*free_skb)(struct sk_buff *skb);
	void (*free_rx_wrapper)(struct ipa_rx_pkt_wrapper *rk_pkt);
	u32 rx_buff_sz;
	u32 rx_pool_sz;
	struct sk_buff *prev_skb;
	unsigned int len_rem;
	unsigned int len_pad;
	unsigned int len_partial;
	bool drop_packet;

	bool no_intr;			/* Transmit requests won't interrupt */
	atomic_t nop_pending;		/* Should a nop be scheduled? */
	struct hrtimer nop_timer;	/* For no-intr PROD pipes only */
	struct work_struct work;

	struct delayed_work replenish_rx_work;
	struct work_struct repl_work;
	void (*repl_hdlr)(struct ipa_sys_context *sys);
	struct ipa_repl_ctx repl;

	/* ordering is important - mutable fields go above */
	struct ipa_ep_context *ep;
	struct list_head head_desc_list;
	struct list_head rcycl_list;
	spinlock_t spinlock;	/* XXX comment this */
	struct workqueue_struct *wq;
	struct workqueue_struct *repl_wq;
	struct ipa_status_stats *status_stat;
	/* ordering is important - other immutable fields go below */
};

/** enum ipa_desc_type - IPA decriptors type
 *
 * IPA decriptors type, IPA supports DD and ICD but no CD
 */
enum ipa_desc_type {
	IPA_DATA_DESC,
	IPA_DATA_DESC_SKB,
	IPA_DATA_DESC_SKB_PAGED,
	IPA_IMM_CMD_DESC,
};

/** struct ipa_tx_pkt_wrapper - IPA Tx packet wrapper
 * @type: specify if this packet is for the skb or immediate command
 * @mem: memory buffer used by this Tx packet
 * @work: work struct for current Tx packet
 * @link: linked to the wrappers on that pipe
 * @callback: IPA client provided callback
 * @user1: cookie1 for above callback
 * @user2: cookie2 for above callback
 * @sys: corresponding IPA sys context
 * @cnt: 1 for single transfers,
 * >1 and <0xFFFF for first of a "multiple" transfer,
 * 0xFFFF for last desc, 0 for rest of "multiple' transfer
 *
 * This struct can wrap both data packet and immediate command packet.
 */
struct ipa_tx_pkt_wrapper {
	enum ipa_desc_type type;
	struct ipa_mem_buffer mem;
	struct work_struct work;
	struct list_head link;
	void (*callback)(void *user1, int user2);
	void *user1;
	int user2;
	struct ipa_sys_context *sys;
	u32 cnt;
};

/** struct ipa_desc - IPA descriptor
 * @type: skb or immediate command or plain old data
 * @pyld: points to skb
 * @frag: points to paged fragment
 * or kmalloc'ed immediate command parameters/plain old data
 * @dma_address: dma mapped address of pyld
 * @dma_address_valid: valid field for dma_address
 * @is_tag_status: flag for IP_PACKET_TAG_STATUS imd cmd
 * @len: length of the pyld
 * @opcode: for immediate commands
 * @callback: IPA client provided completion callback
 * @user1: cookie1 for above callback
 * @user2: cookie2 for above callback
 * @xfer_done: completion object for sync completion
 * @skip_db_ring: specifies whether GSI doorbell should not be rang
 */
struct ipa_desc {
	enum ipa_desc_type type;
	void *pyld;
	skb_frag_t *frag;
	u16 len;
	u16 opcode;
	void (*callback)(void *user1, int user2);
	void *user1;
	int user2;
	struct completion xfer_done;
};

/* Helper function to fill in some IPA descriptor fields for an
 * immediate command using an immediate command payload returned by
 * ipahal_construct_imm_cmd().
 */
static inline void
ipa_desc_fill_imm_cmd(struct ipa_desc *desc, struct ipahal_imm_cmd_pyld *pyld)
{
	desc->type = IPA_IMM_CMD_DESC;
	desc->pyld = ipahal_imm_cmd_pyld_data(pyld);
	desc->len = pyld->len;
	desc->opcode = pyld->opcode;
}

/** struct  ipa_rx_data - information needed
 * to send to wlan driver on receiving data from ipa hw
 * @skb: skb
 * @dma_addr: DMA address of this Rx packet
 */
struct ipa_rx_data {
	struct sk_buff *skb;
	dma_addr_t dma_addr;
};

/** struct ipa_rx_pkt_wrapper - IPA Rx packet wrapper
 * @link: linked to the Rx packets on that pipe
 * @data: skb and DMA address of the received packet
 * @len: how many bytes are copied into skb's flat buffer
 */
struct ipa_rx_pkt_wrapper {
	struct list_head link;
	struct ipa_rx_data data;
	u32 len;
	struct ipa_sys_context *sys;
};

struct ipa_stats {
	u32 tx_sw_pkts;
	u32 tx_hw_pkts;
	u32 rx_pkts;
	u32 rx_excp_pkts[IPAHAL_PKT_STATUS_EXCEPTION_MAX];
	u32 rx_repl_repost;
	u32 tx_pkts_compl;
	u32 rx_q_len;
	u32 stat_compl;
	u32 aggr_close;
	u32 wan_aggr_close;
	u32 wan_rx_empty;
	u32 wan_repl_rx_empty;
	u32 lan_rx_empty;
	u32 lan_repl_rx_empty;
	u32 flow_enable;
	u32 flow_disable;
	u32 tx_non_linear;
};

struct ipa_active_clients {
	struct mutex mutex;	/* XXX comment this */
	atomic_t cnt;
};

struct ipa_wakelock_ref_cnt {
	spinlock_t spinlock;	/* XXX comment this */
	int cnt;
};

struct ipa_tag_completion {
	struct completion comp;
	atomic_t cnt;
};

/** enum ipa_mem_partition - IPA RAM Map is defined as an array of
 * 32-bit values read from DTS whose order is defined by this type.
 * Order and type of members should not be changed without a suitable change
 * to DTS file or the code that reads it.
 *
 * IPA SRAM memory layout:
 * +-------------------------+
 * |	UC MEM		     |
 * +-------------------------+
 * |	UC INFO		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * | V4 FLT HDR HASHABLE     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * | V4 FLT HDR NON-HASHABLE |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * | V6 FLT HDR HASHABLE     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * | V6 FLT HDR NON-HASHABLE |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * | V4 RT HDR HASHABLE	     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * | V4 RT HDR NON-HASHABLE  |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * | V6 RT HDR HASHABLE	     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * | V6 RT HDR NON-HASHABLE  |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |  MODEM HDR		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * | MODEM PROC CTX	     |
 * +-------------------------+
 * | APPS PROC CTX	     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |  MODEM MEM		     |
 * +-------------------------+
 * |	CANARY		     |
 * +-------------------------+
 * |  UC EVENT RING	     | From IPA 3.5
 * +-------------------------+
 */
enum ipa_mem_partition {
	OFST_START,
	V4_FLT_HASH_OFST,
	V4_FLT_HASH_SIZE,
	V4_FLT_HASH_SIZE_DDR,
	V4_FLT_NHASH_OFST,
	V4_FLT_NHASH_SIZE,
	V4_FLT_NHASH_SIZE_DDR,
	V6_FLT_HASH_OFST,
	V6_FLT_HASH_SIZE,
	V6_FLT_HASH_SIZE_DDR,
	V6_FLT_NHASH_OFST,
	V6_FLT_NHASH_SIZE,
	V6_FLT_NHASH_SIZE_DDR,
	V4_RT_NUM_INDEX,
	V4_MODEM_RT_INDEX_LO,
	V4_MODEM_RT_INDEX_HI,
	V4_APPS_RT_INDEX_LO,
	V4_APPS_RT_INDEX_HI,
	V4_RT_HASH_OFST,
	V4_RT_HASH_SIZE,
	V4_RT_HASH_SIZE_DDR,
	V4_RT_NHASH_OFST,
	V4_RT_NHASH_SIZE,
	V4_RT_NHASH_SIZE_DDR,
	V6_RT_NUM_INDEX,
	V6_MODEM_RT_INDEX_LO,
	V6_MODEM_RT_INDEX_HI,
	V6_APPS_RT_INDEX_LO,
	V6_APPS_RT_INDEX_HI,
	V6_RT_HASH_OFST,
	V6_RT_HASH_SIZE,
	V6_RT_HASH_SIZE_DDR,
	V6_RT_NHASH_OFST,
	V6_RT_NHASH_SIZE,
	V6_RT_NHASH_SIZE_DDR,
	MODEM_HDR_OFST,
	MODEM_HDR_SIZE,
	APPS_HDR_OFST,
	APPS_HDR_SIZE,
	APPS_HDR_SIZE_DDR,
	MODEM_HDR_PROC_CTX_OFST,
	MODEM_HDR_PROC_CTX_SIZE,
	APPS_HDR_PROC_CTX_OFST,
	APPS_HDR_PROC_CTX_SIZE,
	MODEM_OFST,
	MODEM_SIZE,
	END_OFST,
	UC_EVENT_RING_OFST,
	UC_EVENT_RING_SIZE,
	IPA_MEM_MAX,
};

/** union ipa_hw_error_event_data - HW->CPU Common Events
 * @error_type : Entered when a system error is detected by the HW. Type of
 * error is specified by IPA_HW_ERRORS
 * @reserved : Reserved
 */
union ipa_hw_error_event_data {
	struct ipa_hw_error_event_params {
		u32 error_type:8;
		u32 reserved:24;
	} __packed params;
	u32 raw32b;
} __packed;

/** struct ipa_hw_shared_mem_common_mapping - Structure referring to the common
 * section in 128B shared memory located in offset zero of SW Partition in IPA
 * SRAM.
 * @cmd_op : CPU->HW command opcode. See IPA_CPU_2_HW_COMMANDS
 * @cmd_params : CPU->HW command parameter lower 32bit.
 * @cmd_params_hi : CPU->HW command parameter higher 32bit.
 * of parameters (immediate parameters) and point on structure in system memory
 * (in such case the address must be accessible for HW)
 * @response_op : HW->CPU response opcode. See IPA_HW_2_CPU_RESPONSES
 * @response_params : HW->CPU response parameter. The parameter filed can hold
 * 32 bits of parameters (immediate parameters) and point on structure in system
 * memory
 * @event_op : HW->CPU event opcode. See IPA_HW_2_CPU_EVENTS
 * @event_params : HW->CPU event parameter. The parameter filed can hold 32
 *		bits of parameters (immediate parameters) and point on
 *		structure in system memory
 * @first_error_address : Contains the address of first error-source on SNOC
 * @hw_state : State of HW. The state carries information regarding the
 *				error type.
 * @warning_counter : The warnings counter. The counter carries information
 *						regarding non fatal errors in HW
 * @interface_version_common : The Common interface version as reported by HW
 *
 * The shared memory is used for communication between IPA HW and CPU.
 */
struct ipa_hw_shared_mem_common_mapping {
	u8  cmd_op;
	u8  reserved_01;
	u16 reserved_03_02;
	u32 cmd_params;
	u32 cmd_params_hi;
	u8  response_op;
	u8  reserved_0D;
	u16 reserved_0F_0E;
	u32 response_params;
	u8  event_op;
	u8  reserved_15;
	u16 reserved_17_16;
	u32 event_params;
	u32 first_error_address;
	u8  hw_state;
	u8  warning_counter;
	u16 reserved_23_22;
	u16 interface_version_common;
	u16 reserved_27_26;
} __packed;

/** struct ipa_uc_ctx - IPA uC context
 * @uc_inited: Indicates if uC interface has been initialized
 * @uc_loaded: Indicates if uC has loaded
 * @uc_failed: Indicates if uC has failed / returned an error
 * @uc_lock: uC interface lock to allow only one uC interaction at a time
 * @uc_completation: Completion mechanism to wait for uC commands
 * @uc_sram_mmio: Pointer to uC mapped memory
 * @pending_cmd: The last command sent waiting to be ACKed
 * @uc_status: The last status provided by the uC
 * @uc_error_type: error type from uC error event
 * @uc_error_timestamp: tag timer sampled after uC crashed
 */
struct ipa_uc_ctx {
	bool uc_inited;
	bool uc_loaded;
	bool uc_failed;
	struct ipa_hw_shared_mem_common_mapping *uc_sram_mmio;
	u32 uc_event_top_ofst;
	u32 pending_cmd;
	u32 uc_status;
	u32 uc_error_type;
	u32 uc_error_timestamp;
};

/** struct ipa_transport_pm - transport power management related members
 * @transport_pm_mutex: Mutex to protect the transport_pm functionality.
 */
struct ipa_transport_pm {
	atomic_t dec_clients;
	struct mutex transport_pm_mutex;	/* XXX comment this */
};

struct ipa_smp2p_info {
	struct qcom_smem_state *valid_state;
	struct qcom_smem_state *enabled_state;
	unsigned int valid_bit;
	unsigned int enabled_bit;
	bool ipa_clk_on;
	bool res_sent;
};

struct ipa_dma_task_info {
	struct ipa_mem_buffer mem;
	struct ipahal_imm_cmd_pyld *cmd_pyld;
};

/** struct ipa_context - IPA context
 * @class: pointer to the struct class
 * @dev_num: device number
 * @dev: the dev_t of the device
 * @cdev: cdev of the device
 * @ep: list of all end points
 * @ep_flt_bitmap: End-points supporting filtering bitmap
 * @ep_flt_num: End-points supporting filtering number
 * @flt_tbl: list of all IPA filter tables
 * @mode: IPA operating mode
 * @mmio: iomem
 * @ipa_wrapper_base: IPA wrapper base address
 * @rt_tbl_set: list of routing tables each of which is a list of rules
 * @reap_rt_tbl_set: list of sys mem routing tables waiting to be reaped
 * @tx_pkt_wrapper_cache: Tx packets cache
 * @rx_pkt_wrapper_cache: Rx packets cache
 * @lock: this does NOT protect the linked lists within ipa_sys_context
 * @smem_sz: shared memory size available for SW use starting
 *  from non-restricted bytes
 * @smem_restricted_bytes: the bytes that SW should not use in the shared mem
 * @nat_mem: NAT memory
 * @hdr_mem: header memory
 * @hdr_proc_ctx_mem: processing context memory
 * @power_mgmt_wq: workqueue for power management
 * @tag_process_before_gating: indicates whether to start tag process before
 *  gating IPA clocks
 * @transport_pm: transport power management related information
 * @ipa_active_clients: structure for reference counting connected IPA clients
 * @logbuf: ipc log buffer for high priority messages
 * @logbuf_low: ipc log buffer for low priority messages
 * @ipa_bus_hdl: msm driver handle for the data path bus
 * @ctrl: holds the core specific operations based on
 *  core version (vtable like)
 * @pkt_init_imm_opcode: opcode for IP_PACKET_INIT imm cmd
 * @wcstats: wlan common buffer stats
 * @uc_ctx: uC interface context
 * @uc_wdi_ctx: WDI specific fields for uC interface
 * @ipa_num_pipes: The number of pipes used by IPA HW
 * @ipa_client_apps_wan_cons_agg_gro: RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA
 * @w_lock: Indicates the wakeup source.
 * @wakelock_ref_cnt: Indicates the number of times wakelock is acquired
 *  finished initializing. Example of use - IOCTLs to /dev/ipa
 * IPA context - holds all relevant info about IPA driver and its state
 */
struct ipa_context {
	struct platform_device *ipa_pdev;
	struct gsi_ctx *gsi_ctx;

	struct device *dev;

	struct class *class;
	dev_t dev_num;
	struct device *chrdev;
	struct cdev cdev;

	struct ipa_ep_context ep[IPA_MAX_NUM_PIPES];
	u32 ep_flt_bitmap;
	u32 ep_flt_num;
	void __iomem *mmio;
	u32 ipa_wrapper_base;
	u32 ipa_wrapper_size;
	struct kmem_cache *tx_pkt_wrapper_cache;
	struct kmem_cache *rx_pkt_wrapper_cache;
	struct mutex lock;	/* XXX comment this */
	u16 smem_sz;
	u16 smem_restricted_bytes;
	u16 smem_reqd_sz;
	struct ipa_active_clients ipa_active_clients;
	struct ipa_active_clients_log_ctx ipa_active_clients_logging;
	char *active_clients_table_buf;
	struct workqueue_struct *power_mgmt_wq;
	struct ipa_transport_pm transport_pm;
	u32 clnt_hdl_cmd;
	u32 clnt_hdl_data_in;
	u32 clnt_hdl_data_out;
	/* featurize if memory footprint becomes a concern */
	struct ipa_stats stats;
	void *logbuf;
	void *logbuf_low;
	u32 ipa_bus_hdl;
	struct msm_bus_scale_pdata *bus_scale_tbl;
	u32 mem_info[IPA_MEM_MAX];
	bool q6_proxy_clk_vote_valid;
	u32 ipa_num_pipes;
	dma_addr_t pkt_init_imm[IPA_MAX_NUM_PIPES];
	struct ipa_mem_buffer pkt_init_mem;

	struct ipa_uc_ctx uc_ctx;

	u32 ee;
	struct wakeup_source w_lock;
	struct ipa_wakelock_ref_cnt wakelock_ref_cnt;
	/* RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA */
	bool ipa_client_apps_wan_cons_agg_gro;
	/* M-release support to know client pipes */
	struct ipa_smp2p_info smp2p_info;
	struct ipa_dma_task_info dma_task_info;
};

extern struct ipa_context *ipa_ctx;

/* public APIs */

int ipa_wwan_init(void);
void ipa_wwan_cleanup(void);

/* Generic GSI channels functions */

int ipa_stop_gsi_channel(u32 clnt_hdl);

void ipa_reset_gsi_channel(u32 clnt_hdl);

/* Configuration */
int ipa_cfg_ep(u32 clnt_hdl, const struct ipa_ep_cfg *ipa_ep_cfg);

int ipa_cfg_ep_holb(u32 clnt_hdl, const struct ipa_ep_cfg_holb *ipa_ep_cfg);

/* Data path */
int ipa_tx_dp(enum ipa_client_type dst, struct sk_buff *skb);

/* System pipes */
int ipa_setup_sys_pipe(struct ipa_sys_connect_params *sys_in);

int ipa_teardown_sys_pipe(u32 clnt_hdl);

u16 ipa_get_smem_restr_bytes(void);

/* interrupts */
int ipa_add_interrupt_handler(enum ipa_irq_type interrupt,
			      ipa_irq_handler_t handler, bool deferred_flag,
			      void *private_data);

int ipa_remove_interrupt_handler(enum ipa_irq_type interrupt);

/* Miscellaneous */
void ipa_proxy_clk_vote(void);
void ipa_proxy_clk_unvote(void);

enum ipa_client_type ipa_get_client_mapping(int pipe_idx);

void ipa_init_ep_flt_bitmap(void);

bool ipa_is_ep_support_flt(int pipe_idx);

u8 ipa_get_qmb_master_sel(enum ipa_client_type client);

/* internal functions */

bool ipa_is_modem_pipe(int pipe_idx);
int ipa_send(struct ipa_sys_context *sys, u32 num_desc, struct ipa_desc *desc);

int ipa_get_ep_mapping(enum ipa_client_type client);
struct ipa_ep_context *ipa_get_ep_context(enum ipa_client_type client);

int ipa_init_hw(void);

void ipa_init_mem_info(struct device_node *dev_node);

struct msm_bus_scale_pdata *ipa_bus_scale_table_init(void);
int ipa_send_cmd_timeout(u16 num_desc, struct ipa_desc *descr, u32 timeout);
int ipa_send_cmd(u16 num_desc, struct ipa_desc *descr);

#define ipa_client_add(id, log_it) \
	_ipa_client_add((id), (log_it), __FILE__, __LINE__)
#define ipa_client_add_additional(id, log_it) \
	_ipa_client_add_additional((id), (log_it), __FILE__, __LINE__)
#define ipa_client_remove(id, log_it) \
	_ipa_client_remove((id), (log_it), __FILE__, __LINE__)
#define ipa_client_remove_wait(id, log_it) \
	_ipa_client_remove_wait((id), (log_it), __FILE__, __LINE__)

void _ipa_client_add(const char *id, bool log_it, const char *file, int line);
bool _ipa_client_add_additional(const char *id, bool log_it,
				const char *file, int line);
void _ipa_client_remove(const char *id, bool log_it, const char *file,
			int line);
void _ipa_client_remove_wait(const char *id, bool log_it, const char *file,
			     int line);

void ipa_cfg_default_route(enum ipa_client_type client);

int ipa_active_clients_log_print_table(char *buf, int size);
int ipa_interrupts_init(u32 ipa_irq, u32 ee, struct device *ipa_dev);

void ipa_suspend_active_aggr_wa(u32 clnt_hdl);
void ipa_suspend_handler(enum ipa_irq_type interrupt, void *private_data,
			 void *interrupt_data);
void ipa_lan_rx_cb(void *priv, enum ipa_dp_evt_type evt, unsigned long data);

void ipa_sram_settings_read(void);

void ipa_skb_recycle(struct sk_buff *skb);

int ipa_cfg_ep_status(u32 clnt_hdl,
		      const struct ipahal_reg_ep_cfg_status *ipa_ep_cfg);

int ipa_init_q6_smem(void);

int ipa_uc_interface_init(void);
int ipa_uc_loaded_check(void);
const struct ipa_gsi_ep_config *ipa_get_gsi_ep_info
	(enum ipa_client_type client);

u32 ipa_get_num_pipes(void);
int ipa_ap_suspend(struct device *dev);
int ipa_ap_resume(struct device *dev);
void ipa_set_resource_groups_min_max_limits(void);
void ipa_suspend_apps_pipes(bool suspend);
int ipa_uc_panic_notifier(struct notifier_block *this, unsigned long event,
			  void *ptr);
void ipa_inc_acquire_wakelock(void);
void ipa_dec_release_wakelock(void);
const char *ipa_hw_error_str(enum ipa_hw_errors err_type);
int ipa_rx_poll(u32 clnt_hdl, int budget);
void ipa_reset_freeze_vote(void);
void ipa_enable_dcd(void);

int ipa_gsi_dma_task_alloc(void);
void ipa_gsi_dma_task_free(void);
int ipa_gsi_dma_task_inject(void);

int ipa_disable_apps_wan_cons_deaggr(u32 agg_size, u32 agg_count);
int ipa_plat_drv_probe(struct platform_device *pdev_p);

void ipa_set_flt_tuple_mask(int pipe_idx, struct ipahal_reg_hash_tuple *tuple);
void ipa_set_rt_tuple_mask(int tbl_idx, struct ipahal_reg_hash_tuple *tuple);

void ipa_gsi_irq_rx_notify_cb(void *chan_data, void *xfer_data, u16 count);
void ipa_gsi_irq_tx_notify_cb(void *chan_data, void *xfer_data, u16 count);

#endif /* _IPA_I_H_ */
