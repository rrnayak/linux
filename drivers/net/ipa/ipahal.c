// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#define pr_fmt(fmt)	"ipahal %s:%d " fmt, __func__, __LINE__

#include <linux/debugfs.h>
#include "ipahal.h"
#include "ipahal_i.h"
#include "ipahal_reg_i.h"

/* Produce a contiguous bitmask with a positive number of low-order bits set. */
#define MASK(bits)	GENMASK((bits) - 1, 0)

static struct ipahal_context ipahal_ctx_struct;
struct ipahal_context *ipahal_ctx = &ipahal_ctx_struct;

static const char * const ipahal_pkt_status_exception_to_str[] = {
	__stringify(IPAHAL_PKT_STATUS_EXCEPTION_NONE),
	__stringify(IPAHAL_PKT_STATUS_EXCEPTION_DEAGGR),
	__stringify(IPAHAL_PKT_STATUS_EXCEPTION_IPTYPE),
	__stringify(IPAHAL_PKT_STATUS_EXCEPTION_PACKET_LENGTH),
	__stringify(IPAHAL_PKT_STATUS_EXCEPTION_PACKET_THRESHOLD),
	__stringify(IPAHAL_PKT_STATUS_EXCEPTION_FRAG_RULE_MISS),
	__stringify(IPAHAL_PKT_STATUS_EXCEPTION_SW_FILT),
	__stringify(IPAHAL_PKT_STATUS_EXCEPTION_NAT),
	__stringify(IPAHAL_PKT_STATUS_EXCEPTION_IPV6CT),
};

/* struct ipahal_imm_cmd_obj - immediate command H/W information for
 *  specific IPA version
 * @name - Command "name" (i.e., symbolic identifier)
 * @opcode - Immediate command OpCode
 */
struct ipahal_imm_cmd_obj {
	const char	*name;
	u16		opcode;
};

static struct ipahal_imm_cmd_obj ipahal_imm_cmds[IPA_IMM_CMD_MAX];

static struct ipahal_imm_cmd_pyld *
ipahal_imm_cmd_pyld_alloc_common(u16 opcode, size_t pyld_size, gfp_t flags)
{
	struct ipahal_imm_cmd_pyld *pyld;

	ipa_debug_low("immediate command: %s\n", ipahal_imm_cmds[opcode].name);

	pyld = kzalloc(sizeof(*pyld) + pyld_size, flags);
	if (unlikely(!pyld)) {
		ipa_err("kzalloc err (opcode %hu pyld_size %zu)\n", opcode,
			pyld_size);
		return NULL;
	}
	pyld->opcode = opcode;
	pyld->len = pyld_size;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *
ipahal_imm_cmd_pyld_alloc(u16 opcode, size_t pyld_size)
{
	return ipahal_imm_cmd_pyld_alloc_common(opcode, pyld_size, GFP_KERNEL);
}

static struct ipahal_imm_cmd_pyld *
ipahal_imm_cmd_pyld_alloc_atomic(u16 opcode, size_t pyld_size)
{
	return ipahal_imm_cmd_pyld_alloc_common(opcode, pyld_size, GFP_ATOMIC);
}

/* Returns true if the value provided is too big to be represented
 * in the given number of bits.  In this case, WARN_ON() is called,
 * and a message is printed and using ipa_err().
 *
 * Returns false if the value is OK (not too big).
 */
static bool check_too_big(char *name, u64 value, u8 bits)
{
	if (!WARN_ON(value & ~MASK(bits)))
		return false;

	ipa_err("%s is bigger than %hhubit width 0x%llx\n", name, bits, value);

	return true;
}

/* The The opcode used for certain immediate commands may change
 * between different versions of IPA hardware.  The format of the
 * command data passed to the IPA can change slightly with new
 * hardware.  The "ipahal" layer uses the ipahal_imm_cmd_obj[][]
 * table to hide the version-specific details of creating immediate
 * commands.
 *
 * The following table consists of blocks of "immediate command
 * object" definitions associated with versions of IPA hardware.
 * The entries for each immediate command contain a construct
 * functino and an opcode to use for a given version of IPA
 * hardware.  The first version of IPA hardware supported by the
 * "ipahal" layer is 3.0.
 *
 * Versions of IPA hardware newer than 3.0 do not need to specify
 * immediate command object entries if they are accessed the same
 * way as was defined by an older version.  The only entries defined
 * for newer hardware are for immediate commands whose opcode or
 * command format has changed or are deprecated, or immediate
 * commands that are new and not present in older hardware.
 *
 * The construct function for an immediate command is given an IPA
 * opcode, plus a non-null pointer to a command-specific parameter
 * block used to initialize the command.  The construct function
 * allocates a buffer to hold the command payload, and a pointer to
 * that buffer is returned once the parameters have been formatted
 * into it.  It is the caller's responsibility to ensure this buffer
 * gets freed when it is no longer needed.  The construct function
 * returns null if the buffer could not be allocated.
 *
 * No opcodes or command formats changed between IPA version 3.0
 * and IPA version 3.5.1, so all definitions from version 3.0 are
 * inherited by these newer versions.  We know, however, that some
 * of these *are* changing for upcoming hardware.
 *
 * The entries in this table have the following constraints:
 * - 0 is not a valid opcode; an entry having a 0 opcode indicates
 *   that the corresponding immediate command is formatted according
 *   to an immediate command object defined for an earlier hardware
 *   version.
 * - An opcode of OPCODE_INVAL indicates that a command is not
 *   supported for a particular hardware version.  It is an error
 *   for code to attempt to execute a command that is not
 *   unsupported by the current IPA hardware.
 *
 * A caller constructs an immediate command by providing a command
 * id and a parameter block to ipahal_construct_imm_cmd().  Such
 * calls are subject to these constraints:
 * - The command id supplied must be valid:
 *     - It must be a member of the ipahal_imm_cmd_name enumerated
 *	 type less than IPA_IMM_CMD_MAX
 *     - It must be a command supported by the underlying hardware
 * - The parameter block must be a non-null pointer referring to
 *   parameter data that is formatted properly for the command.
 */
#define OPCODE_INVAL	((u16)0xffff)
#define idsym(id)	IPA_IMM_CMD_ ## id
#define imm_cmd_obj(id, o)			\
	[idsym(id)] = {				\
		.name = #id,			\
		.opcode = o,			\
	}
#define imm_cmd_obj_inval(id)			\
	[idsym(id)] = {				\
		.name = NULL,			\
		.opcode = OPCODE_INVAL,		\
	}
static const struct ipahal_imm_cmd_obj
		ipahal_imm_cmd_objs[][IPA_IMM_CMD_MAX] = {
	/* IPAv3.5.1 */
	[IPA_HW_v3_5_1] = {
		imm_cmd_obj(IP_V4_FILTER_INIT,		3),
		imm_cmd_obj(IP_V6_FILTER_INIT,		4),
		imm_cmd_obj(IP_V4_ROUTING_INIT,		7),
		imm_cmd_obj(IP_V6_ROUTING_INIT,		8),
		imm_cmd_obj(HDR_INIT_LOCAL,		9),
		imm_cmd_obj(REGISTER_WRITE,		12),
		imm_cmd_obj(IP_PACKET_INIT,		16),
		imm_cmd_obj(DMA_TASK_32B_ADDR,		17),
		imm_cmd_obj(DMA_SHARED_MEM,		19),
		imm_cmd_obj(IP_PACKET_TAG_STATUS,	20),
	},
};

#undef imm_cmd_obj
#undef idsym
#undef cfunc

/* ipahal_imm_cmd_init() - Build the Immediate command information table
 *  See ipahal_imm_cmd_objs[][] comments
 */
static void ipahal_imm_cmd_init(enum ipa_hw_version hw_version)
{
	int i;
	int j;

	ipa_assert(hw_version < ARRAY_SIZE(ipahal_imm_cmd_objs));

	ipa_debug_low("Entry - HW_TYPE=%d\n", hw_version);

	/* Build up the immediate command descriptions we'll use */
	for (i = 0; i < IPA_IMM_CMD_MAX ; i++) {
		for (j = hw_version; j >= 0; j--) {
			const struct ipahal_imm_cmd_obj *imm_cmd;

			imm_cmd = &ipahal_imm_cmd_objs[j][i];
			if (imm_cmd->opcode) {
				ipahal_imm_cmds[i] = *imm_cmd;
				break;
			}
		}
	}
}

struct ipahal_imm_cmd_pyld *
ipahal_dma_shared_mem_write_pyld(struct ipa_mem_buffer *mem, u32 offset)
{
	struct ipa_imm_cmd_hw_dma_shared_mem *data;
	struct ipahal_imm_cmd_pyld *pyld;
	u16 opcode;

	if (check_too_big("size", mem->size, 16))
		return NULL;
	if (check_too_big("offset", offset, 16))
		return NULL;

	opcode = ipahal_imm_cmds[IPA_IMM_CMD_DMA_SHARED_MEM].opcode;
	pyld = ipahal_imm_cmd_pyld_alloc(opcode, sizeof(*data));
	if (!pyld)
		return NULL;
	data = ipahal_imm_cmd_pyld_data(pyld);

	data->size = mem->size;
	data->local_addr = offset;
	data->direction = 0;	/* 0 = write to IPA; 1 = read from IPA */
	data->skip_pipeline_clear = 0;
	data->pipeline_clear_options = IPAHAL_HPS_CLEAR;
	data->system_addr = mem->phys_base;

	return pyld;
}

struct ipahal_imm_cmd_pyld *
ipahal_register_write_pyld(u32 offset, u32 value, u32 mask, bool clear)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_register_write *data;
	u16 opcode;

	if (check_too_big("offset", offset, 16))
		return NULL;

	opcode = ipahal_imm_cmds[IPA_IMM_CMD_DMA_SHARED_MEM].opcode;
	pyld = ipahal_imm_cmd_pyld_alloc(opcode, sizeof(*data));
	if (!pyld)
		return NULL;
	data = ipahal_imm_cmd_pyld_data(pyld);

	data->skip_pipeline_clear = 0;
	data->offset = offset;
	data->value = value;
	data->value_mask = mask;
	data->pipeline_clear_options = clear ? IPAHAL_FULL_PIPELINE_CLEAR
					     : IPAHAL_HPS_CLEAR;

	return pyld;
}

struct ipahal_imm_cmd_pyld *
ipahal_hdr_init_local_pyld(struct ipa_mem_buffer *mem, u32 offset)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_hdr_init_local *data;
	u16 opcode;

	if (check_too_big("size", mem->size, 12))
		return NULL;
	if (check_too_big("offset", offset, 16))
		return NULL;

	opcode = ipahal_imm_cmds[IPA_IMM_CMD_HDR_INIT_LOCAL].opcode;
	pyld = ipahal_imm_cmd_pyld_alloc(opcode, sizeof(*data));
	if (!pyld)
		return NULL;
	data = ipahal_imm_cmd_pyld_data(pyld);

	data->hdr_table_addr = mem->phys_base;
	data->size_hdr_table = mem->size;
	data->hdr_addr = offset;

	return pyld;
}

struct ipahal_imm_cmd_pyld *ipahal_ip_packet_init_pyld(u32 dest_pipe_idx)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_ip_packet_init *data;
	u16 opcode;

	if (check_too_big("dest_pipe_idx", dest_pipe_idx, 5))
		return NULL;

	opcode = ipahal_imm_cmds[IPA_IMM_CMD_IP_PACKET_INIT].opcode;
	pyld = ipahal_imm_cmd_pyld_alloc(opcode, sizeof(*data));
	if (!pyld)
		return NULL;
	data = ipahal_imm_cmd_pyld_data(pyld);

	data->destination_pipe_index = dest_pipe_idx;

	return pyld;
}

static struct ipahal_imm_cmd_pyld *
fltrt_init_common(u16 opcode, struct ipa_mem_buffer *mem, u32 hash_offset,
		  u32 nhash_offset)
{
	struct ipa_imm_cmd_hw_ip_fltrt_init *data;
	struct ipahal_imm_cmd_pyld *pyld;

	if (check_too_big("hash_rules_size", mem->size, 12))
		return NULL;
	if (check_too_big("hash_local_addr", hash_offset, 16))
		return NULL;
	if (check_too_big("nhash_rules_size", mem->size, 12))
		return NULL;
	if (check_too_big("nhash_local_addr", nhash_offset, 16))
		return NULL;

	pyld = ipahal_imm_cmd_pyld_alloc(opcode, sizeof(*data));
	if (!pyld)
		return NULL;
	data = ipahal_imm_cmd_pyld_data(pyld);

	ipa_debug("putting hashable rules to phys 0x%x\n", hash_offset);
	ipa_debug("putting non-hashable rules to phys 0x%x\n", nhash_offset);

	data->hash_rules_addr = (u64)mem->phys_base;
	data->hash_rules_size = (u32)mem->size;
	data->hash_local_addr = hash_offset;
	data->nhash_rules_addr = (u64)mem->phys_base;
	data->nhash_rules_size = (u32)mem->size;
	data->nhash_local_addr = nhash_offset;

	return pyld;
}

struct ipahal_imm_cmd_pyld *
ipahal_ip_v4_routing_init_pyld(struct ipa_mem_buffer *mem, u32 hash_offset,
			       u32 nhash_offset)
{
	u16 opcode = ipahal_imm_cmds[IPA_IMM_CMD_IP_V4_ROUTING_INIT].opcode;

	ipa_debug("IPv4 routing\n");

	return fltrt_init_common(opcode, mem, hash_offset, nhash_offset);
}

struct ipahal_imm_cmd_pyld *
ipahal_ip_v6_routing_init_pyld(struct ipa_mem_buffer *mem, u32 hash_offset,
			       u32 nhash_offset)
{
	u16 opcode = ipahal_imm_cmds[IPA_IMM_CMD_IP_V6_ROUTING_INIT].opcode;

	ipa_debug("IPv6 routing\n");

	return fltrt_init_common(opcode, mem, hash_offset, nhash_offset);
}

struct ipahal_imm_cmd_pyld *
ipahal_ip_v4_filter_init_pyld(struct ipa_mem_buffer *mem, u32 hash_offset,
			      u32 nhash_offset)
{
	u16 opcode = ipahal_imm_cmds[IPA_IMM_CMD_IP_V4_FILTER_INIT].opcode;

	ipa_debug("IPv4 filtering\n");

	return fltrt_init_common(opcode, mem, hash_offset, nhash_offset);
}

struct ipahal_imm_cmd_pyld *
ipahal_ip_v6_filter_init_pyld(struct ipa_mem_buffer *mem, u32 hash_offset,
			      u32 nhash_offset)
{
	u16 opcode = ipahal_imm_cmds[IPA_IMM_CMD_IP_V6_FILTER_INIT].opcode;

	ipa_debug("IPv6 filtering\n");

	return fltrt_init_common(opcode, mem, hash_offset, nhash_offset);
}

/* NOTE:  this function is called in atomic state */
struct ipahal_imm_cmd_pyld *ipahal_ip_packet_tag_status_pyld(u64 tag)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_ip_packet_tag_status *data;
	u16 opcode = ipahal_imm_cmds[IPA_IMM_CMD_IP_PACKET_TAG_STATUS].opcode;

	if (check_too_big("tag", tag, 48))
		return NULL;

	pyld = ipahal_imm_cmd_pyld_alloc_atomic(opcode, sizeof(*data));
	if (!pyld)
		return NULL;
	data = ipahal_imm_cmd_pyld_data(pyld);

	data->tag = tag;

	return pyld;
}

struct ipahal_imm_cmd_pyld *
ipahal_dma_task_32b_addr_pyld(struct ipa_mem_buffer *mem)
{
	struct ipahal_imm_cmd_pyld *pyld;
	struct ipa_imm_cmd_hw_dma_task_32b_addr *data;
	u16 opcode = ipahal_imm_cmds[IPA_IMM_CMD_DMA_TASK_32B_ADDR].opcode;

	if (check_too_big("size1", mem->size, 16))
		return NULL;
	if (check_too_big("packet_size", mem->size, 16))
		return NULL;

	pyld = ipahal_imm_cmd_pyld_alloc(opcode, sizeof(*data));
	if (!pyld)
		return NULL;
	data = ipahal_imm_cmd_pyld_data(pyld);

	data->cmplt = 0;
	data->eof = 0;
	data->flsh = 1;
	data->lock = 0;
	data->unlock = 0;
	data->size1 = mem->size;
	data->addr1 = mem->phys_base;
	data->packet_size = mem->size;

	return pyld;
}

/* IPA Packet Status Logic */

static bool status_opcode_valid(u8 status_opcode)
{
	switch ((enum ipahal_pkt_status_opcode)status_opcode) {
	case IPAHAL_PKT_STATUS_OPCODE_PACKET:
	case IPAHAL_PKT_STATUS_OPCODE_NEW_FRAG_RULE:
	case IPAHAL_PKT_STATUS_OPCODE_DROPPED_PACKET:
	case IPAHAL_PKT_STATUS_OPCODE_SUSPENDED_PACKET:
	case IPAHAL_PKT_STATUS_OPCODE_LOG:
	case IPAHAL_PKT_STATUS_OPCODE_DCMP:
	case IPAHAL_PKT_STATUS_OPCODE_PACKET_2ND_PASS:
		return true;
	default:
		return false;
	}
}

static bool nat_type_valid(u8 nat_type)
{
	switch (nat_type) {
	case IPAHAL_PKT_STATUS_NAT_NONE:
	case IPAHAL_PKT_STATUS_NAT_SRC:
	case IPAHAL_PKT_STATUS_NAT_DST:
		return true;
	default:
		return false;
	}
}

/* Maps an exception type returned in a ipa_pkt_status_hw structure
 * to the ipahal_pkt_status_exception value that represents it in
 * the exception field of a ipahal_pkt_status structure.  Returns
 * IPAHAL_PKT_STATUS_EXCEPTION_MAX for an unrecognized value.
 */
static enum ipahal_pkt_status_exception
exception_map(u8 exception, bool is_ipv6)
{
	switch (exception) {
	case 0x00:	return IPAHAL_PKT_STATUS_EXCEPTION_NONE;
	case 0x01:	return IPAHAL_PKT_STATUS_EXCEPTION_DEAGGR;
	case 0x04:	return IPAHAL_PKT_STATUS_EXCEPTION_IPTYPE;
	case 0x08:	return IPAHAL_PKT_STATUS_EXCEPTION_PACKET_LENGTH;
	case 0x10:	return IPAHAL_PKT_STATUS_EXCEPTION_FRAG_RULE_MISS;
	case 0x20:	return IPAHAL_PKT_STATUS_EXCEPTION_SW_FILT;
	case 0x40:	return is_ipv6 ? IPAHAL_PKT_STATUS_EXCEPTION_IPV6CT
				       : IPAHAL_PKT_STATUS_EXCEPTION_NAT;
	default:	return IPAHAL_PKT_STATUS_EXCEPTION_MAX;
	}
}

static void ipa_pkt_status_parse(
	const void *unparsed_status, struct ipahal_pkt_status *status)
{
	const struct ipa_pkt_status_hw *hw_status = unparsed_status;
	u8 status_opcode = (u8)hw_status->status_opcode;
	u8 nat_type = (u8)hw_status->nat_type;
	enum ipahal_pkt_status_exception exception;
	bool is_ipv6;

	is_ipv6 = (hw_status->status_mask & 0x80) ? false : true;

	status->pkt_len = hw_status->pkt_len;
	status->endp_src_idx = hw_status->endp_src_idx;
	status->endp_dest_idx = hw_status->endp_dest_idx;
	status->metadata = hw_status->metadata;
	status->flt_local = hw_status->flt_local;
	status->flt_hash = hw_status->flt_hash;
	status->flt_global = hw_status->flt_hash;
	status->flt_ret_hdr = hw_status->flt_ret_hdr;
	status->flt_miss = ~(hw_status->flt_rule_id) ? false : true;
	status->flt_rule_id = hw_status->flt_rule_id;
	status->rt_local = hw_status->rt_local;
	status->rt_hash = hw_status->rt_hash;
	status->ucp = hw_status->ucp;
	status->rt_tbl_idx = hw_status->rt_tbl_idx;
	status->rt_miss = ~(hw_status->rt_rule_id) ? false : true;
	status->rt_rule_id = hw_status->rt_rule_id;
	status->nat_hit = hw_status->nat_hit;
	status->nat_entry_idx = hw_status->nat_entry_idx;
	status->tag_info = hw_status->tag_info;
	status->seq_num = hw_status->seq_num;
	status->time_of_day_ctr = hw_status->time_of_day_ctr;
	status->hdr_local = hw_status->hdr_local;
	status->hdr_offset = hw_status->hdr_offset;
	status->frag_hit = hw_status->frag_hit;
	status->frag_rule = hw_status->frag_rule;

	if (WARN_ON(!status_opcode_valid(status_opcode)))
		ipa_err("unsupported Status Opcode 0x%x\n", status_opcode);
	else
		status->status_opcode = status_opcode;

	if (WARN_ON(!nat_type_valid((nat_type))))
		ipa_err("unsupported Status NAT type 0x%x\n", nat_type);
	else
		status->nat_type = nat_type;

	exception = exception_map((u8)hw_status->exception, is_ipv6);
	if (WARN_ON(exception == IPAHAL_PKT_STATUS_EXCEPTION_MAX))
		ipa_err("unsupported Status Exception type 0x%x\n",
			hw_status->exception);
	else
		status->exception = exception;

	/* If hardware status values change we may have to re-map this */
	status->status_mask = hw_status->status_mask;
}

/* ipahal_pkt_status_get_size() - Get H/W size of packet status */
u32 ipahal_pkt_status_get_size(void)
{
	return sizeof(struct ipa_pkt_status_hw);
}

/* ipahal_pkt_status_parse() - Parse Packet Status payload to abstracted form
 * @unparsed_status: Pointer to H/W format of the packet status as read from H/W
 * @status: Pointer to pre-allocated buffer where the parsed info will be stored
 */
void ipahal_pkt_status_parse(const void *unparsed_status,
			     struct ipahal_pkt_status *status)
{
	ipa_debug_low("Parse Status Packet\n");
	memset(status, 0, sizeof(*status));
	ipa_pkt_status_parse(unparsed_status, status);
}

/* ipahal_pkt_status_exception_str() - returns string represents exception type
 * @exception: [in] The exception type
 */
const char *
ipahal_pkt_status_exception_str(enum ipahal_pkt_status_exception exception)
{
	return ipahal_pkt_status_exception_to_str[exception];
}

int ipahal_dma_alloc(struct ipa_mem_buffer *mem, u32 size, gfp_t gfp)
{
	dma_addr_t phys;
	void *cpu_addr;

	cpu_addr = dma_zalloc_coherent(ipahal_ctx->ipa_pdev, size, &phys, gfp);
	if (!cpu_addr) {
		ipa_err("failed to alloc DMA buff of size %u\n", size);
		return -ENOMEM;
	}

	mem->base = cpu_addr;
	mem->phys_base = phys;
	mem->size = size;

	return 0;
}

void ipahal_dma_free(struct ipa_mem_buffer *mem)
{
	dma_free_coherent(ipahal_ctx->ipa_pdev, mem->size, mem->base,
			  mem->phys_base);
	memset(mem, 0, sizeof(*mem));
}

void ipahal_init(enum ipa_hw_version hw_version, void __iomem *base)
{
	ipa_debug("Entry - IPA HW TYPE=%d base=%p\n", hw_version, base);

	ipahal_ctx->base = base;
	/* ipahal_ctx->ipa_pdev must be set by a call to ipahal_dev_init() */

	/* Packet status parsing code requires no initialization */
	ipahal_reg_init(hw_version);
	ipahal_imm_cmd_init(hw_version);
	ipahal_fltrt_init(hw_version);
}

/* Assign the IPA HAL's device pointer.  Once it's assigned we can
 * initialize the empty table entry.
 */
int ipahal_dev_init(struct device *dev)
{
	int ret;

	ipa_debug("IPA HAL ipa_pdev=%p\n", dev);

	ipahal_ctx->ipa_pdev = dev;
	ret = ipahal_empty_fltrt_init();
	if (ret)
		ipahal_ctx->ipa_pdev = NULL;

	return ret;
}

void ipahal_dev_destroy(void)
{
	ipahal_empty_fltrt_destroy();
	ipahal_ctx->ipa_pdev = NULL;
}

void ipahal_destroy(void)
{
	ipa_debug("Entry\n");
}
