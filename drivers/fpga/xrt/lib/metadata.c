// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Metadata parse APIs
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#include <linux/vmalloc.h>
#include <linux/xrt/metadata.h>
/*
 * Device metadata format
 */
struct xrt_md_data {
	u32		md_size;
	u32		priv_data_sz;
	u32		endpoint_off;
	u32		endpoint_num;
	u64		data[0]; /* priv_data blob, endpoint entries */
};

/* endpoint format */
struct xrt_md_endpoint {
	const char	*name;
	u64		prop_bitmap;
	u64		prop[XRT_MD_PROP_NUM];
};

#define XRT_MAX_MD_SIZE		(4096 * 25)

int xrt_md_create(struct device *dev, void **md)
{
	*md = vzalloc(XRT_MAX_MD_SIZE);
	if (!*md)
		return -ENOMEM;

	return 0;
};
EXPORT_SYMBOL_GPL(xrt_md_create);

static int xrt_md_get_endpoint(struct device *dev, void *metadata, const char *ep_name,
			       struct xrt_md_endpoint **endpoint)
{
	struct xrt_md_data *md = metadata;
	struct xrt_md_endpoint *ep;
	int i;

	ep = (struct xrt_md_endpoint *)((char *)md->data + md->endpoint_off);
	for (i = 0; i < md->endpoint_num; i++) {
		if (!strncmp(ep->name, ep_name, strlen(ep->name) + 1)) {
			*endpoint = ep;
			return 0;
		}
		ep++;
	}
	*endpoint = ep;

	return -ENOENT;
}

int xrt_md_add_endpoint(struct device *dev, void *metadata, const char *ep_name)
{
	struct xrt_md_data *md = metadata;
	struct xrt_md_endpoint *ep = NULL;
	int ret;

	ret = xrt_md_get_endpoint(dev, metadata, ep_name, &ep);
	if (!ret)
		return -EEXIST;

	ep->name = ep_name;
	md->endpoint_num++;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_add_endpoint);

int xrt_md_set_data(struct device *dev, void *metadata, void *data, u32 len)
{
	struct xrt_md_data *md = metadata;
	u32 new_ep_off;
	int eps_sz;

	if (len <= md->endpoint_off) {
		memcpy(md->data, data, len);
		md->priv_data_sz = len;
		return 0;
	}

	new_ep_off = round_up(len, sizeof(u64));
	if (md->endpoint_num > 0) {
		eps_sz = sizeof(struct xrt_md_endpoint) * md->endpoint_num;
		if (eps_sz + new_ep_off > md->md_size - sizeof(*md)) {
			dev_err(dev, "data is too long, %d", len);
			return -EINVAL;
		}
		
		memmove((char *)md->data + new_ep_off, (char *)md->data + md->endpoint_off,
			eps_sz);
	}
	memcpy(md->data, data, len);
	md->priv_data_sz = len;
	md->endpoint_off = new_ep_off;
	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_set_data);

int xrt_md_set_prop(struct device *dev, void *metadata, const char *ep_name,
		    u32 prop, u64 prop_val)
{
	struct xrt_md_endpoint *ep = NULL;
	int ret;

	ret = xrt_md_get_endpoint(dev, metadata, ep_name, &ep);
	if (ret)
		return -ENOENT;

	ep->prop_bitmap |= (1ULL << prop);
	ep->prop[prop] = prop_val;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_set_prop);

int xrt_md_get_prop(struct device *dev, void *metadata, const char *ep_name,
		    u32 prop, u64 *prop_val)
{
	struct xrt_md_endpoint *ep = NULL;
	int ret;

	ret = xrt_md_get_endpoint(dev, metadata, ep_name, &ep);
	if (ret)
		return -ENOENT;

	if (!(ep->prop_bitmap & (1ULL << prop)))
		return -ENOENT;

	*prop_val = ep->prop[prop];

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_get_prop);

int xrt_md_get_next_endpoint(struct device *dev, void *metadata, const char *ep_name,
			     const char **next_ep_name)
{
	struct xrt_md_data *md = metadata;
	struct xrt_md_endpoint *ep = NULL;
	int ret;

	if (!ep_name) {
		if (!md->endpoint_num) {
			*next_ep_name = NULL;
			return -ENOENT;
		}
		ep = (void *)md->data + md->endpoint_off;
		*next_ep_name = ep->name;
		return 0;
	}

	ret = xrt_md_get_endpoint(dev, metadata, ep_name, &ep);
	if (ret)
		return -ENOENT;

	ep++;
	if ((uintptr_t)ep - (uintptr_t)md->data >= md->endpoint_num * sizeof(*ep)) {
		*next_ep_name = NULL;
		return -ENOENT;
	}
	*next_ep_name = ep->name;
	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_get_next_endpoint);

int xrt_md_pack(struct device *dev, void *metadata, void **packed_md, u32 *packed_md_len)
{
	struct xrt_md_data *md = metadata;
	u32 new_ep_off, eps_sz;

	new_ep_off = round_up(md->endpoint_off, md->priv_data_sz);
	eps_sz = sizeof(struct xrt_md_endpoint) * md->endpoint_num;
	*packed_md_len = sizeof(*md) + new_ep_off + eps_sz;
	*packed_md = vzalloc(*packed_md_len);
	if (!*packed_md) {
		*packed_md_len = 0;
		return -ENOMEM;
	}

	memcpy(*packed_md, md, sizeof(*md) + new_ep_off);
	memcpy(*packed_md + new_ep_off, (char *)md->data + md->endpoint_off, eps_sz);

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_pack);
