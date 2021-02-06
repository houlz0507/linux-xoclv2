/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header file for Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XMGMT_MAIN_IMPL_H_
#define _XMGMT_MAIN_IMPL_H_

#include <linux/platform_device.h>
#include "xmgmt-main.h"

struct fpga_manager;
int xmgmt_process_xclbin(struct platform_device *pdev,
			 struct fpga_manager *fmgr,
			 const struct axlf *xclbin,
			 enum provider_kind kind);
void xmgmt_region_cleanup_all(struct platform_device *pdev);

int bitstream_axlf_mailbox(struct platform_device *pdev, const void *xclbin);
int xmgmt_hot_reset(struct platform_device *pdev);

/* Getting dtb for specified group. Caller should vfree returned dtb .*/
char *xmgmt_get_dtb(struct platform_device *pdev, enum provider_kind kind);
char *xmgmt_get_vbnv(struct platform_device *pdev);
int xmgmt_get_provider_uuid(struct platform_device *pdev,
			    enum provider_kind kind, uuid_t *uuid);

int xmgmt_main_register_leaf(void);
void xmgmt_main_unregister_leaf(void);

#endif	/* _XMGMT_MAIN_IMPL_H_ */
