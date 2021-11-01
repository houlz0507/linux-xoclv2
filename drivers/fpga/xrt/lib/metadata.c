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
#include <linux/fpga-xrt.h>
/*
 * Device metadata format
 */
struct xrt_md_data {
	u32		md_size;
	u32		ep_num;
	u32		ep_end;
	u64		eps[0]; /* endpoint entries */
};

/* endpoint format */
struct xrt_md_endpoint {
	const char	*name;
	u64		prop_bitmap;
	u64		prop[XRT_MD_PROP_NUM];
	u32		priv_data_sz;
	u64		priv_data[0];
};

#define ENDPOINT_OFFSET(md, ep)	((uintptr_t)(ep) - (uintptr_t)(md))
#define ENDPOINT_SIZE(ep)	(sizeof(struct xrt_md_endpoint) + (ep)->priv_data_sz)

int xrt_md_create(struct device *dev, u32 max_ep_num, u32 max_ep_sz, void **md)
{
	u32 sz = sizeof(struct xrt_md_data) + max_ep_num *
		(roundup(max_ep_sz, sizeof(u64)) + sizeof(struct xrt_md_endpoint));

	*md = vzalloc(sz);
	if (!*md)
		return -ENOMEM;

	((struct xrt_md_data *)*md)->md_size = sz;
	((struct xrt_md_data *)*md)->ep_end = sizeof(struct xrt_md_data);

	return 0;
};
EXPORT_SYMBOL_GPL(xrt_md_create);

static int xrt_md_get_endpoint(struct device *dev, void *metadata, const char *ep_name,
			       struct xrt_md_endpoint **endpoint)
{
	struct xrt_md_data *md = metadata;
	struct xrt_md_endpoint *ep;
	int i;

	ep = (struct xrt_md_endpoint *)md->eps;
	for (i = 0; i < md->ep_num; i++) {
		if (!strncmp(ep->name, ep_name, strlen(ep->name) + 1)) {
			*endpoint = ep;
			return 0;
		}
		ep = (void *)ep + ENDPOINT_SIZE(ep);
	}

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

	if (md->ep_end + sizeof(*ep) > md->md_size) {
		dev_err(dev, "no space for new endpoint %s", ep_name);
		return -ENOMEM;
	}
	ep = metadata + md->ep_end;
	ep->name = ep_name;
	md->ep_num++;
	md->ep_end += sizeof(*ep);

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_add_endpoint);

static int xrt_md_set_data(struct device *dev, struct xrt_md_data *md, struct xrt_md_endpoint *ep,
			   void *data, u32 len)
{
	u32 next_ep, new_next_ep;

	if (len > ep->priv_data_sz && len - ep->priv_data_sz + md->ep_end > md->md_size) {
		dev_err(dev, "no space for priv data");
		return -ENOMEM;
	}

	next_ep = ENDPOINT_OFFSET(md, ep) + ENDPOINT_SIZE(ep);
	new_next_ep = next_ep + len - ep->priv_data_sz;
	if (next_ep != new_next_ep && next_ep != md->ep_end) {
		memmove((char *)md + new_next_ep, (char *)md + next_ep,
			md->ep_end - next_ep);
		md->ep_end = md->ep_end + len - ep->priv_data_sz;
	}

	memcpy(ep->priv_data, data, len);
	ep->priv_data_sz = len;

	return 0;
}

int xrt_md_set_prop(struct device *dev, void *metadata, const char *ep_name,
		    u32 prop, u64 prop_val, u32 len)
{
	struct xrt_md_endpoint *ep = NULL;
	int ret;

	ret = xrt_md_get_endpoint(dev, metadata, ep_name, &ep);
	if (ret)
		return -ENOENT;

	if (prop == XRT_MD_PROP_PRIV_DATA) {
		ret = xrt_md_set_data(dev, metadata, ep, (void *)prop_val, len);
		if (ret)
			return ret;
	} else {
		ep->prop[prop] = prop_val;
	}
	ep->prop_bitmap |= (1ULL << prop);

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_set_prop);

int xrt_md_get_prop(struct device *dev, void *metadata, const char *ep_name,
		    u32 prop, u64 *prop_val, u32 *len)
{
	struct xrt_md_endpoint *ep = NULL;
	int ret;

	if (len)
		*len = 0;

	ret = xrt_md_get_endpoint(dev, metadata, ep_name, &ep);
	if (ret)
		return -ENOENT;

	if (!(ep->prop_bitmap & (1ULL << prop)))
		return -ENOENT;

	if (prop == XRT_MD_PROP_PRIV_DATA) {
		if (len)
			*len = ep->priv_data_sz;
		if (prop_val)
			*(void **)prop_val = ep->priv_data;
	} else {
		*prop_val = ep->prop[prop];
	}

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
		if (!md->ep_num) {
			ret = -ENOENT;
			goto failed;
		}
		ep = (struct xrt_md_endpoint *)md->eps;
		*next_ep_name = ep->name;
		return 0;
	}

	ret = xrt_md_get_endpoint(dev, metadata, ep_name, &ep);
	if (ret)
		goto failed;

	ep = (void *)ep + ENDPOINT_SIZE(ep);
	if (ENDPOINT_OFFSET(md, ep) == md->ep_end) {
		ret = -ENOENT;
		goto failed;
	}

	*next_ep_name = ep->name;
	return 0;

failed:
	*next_ep_name = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(xrt_md_get_next_endpoint);

int xrt_md_copy_endpoint(struct device *dev, void *metadata, const char *ep_name,
			 void *dst_metadata)
{
	struct xrt_md_endpoint *ep = NULL, *dst_ep = NULL;
	int ret;

	ret = xrt_md_get_endpoint(dev, metadata, ep_name, &ep);
	if (ret)
		return ret;

	ret = xrt_md_get_endpoint(dev, dst_metadata, ep_name, &dst_ep);
	if (ret) {
		ret = xrt_md_add_endpoint(dev, dst_metadata, ep_name);
		if (ret)
			return ret;
	}
	ret = xrt_md_get_endpoint(dev, dst_metadata, ep_name, &dst_ep);
	if (ret)
		return ret;

	if (ep->priv_data_sz) {
		ret = xrt_md_set_prop(dev, dst_metadata, ep_name, XRT_MD_PROP_PRIV_DATA,
				      (u64)ep->priv_data, ep->priv_data_sz);
		if (ret)
			return ret;
	}
	memcpy(dst_ep, ep, sizeof(*ep));

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_md_copy_endpoint);

u32 xrt_md_size(void *metadata)
{
	struct xrt_md_data *md = metadata;

	return md->md_size;
}
EXPORT_SYMBOL_GPL(xrt_md_size);
