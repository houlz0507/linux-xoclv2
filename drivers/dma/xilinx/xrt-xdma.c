// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA XDMA Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/xrt/metadata.h>
#include <linux/xrt/xdevice.h>
#include <linux/xrt/xleaf.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/xrt/xleaf/xdma.h>
#include "../dmaengine.h"
#include "xrt-xdma.h"

#define XRT_XDMA "xrt_xdma"
#define XRT_XDMA_CHANNEL_H2C "xrt_xdma_channel_h2c"
#define XRT_XDMA_CHANNEL_C2H "xrt_xdma_channel_c2h"

#define XRT_DESC_BLOCK_NUM 128
#define XRT_DESC_NUM	(XRT_DESC_BLOCK_NUM * XDMA_DESC_ADJACENT)

#define XRT_DESC_CONTROL(adjacent, flag)			\
	((XDMA_DESC_MAGIC << 16) | (((adjacent) - 1) << XDMA_DESC_ADJACENT_SHIFT) | (flag))

static const struct regmap_config xdma_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = XDMA_MAX_REGISTER_RANGE,
};

struct xdma_channel {
	struct dma_chan		chan;
	struct xrt_device	*xdev;
	u32			base;
	u32			chan_id;
	int			irq;
	u32			type;
	struct xdma_desc	*descs;
	dma_addr_t		desc_dma_addr;
	u32			submitted_desc_count;
	struct completion	req_compl;
	enum dma_status		status;
	spinlock_t		chan_lock;	/* lock for channel data */
};

struct xdma_chan_info {
	u32			start_index;
	u32			channel_num;
	unsigned long		channel_bitmap;
	struct semaphore	channel_sem;
};

struct xrt_xdma {
	struct xrt_device	*xdev;
	struct dma_device	dma_dev;
	struct regmap		*regmap;
	struct xdma_channel	channels[XDMA_MAX_CHANNEL_NUM];
	struct xdma_chan_info	h2c; /* host to card */
	struct xdma_chan_info	c2h; /* card to host */
};

static inline struct xdma_channel *to_xdma_channel(struct dma_chan *chan)
{
	return container_of(chan, struct xdma_channel, chan);
}

enum dma_status xdma_tx_status(struct dma_chan *chan, dma_cookie_t cookie,
			       struct dma_tx_state *txstate)
{
	struct xdma_channel *channel = to_xdma_channel(chan);
	enum dma_status status;
	unsigned long flags;

	spin_lock_irqsave(&channel->chan_lock, flags);
	status = channel->status;
	spin_unlock_irqrestore(&channel->chan_lock, flags);

	return status;
}

static void xdma_issue_pending(struct dma_chan *dma_chan)
{
}

static void xdma_free_chan_resources(struct dma_chan *chan)
{
	struct xdma_channel *channel = to_xdma_channel(chan);

pr_info("FREE RESOURCE FOR CHAN %d\n", channel->chan_id);
	if (!channel->descs || !channel->desc_dma_addr)
		return;

	dma_free_coherent(chan->device->dev, XRT_DESC_NUM * sizeof(struct xdma_desc),
			  channel->descs, channel->desc_dma_addr);
}

static int xdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct xdma_channel *channel = to_xdma_channel(chan);
	struct xdma_desc *desc;
	dma_addr_t dma_addr;
	int i, j;

pr_info("ALLOC RESOURCE FOR CHAN %d\n", channel->chan_id);
	channel->descs = dma_alloc_coherent(chan->device->dev, XRT_DESC_NUM * sizeof(*desc),
					    &channel->desc_dma_addr, GFP_KERNEL);
	if (!channel->descs)
		return -ENOMEM;

	desc = channel->descs;
	dma_addr = channel->desc_dma_addr;
	for (i = 0; i < XRT_DESC_BLOCK_NUM; i++) {
		for (j = 0; j < XDMA_DESC_ADJACENT - 1; j++) {
			desc->control = cpu_to_le32(XRT_DESC_CONTROL(1, 0));
			desc++;
		}
		dma_addr += sizeof(*desc) * XDMA_DESC_ADJACENT;
		desc->control = cpu_to_le32(XRT_DESC_CONTROL(XDMA_DESC_ADJACENT, 0));
		desc->next_lo = cpu_to_le32(XDMA_DMA_L(dma_addr));
		desc->next_hi = cpu_to_le32(XDMA_DMA_H(dma_addr));
		desc++;
	}

	return 0;
}

static irqreturn_t xdma_channel_irq_handler(int irq, void *dev_id)
{
	struct xdma_channel *channel = dev_id;

	complete(&channel->req_compl);

	return IRQ_HANDLED;
}

static int xdma_probe_channel(struct xrt_xdma *xdma, u32 base)
{
	struct xdma_channel *channel;
	u32 identifier, index;
	char *irq_name;
	int ret;

	ret = regmap_read(xdma->regmap, XDMA_CHANNEL_IDENTIFIER(base), &identifier);
	if (ret) {
		xrt_err(xdma->xdev, "failed to read identifier: %d", ret);
		return ret;
	}

	if (XDMA_GET_SUBSYSTEM_ID(identifier) != XDMA_SUBSYSTEM_ID)
		return -EINVAL;

	if (XDMA_IS_STREAM(identifier))
		return -EOPNOTSUPP;

	index = xdma->h2c.channel_num + xdma->c2h.channel_num;
	channel = &xdma->channels[index];
	channel->xdev = xdma->xdev;
	channel->chan_id = XDMA_GET_CHANNEL_ID(identifier);
	channel->type = XDMA_GET_CHANNEL_TARGET(identifier);

	if (channel->type == XDMA_TARGET_H2C_CHANNEL) {
		if (channel->chan_id != xdma->h2c.channel_num) {
			xrt_err(xdma->xdev, "Invalid id %d for H2C channel %d",
				channel->chan_id, index);
			return -EINVAL;
		}
		xdma->h2c.channel_num++;
		xdma->h2c.channel_bitmap |= BIT(channel->chan_id);
		irq_name = XRT_XDMA_CHANNEL_H2C;
	} else if (channel->type == XDMA_TARGET_C2H_CHANNEL) {
		if (channel->chan_id != xdma->c2h.channel_num) {
			xrt_err(xdma->xdev, "Invalid id %d for C2H channel %d",
				channel->chan_id, index);
			return -EINVAL;
		}
		xdma->c2h.channel_num++;
		xdma->c2h.channel_bitmap |= BIT(channel->chan_id);
		irq_name = XRT_XDMA_CHANNEL_C2H;
	} else {
		return -EINVAL;
	}

	channel->base = base;

	ret = regmap_write(xdma->regmap, XDMA_CHANNEL_CONTROL_W1C(base),
			   XDMA_CTRL_NON_INCR_ADDR);
	if (ret) {
		xrt_err(xdma->xdev, "failed to clear non_incr_addr bit");
		goto failed;
	}

	ret = regmap_write(xdma->regmap, XDMA_CHANNEL_INTERRUPT_EN(base),
			   XDMA_IE_DEFAULT);
	if (ret) {
		xrt_err(xdma->xdev, "failed to set interrupt enable reg");
		goto failed;
	}

	ret = regmap_write(xdma->regmap, XDMA_DMA_DESC_LO(base),
			   XDMA_DMA_L(channel->desc_dma_addr));
	if (ret) {
		xrt_err(xdma->xdev, "failed to set DMA descriptor low addr");
		goto failed;
	}

	ret = regmap_write(xdma->regmap, XDMA_DMA_DESC_HI(base),
			   XDMA_DMA_H(channel->desc_dma_addr));
	if (ret) {
		xrt_err(xdma->xdev, "failed to set DMA descriptor high addr");
		goto failed;
	}

	ret = xleaf_irq_request(xdma->xdev, NULL, index, xdma_channel_irq_handler, channel);
	if (ret) {
		xrt_err(xdma->xdev, "request h2c interrupt failed: %d", ret);
		goto failed;
	}
	channel->irq = index;

	init_completion(&channel->req_compl);
	channel->chan.device = &xdma->dma_dev;
	dma_cookie_init(&channel->chan);
	list_add_tail(&channel->chan.device_node, &xdma->dma_dev.channels);

	return 0;

failed:
	return ret;
}

static int xdma_init_channels(struct xrt_xdma *xdma)
{
	int i;

	for (i = 0; i < XDMA_MAX_CHANNEL_NUM; i++)
		xdma_probe_channel(xdma, XDMA_CHANNEL_RANGE * i);

	return 0;
}

static int xrt_xdma_leaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	struct xrt_xdma *xdma;
	int ret = 0;

	xdma = xrt_get_drvdata(xdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	case XRT_XDMA_REQUEST:
		break;
	default:
		xrt_err(xdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static void xrt_xdma_remove(struct xrt_device *xdev)
{
	struct xrt_xdma *xdma;

	xdma = xrt_get_drvdata(xdev);
	dma_async_device_unregister(&xdma->dma_dev);
}

static int xrt_xdma_probe(struct xrt_device *xdev)
{
	struct xrt_xdma *xdma = NULL;
	void __iomem *base = NULL;
	struct resource *res;
	int ret;

	xdma = devm_kzalloc(&xdev->dev, sizeof(*xdma), GFP_KERNEL);
	if (!xdma)
		return -ENOMEM;

	xdma->xdev = xdev;
	xrt_set_drvdata(xdev, xdma);

	res = xrt_get_resource(xdev, IORESOURCE_MEM, 0);
	if (!res) {
		xrt_err(xdev, "Empty resource 0");
		ret = -EINVAL;
		goto failed;
	}

	base = devm_ioremap_resource(&xdev->dev, res);
	if (IS_ERR(base)) {
		xrt_err(xdev, "map base iomem failed");
		ret = PTR_ERR(base);
		goto failed;
	}

	xdma->regmap = devm_regmap_init_mmio(&xdev->dev, base, &xdma_regmap_config);
	if (IS_ERR(xdma->regmap)) {
		xrt_err(xdev, "regmap %pR failed", res);
		ret = PTR_ERR(xdma->regmap);
		goto failed;
	}

	dma_cap_set(DMA_SLAVE, xdma->dma_dev.cap_mask);
	dma_cap_set(DMA_PRIVATE, xdma->dma_dev.cap_mask);

	xdma->dma_dev.dev = DEV(xdma->xdev);
	xdma->dma_dev.device_alloc_chan_resources = xdma_alloc_chan_resources;
	xdma->dma_dev.device_free_chan_resources = xdma_free_chan_resources;
	xdma->dma_dev.device_tx_status = xdma_tx_status;
	xdma->dma_dev.device_issue_pending = xdma_issue_pending;
	INIT_LIST_HEAD(&xdma->dma_dev.channels);

	ret = xdma_init_channels(xdma);
	if (ret) {
		xrt_err(xdev, "init channels failed %d", ret);
		goto failed;
	}

	ret = dma_async_device_register(&xdma->dma_dev);
	if (ret < 0) {
		xrt_err(xdma->xdev, "failed to register device: %d", ret);
		goto failed;
	}
	
	return 0;

failed:
	return ret;
}

static struct xrt_dev_endpoints xrt_xdma_endpoints[] = {
	{
		.xse_names = (struct xrt_dev_ep_names[]) {
			{ .ep_name = XRT_MD_NODE_XDMA },
			{NULL},
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static const struct xrt_device_id xrt_xdma_ids[] = {
        { XRT_SUBDEV_XDMA },
        { }
};
MODULE_DEVICE_TABLE(xrt, xrt_xdma_ids);

static struct xrt_driver xrt_xdma_driver = {
        .driver = {
                .name = XRT_XDMA,
                .owner = THIS_MODULE,
        },
        .id_table = xrt_xdma_ids,
        .subdev_id = XRT_SUBDEV_XDMA,
        .endpoints = xrt_xdma_endpoints,
        .probe = xrt_xdma_probe,
        .remove = xrt_xdma_remove,
        .leaf_call = xrt_xdma_leaf_call,
};
module_xrt_driver(xrt_xdma_driver);

MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx XRT XDMA driver");
MODULE_LICENSE("GPL v2");
