/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XRT_METADATA_H
#define _XRT_METADATA_H

#include <linux/device.h>
#include <linux/vmalloc.h>
#include <linux/uuid.h>

/* metadata properties */
enum xrt_md_property {
	XRT_MD_PROP_REG_BAR_IDX,
	XRT_MD_PROP_REG_BAR_OFF,
	XRT_MD_PROP_REG_SIZE,
	XRT_MD_PROP_IRQ_START,
	XRT_MD_PROP_IRQ_NUM,
	XRT_MD_PROP_PF_INDEX,
	XRT_MD_PROP_DEVICE_ID,
	XRT_MD_PROP_PRIV_DATA,
	XRT_MD_PROP_NUM
};

#define XRT_MD_MAX_LEN		(32 * 1024)

/* endpoint node names */
#define XRT_MD_NODE_ENDPOINTS	"addressable_endpoints"
#define XRT_MD_NODE_VSEC	"drv_ep_vsec_00"
#define XRT_MD_NODE_DTB		"drv_ep_dtb_00"

int xrt_md_create(struct device *dev, u32 max_ep_num, u32 max_ep_sz, void **md);
int xrt_md_set_prop(struct device *dev, void *metadata, const char *ep_name,
		    u32 prop, u64 prop_val, u32 len);
int xrt_md_get_prop(struct device *dev, void *metadata, const char *ep_name,
		    u32 prop, u64 *prop_val, u32 *len);
int xrt_md_add_endpoint(struct device *dev, void *metadata, const char *ep_name);
int xrt_md_get_next_endpoint(struct device *dev, void *metadata, const char *ep_name,
			     const char **next_ep_name);
int xrt_md_copy_endpoint(struct device *dev, void *metadata, const char *ep_name,
			 void *dst_metadata);
u32 xrt_md_size(void *metadata);

#define for_each_endpoint(dev, md, ep_name)				\
	for (xrt_md_get_next_endpoint(dev, md, NULL, &(ep_name));	\
	     ep_name;							\
	     xrt_md_get_next_endpoint(dev, md, ep_name, &(ep_name)))

/*
 * The firmware provides a 128 bit hash string as a unique id to the
 * partition/interface.
 * Existing hw does not yet use the cononical form, so it is necessary to
 * use a translation function.
 */
static inline void xrt_md_trans_uuid2str(const uuid_t *uuid, char *uuidstr)
{
	int i, p;
	u8 tmp[UUID_SIZE];

	BUILD_BUG_ON(UUID_SIZE != 16);
	export_uuid(tmp, uuid);
	for (p = 0, i = UUID_SIZE - 1; i >= 0; p++, i--)
		snprintf(&uuidstr[p * 2], 3, "%02x", tmp[i]);
}

static inline int xrt_md_trans_str2uuid(struct device *dev, const char *uuidstr, uuid_t *p_uuid)
{
	u8 p[UUID_SIZE];
	const char *str;
	char tmp[3] = { 0 };
	int i, ret;

	if (strlen(uuidstr) != UUID_SIZE * 2)
		return -EINVAL;

	str = uuidstr + strlen(uuidstr) - 2;

	for (i = 0; i < sizeof(*p_uuid) && str >= uuidstr; i++) {
		tmp[0] = *str;
		tmp[1] = *(str + 1);
		ret = kstrtou8(tmp, 16, &p[i]);
		if (ret)
			return -EINVAL;
		str -= 2;
	}
	import_uuid(p_uuid, p);

	return 0;
}

#endif
