/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XRT_XDMA_H_
#define _XRT_XDMA_H_

#include <linux/dma-direction.h>
#include <linux/scatterlist.h>
#include <linux/xrt/xleaf/xdma.h>
#include <linux/xrt/metadata.h>

/* maximum number of channel */
#define XRT_XDMA_MAX_CHANNEL_NUM	32

/* channel name */
#define XRT_XDMA_CHANNEL_H2C	"xrt_xdma_chan_h2c"
#define XRT_XDMA_CHANNEL_C2H	"xrt_xdma_chan_c2h"

/*
 * XDMA driver leaf calls.
 */
enum xrt_xdma_leaf_cmd {
	XRT_XDMA_REQUEST = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
};

struct xrt_xdma_request {
	u32 direction;
	bool dma_mapped;
	struct sg_table sgt;
	u64 endpoint_addr;
};

#endif	/* _XRT_XDMA_H_ */
