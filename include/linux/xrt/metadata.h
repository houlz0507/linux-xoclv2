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

/* endpoint node names */
#define XRT_MD_NODE_VSEC	"drv_ep_vsec_00"

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

#endif
