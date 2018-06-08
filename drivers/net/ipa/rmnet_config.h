// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#ifndef _RMNET_CONFIG_H_
#define _RMNET_CONFIG_H_

#include <linux/skbuff.h>

struct rmnet_phys_ep_conf_s {
	void	(*recycle)(struct sk_buff *); /* Destruct function */
	void	*config;
};

struct rmnet_map_header_s {
#ifndef RMNET_USE_BIG_ENDIAN_STRUCTS
	u8	pad_len:6;
	u8	reserved_bit:1;
	u8	cd_bit:1;
#else
	u8	cd_bit:1;
	u8	reserved_bit:1;
	u8	pad_len:6;
#endif /* RMNET_USE_BIG_ENDIAN_STRUCTS */
	u8	mux_id;
	u16	pkt_len;
}  __aligned(1);

#define RMNET_MAP_GET_MUX_ID(Y) (((struct rmnet_map_header_s *)Y->data)->mux_id)
#define RMNET_MAP_GET_CD_BIT(Y) (((struct rmnet_map_header_s *)Y->data)->cd_bit)
#define RMNET_MAP_GET_PAD(Y) (((struct rmnet_map_header_s *)Y->data)->pad_len)
#define RMNET_MAP_GET_CMD_START(Y) ((struct rmnet_map_control_command_s *) \
				  (Y->data + sizeof(struct rmnet_map_header_s)))
#define RMNET_MAP_GET_LENGTH(Y) (ntohs( \
			       ((struct rmnet_map_header_s *)Y->data)->pkt_len))

#endif /* _RMNET_CONFIG_H_ */
