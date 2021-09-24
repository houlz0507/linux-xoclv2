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

struct xdma_tx_desc {
	struct dma_async_tx_descriptor	txd;
	struct scatterlist		*sg;
	u32				sg_len;
	u32				submitted_count;
};

struct xdma_channel {
	struct dma_chan		chan;
	struct xrt_device	*xdev;
	u32			base;
	char			name[32];
	u32			chan_id;
	int			irq;
	u32			type;
	struct xdma_desc	*descs;
	dma_addr_t		desc_dma_addr;
	struct completion	req_compl;
	enum dma_status		status;
	spinlock_t		chan_lock;	/* lock for channel data */
	phys_addr_t		endpoint_addr;
	struct xdma_tx_desc	sw_desc;
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
	struct device		*root_dev;
	struct regmap		*regmap;
	struct xdma_channel	channels[XRT_XDMA_MAX_CHANNEL_NUM];
	struct xdma_chan_info	h2c; /* host to card */
	struct xdma_chan_info	c2h; /* card to host */
};

static inline struct xdma_channel *to_xdma_channel(struct dma_chan *chan)
{
	return container_of(chan, struct xdma_channel, chan);
}

static inline struct xdma_tx_desc *to_xdma_desc(struct dma_async_tx_descriptor *txd)
{
	return container_of(txd, struct xdma_tx_desc, txd);
}

static inline struct xdma_channel *desc_to_channel(struct xdma_tx_desc *desc)
{
	return container_of(desc, struct xdma_channel, desc);
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

static void xdma_issue_pending(struct dma_chan *chan)
{
}

static dma_cookie_t xdma_tx_submit(struct dma_async_tx_descriptor *txd)
{
	struct xdma_tx_desc *desc = to_xdma_desc(txd);
	struct scatterlist *sg = desc->sg;
	struct xdma_channel *channel;
	struct xrt_xdma *xdma;
	u32 val, sg_off = 0;
	u64 done_bytes = 0;
	int nents, ret = 0;

	channel = desc_to_channel(desc);
	xdma = xrt_get_drvdata(channel->xdev);
	while (sg && !ret) {
		done_bytes += xrt_xdma_start(channel, &sg, &sg_off);
		if (!wait_for_completion_timeout(&channel->req_compl,
						 msecs_to_jiffies(XDMA_REQUEST_MAX_WAIT))) {
			xrt_err(xdma->xdev, "Wait for request timed out");
			xdma_channel_reg_dump(channel);
			ret = -EIO;
		}
		ret = regmap_read(xdma->regmap, XDMA_CHANNEL_COMPL_COUNT(channel->base), &val);
		if (ret || val != channel->sw_desc.submitted_count) {
			xrt_err(xdma->xdev, "Invalid completed count %d, expected %d",
				val, channel->sw_desc.submitted_count);
			ret = -EINVAL;
		}
		xdma_desc_clear_last(channel->sw_desc);
		ret = regmap_read(xdma->regmap, XDMA_CHANNEL_STATUS_RC(channel->base), &val);
		if (ret)
			xrt_err(xdma->xdev, "failed read status register, ret %d", ret);

		ret = regmap_write(xdma->regmap, XDMA_CHANNEL_CONTROL_W1C(channel->base),
				   XDMA_CTRL_RUN_STOP);
		if (ret)
			xrt_err(xdma->xdev, "failed to write control_w1c, ret %d", ret);
	}
}

static int xdma_slave_config(struct dma_chan *chan, struct dma_slave_config *config)
{
	struct xdma_channel *channel = to_xdma_channel(chan);
	struct xrt_xdma *xdma;

	xdma = xrt_get_drvdata(channel->xdev);
	if (channel->type == XDMA_TARGET_H2C_CHANNEL) {
		if (config->direction != DMA_MEM_TO_DEV) {
			xrt_err(xdma->xdev, "direction does not match");
			return -EINVAL;
		}
		channel->endpoint_addr = config->dst_addr;
	} else {
		if (config->direction != DMA_DEV_TO_MEM) {
			xrt_err(xdma->xdev, "direction does not match");
			return -EINVAL;
		}
		channel->endpoint_addr = config->src_addr;
	}

	return 0;
}

static struct dma_async_tx_descriptor *
xdma_prep_slave_sg(struct dma_chan * chan, struct scatterlist *sgl,
		   unsigned int sg_len, enum dma_transfer_direction dir,
		   unsigned long flags, void *context)
{
	struct xdma_channel *channel = to_xdma_channel(chan);
	struct xrt_xdma *xdma;

	xdma = xrt_get_drvdata(channel->xdev);
	if (!is_slave_direction(direction)) {
		xrt_err(xdma->xdev, "invalid dma direction");
		return NULL;
	}

	channel->sw_desc.sg = sgl;
	channel->sw_desc.sg_len = sg_len;
	channel->sw_desc.txd.flags = flags;

	return &channel->sw_desc.txd;
}

static void xdma_free_chan_resources(struct dma_chan *chan)
{
	struct xdma_channel *channel = to_xdma_channel(chan);
	struct xrt_xdma *xdma;

	if (!channel->descs || !channel->desc_dma_addr)
		return;

	xdma = xrt_get_drvdata(channel->xdev);
	dma_free_coherent(xdma->root_dev, XRT_DESC_NUM * sizeof(struct xdma_desc),
			  channel->descs, channel->desc_dma_addr);
}

static int xdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct xdma_channel *channel = to_xdma_channel(chan);
	struct xrt_xdma *xdma;
	struct xdma_desc *desc;
	dma_addr_t dma_addr;
	int i, j;

	xdma = xrt_get_drvdata(channel->xdev);
	channel->descs = dma_alloc_coherent(xdma->root_dev, XRT_DESC_NUM * sizeof(*desc),
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

static void xdma_cleanup_channels(struct xrt_xdma *xdma)
{
	struct dma_chan *chan, *_chan;
	struct xdma_channel *channel;
	int ret;

	list_for_each_entry_safe(chan, _chan, &xdma->dma_dev.channels, device_node) {
		channel = to_xdma_channel(chan);
		list_del(&chan->device_node);

		dma_release_channel(chan);
		ret = regmap_write(xdma->regmap, XDMA_IRQ_CHANNEL_ENABLE_W1C, 1 << channel->irq);
		if (ret)
			xrt_err(xdma->xdev, "failed write IRQ Enable w1c, ret %d", ret);

		ret = regmap_write(xdma->regmap, XDMA_CHANNEL_INTERRUPT_EN(channel->base), 0);
		if (ret) {
			xrt_err(xdma->xdev, "failed to write channel interrupt enable, ret %d",
				ret);
		}

		ret = xleaf_irq_request(xdma->xdev, NULL, channel->irq, NULL, channel);
		if (ret)
			xrt_err(xdma->xdev, "failed to free irq %d", channel->irq);
	}

	return;
}

static int xdma_probe_channel(struct xrt_xdma *xdma, u32 base)
{
	struct xdma_channel *channel;
	u32 identifier, index;
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

	channel->base = base;
	channel->xdev = xdma->xdev;
	channel->chan_id = XDMA_GET_CHANNEL_ID(identifier);
	channel->type = XDMA_GET_CHANNEL_TARGET(identifier);
	channel->irq = index;

	if (channel->type == XDMA_TARGET_H2C_CHANNEL) {
		if (channel->chan_id != xdma->h2c.channel_num) {
			xrt_err(xdma->xdev, "Invalid id %d for H2C channel %d",
				channel->chan_id, index);
			return -EINVAL;
		}
		xdma->h2c.channel_num++;
		xdma->h2c.channel_bitmap |= BIT(channel->chan_id);
		snprintf(channel->name, sizeof(channel->name), "%s%d",
			 XRT_XDMA_CHANNEL_H2C, channel->chan_id);
	} else if (channel->type == XDMA_TARGET_C2H_CHANNEL) {
		if (channel->chan_id != xdma->c2h.channel_num) {
			xrt_err(xdma->xdev, "Invalid id %d for C2H channel %d",
				channel->chan_id, index);
			return -EINVAL;
		}
		xdma->c2h.channel_num++;
		xdma->c2h.channel_bitmap |= BIT(channel->chan_id);
		snprintf(channel->name, sizeof(channel->name), "%s%d",
			 XRT_XDMA_CHANNEL_C2H, channel->chan_id);
	} else {
		return -EINVAL;
	}

	return 0;
}

static int xdma_init_channels(struct xrt_xdma *xdma)
{
	struct xdma_channel *channel;
	u32 val = 0;
	int i, ret;

	for (i = 0; i < xdma->h2c.channel_num + xdma->c2h.channel_num; i++) {
		val <<= XDMA_IRQ_VEC_SHIFT;
		val |= i;
		if (i % sizeof(u32) == sizeof(u32) - 1) {
			ret = regmap_write(xdma->regmap, XDMA_IRQ_CHANNEL_VEC +
					   rounddown(i, sizeof(u32)), cpu_to_be32(val));
			if (ret) {
				xrt_err(xdma->xdev, "Init channel vector failed, %d", ret);
				goto failed;
			}
			val = 0;
		}

		channel = &xdma->channels[i];
		ret = regmap_write(xdma->regmap, XDMA_IRQ_CHANNEL_ENABLE_W1S, 1 << channel->irq);
		if (ret) {
			xrt_err(xdma->xdev, "Enable channel interrupt failed, %d", ret);
			goto failed;
		}

		ret = regmap_write(xdma->regmap, XDMA_CHANNEL_CONTROL_W1C(channel->base),
				   XDMA_CTRL_NON_INCR_ADDR);
		if (ret) {
			xrt_err(xdma->xdev, "failed to clear non_incr_addr bit");
			goto failed;
		}

		ret = regmap_write(xdma->regmap, XDMA_CHANNEL_INTERRUPT_EN(channel->base),
				   XDMA_IE_DEFAULT);
		if (ret) {
			xrt_err(xdma->xdev, "failed to set interrupt enable reg");
			goto failed;
		}

		ret = regmap_write(xdma->regmap, XDMA_DMA_DESC_LO(channel->base),
				   XDMA_DMA_L(channel->desc_dma_addr));
		if (ret) {
			xrt_err(xdma->xdev, "failed to set DMA descriptor low addr");
			goto failed;
		}

		ret = regmap_write(xdma->regmap, XDMA_DMA_DESC_HI(channel->base),
				   XDMA_DMA_H(channel->desc_dma_addr));
		if (ret) {
			xrt_err(xdma->xdev, "failed to set DMA descriptor high addr");
			goto failed;
		}

		ret = xleaf_irq_request(xdma->xdev, channel->name, channel->irq,
					xdma_channel_irq_handler, channel);
		if (ret) {
			xrt_err(xdma->xdev, "request h2c interrupt failed: %d", ret);
			goto failed;
		}

		channel->chan.device = &xdma->dma_dev;
		init_completion(&channel->req_compl);
		dma_async_tx_descriptor_init(&channel->desc.txd, &channel->chan);
		if (!dma_get_slave_channel(&channel->chan)) {
			xrt_err(xdma->xdev, "failed to get slave channel");
			ret = -EINVAL;
			goto failed;
		}

		list_add_tail(&channel->chan.device_node, &xdma->dma_dev.channels);
	}
	sema_init(&xdma->h2c.channel_sem, xdma->h2c.channel_num);
	sema_init(&xdma->c2h.channel_sem, xdma->c2h.channel_num);
	xdma->c2h.start_index = xdma->h2c.channel_num;

	return 0;

failed:
	xdma_cleanup_channels(xdma);
	return ret;
}

static void xdma_event_cb(struct xrt_device *xdev, void *arg)
{
	struct xrt_event *evt = (struct xrt_event *)arg;
	struct xrt_xdma *xdma = xrt_get_drvdata(xdev);
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id;
	int ret;

	id = evt->xe_subdev.xevt_subdev_id;
	switch (e) {
	case XRT_EVENT_POST_CREATION:
		if (id == XRT_SUBDEV_XDMA) {
			ret = xdma_init_channels(xdma);
			if (ret)
				xrt_err(xdma->xdev, "failed to init channels, %d", ret);
		}
		break;
	case XRT_EVENT_PRE_REMOVAL:
		if (id == XRT_SUBDEV_XDMA)
			xdma_cleanup_channels(xdma);
		break;
	default:
		xrt_dbg(xdev, "ignored event %d", e);
		break;
	}
}

static void xdma_acquire_channel(struct xrt_xdma *xdma, struct xrt_xdma_channel_req *req)
{
	struct xdma_chan_info *chan_info;
	int channel_index;

	if (req->direction == DMA_MEM_TO_DEV)
		chan_info = &xdma->h2c;
	else
		chan_info = &xdma->c2h;

	if (down_killable(&chan_info->channel_sem)) {
		req->chan = NULL;
		return;
	}

	for (channel_index = 0; channel_index < chan_info->channel_num; channel_index++) {
		if (test_and_clear_bit(channel_index, &chan_info->channel_bitmap)) {
			req->chan = &xdma->channels[channel_index + chan_info->start_index];
			return;
		}
	}
}

static void xdma_release_channel(struct xrt_xdma *xdma, struct xrt_xdma_channel_req *req)
{
	struct xdma_channel *channel = to_xdma_channel(chan);
	struct xrt_xdma *xdma;

	xdma = xrt_get_drvdata(channel->xdev);
	if (channel->type == XDMA_TARGET_H2C_CHANNEL)
		chan_info = &xdma->h2c;
	else
		chan_info = &xdma->c2h;

	set_bit(channel->chan_id, &chan_info->channel_bitmap);
	up(&chan_info->channel_sem);
}

static int xrt_xdma_leaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	struct xrt_xdma *xdma;

	xdma = xrt_get_drvdata(xdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		xdma_event_cb(xdev, arg);
		break;
	case XRT_XDMA_GET_CHANNEL:
		xdma_acquire_channel(xdma, arg);
		break;
	case XRT_XDMA_PUT_CHANNEL:
		xdma_release_channel(xdma, arg);
		break;
	default:
		xrt_err(xdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return 0;
}

static int xrt_xdma_probe(struct xrt_device *xdev)
{
	struct xrt_xdma *xdma = NULL;
	void __iomem *base = NULL;
	struct resource *res;
	int i, ret;

	xdma = devm_kzalloc(&xdev->dev, sizeof(*xdma), GFP_KERNEL);
	if (!xdma)
		return -ENOMEM;

	xdma->xdev = xdev;
	xrt_set_drvdata(xdev, xdma);

	res = xrt_get_resource(xdev, IORESOURCE_MEM, 0);
	if (!res) {
		xrt_err(xdev, "Empty resource 0");
		return -EINVAL;
	}

	base = devm_ioremap_resource(&xdev->dev, res);
	if (IS_ERR(base)) {
		xrt_err(xdev, "map base iomem failed");
		return PTR_ERR(base);
	}

	xdma->regmap = devm_regmap_init_mmio(&xdev->dev, base, &xdma_regmap_config);
	if (IS_ERR(xdma->regmap)) {
		xrt_err(xdev, "regmap %pR failed", res);
		return PTR_ERR(xdma->regmap);
	}

	xdma->root_dev = xleaf_get_root_dev(xdma->xdev);
	if (!xdma->root_dev) {
		xrt_err(xdev, "can not get root device");
		return -EINVAL;
	}

	ret = dma_set_mask_and_coherent(xdma->root_dev, DMA_BIT_MASK(64));
	if (ret) {
		xrt_err(xdev, "set dma mask failed, ret %d", ret);
		return ret;
	}

	dma_cap_set(DMA_SLAVE, xdma->dma_dev.cap_mask);
	dma_cap_set(DMA_PRIVATE, xdma->dma_dev.cap_mask);

	xdma->dma_dev.dev = &xdev->dev;
	xdma->dma_dev.device_alloc_chan_resources = xdma_alloc_chan_resources;
	xdma->dma_dev.device_free_chan_resources = xdma_free_chan_resources;
	xdma->dma_dev.device_tx_status = xdma_tx_status;
	xdma->dma_dev.device_issue_pending = xdma_issue_pending;
	INIT_LIST_HEAD(&xdma->dma_dev.channels);

	for (i = 0; i < XRT_XDMA_MAX_CHANNEL_NUM; i++)
		xdma_probe_channel(xdma, XDMA_CHANNEL_RANGE * i);

	if (!xdma->h2c.channel_num) {
		xrt_err(xdma->xdev, "Not find h2c channel");
		return -EINVAL;
	}
	if (!xdma->c2h.channel_num) {
		xrt_err(xdma->xdev, "Not find c2h channel");
		return -EINVAL;
	}

	ret = dmaenginem_async_device_register(&xdma->dma_dev);
	if (ret < 0) {
		xrt_err(xdma->xdev, "failed to register device: %d", ret);
		return ret;
	}

	return 0;
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
        .leaf_call = xrt_xdma_leaf_call,
};
module_xrt_driver(xrt_xdma_driver);

MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx XRT XDMA driver");
MODULE_LICENSE("GPL v2");
