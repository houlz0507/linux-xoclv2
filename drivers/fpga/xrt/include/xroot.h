/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_ROOT_H_
#define _XRT_ROOT_H_

struct xroot_range {
        __be64 child_addr;
        __be64 parent_addr;
        __be64 child_size;
};

struct xroot_info {
	u32 addr;
	int num_range;
	struct xroot_range *ranges;
};

int xroot_probe(struct device *dev, struct xroot_info *info, void **root);
void xroot_remove(void *root);

int xroot_create_group(void *xr, void *dtb);
void xroot_destroy_group(void *xr, u32 grp_id);

#endif	/* _XRT_ROOT_H_ */
