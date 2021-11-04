/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_ROOT_H_
#define _XRT_ROOT_H_

int xroot_probe(struct device *dev, void **root);
void xroot_remove(void *root);

int xroot_create_group(void *xr, const char *name, void *dtb);
void xroot_destroy_group(void *xr, const char *name);

#endif	/* _XRT_ROOT_H_ */
