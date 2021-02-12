// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Metadata parse APIs
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#include <linux/libfdt_env.h>
#include "libfdt.h"
#include "metadata.h"

#define MAX_BLOB_SIZE	(4096 * 25)

static int xrt_md_setprop(struct device *dev, char *blob, int offset,
			  const char *prop, const void *val, int size);
static int xrt_md_overlay(struct device *dev, char *blob, int target,
			  const char *overlay_blob, int overlay_offset);
static int xrt_md_get_endpoint(struct device *dev, const char *blob,
			       const char *ep_name, const char *regmap_name,
			       int *ep_offset);

unsigned long xrt_md_size(struct device *dev, const char *blob)
{
	unsigned long len = (long)fdt_totalsize(blob);

	len = (len > MAX_BLOB_SIZE) ? XRT_MD_INVALID_LENGTH : len;
	return len;
}
EXPORT_SYMBOL_GPL(xrt_md_size);

int xrt_md_create(struct device *dev, char **blob)
{
	int ret = 0;

	WARN_ON(!blob);

	*blob = vmalloc(MAX_BLOB_SIZE);
	if (!*blob)
		return -ENOMEM;

	ret = fdt_create_empty_tree(*blob, MAX_BLOB_SIZE);
	if (ret) {
		dev_err(dev, "format blob failed, ret = %d", ret);
		goto failed;
	}

	ret = fdt_next_node(*blob, -1, NULL);
	if (ret < 0) {
		dev_err(dev, "No Node, ret = %d", ret);
		goto failed;
	}

	ret = fdt_add_subnode(*blob, ret, XRT_MD_NODE_ENDPOINTS);
	if (ret < 0) {
		dev_err(dev, "add node failed, ret = %d", ret);
		goto failed;
	}

	return 0;

failed:
	vfree(*blob);
	*blob = NULL;

	return ret;
}
EXPORT_SYMBOL_GPL(xrt_md_create);

static int xrt_md_add_node(struct device *dev, char *blob, int parent_offset,
			   const char *ep_name)
{
	int ret;

	ret = fdt_add_subnode(blob, parent_offset, ep_name);
	if (ret < 0 && ret != -FDT_ERR_EXISTS)
		dev_err(dev, "failed to add node %s. %d", ep_name, ret);

	return ret;
}

int xrt_md_del_endpoint(struct device *dev, char *blob, const char *ep_name,
			char *regmap_name)
{
	int ret;
	int ep_offset;

	ret = xrt_md_get_endpoint(dev, blob, ep_name, regmap_name, &ep_offset);
	if (ret) {
		dev_err(dev, "can not find ep %s", ep_name);
		return -EINVAL;
	}

	ret = fdt_del_node(blob, ep_offset);
	if (ret)
		dev_err(dev, "delete node %s failed, ret %d", ep_name, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(xrt_md_del_endpoint);

static int __xrt_md_add_endpoint(struct device *dev, char *blob,
				 struct xrt_md_endpoint *ep, int *offset, bool root)
{
	int ret = 0;
	int ep_offset = 0;
	u32 val, count = 0;
	u64 io_range[2];
	char comp[128];

	if (!ep->ep_name) {
		dev_err(dev, "empty name");
		return -EINVAL;
	}

	if (!root) {
		ret = xrt_md_get_endpoint(dev, blob, XRT_MD_NODE_ENDPOINTS, NULL,
					  &ep_offset);
		if (ret) {
			dev_err(dev, "invalid blob, ret = %d", ret);
			return -EINVAL;
		}
	}

	ep_offset = xrt_md_add_node(dev, blob, ep_offset, ep->ep_name);
	if (ep_offset < 0) {
		dev_err(dev, "add endpoint failed, ret = %d", ret);
		return -EINVAL;
	}
	if (offset)
		*offset = ep_offset;

	if (ep->size != 0) {
		val = cpu_to_be32(ep->bar);
		ret = xrt_md_setprop(dev, blob, ep_offset, XRT_MD_PROP_BAR_IDX,
				     &val, sizeof(u32));
		if (ret) {
			dev_err(dev, "set %s failed, ret %d",
				XRT_MD_PROP_BAR_IDX, ret);
			goto failed;
		}
		io_range[0] = cpu_to_be64((u64)ep->bar_off);
		io_range[1] = cpu_to_be64((u64)ep->size);
		ret = xrt_md_setprop(dev, blob, ep_offset, XRT_MD_PROP_IO_OFFSET,
				     io_range, sizeof(io_range));
		if (ret) {
			dev_err(dev, "set %s failed, ret %d",
				XRT_MD_PROP_IO_OFFSET, ret);
			goto failed;
		}
	}

	if (ep->regmap) {
		if (ep->regmap_ver) {
			count = snprintf(comp, sizeof(comp),
					 "%s-%s", ep->regmap, ep->regmap_ver);
			count++;
		}

		count += snprintf(comp + count, sizeof(comp) - count,
				  "%s", ep->regmap);
		count++;

		ret = xrt_md_setprop(dev, blob, ep_offset, XRT_MD_PROP_COMPATIBLE,
				     comp, count);
		if (ret) {
			dev_err(dev, "set %s failed, ret %d",
				XRT_MD_PROP_COMPATIBLE, ret);
			goto failed;
		}
	}

failed:
	if (ret)
		xrt_md_del_endpoint(dev, blob, ep->ep_name, NULL);

	return ret;
}

int xrt_md_add_endpoint(struct device *dev, char *blob,
			struct xrt_md_endpoint *ep)
{
	return __xrt_md_add_endpoint(dev, blob, ep, NULL, false);
}
EXPORT_SYMBOL_GPL(xrt_md_add_endpoint);

static int xrt_md_get_endpoint(struct device *dev, const char *blob,
			       const char *ep_name, const char *regmap_name,
			       int *ep_offset)
{
	int offset;
	const char *name;

	for (offset = fdt_next_node(blob, -1, NULL);
	    offset >= 0;
	    offset = fdt_next_node(blob, offset, NULL)) {
		name = fdt_get_name(blob, offset, NULL);
		if (!name || strncmp(name, ep_name, strlen(ep_name) + 1))
			continue;
		if (!regmap_name ||
		    !fdt_node_check_compatible(blob, offset, regmap_name))
			break;
	}
	if (offset < 0)
		return -ENODEV;

	*ep_offset = offset;

	return 0;
}

int xrt_md_find_endpoint(struct device *dev, const char *blob,
			 const char *ep_name, const char *regmap_name,
			 const char **epname)
{
	int offset;
	int ret;

	ret = xrt_md_get_endpoint(dev, blob, ep_name, regmap_name,
				  &offset);
	if (!ret && epname && offset >= 0)
		*epname = fdt_get_name(blob, offset, NULL);

	return ret;
}
EXPORT_SYMBOL_GPL(xrt_md_find_endpoint);

int xrt_md_get_prop(struct device *dev, const char *blob, const char *ep_name,
		    const char *regmap_name, const char *prop,
		    const void **val, int *size)
{
	int offset;
	int ret;

	if (val)
		*val = NULL;
	if (ep_name) {
		ret = xrt_md_get_endpoint(dev, blob, ep_name, regmap_name,
					  &offset);
		if (ret) {
			dev_err(dev, "cannot get ep %s, regmap %s, ret = %d",
				ep_name, regmap_name, ret);
			return -EINVAL;
		}
	} else {
		offset = fdt_next_node(blob, -1, NULL);
		if (offset < 0) {
			dev_err(dev, "internal error, ret = %d", offset);
			return -EINVAL;
		}
	}

	if (val) {
		*val = fdt_getprop(blob, offset, prop, size);
		if (!*val) {
			dev_dbg(dev, "get ep %s, prop %s failed", ep_name, prop);
			return -EINVAL;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_get_prop);

static int xrt_md_setprop(struct device *dev, char *blob, int offset,
			  const char *prop, const void *val, int size)
{
	int ret;

	ret = fdt_setprop(blob, offset, prop, val, size);
	if (ret)
		dev_err(dev, "failed to set prop %d", ret);

	return ret;
}

int xrt_md_set_prop(struct device *dev, char *blob,
		    const char *ep_name, const char *regmap_name,
		    const char *prop, const void *val, int size)
{
	int offset;
	int ret;

	if (ep_name) {
		ret = xrt_md_get_endpoint(dev, blob, ep_name,
					  regmap_name, &offset);
		if (ret) {
			dev_err(dev, "cannot get node %s, ret = %d",
				ep_name, ret);
			return -EINVAL;
		}
	} else {
		offset = fdt_next_node(blob, -1, NULL);
		if (offset < 0) {
			dev_err(dev, "internal error, ret = %d", offset);
			return -EINVAL;
		}
	}

	ret = xrt_md_setprop(dev, blob, offset, prop, val, size);
	if (ret)
		dev_err(dev, "set prop %s failed, ret = %d", prop, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(xrt_md_set_prop);

int xrt_md_copy_endpoint(struct device *dev, char *blob, const char *src_blob,
			 const char *ep_name, const char *regmap_name,
			 const char *new_ep_name)
{
	int offset, target;
	int ret;
	struct xrt_md_endpoint ep = {0};
	const char *newepnm = new_ep_name ? new_ep_name : ep_name;

	ret = xrt_md_get_endpoint(dev, src_blob, ep_name, regmap_name,
				  &offset);
	if (ret)
		return -EINVAL;

	ret = xrt_md_get_endpoint(dev, blob, newepnm, regmap_name, &target);
	if (ret) {
		ep.ep_name = newepnm;
		ret = __xrt_md_add_endpoint(dev, blob, &ep, &target,
					    fdt_parent_offset(src_blob, offset) == 0);
		if (ret)
			return -EINVAL;
	}

	ret = xrt_md_overlay(dev, blob, target, src_blob, offset);
	if (ret)
		dev_err(dev, "overlay failed, ret = %d", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(xrt_md_copy_endpoint);

char *xrt_md_dup(struct device *dev, const char *blob)
{
	int ret;
	char *dup_blob;

	ret = xrt_md_create(dev, &dup_blob);
	if (ret)
		return NULL;
	ret = xrt_md_overlay(dev, dup_blob, -1, blob, -1);
	if (ret) {
		vfree(dup_blob);
		return NULL;
	}

	return dup_blob;
}
EXPORT_SYMBOL_GPL(xrt_md_dup);

static int xrt_md_overlay(struct device *dev, char *blob, int target,
			  const char *overlay_blob, int overlay_offset)
{
	int	property, subnode;
	int	ret;

	WARN_ON(!blob || !overlay_blob);

	if (!blob) {
		dev_err(dev, "blob is NULL");
		return -EINVAL;
	}

	if (target < 0) {
		target = fdt_next_node(blob, -1, NULL);
		if (target < 0) {
			dev_err(dev, "invalid target");
			return -EINVAL;
		}
	}
	if (overlay_offset < 0) {
		overlay_offset = fdt_next_node(overlay_blob, -1, NULL);
		if (overlay_offset < 0) {
			dev_err(dev, "invalid overlay");
			return -EINVAL;
		}
	}

	fdt_for_each_property_offset(property, overlay_blob, overlay_offset) {
		const char *name;
		const void *prop;
		int prop_len;

		prop = fdt_getprop_by_offset(overlay_blob, property, &name,
					     &prop_len);
		if (!prop || prop_len >= MAX_BLOB_SIZE) {
			dev_err(dev, "internal error");
			return -EINVAL;
		}

		ret = xrt_md_setprop(dev, blob, target, name, prop,
				     prop_len);
		if (ret) {
			dev_err(dev, "setprop failed, ret = %d", ret);
			return ret;
		}
	}

	fdt_for_each_subnode(subnode, overlay_blob, overlay_offset) {
		const char *name = fdt_get_name(overlay_blob, subnode, NULL);
		int nnode;

		nnode = xrt_md_add_node(dev, blob, target, name);
		if (nnode == -FDT_ERR_EXISTS)
			nnode = fdt_subnode_offset(blob, target, name);
		if (nnode < 0) {
			dev_err(dev, "add node failed, ret = %d", nnode);
			return nnode;
		}

		ret = xrt_md_overlay(dev, blob, nnode, overlay_blob, subnode);
		if (ret)
			return ret;
	}

	return 0;
}

int xrt_md_get_next_endpoint(struct device *dev, const char *blob,
			     const char *ep_name, const char *regmap_name,
			     char **next_ep, char **next_regmap)
{
	int offset, ret;

	if (!ep_name) {
		ret = xrt_md_get_endpoint(dev, blob, XRT_MD_NODE_ENDPOINTS, NULL,
					  &offset);
	} else {
		ret = xrt_md_get_endpoint(dev, blob, ep_name, regmap_name,
					  &offset);
	}

	if (ret) {
		*next_ep = NULL;
		*next_regmap = NULL;
		return -EINVAL;
	}

	offset = ep_name ? fdt_next_subnode(blob, offset) :
		fdt_first_subnode(blob, offset);
	if (offset < 0) {
		*next_ep = NULL;
		*next_regmap = NULL;
		return -EINVAL;
	}

	*next_ep = (char *)fdt_get_name(blob, offset, NULL);
	*next_regmap = (char *)fdt_stringlist_get(blob, offset, XRT_MD_PROP_COMPATIBLE,
						  0, NULL);

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_get_next_endpoint);

int xrt_md_get_compatible_endpoint(struct device *dev, const char *blob,
				   const char *regmap_name, const char **ep_name)
{
	int ep_offset;

	ep_offset = fdt_node_offset_by_compatible(blob, -1, regmap_name);
	if (ep_offset < 0) {
		*ep_name = NULL;
		return -ENOENT;
	}

	*ep_name = (char *)fdt_get_name(blob, ep_offset, NULL);

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_get_compatible_endpoint);

void xrt_md_pack(struct device *dev, char *blob)
{
	int ret;

	ret = fdt_pack(blob);
	if (ret)
		dev_err(dev, "pack failed %d", ret);
}
EXPORT_SYMBOL_GPL(xrt_md_pack);

int xrt_md_get_intf_uuids(struct device *dev, const char *blob,
			  u32 *num_uuids, uuid_t *intf_uuids)
{
	int offset, count = 0;
	int ret;
	const char *uuid_str;

	ret = xrt_md_get_endpoint(dev, blob, XRT_MD_NODE_INTERFACES, NULL, &offset);
	if (ret)
		return -ENOENT;

	for (offset = fdt_first_subnode(blob, offset);
	    offset >= 0;
	    offset = fdt_next_subnode(blob, offset)) {
		uuid_str = fdt_getprop(blob, offset, XRT_MD_PROP_INTERFACE_UUID,
				       NULL);
		if (!uuid_str) {
			dev_err(dev, "empty intf uuid node");
			return -EINVAL;
		}

		if (intf_uuids && count < *num_uuids) {
			ret = xrt_md_trans_str2uuid(dev, uuid_str,
						    &intf_uuids[count]);
			if (ret)
				return -EINVAL;
		}
		count++;
	}

	*num_uuids = count;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_get_intf_uuids);
