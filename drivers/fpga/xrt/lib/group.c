// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Group Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 *	Lizhi Hou <lizhih@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/fpga-xrt.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include "group.h"
#include "subdev_pool.h"
#include "lib-drv.h"

#define XRT_GRP "xrt_group"

struct xrt_group {
	struct xrt_device *xdev;
	struct xrt_subdev_pool leaves;
	bool leaves_created;
	struct mutex lock; /* lock for group */
	struct device_node *dt;
	void *dt_mem;
};

static int xrt_grp_root_cb(struct device *dev, void *parg,
			   enum xrt_root_cmd cmd, void *arg)
{
	int rc;
	struct xrt_device *xdev =
		container_of(dev, struct xrt_device, dev);
	struct xrt_group *xg = (struct xrt_group *)parg;

	switch (cmd) {
	case XRT_ROOT_GET_LEAF_HOLDERS: {
		struct xrt_root_get_holders *holders =
			(struct xrt_root_get_holders *)arg;
		rc = xrt_subdev_pool_get_holders(&xg->leaves,
						 holders->xpigh_xdev,
						 holders->xpigh_holder_buf,
						 holders->xpigh_holder_buf_len);
		break;
	}
	default:
		/* Forward parent call to root. */
		rc = xrt_subdev_root_request(xdev, cmd, arg);
		break;
	}

	return rc;
}

static struct device_node *xrt_subdev_match_drv(struct xrt_group *xg, struct xrt_driver *drv)
{
	struct device_node *eps, *dn = NULL;
	const __be32 *pf_p;
	int len;

	eps = of_find_node_by_name(xg->dt, XRT_MD_NODE_ENDPOINTS);
	if (!eps) {
		xrt_err(xg->xdev, "Does not find %s", XRT_MD_NODE_ENDPOINTS);
		return NULL;
	}

	for (dn = of_get_next_child(eps, dn); dn; dn = of_get_next_child(eps, dn)) {
		pf_p = of_get_property(dn, "pcie_physical_function", &len);
		if (pf_p && be32_to_cpu(*pf_p))
			continue;

		pr_info("NODE NAME %s\n", of_node_full_name(dn));
		if (of_match_node(drv->driver.of_match_table, dn))
			return dn;
	}

	return NULL;
}

static int xrt_grp_create_leaves(struct xrt_group *xg)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(xg->xdev);
	struct device *dev = DEV(xg->xdev);
	struct device_node *dn = NULL;
	const char *ep_name = NULL;
	int ret = 0, failed = 0;
	enum xrt_subdev_id did;
	struct xrt_driver *drv;
	void *dev_md = NULL;
	u64 prop_did;
	bool found;
	void *dtb;
	u32 len;

	if (!pdata)
		return -EINVAL;

	mutex_lock(&xg->lock);

	if (xg->leaves_created) {
		/*
		 * This is expected since caller does not keep track of the state of the group
		 * and may, in some cases, still try to create leaves after it has already been
		 * created. This special error code will let the caller know what is going on.
		 */
		mutex_unlock(&xg->lock);
		return -EEXIST;
	}

	/* Create all leaves based on metadata */
	xrt_info(xg->xdev, "bringing up leaves...");
	ret = xrt_md_get_prop(dev, pdata->xsp_data, XRT_MD_NODE_DTB, XRT_MD_PROP_PRIV_DATA,
			      (u64 *)&dtb, NULL);
	if (!ret) {
		xg->dt_mem = of_fdt_unflatten_tree(dtb, NULL, &xg->dt);
		if (!xg->dt_mem) {
			xrt_info(xg->xdev, "failed to unflatten tree");
			return -EINVAL;
		}
	}

	for (did = 0; did < XRT_SUBDEV_NUM; did++) {
		drv = xrt_drv_get(did);
		if (!drv)
			continue;

		found = false;
		for_each_endpoint(dev, pdata->xsp_data, ep_name) {
			ret = xrt_md_get_prop(dev, pdata->xsp_data, ep_name, XRT_MD_PROP_DEVICE_ID,
					      &prop_did, NULL);
			if (!ret && prop_did == did) {
				found = true;
				break;
			}
		}
		if (found) {
			len = 0;
			xrt_md_get_prop(dev, pdata->xsp_data, ep_name, XRT_MD_PROP_PRIV_DATA,
					NULL, &len);
			ret = xrt_md_create(dev, 1, len, &dev_md);
			if (ret) {
				xrt_err(xg->xdev, "create device metadata for %s failed, ret %d",
					ep_name, ret);
				failed++;
				xrt_drv_put(did);
				continue;
			}
			ret = xrt_md_copy_endpoint(dev, pdata->xsp_data, ep_name, dev_md);
			if (ret) {
				xrt_err(xg->xdev, "copy device metadata for %s failed, ret %d",
					ep_name, ret);
				failed++;
				xrt_drv_put(did);
				continue;
			}
		} else if (xg->dt && drv->driver.of_match_table) {
			dn = xrt_subdev_match_drv(xg, drv);
			if (!dn) {
				xrt_drv_put(did);
				continue;
			}
		} else {
			xrt_drv_put(did);
			continue;
		}

		ret = xrt_subdev_pool_add(&xg->leaves, did, xrt_grp_root_cb, xg, dev_md, dn);
		vfree(dev_md);
		dev_md = NULL;
		if (ret < 0) {
			/*
			 * It is not a fatal error here. Some functionality is not usable
			 * due to this missing device, but the error can be handled
			 * when the functionality is used.
			 */
			failed++;
			xrt_drv_put(did);
			xrt_err(xg->xdev, "failed to add %s: %d", xrt_drv_name(did), ret);
		}
	}

	xg->leaves_created = true;
	vfree(dev_md);
	mutex_unlock(&xg->lock);

	if (failed)
		return -ECHILD;

	return 0;
}

static void xrt_grp_remove_leaves(struct xrt_group *xg)
{
	mutex_lock(&xg->lock);

	if (!xg->leaves_created) {
		mutex_unlock(&xg->lock);
		return;
	}

	xrt_info(xg->xdev, "tearing down leaves...");
	xrt_subdev_pool_fini(&xg->leaves);
	xg->leaves_created = false;

	mutex_unlock(&xg->lock);
}

static int xrt_grp_probe(struct xrt_device *xdev)
{
	struct xrt_group *xg;

	xrt_info(xdev, "probing...");

	xg = devm_kzalloc(&xdev->dev, sizeof(*xg), GFP_KERNEL);
	if (!xg)
		return -ENOMEM;

	xg->xdev = xdev;
	mutex_init(&xg->lock);
	xrt_subdev_pool_init(DEV(xdev), &xg->leaves);
	xrt_set_drvdata(xdev, xg);

	return 0;
}

static void xrt_grp_remove(struct xrt_device *xdev)
{
	struct xrt_group *xg = xrt_get_drvdata(xdev);

	xrt_info(xdev, "leaving...");
	xrt_grp_remove_leaves(xg);

	kfree(xg->dt_mem);
}

static int xrt_grp_leaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	int rc = 0;
	struct xrt_group *xg = xrt_get_drvdata(xdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Simply forward to every child. */
		xrt_subdev_pool_handle_event(&xg->leaves,
					     (struct xrt_event *)arg);
		break;
	case XRT_GROUP_GET_LEAF: {
		struct xrt_root_get_leaf *get_leaf =
			(struct xrt_root_get_leaf *)arg;

		rc = xrt_subdev_pool_get(&xg->leaves, get_leaf->xpigl_match_cb,
					 get_leaf->xpigl_match_arg,
					 DEV(get_leaf->xpigl_caller_xdev),
					 &get_leaf->xpigl_tgt_xdev);
		break;
	}
	case XRT_GROUP_PUT_LEAF: {
		struct xrt_root_put_leaf *put_leaf =
			(struct xrt_root_put_leaf *)arg;

		rc = xrt_subdev_pool_put(&xg->leaves, put_leaf->xpipl_tgt_xdev,
					 DEV(put_leaf->xpipl_caller_xdev));
		break;
	}
	case XRT_GROUP_INIT_CHILDREN:
		rc = xrt_grp_create_leaves(xg);
		break;
	case XRT_GROUP_FINI_CHILDREN:
		xrt_grp_remove_leaves(xg);
		break;
	case XRT_GROUP_TRIGGER_EVENT:
		xrt_subdev_pool_trigger_event(&xg->leaves, (enum xrt_events)(uintptr_t)arg);
		break;
	default:
		xrt_err(xdev, "unknown IOCTL cmd %d", cmd);
		rc = -EINVAL;
		break;
	}
	return rc;
}

static struct xrt_driver xrt_group_driver = {
	.driver	= {
		.name    = XRT_GRP,
	},
	.subdev_id = XRT_SUBDEV_GRP,
	.probe = xrt_grp_probe,
	.remove = xrt_grp_remove,
	.leaf_call = xrt_grp_leaf_call,
};

XRT_LEAF_INIT_FINI_FUNC(group);
