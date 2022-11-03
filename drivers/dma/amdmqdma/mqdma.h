/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022, Advanced Micro Devices, Inc.
 */

#ifndef __DMA_MQDMA_H
#define __DMA_MQDMA_H

#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <linux/dmaengine.h>
#include <linux/platform_device.h>
#include "mqdma-hw.h"

/*
 * struct qdma_version - DMA version information
 */
struct qdma_version {
	u32	ip_type;
	u32	device_type;
};

struct qdma_dev_info {
	u32	dev_type;
	u32	func_id;
	u32	num_qs;
};

struct qdma_device;
/*
 * struct qdma_hw_access - HW access callback functions
 */
struct qdma_hw_access {
	int (*qdma_hw_get_attrs)(struct qdma_device *qdev);
};

/*
 * struct qdma_device - DMA device structure
 */
struct qdma_device {
        struct platform_device  *pdev;
        struct dma_device       dma_dev;
        struct regmap           *regmap;
	struct qdma_version	version_info;
	u32			irq_start;
	u32			irq_num;
	struct qdma_hw_access	*hw_access;
	struct qdma_dev_info	dev_info;
};

extern struct qdma_hw_access qdma_cpm5_access;

static inline int qdma_read_reg(struct qdma_device *qdev, u32 base, u32 reg,
				u32 *val)
{
	return regmap_read(qdev->regmap, base + reg, val);
}

static inline int qdma_write_reg(struct qdma_device *qdev, u32 base, u32 reg,
				 u32 val)
{
	return regmap_write(qdev->regmap, base + reg, val);
}

#define qdma_err(qdev, fmt, args...)					\
	dev_err(&(qdev)->pdev->dev, fmt, ##args)
#define qdma_info(qdev, fmt, args...)					\
	dev_info(&(qdev)->pdev->dev, fmt, ##args)

#endif
