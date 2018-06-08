// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#define pr_fmt(fmt)    "ipa-qmi %s:%d " fmt, __func__, __LINE__

#include <linux/string.h>
#include <linux/qrtr.h>
#include <linux/soc/qcom/qmi.h>

#include "ipa_qmi_msg.h"

#include "ipa_i.h" /* For ipa_err(), ipa_context, and ipa_mem_partition */

#define QMI_INIT_DRIVER_TIMEOUT	60000	/* A minute in milliseconds */

static bool ipa_qmi_initialized;

/* The AP and modem perform a "handshake" at initialization time to
 * ensure each side knows the other side is ready.  Two pairs of QMI
 * handles (endpoints) are used for this; one provides service on
 * the modem for AP requests, and the other is on the AP to service
 * modem requests (and to supply an indication from the AP).
 *
 * The QMI service on the modem expects to receive an INIT_DRIVER
 * request from the AP, which contains parameters used by the
 * modem during initialization.  The AP sends this request as soon
 * as it is knows the service is available.  The modem responds to
 * this request immediately.
 *
 * When the modem learns the AP service is available, it is able
 * to communicate its status to the AP.  The modem uses this to
 * tell the AP when it is ready to receive an indication, sending
 * an INDICATION_REGISTER request to the handle served by the AP.
 * This is separate from the modem driver initialization.
 *
 * When the modem has completed the driver initialization requested
 * by the AP, it sends a DRIVER_INIT_COMPLETE request to the AP.
 * This request could arrive at the AP either before or after the
 * INDICATION_REGISTER request.
 *
 * The final step in the handshake occurs after the AP has received
 * both requests from the modem.  The AP completes the handshake by
 * sending an INIT_COMPLETE_IND indication message to the modem.
 */

#define IPA_HOST_SERVICE_SVC_ID		0x31
#define IPA_HOST_SVC_VERS		1
#define IPA_HOST_SERVICE_INS_ID		1

#define IPA_MODEM_SERVICE_SVC_ID	0x31
#define IPA_MODEM_SERVICE_INS_ID	2
#define IPA_MODEM_SVC_VERS		1

/* Used to send an INIT_DRIVER request to the modem */
static struct qmi_handle client_handle;

/* Requests from the modem arrive on the server handle to tell us
 * when it is prepared to receive an INIT_COMPLETE indication, and
 * when its driver initialization is complete.  The AP sends the
 * indication after it has received and responded to both requests.
 */
static struct qmi_handle server_handle;

/* These track state during the handshake */
static bool indication_register_received;
static bool init_driver_response_received;

/* Send an INIT_COMPLETE_IND indication message to the modem */
static int ipa_send_master_driver_init_complete_ind(struct qmi_handle *qmi,
						    struct sockaddr_qrtr *sq)
{
	struct ipa_init_complete_ind ind;

	memset(&ind, 0, sizeof(ind));
	ind.status.result = QMI_RESULT_SUCCESS_V01;
	ind.status.error = QMI_ERR_NONE_V01;

	return qmi_send_indication(qmi, sq, IPA_QMI_INIT_COMPLETE_IND,
				IPA_QMI_INIT_COMPLETE_IND_SZ,
				ipa_init_complete_ind_ei, &ind);
}

/* This function is called to determine whether to complete the
 * handshake by sending an INIT_COMPLETE_IND indication message to
 * the modem.  The "init_driver" parameter is false when we've
 * received an INDICATION_REGISTER request message from the modem,
 * or true when we've received the response from the INIT_DRIVER
 * request message we send.  If this function decides the message
 * should be sent, it calls ipa_send_master_driver_init_complete_ind()
 * to send it.
 */
static int ipa_handshake_complete(struct qmi_handle *qmi,
				  struct sockaddr_qrtr *sq, bool init_driver)
{
	bool send_it;

	if (init_driver) {
		init_driver_response_received = true;
		send_it = indication_register_received;
	} else {
		indication_register_received = true;
		send_it = init_driver_response_received;
	}
	if (!send_it)
		return 0;

	return ipa_send_master_driver_init_complete_ind(qmi, sq);
}

/* Callback function to handle an INDICATION_REGISTER request message
 * from the modem.  This informs the AP that the modem is now ready to
 * receive the INIT_COMPLETE_IND indication message.
 */
static void ipa_indication_register_fn(struct qmi_handle *qmi,
				       struct sockaddr_qrtr *sq,
				       struct qmi_txn *txn,
				       const void *decoded)
{
	const struct ipa_indication_register_req *req = decoded;
	struct ipa_indication_register_rsp rsp;
	int ret;

	/* Both of these should be true (1), but we ignore them.
	 * Supposedly we're supposed to only send the master
	 * driver init complete indication if this is true.
	 */
	ipa_debug("req->master_driver_init_complete_valid = %hhu\n",
		  req->master_driver_init_complete_valid);
	ipa_debug("req->master_driver_init_complete = %hhu\n",
		  req->master_driver_init_complete);

	memset(&rsp, 0, sizeof(rsp));
	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_INDICATION_REGISTER,
				IPA_QMI_INDICATION_REGISTER_RSP_SZ,
				ipa_indication_register_rsp_ei, &rsp);
	if (ret) {
		ipa_err("error %d sending response\n", ret);
		return;
	}

	ret = ipa_handshake_complete(qmi, sq, false);
	if (ret)
		ipa_err("error %d completing handshake\n", ret);
}

/* Callback function to handle a DRIVER_INIT_COMPLETE request message
 * from the modem.  This informs the AP that the modem has completed
 * the initializion of its driver.
 */
static void ipa_driver_init_complete_fn(struct qmi_handle *qmi,
					struct sockaddr_qrtr *sq,
					struct qmi_txn *txn,
					const void *decoded)
{
	const struct ipa_driver_init_complete_req *req = decoded;
	struct ipa_driver_init_complete_rsp rsp;
	int ret;

	/* I'm not sure what value is provided in the status field
	 * (presumably it indicates success).  We ignore it though...
	 */
	ipa_debug("req->status = %hhu\n", req->status);

	memset(&rsp, 0, sizeof(rsp));
	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_DRIVER_INIT_COMPLETE,
				IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ,
				ipa_driver_init_complete_rsp_ei, &rsp);
	if (ret)
		ipa_err("error %d sending response\n", ret);
}

/* The server handles two request message types sent by the modem. */
static struct qmi_msg_handler ipa_server_msg_handlers[] = {
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_INDICATION_REGISTER,
		.ei		= ipa_indication_register_req_ei,
		.decoded_size	= IPA_QMI_INDICATION_REGISTER_REQ_SZ,
		.fn		= ipa_indication_register_fn,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_DRIVER_INIT_COMPLETE,
		.ei		= ipa_driver_init_complete_req_ei,
		.decoded_size	= IPA_QMI_DRIVER_INIT_COMPLETE_REQ_SZ,
		.fn		= ipa_driver_init_complete_fn,
	},
};

/* Callback function to handle an IPA_QMI_INIT_DRIVER response message
 * from the modem.  This only acknowledges that the modem received the
 * request.  The modem will eventually report that it has completed its
 * modem initialization by sending a IPA_QMI_DRIVER_INIT_COMPLETE request.
 */
static void ipa_init_driver_rsp_fn(struct qmi_handle *qmi,
				   struct sockaddr_qrtr *sq,
				   struct qmi_txn *txn,
				   const void *decoded)
{
	int ret;

	kfree(txn);

	ret = ipa_handshake_complete(qmi, sq, true);
	if (ret)
		ipa_err("error %d completing handshake\n", ret);
}

/* The client handles one response message type sent by the modem. */
static struct qmi_msg_handler ipa_client_msg_handlers[] = {
	{
		.type		= QMI_RESPONSE,
		.msg_id		= IPA_QMI_INIT_DRIVER,
		.ei		= ipa_init_modem_driver_rsp_ei,
		.decoded_size	= IPA_QMI_INIT_DRIVER_RSP_SZ,
		.fn		= ipa_init_driver_rsp_fn,
	},
};

/* Return a pointer to an init modem driver request structure, which
 * contains configuration parameters for the modem.  The modem may
 * be started multiple times, but generally these parameters don't
 * change so we can reuse the request structure once it's initialized.
 * The only exception is the skip_uc_load field, which will be set
 * only after the microcontroller has reported it has completed its
 * initialization.
 */
static const struct ipa_init_modem_driver_req *init_modem_driver_req(void)
{
	static struct ipa_init_modem_driver_req req;
	u32 base;

	/* This is not the first boot if the microcontroller is loaded */
	req.skip_uc_load_valid = ipa_ctx->uc_ctx.uc_loaded ? 1 : 0;
	req.skip_uc_load = req.skip_uc_load_valid;

	/* We only have to initialize most of it once */
	if (req.platform_type_valid)
		return &req;

	/* All offsets are relative to the start of IPA shared memory */
	base = (u32)ipa_ctx->smem_restricted_bytes;

	req.platform_type_valid = true;
	req.platform_type = IPA_QMI_PLATFORM_TYPE_MSM_ANDROID;

	req.hdr_tbl_info_valid = ipa_ctx->mem_info[MODEM_HDR_SIZE] ? 1 : 0;
	req.hdr_tbl_info.start = base + ipa_ctx->mem_info[MODEM_HDR_OFST];
	req.hdr_tbl_info.end = req.hdr_tbl_info.start +
					ipa_ctx->mem_info[MODEM_HDR_SIZE] - 1;

	req.v4_route_tbl_info_valid = true;
	req.v4_route_tbl_info.start =
			base + ipa_ctx->mem_info[V4_RT_NHASH_OFST];
	req.v4_route_tbl_info.count = ipa_ctx->mem_info[V4_MODEM_RT_INDEX_HI];

	req.v6_route_tbl_info_valid = true;
	req.v6_route_tbl_info.start =
			base + ipa_ctx->mem_info[V6_RT_NHASH_OFST];
	req.v6_route_tbl_info.count = ipa_ctx->mem_info[V6_MODEM_RT_INDEX_HI];

	req.v4_filter_tbl_start_valid = true;
	req.v4_filter_tbl_start = base + ipa_ctx->mem_info[V4_FLT_NHASH_OFST];

	req.v6_filter_tbl_start_valid = true;
	req.v6_filter_tbl_start = base + ipa_ctx->mem_info[V6_FLT_NHASH_OFST];

	req.modem_mem_info_valid = ipa_ctx->mem_info[MODEM_SIZE] ? 1 : 0;
	req.modem_mem_info.start = base + ipa_ctx->mem_info[MODEM_OFST];
	req.modem_mem_info.size = ipa_ctx->mem_info[MODEM_SIZE];

	req.ctrl_comm_dest_end_pt_valid = true;
	req.ctrl_comm_dest_end_pt =
			ipa_get_ep_mapping(IPA_CLIENT_APPS_WAN_CONS);

	req.hdr_proc_ctx_tbl_info_valid =
			ipa_ctx->mem_info[MODEM_HDR_PROC_CTX_SIZE] ? 1 : 0;
	req.hdr_proc_ctx_tbl_info.start =
			base + ipa_ctx->mem_info[MODEM_HDR_PROC_CTX_OFST];
	req.hdr_proc_ctx_tbl_info.end = req.hdr_proc_ctx_tbl_info.start +
			ipa_ctx->mem_info[MODEM_HDR_PROC_CTX_SIZE] - 1;

	req.v4_hash_route_tbl_info_valid = true;
	req.v4_hash_route_tbl_info.start =
			base + ipa_ctx->mem_info[V4_RT_HASH_OFST];
	req.v4_hash_route_tbl_info.count =
			ipa_ctx->mem_info[V4_MODEM_RT_INDEX_HI];

	req.v6_hash_route_tbl_info_valid = true;
	req.v6_hash_route_tbl_info.start =
			base + ipa_ctx->mem_info[V6_RT_HASH_OFST];
	req.v6_hash_route_tbl_info.count =
			ipa_ctx->mem_info[V6_MODEM_RT_INDEX_HI];

	req.v4_hash_filter_tbl_start_valid = true;
	req.v4_hash_filter_tbl_start =
			base + ipa_ctx->mem_info[V4_FLT_HASH_OFST];

	req.v6_hash_filter_tbl_start_valid = true;
	req.v6_hash_filter_tbl_start =
			base + ipa_ctx->mem_info[V6_FLT_HASH_OFST];

	return &req;
}

/* The modem service we requested is now available via the client
 * handle.  Send an INIT_DRIVER request to the modem.
 */
static int
ipa_client_new_server(struct qmi_handle *qmi, struct qmi_service *svc)
{
	const struct ipa_init_modem_driver_req *req = init_modem_driver_req();
	struct sockaddr_qrtr sq;
	struct qmi_txn *txn;
	int ret;

	txn = kzalloc(sizeof(*txn), GFP_KERNEL);
	if (!txn)
		return -ENOMEM;

	ret = qmi_txn_init(qmi, txn, NULL, NULL);
	if (ret) {
		kfree(txn);
		return ret;
	}

	sq.sq_family = AF_QIPCRTR;
	sq.sq_node = svc->node;
	sq.sq_port = svc->port;

	ret = qmi_send_request(qmi, &sq, txn, IPA_QMI_INIT_DRIVER,
			       IPA_QMI_INIT_DRIVER_REQ_SZ,
			       ipa_init_modem_driver_req_ei, req);
	if (ret) {
		qmi_txn_cancel(txn);
		kfree(txn);
	}

	return ret;
}

/* The only callback we supply for the client handle is notification
 * that the service on the modem has become available.
 */
static struct qmi_ops ipa_client_ops = {
	.new_server	= ipa_client_new_server,
};

static int ipa_qmi_initialize(void)
{
	int ret;

	/* The only handle operation that might be interesting for the
	 * server would be del_client, to find out when the modem side
	 * client has disappeared.  But other than reporting the event,
	 * we wouldn't do anything about that.  So we just pass a null
	 * pointer for its handle operations.  All the real work is
	 * done by the message handlers.
	 */
	ret = qmi_handle_init(&server_handle, IPA_QMI_SERVER_MAX_RCV_SZ,
			      NULL, ipa_server_msg_handlers);
	if (ret < 0)
		return ret;

	ret = qmi_add_server(&server_handle, IPA_HOST_SERVICE_SVC_ID,
			     IPA_HOST_SVC_VERS, IPA_HOST_SERVICE_INS_ID);
	if (ret < 0)
		goto err_release_server_handle;

	/* The client handle is only used for sending an INIT_DRIVER
	 * request to the modem, and receiving its response message.
	 */
	ret = qmi_handle_init(&client_handle, IPA_QMI_CLIENT_MAX_RCV_SZ,
			      &ipa_client_ops, ipa_client_msg_handlers);
	if (ret < 0)
		goto err_release_server_handle;

	ret = qmi_add_lookup(&client_handle, IPA_MODEM_SERVICE_SVC_ID,
			     IPA_MODEM_SVC_VERS, IPA_MODEM_SERVICE_INS_ID);
	if (ret < 0)
		goto err_release_client_handle;

	ipa_qmi_initialized = true;

	return 0;

err_release_client_handle:
	/* Releasing the handle also removes registered lookups */
	qmi_handle_release(&client_handle);
	memset(&client_handle, 0, sizeof(client_handle));
err_release_server_handle:
	/* Releasing the handle also removes registered services */
	qmi_handle_release(&server_handle);
	memset(&server_handle, 0, sizeof(server_handle));

	return ret;
}

/* This is called by the rmnet probe routine.  The rmnet driver can
 * be unregistered after it has been initialized as a result of a
 * subsystem shutdown; it can later be registered again if a
 * subsystem restart occurs.  This function can therefore be called
 * more than once.
 */
int ipa_qmi_init(void)
{
	init_driver_response_received = false;
	indication_register_received = false;

	if (!ipa_qmi_initialized)
		return ipa_qmi_initialize();

	return 0;
}

void ipa_qmi_exit(void)
{
	if (!ipa_qmi_initialized)
		return;

	qmi_handle_release(&client_handle);
	memset(&client_handle, 0, sizeof(client_handle));

	qmi_handle_release(&server_handle);
	memset(&server_handle, 0, sizeof(server_handle));

	ipa_qmi_initialized = false;
}
