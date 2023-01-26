/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023, Advanced Micro Devices, Inc.
 */

#ifndef __DMA_MQDMA_H
#define __DMA_MQDMA_H

#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <linux/dmaengine.h>
#include <linux/platform_device.h>
#include "../virt-dma.h"
#include "mqdma-hw.h"

/* QDMA HW version string array length */
#define QDMA_VERSION_STRING_LEN          32
#define QDMA_QUEUE_NAME_MAXLEN           20

/**
 * struct qdma_request - DMA request structure
 * @vdesc: Virtual DMA descriptor
 * @dir: Transferring direction of the request
 * @slave_addr: Physical address on DMA slave side
 * @sgl: Scatter gather list on host side
 * @sg_off: Start offset of the first sgl segment
 * @nents: Number of sgl segments to transfer
 */
struct qdma_request {
	struct virt_dma_desc		vdesc;
	enum dma_transfer_direction	dir;
	u64				slave_addr;
	struct scatterlist		*sgl;
	u32				sg_off;
	u32				nents;
};

/**
 * struct qdma_chan - Driver specific DMA channel structure
 * @vchan: Virtual channel
 * @qdev_hdl: Pointer to DMA device structure
 * @busy: Busy flag of the channel
 * @dir: Transferring direction of the channel
 * @cfg: Transferring config of the channel
 */
struct qdma_chan {
	struct virt_dma_chan		vchan;
	void				*qdev_hdl;
	bool				busy;
	enum dma_transfer_direction	dir;
	struct dma_slave_config		cfg;
};

/**
 * struct qdma_version - DMA version information
 * @ip_type: QDMA IP type
 * @device_type: QDMA Device Type
 * @device_type_str: QDMA device type string
 * @ip_type_str: QDMA IP type string
 */
struct qdma_version {
	u32	ip_type;
	u32	device_type;
	char	device_type_str[QDMA_VERSION_STRING_LEN];
	char	ip_type_str[QDMA_VERSION_STRING_LEN];
};

/*
 * struct qdma_dev_info- - QDMA device attributes
 * @dev_type: QDMA Device Type
 * @func_id:
 * @num_pfs: Num of PFs
 * @num_qs: Maximum number of queues per device
 * @mm_channel_max: Num of MM channels
 * @flr_present: FLR resent or not
 * @mm_en: MM mode supported or not
 */
struct qdma_dev_info {
	u32	dev_type;
	u32	func_id;
	u16	num_qs;
	u8	num_pfs;
	u8	mm_channel_max;
	bool	flr_present;
	bool	mm_en;
	bool	mailbox_en;
};

/*
 * struct qdma_device - DMA device structure
 * @qdma_dev_info: Defines per device qdma property
 * @qdma_version: Version information
 * @ind_ctxt_lock: Context programming lock
 */
struct qdma_device {
	struct platform_device	*pdev;
	struct dma_device	dma_dev;
	u32			qbase;
	u32			max_queues;
	u32			func_id;
	u32			flags;
	struct regmap           *regmap;
	struct qdma_dev_info	dev_info;
	struct qdma_version	version_info;
	spinlock_t              ind_ctxt_lock; /* context programming lock */
};

#define qdma_err(qdev, fmt, args...)					\
	dev_err(&(qdev)->pdev->dev, fmt, ##args)
#define qdma_info(qdev, fmt, args...)					\
	dev_info(&(qdev)->pdev->dev, fmt, ##args)
#define qdma_dbg(qdev, fmt, args...)					\
	dev_dbg(&(qdev)->pdev->dev, fmt, ##args)

#endif
