// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA USER PF entry point driver
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <lizhih@xilinx.com>
 */

#include <linux/xrt/subdev_id.h>
#include <linux/xrt/metadata.h>
#include <linux/xrt/xdevice.h>
#include <linux/xrt/xleaf.h>
#include <linux/slab.h>
#include <linux/xrt/xleaf/xdma.h>
#include "xuser.h"

#define XUSER_MAIN "xuser_main"
#define XUSER_UUID_STR_LEN	(UUID_SIZE * 2 + 1)

struct xuser_main {
	struct xrt_device *xdev;
	char *firmware_dtb;
	int firmware_group_instance;
	void *mailbox_hdl;
	uuid_t logic_uuid;
	uuid_t *interface_uuids;
	u32 interface_uuid_num;
};

/* logic uuid is the uuid uniquely identfy the partition */
static ssize_t logic_uuids_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	char uuid[XUSER_UUID_STR_LEN];
	struct xuser_main *xum;

	xum = xrt_get_drvdata(xdev);
	xrt_md_trans_uuid2str(&xum->logic_uuid, uuid);
	return sprintf(buf, "%s\n", uuid);
}
static DEVICE_ATTR_RO(logic_uuids);

static ssize_t interface_uuids_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xuser_main *xum;
	ssize_t count = 0;
	int i;

	xum = xrt_get_drvdata(xdev);
	for (i = 0; i < xum->interface_uuid_num; i++) {
		char uuidstr[XUSER_UUID_STR_LEN];

		xrt_md_trans_uuid2str(&xum->interface_uuids[i], uuidstr);
		count += sprintf(buf + count, "%s\n", uuidstr);
	}

        return count;
}
static DEVICE_ATTR_RO(interface_uuids);

static struct attribute *xuser_main_attrs[] = {
	&dev_attr_logic_uuids.attr,
	&dev_attr_interface_uuids.attr,
	NULL,
};

static const struct attribute_group xuser_main_attrgroup = {
	.attrs = xuser_main_attrs,
};

static int xuser_refresh_firmware(struct xuser_main *xum)
{
	struct xrt_device *xdev = xum->xdev;
	const void *uuid = NULL;
	char *dtb;
	int ret;

	ret = xuser_peer_get_metadata(xum->mailbox_hdl, &dtb);
	if (ret) {
		xrt_err(xdev, "failed to get metadata, ret %d", ret);
		return ret;
	}

	if (xum->firmware_group_instance != XRT_INVALID_DEVICE_INST) {
		ret = xleaf_destroy_group(xdev, xum->firmware_group_instance);
		if (ret) {
			xrt_err(xdev, "failed to remove current group %d, ret %d",
				xum->firmware_group_instance, ret);
			return ret;
		}
		xum->firmware_group_instance = XRT_INVALID_DEVICE_INST;

		WARN_ON(!xum->firmware_dtb);
		vfree(xum->firmware_dtb);
		xum->firmware_dtb = NULL;
	}

	WARN_ON(xum->firmware_dtb);
	if (dtb) {
		ret = xleaf_create_group(xdev, dtb);
		if (ret < 0) {
			xrt_err(xdev, "failed to create group, ret %d", ret);
			return ret;
		}
		xum->firmware_group_instance = ret;
		xum->firmware_dtb = dtb;

		ret = xrt_md_get_prop(&xdev->dev, dtb, NULL, NULL, XRT_MD_PROP_LOGIC_UUID,
				      &uuid, NULL);
		if (!ret)
			xrt_md_trans_str2uuid(&xdev->dev, uuid, &xum->logic_uuid);

		ret = xrt_md_get_interface_uuids(&xdev->dev, dtb, 0, NULL);
		if (ret > 0) {
			xum->interface_uuid_num = ret;
			xum->interface_uuids = kcalloc(xum->interface_uuid_num, sizeof(uuid_t),
						       GFP_KERNEL);
			if (!xum->interface_uuids)
				return -ENOMEM;
			xrt_md_get_interface_uuids(&xdev->dev, dtb, xum->interface_uuid_num,
						   xum->interface_uuids);
		}
	}

	return 0;
}

static void xuser_main_event_cb(struct xrt_device *xdev, void *arg)
{
	struct xrt_event *evt = (struct xrt_event *)arg;
	struct xuser_main *xum = xrt_get_drvdata(xdev);
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id;

	id = evt->xe_subdev.xevt_subdev_id;
	switch (e) {
	case XRT_EVENT_POST_CREATION:
		/* user driver finishes attaching, get its partition metadata */
		if (id == XRT_ROOT)
			xuser_refresh_firmware(xum);
		break;
	case XRT_EVENT_PEER_ONLINE:
		xuser_refresh_firmware(xum);
		break;
	default:
		xrt_dbg(xdev, "ignored event %d", e);
		break;
	}
}

static int xuser_main_probe(struct xrt_device *xdev)
{
	struct xuser_main *xum;

	xrt_info(xdev, "probing...");

	xum = devm_kzalloc(DEV(xdev), sizeof(*xum), GFP_KERNEL);
	if (!xum)
		return -ENOMEM;

	xum->xdev = xdev;
	xum->firmware_group_instance = XRT_INVALID_DEVICE_INST;
	xrt_set_drvdata(xdev, xum);
	xum->mailbox_hdl = xuser_mailbox_probe(xdev);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(xdev)->kobj, &xuser_main_attrgroup))
		xrt_err(xdev, "failed to create sysfs group");

	return 0;
}

static void xuser_main_remove(struct xrt_device *xdev)
{
	struct xuser_main *xum = xrt_get_drvdata(xdev);

	/* By now, group driver should prevent any inter-leaf call. */

	xrt_info(xdev, "leaving...");

	xuser_mailbox_remove(xum->mailbox_hdl);
	sysfs_remove_group(&DEV(xdev)->kobj, &xuser_main_attrgroup);
}

static int xuser_mainleaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	int ret = 0;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		xuser_mailbox_event_cb(xdev, arg);
		xuser_main_event_cb(xdev, arg);
		break;
	default:
		xrt_err(xdev, "unknown cmd: %d", cmd);
		ret = -EINVAL;
		break;
	}

	return ret;
}

void *xuser_xdev2mailbox(struct xrt_device *xdev)
{
	struct xuser_main *xum = xrt_get_drvdata(xdev);

	return xum->mailbox_hdl;
}

static int xuser_main_open(struct inode *inode, struct file *file)
{
#if 0
	struct xrt_device *xdev = xleaf_devnode_open(inode);
	struct xrt_device *xdma;
	int ret;

	xdma = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_XDMA, XRT_INVALID_DEVICE_INST);
	if (!xdma)
		return -ENODEV;

	ret = xleaf_call(xdma, XRT_XDMA_START, NULL);
	if (ret && ret != -ENODEV) {
		xrt_err(xdev, "failed to start xdma, ret %d", ret);
		return ret;
	}
	if (!ret)
		xleaf_put_leaf(xdev, xdma);
	file->private_data = xrt_get_drvdata(xdev);
#endif

	return 0;
}

static int xuser_main_close(struct inode *inode, struct file *file)
{
#if 0
	struct xuser_main *xum = file->private_data;
	struct xrt_device *xdev = xum->xdev;
	struct xrt_device *xdma;
	int ret;

	xleaf_devnode_close(inode);

	xdma = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_XDMA, XRT_INVALID_DEVICE_INST);
	if (!xdma)
		return -ENODEV;

	ret = xleaf_call(xdma, XRT_XDMA_STOP, NULL);
	if (ret && ret != -ENODEV) {
		xrt_err(xdev, "failed to stop xdma, ret %d", ret);
		return ret;
	}
	if (!ret)
		xleaf_put_leaf(xdev, xdma);
#endif

	return 0;
}

static long xuser_main_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static struct xrt_dev_endpoints xrt_user_main_endpoints[] = {
	{
		.xse_names = (struct xrt_dev_ep_names[]){
			{ .ep_name = XRT_MD_NODE_USER_MAIN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_driver xuser_main_driver = {
	.driver = {
		.name = XUSER_MAIN,
	},
	.file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = xuser_main_open,
			.release = xuser_main_close,
			.unlocked_ioctl = xuser_main_ioctl,
		},
		.xsf_dev_name = "xuser",
	},
	.subdev_id = XRT_SUBDEV_USER_MAIN,
	.endpoints = xrt_user_main_endpoints,
	.probe = xuser_main_probe,
	.remove = xuser_main_remove,
	.leaf_call = xuser_mainleaf_call,
};

int xuser_register_leaf(void)
{
	return xrt_register_driver(&xuser_main_driver);
}

void xuser_unregister_leaf(void)
{
	xrt_unregister_driver(&xuser_main_driver);
}
