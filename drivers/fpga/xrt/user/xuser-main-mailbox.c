// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Peer communication via mailbox
 *
 * Authors:
 *      Lizhi Hou <lizhih@xilinx.com>
 */

#include <linux/io.h>
#include <linux/fpga-xrt-mailbox.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/xrt/xleaf.h>
#include <linux/xrt/xleaf/mailbox.h>
#include "xuser.h"

#define XUSER_MSG_SUBDEV_DATA_LEN	(512 * 1024)

struct xuser_mailbox {
	struct xrt_device *xdev;
	struct xrt_device *mailbox;
	struct mutex lock; /* lock for xuser_mailbox */
	char *test_msg;
};

static inline const char *mailbox_chan2name(bool sw_ch)
{
	return sw_ch ? "SW" : "HW";
}

static inline void xuser_mailbox_prt_req(struct xuser_mailbox *xmbx, bool send,
                                         struct xcl_mailbox_req *request, bool sw_ch)
{
	const char *dir = send ? ">>>" : "<<<";

	if (request->req == XCL_MAILBOX_REQ_PEER_DATA) {
		struct xcl_mailbox_peer_data *p = (struct xcl_mailbox_peer_data *)request->data;

		xrt_info(xmbx->xdev, "%s(%s) %s%s%s", mailbox_req2name(request->req),
			 mailbox_group_kind2name(p->kind), dir, mailbox_chan2name(sw_ch), dir);
	} else {
		xrt_info(xmbx->xdev, "%s %s%s%s", mailbox_req2name(request->req),
			 dir, mailbox_chan2name(sw_ch), dir);
	}
}

#define XUSER_MAILBOX_PRT_REQ_SEND(xmbx, req, sw_ch)			\
	xuser_mailbox_prt_req(xmbx, true, req, sw_ch)
#define XUSER_MAILBOX_PRT_REQ_RECV(xmbx, req, sw_ch)			\
	xuser_mailbox_prt_req(xmbx, false, req, sw_ch)

static inline void xuser_mailbox_prt_resp(struct xuser_mailbox *xmbx,
					  struct xrt_mailbox_post *resp)
{
	xrt_info(xmbx->xdev, "respond %zu bytes >>>%s>>>", resp->xmip_data_size,
		 mailbox_chan2name((resp)->xmip_sw_ch));
}

static inline struct xuser_mailbox *xdev2mbx(struct xrt_device *xdev)
{
	return (struct xuser_mailbox *)xuser_xdev2mailbox(xdev);
}

static void xuser_mailbox_post(struct xuser_mailbox *xmbx,
			       u64 msgid, bool sw_ch, void *buf, size_t len)
{
	struct xrt_mailbox_post post = { 0 };
	int rc;

	WARN_ON(!mutex_is_locked(&xmbx->lock));
	if (!xmbx->mailbox) {
		xrt_err(xmbx->xdev, "mailbox not available");
		return;
	}

	if (msgid == 0)
		XUSER_MAILBOX_PRT_REQ_SEND(xmbx, (struct xcl_mailbox_req *)buf, sw_ch);
	else
		xuser_mailbox_prt_resp(xmbx, &post);

	post.xmip_req_id = msgid;
	post.xmip_sw_ch = sw_ch;
	post.xmip_data = buf;
	post.xmip_data_size = len;

	rc = xleaf_call(xmbx->mailbox, XRT_MAILBOX_POST, &post);
	if (rc && rc != -ESHUTDOWN)
		xrt_err(xmbx->xdev, "failed to post msg: %d", rc);
}

static int xuser_mailbox_request(struct xuser_mailbox *xmbx,
				 void *request, size_t request_len,
				 void *response, size_t response_len,
				 size_t *actual_response_len)
{
	struct xrt_mailbox_request req = { 0 };
	int rc;

	*actual_response_len = 0;
	if (!xmbx->mailbox) {
		xrt_err(xmbx->xdev, "mailbox not available");
		return -ENODEV;
	}

	req.xmir_sw_ch = false;
	req.xmir_resp_ttl = 1;
	req.xmir_req = request;
	req.xmir_req_size = request_len;
	req.xmir_resp = response;
	req.xmir_resp_size = response_len;

	mutex_lock(&xmbx->lock);
	XUSER_MAILBOX_PRT_REQ_SEND(xmbx, (struct xcl_mailbox_req *)request, req.xmir_sw_ch);
	rc = xleaf_call(xmbx->mailbox, XRT_MAILBOX_REQUEST, &req);
	mutex_unlock(&xmbx->lock);

	if (!rc)
		*actual_response_len = req.xmir_resp_size;

	return rc;
}

static void xuser_mailbox_respond(struct xuser_mailbox *xmbx,
				  u64 msgid, bool sw_ch, void *buf, size_t len)
{
	mutex_lock(&xmbx->lock);
	xuser_mailbox_post(xmbx, msgid, sw_ch, buf, len);
	mutex_unlock(&xmbx->lock);
}


static void xuser_mailbox_resp_test_msg(struct xuser_mailbox *xmbx, u64 msgid, bool sw_ch)
{
	struct xrt_device *xdev = xmbx->xdev;
	char *msg;

	mutex_lock(&xmbx->lock);

	if (!xmbx->test_msg) {
		mutex_unlock(&xmbx->lock);
		xrt_err(xdev, "test msg is not set, drop request");
		return;
	}
	msg = xmbx->test_msg;
	xmbx->test_msg = NULL;

	mutex_unlock(&xmbx->lock);

	xuser_mailbox_respond(xmbx, msgid, sw_ch, msg, strlen(msg) + 1);
	vfree(msg);
}

static void xuser_mailbox_proc_mgmt_state(struct xuser_mailbox *xmbx,
					  struct xcl_mailbox_req *req, u64 msgid, bool sw_ch)
{
	struct xrt_device *xdev = xmbx->xdev;
	struct xcl_mailbox_peer_state *st;

	st = (struct xcl_mailbox_peer_state *)req->data;
	if (st->state_flags & XCL_MB_STATE_ONLINE)
		xleaf_broadcast_event(xdev, XRT_EVENT_PEER_ONLINE, true);
	else if (st->state_flags & XCL_MB_STATE_OFFLINE)
		xleaf_broadcast_event(xdev, XRT_EVENT_PEER_OFFLINE, true);
}

static void xuser_mailbox_listener(void *arg, void *data, size_t len,
				   u64 msgid, int err, bool sw_ch)
{
	struct xuser_mailbox *xmbx = (struct xuser_mailbox *)arg;
	struct xrt_device *xdev = xmbx->xdev;
	struct xcl_mailbox_req *req;

	if (err) {
		xrt_err(xdev, "failed to receive request: %d", err);
		return;
	}
	if (len < sizeof(*req)) {
		xrt_err(xdev, "received corrupted request");
		return;
	}

	req = (struct xcl_mailbox_req *)data;
	XUSER_MAILBOX_PRT_REQ_RECV(xmbx, req, sw_ch);
	switch (req->req) {
	case XCL_MAILBOX_REQ_TEST_READ:
		/*
		 * there should not be any incoming request on userpf side.
		 * This is for test only.
		 */
		xuser_mailbox_resp_test_msg(xmbx, msgid, sw_ch);
		break;
	case XCL_MAILBOX_REQ_MGMT_STATE:
		xuser_mailbox_proc_mgmt_state(xmbx, req, msgid, sw_ch);
		break;
	default:
		xrt_err(xdev, "%s(%d) request not handled", mailbox_req2name(req->req), req->req);
		break;
	}
}

static void xuser_mailbox_reg_listener(struct xuser_mailbox *xmbx)
{
	struct xrt_mailbox_listen listen = { xuser_mailbox_listener, xmbx };

	WARN_ON(!mutex_is_locked(&xmbx->lock));
	if (!xmbx->mailbox)
		return;
	xleaf_call(xmbx->mailbox, XRT_MAILBOX_LISTEN, &listen);
}

static void xuser_mailbox_unreg_listener(struct xuser_mailbox *xmbx)
{
	struct xrt_mailbox_listen listen = { 0 };

	WARN_ON(!mutex_is_locked(&xmbx->lock));
	WARN_ON(!xmbx->mailbox);
	xleaf_call(xmbx->mailbox, XRT_MAILBOX_LISTEN, &listen);
}

void xuser_mailbox_event_cb(struct xrt_device *xdev, void *arg)
{
	struct xrt_event *evt = (struct xrt_event *)arg;
	struct xuser_mailbox *xmbx = xdev2mbx(xdev);
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id;

	id = evt->xe_subdev.xevt_subdev_id;
	if (id != XRT_SUBDEV_MAILBOX)
		return;

	switch (e) {
	case XRT_EVENT_POST_CREATION:
		WARN_ON(xmbx->mailbox);
		mutex_lock(&xmbx->lock);
		xmbx->mailbox = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_MAILBOX,
						     XRT_INVALID_DEVICE_INST);
		xuser_mailbox_reg_listener(xmbx);
		mutex_unlock(&xmbx->lock);
		break;
	case XRT_EVENT_PRE_REMOVAL:
		WARN_ON(!xmbx->mailbox);
		mutex_lock(&xmbx->lock);
		xuser_mailbox_unreg_listener(xmbx);
		xleaf_put_leaf(xdev, xmbx->mailbox);
		xmbx->mailbox = NULL;
		mutex_unlock(&xmbx->lock);
		break;
	default:
		break;
	}
}

static int xuser_mailbox_get_test_msg(struct xuser_mailbox *xmbx, bool sw_ch,
				      char *buf, size_t *len)
{
        struct xcl_mailbox_req req = { 0, XCL_MAILBOX_REQ_TEST_READ, };
        struct xrt_mailbox_request leaf_req = { 0 };
        struct xrt_device *xdev = xmbx->xdev;
        int rc;

	leaf_req.xmir_sw_ch = sw_ch;
	leaf_req.xmir_resp_ttl = 1;
	leaf_req.xmir_req = &req;
	leaf_req.xmir_req_size = sizeof(req);
	leaf_req.xmir_resp = buf;
	leaf_req.xmir_resp_size = *len;

	mutex_lock(&xmbx->lock);
	if (xmbx->mailbox) {
		XUSER_MAILBOX_PRT_REQ_SEND(xmbx, &req, leaf_req.xmir_sw_ch);
		rc = xleaf_call(xmbx->mailbox, XRT_MAILBOX_REQUEST, &leaf_req);
	} else {
		rc = -ENODEV;
		xrt_err(xdev, "mailbox not available");
	}
	mutex_unlock(&xmbx->lock);

	if (rc == 0)
		*len = leaf_req.xmir_resp_size;
	return rc;
}

static int xuser_mailbox_set_test_msg(struct xuser_mailbox *xmbx, char *buf, size_t len)
{
	mutex_lock(&xmbx->lock);

	if (xmbx->test_msg)
		vfree(xmbx->test_msg);
	xmbx->test_msg = vmalloc(len);
	if (!xmbx->test_msg) {
		mutex_unlock(&xmbx->lock);
		return -ENOMEM;
	}
	memcpy(xmbx->test_msg, buf, len);

	mutex_unlock(&xmbx->lock);
	return 0;
}

static ssize_t peer_msg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xuser_mailbox *xmbx;
	size_t len = 4096;
	int ret;

	xmbx = xdev2mbx(xdev);
	ret = xuser_mailbox_get_test_msg(xmbx, false, buf, &len);
	if (ret)
		return -EINVAL;

	return len;
}

static ssize_t peer_msg_store(struct device *dev,
			      struct device_attribute *da, const char *buf, size_t count)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xuser_mailbox *xmbx;
	int ret;

	xmbx = xdev2mbx(xdev);
	ret = xuser_mailbox_set_test_msg(xmbx, (char *)buf, count);
	if (ret)
		return -EINVAL;

	return count;
}

/* Message test i/f. */
static DEVICE_ATTR_RW(peer_msg);

static struct attribute *xuser_mailbox_attrs[] = {
        &dev_attr_peer_msg.attr,
        NULL,
};

static const struct attribute_group xuser_mailbox_attrgroup = {
        .attrs = xuser_mailbox_attrs,
};

int xuser_peer_get_metadata(void *handle, char **dtb)
{
	size_t data_len, reqlen, resp_len, actual_resp_len, offset = 0;
	struct xuser_mailbox *xmbx = (struct xuser_mailbox *)handle;
	struct xcl_mailbox_peer_data subdev_peer = {0};
	struct xcl_mailbox_req *mb_req = NULL;
	struct xrt_device *xdev = xmbx->xdev;
	struct xcl_subdev *resp = NULL;
	char *tmp;
	int ret;

	*dtb = NULL;

	data_len = sizeof(struct xcl_mailbox_peer_data);
	reqlen = sizeof(struct xcl_mailbox_req) + data_len;
	mb_req = kzalloc(reqlen, GFP_KERNEL);
	if (!mb_req) {
		ret = -ENOMEM;
		goto out;
	}

	resp_len = sizeof(*resp) + XUSER_MSG_SUBDEV_DATA_LEN;
	resp = vzalloc(resp_len);
	if (!resp) {
		ret = -ENOMEM;
		goto out;
	}

	mb_req->req = XCL_MAILBOX_REQ_PEER_DATA;

	subdev_peer.size = resp_len;
	subdev_peer.kind = XCL_SUBDEV;
	subdev_peer.entries = 1;
	memcpy(mb_req->data, &subdev_peer, data_len);

	do {
		tmp = vzalloc(offset + resp_len);
		if (!tmp) {
			ret = -ENOMEM;
			goto out;
		}

		if (*dtb) {
			memcpy(tmp, *dtb, offset);
			vfree(*dtb);
		}
		*dtb = tmp;

		subdev_peer.offset = offset;
		ret = xuser_mailbox_request(xmbx, mb_req, reqlen, resp, resp_len, &actual_resp_len);
		if (ret)
			goto out;
		if (offset != resp->offset) {
			ret = -EINVAL;
			goto out;
		}

		memcpy(*dtb, resp->data, resp->size);
		offset += resp->size;
	} while (resp->rtncode == XRT_MSG_SUBDEV_RTN_PARTIAL);

	if (resp->rtncode != XRT_MSG_SUBDEV_RTN_COMPLETE) {
		xrt_err(xdev, "invalid return code %d", resp->rtncode);
		ret = -EINVAL;
		goto out;
	}

out:
	if (ret || !offset) {
		vfree(*dtb);
		*dtb = NULL;
	}
	kfree(mb_req);
	vfree(resp);

	return ret;
}

void *xuser_mailbox_probe(struct xrt_device *xdev)
{
	struct xuser_mailbox *xmbx = devm_kzalloc(DEV(xdev), sizeof(*xmbx), GFP_KERNEL);
	int ret;

	if (!xmbx)
		return NULL;
	xmbx->xdev = xdev;
	mutex_init(&xmbx->lock);

	ret = sysfs_create_group(&DEV(xdev)->kobj, &xuser_mailbox_attrgroup);
	if (ret) {
		xrt_err(xdev, "create sysfs group failed, ret %d", ret);
		return NULL;
	}

	return xmbx;
}

void xuser_mailbox_remove(void *handle)
{
	struct xuser_mailbox *xmbx = (struct xuser_mailbox *)handle;
	struct xrt_device *xdev = xmbx->xdev;

	sysfs_remove_group(&DEV(xdev)->kobj, &xuser_mailbox_attrgroup);
	if (xmbx->mailbox)
		xleaf_put_leaf(xdev, xmbx->mailbox);
}
