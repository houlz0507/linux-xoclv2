/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022, Advanced Micro Devices, Inc.
 */

#ifndef _PLATDATA_AMD_MQDMA_H
#define _PLATDATA_AMD_MQDMA_H

#include <linux/dmaengine.h>

/**
 * struct amdmqdma_chan_info - DMA channel information
 *	This information is used to match channel when request dma channel
 * @dir: Channel transfer direction
 */
struct amdmqdma_chan_info {
	enum dma_transfer_direction dir;
};

#define XDMA_FILTER_PARAM(chan_info)	((void *)(chan_info))

struct dma_slave_map;

/**
 * struct amdmqdma_platdata - platform specific data for DMA engine
 * @max_dma_channels: Maximum dma channels in each direction
 */
struct amdmqdma_platdata {
	u32 max_dma_channels;
	u32 device_map_cnt;
	struct dma_slave_map *device_map;
};

#endif /* _PLATDATA_AMD_MQDMA_H */
