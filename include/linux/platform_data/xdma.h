// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef _XDMA_H
#define _XDMA_H

#include <linux/dmaengine.h>

/**
 * struct xdma_chan_info - DMA channel infomation
 *	This information is used to match channel when request dma channel
 * @dir: Channel tranfer direction
 */
struct xdma_chan_info {
	enum dma_transfer_direction dir;
};

#define XDMA_FILTER_PARAM(chan_info)	(void *)(chan_info)

struct dma_slave_map;

/**
 * struct xdma_platdata - platform specific data for XDMA engine
 * @max_dma_channels: Maximium dma channels in each direction
 * @user_irqs: Required user IRQs
 */
struct xdma_platdata {
	u32 max_dma_channels;
	u32 user_irqs;
	u32 slave_map_cnt;
	struct dma_slave_map *slave_map;
};

#endif /* _XDMA_H */
