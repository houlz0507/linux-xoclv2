// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Root Functions
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 *	Lizhi Hou <lizhih@xilinx.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/xrt/xdevice.h>
#include "xroot.h"

#define xroot_err(xr, fmt, args...) dev_err((xr)->dev, "%s: " fmt, __func__, ##args)
#define xroot_warn(xr, fmt, args...) dev_warn((xr)->dev, "%s: " fmt, __func__, ##args)
#define xroot_info(xr, fmt, args...) dev_info((xr)->dev, "%s: " fmt, __func__, ##args)
#define xroot_dbg(xr, fmt, args...) dev_dbg((xr)->dev, "%s: " fmt, __func__, ##args)

struct xroot_group {
	struct list_head node;
	struct xrt_device *grp_dev;
	struct property compatible;
	struct of_changeset chgset;
	bool chgset_applied;
	void *dn_mem;
};

struct xroot {
	struct device *dev;
	struct list_head groups;
};

#define XRT_GRP_NAME(grp)	((grp)->grp_dev->dev.of_node->full_name)

static void xroot_cleanup_group(struct xroot_group *grp)
{
	if (grp->grp_dev)
		xrt_device_unregister(grp->grp_dev);
	if (grp->chgset_applied) {
		pr_info("revert change set\n");
		of_changeset_revert(&grp->chgset);
	}
	of_changeset_destroy(&grp->chgset);

	kfree(grp->dn_mem);
}

void xroot_destroy_group(void *root, const char *name)
{
	struct xroot *xr = root;
	struct xroot_group *grp;
	const char *grp_name;

	list_for_each_entry(grp, &xr->groups, node) {
		grp_name = XRT_GRP_NAME(grp);
		if (!strncmp(grp_name, name, strlen(grp_name) + 1))
			break;
	}
	if (list_entry_is_head(grp, &xr->groups, node))
		return;

	list_del(&grp->node);

	xroot_cleanup_group(grp);
	kfree(grp);
}
EXPORT_SYMBOL_GPL(xroot_destroy_group);

int xroot_create_group(void *root, const char *name, void *dtb)
{
	struct device_node *dn, *bus, *grp_dn;
	struct xroot *xr = root;
	struct xroot_group *grp;
	int ret;

	grp = kzalloc(sizeof(*grp), GFP_KERNEL);
	if (!grp)
		return -ENOMEM;

	bus = of_find_node_by_path("/xrt-bus");
	if (!bus) {
		kfree(grp);
		return -EINVAL;
	}
	of_changeset_init(&grp->chgset);

	grp->dn_mem = of_fdt_unflatten_tree(dtb, NULL, &grp_dn);
	if (!grp->dn_mem) {
		ret = -EINVAL;
		goto failed;
	}

	grp_dn->full_name = name;
	grp_dn->parent = bus;
	for (dn = grp_dn; dn; dn = of_find_all_nodes(dn))
		of_changeset_attach_node(&grp->chgset, dn);

	grp->compatible.name = "compatible";
	grp->compatible.value = "xrt-group";
	grp->compatible.length = strlen(grp->compatible.value) + 1;
	ret = of_changeset_add_property(&grp->chgset, grp_dn, &grp->compatible);
	if (ret)
		goto failed;

	ret = of_changeset_apply(&grp->chgset);
	if (ret)
		goto failed;
	grp->chgset_applied = true;

	of_node_put(bus);
	bus = NULL;

	grp->grp_dev = xrt_device_register(xr->dev, grp_dn, NULL, 0, NULL, 0);
	if (!grp->grp_dev)
		goto failed;

	list_add(&grp->node, &xr->groups);

	return 0;

failed:
	if (bus)
		of_node_put(bus);
	xroot_cleanup_group(grp);
	return ret;
}
EXPORT_SYMBOL_GPL(xroot_create_group);

int xroot_probe(struct device *dev, void **root)
{
	struct xroot *xr = NULL;

	dev_info(dev, "%s: probing...", __func__);

	xr = devm_kzalloc(dev, sizeof(*xr), GFP_KERNEL);
	if (!xr)
		return -ENOMEM;

	xr->dev = dev;
	INIT_LIST_HEAD(&xr->groups);

	*root = xr;
	return 0;
}
EXPORT_SYMBOL_GPL(xroot_probe);

void xroot_remove(void *root)
{
	struct xroot *xr = (struct xroot *)root;
	struct xroot_group *grp, *tmp;

	xroot_info(xr, "leaving...");
	list_for_each_entry_safe(grp, tmp, &xr->groups, node) {
		list_del(&grp->node);
		xroot_cleanup_group(grp);
		kfree(grp);
	}
}
EXPORT_SYMBOL_GPL(xroot_remove);

