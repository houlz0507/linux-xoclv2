// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA devctl Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/xrt/metadata.h>
#include <linux/xrt/xleaf.h>
#include <linux/xrt/xleaf/xdma.h>
#include "../lib-drv.h"
#include "xdma-impl.h"

#define XRT_XDMA "xrt_xdma"
#define XRT_XDMA_CHANNEL_H2C "xrt_xdma_channel_h2c"
#define XRT_XDMA_CHANNEL_C2H "xrt_xdma_channel_c2h"
#define XRT_XDMA_CHANNEL_NAME_LEN	64

#define XRT_DESC_BLOCK_NUM 128
#define XRT_DESC_NUM	(XRT_DESC_BLOCK_NUM * XDMA_DESC_ADJACENT)

#define XRT_DESC_CONTROL(adjacent, flag)				\
	((XDMA_DESC_MAGIC << 16) | (((adjacent) - 1) << XDMA_DESC_ADJACENT_SHIFT) | (flag))

static const struct regmap_config xdma_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = XDMA_MAX_REGISTER_RANGE,
};

struct xdma_channel {
	struct xrt_device	*xdev;
	u32			base;
	u32			chan_id;
	char			name[XRT_XDMA_CHANNEL_NAME_LEN];
	int			irq;
	u32			type;
	struct xdma_desc	*descs;
	dma_addr_t		desc_dma_addr;
	u32			submitted_desc_count;
	struct completion	req_compl;
};

struct xdma_chan_info {
	u32			start_index;
	u32			channel_num;
	unsigned long		channel_bitmap;
	struct semaphore	channel_sem;
};

struct xrt_xdma {
	struct xrt_device	*xdev;
	struct device		*dma_dev;
	struct regmap		*regmap;
	struct xdma_channel	channels[XDMA_MAX_CHANNEL_NUM];
	struct xdma_chan_info	h2c; /* host to card */
	struct xdma_chan_info	c2h; /* card to host */
};

struct xdma_ioc_read_write {
	u64			paddr;
	u64			size;
	u64			data_ptr;
};

enum {
	XDMA_IOC_READ = 0,
	XDMA_IOC_WRITE = 1,
};

static irqreturn_t xdma_channel_irq_handler(int irq, void *dev_id)
{
	struct xdma_channel *channel = dev_id;

	complete(&channel->req_compl);

	return IRQ_HANDLED;
}

static void xdma_channel_reg_dump(struct xrt_xdma *xdma, struct xdma_channel *channel)
{
	char chan_name[32];
	u32 val;

	if (channel->type == XDMA_TARGET_H2C_CHANNEL)
		snprintf(chan_name, sizeof(chan_name), "H2C-%d", channel->chan_id);
	else
		snprintf(chan_name, sizeof(chan_name), "C2H-%d", channel->chan_id);

	xrt_info(xdma->xdev, "%s: base: 0x%08x", chan_name, channel->base);
	regmap_read(xdma->regmap, XDMA_CHANNEL_IDENTIFIER(channel->base), &val);
	xrt_info(xdma->xdev, "%s: id: 0x%08x", chan_name, val);
	regmap_read(xdma->regmap, XDMA_CHANNEL_STATUS(channel->base), &val);
	xrt_info(xdma->xdev, "%s: status: 0x%08x", chan_name, val);
	regmap_read(xdma->regmap, XDMA_CHANNEL_COMPL_COUNT(channel->base), &val);
	xrt_info(xdma->xdev, "%s: completed desc: 0x%08x", chan_name, val);
	regmap_read(xdma->regmap, XDMA_CHANNEL_INTERRUPT_EN(channel->base), &val);
	xrt_info(xdma->xdev, "%s: interrupt: 0x%08x", chan_name, val);
}

static void xdma_free_channel_resource(struct xrt_xdma *xdma, struct xdma_channel *channel)
{
	if (!channel->descs || !channel->desc_dma_addr)
		return;

	dma_free_coherent(xdma->dma_dev, XRT_DESC_NUM * sizeof(struct xdma_desc),
			  channel->descs, channel->desc_dma_addr);
}

static int xdma_alloc_channel_resource(struct xrt_xdma *xdma, struct xdma_channel *channel)
{
	struct xdma_desc *desc;
	dma_addr_t dma_addr;
	int i, j;

	channel->descs = dma_alloc_coherent(xdma->dma_dev, XRT_DESC_NUM * sizeof(*desc),
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

static void xdma_cleanup_channel(struct xrt_xdma *xdma, struct xdma_channel *channel)
{
	int ret;

	ret = regmap_write(xdma->regmap, XDMA_CHANNEL_INTERRUPT_EN(channel->base), 0);
	if (ret)
		xrt_err(xdma->xdev, "failed to write channel interrupt enable, ret %d", ret);

	ret = xleaf_irq_request(xdma->xdev, channel->name, channel->irq, NULL, channel);
	if (ret)
		xrt_err(xdma->xdev, "failed to unregister irq %d", channel->irq);

	xdma_free_channel_resource(xdma, channel);
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
	channel->xdev = xdma->xdev;

	channel->chan_id = XDMA_GET_CHANNEL_ID(identifier);
	channel->type = XDMA_GET_CHANNEL_TARGET(identifier);
	if (channel->type == XDMA_TARGET_H2C_CHANNEL) {
		if (channel->chan_id != xdma->h2c.channel_num) {
			xrt_err(xdma->xdev, "Invalid id %d for H2C channel %d",
				channel->chan_id, index);
		}
		snprintf(channel->name, sizeof(channel->name), "%s%d",
			 XRT_XDMA_CHANNEL_H2C, xdma->h2c.channel_num);
		xdma->h2c.channel_num++;
		xdma->h2c.channel_bitmap |= BIT(channel->chan_id);
	} else if (channel->type == XDMA_TARGET_C2H_CHANNEL) {
		if (channel->chan_id != xdma->c2h.channel_num) {
			xrt_err(xdma->xdev, "Invalid id %d for C2H channel %d",
				channel->chan_id, index);
		}
		snprintf(channel->name, sizeof(channel->name), "%s%d",
			 XRT_XDMA_CHANNEL_C2H, xdma->h2c.channel_num);
		xdma->c2h.channel_num++;
		xdma->c2h.channel_bitmap |= BIT(channel->chan_id);
	} else {
		return -EINVAL;
	}

	channel->base = base;

	ret = xdma_alloc_channel_resource(xdma, channel);
	if (ret)
		return ret;

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

	ret = xleaf_irq_request(xdma->xdev, channel->name, index,
				xdma_channel_irq_handler, channel);
	if (ret) {
		xrt_err(xdma->xdev, "request h2c interrupt failed: %d", ret);
		goto failed;
	}
	channel->irq = index;

	init_completion(&channel->req_compl);

	return 0;

failed:
	xdma_free_channel_resource(xdma, channel);
	return ret;
}

static void xdma_cleanup_channel_all(struct xrt_xdma *xdma)
{
	int i, ret;

	for (i = 0; i < xdma->h2c.channel_num + xdma->c2h.channel_num; i++) {
		ret = regmap_write(xdma->regmap, XDMA_IRQ_CHANNEL_ENABLE_W1C, 1 << i);
		if (ret)
			xrt_err(xdma->xdev, "failed write IRQ Enable w1c, ret %d", ret);
		xdma_cleanup_channel(xdma, &xdma->channels[i]);
	}
}

static int xdma_init_channels(struct xrt_xdma *xdma)
{
	u32 val = 0;
	int i, ret;

	for (i = 0; i < XDMA_MAX_CHANNEL_NUM; i++)
		xdma_probe_channel(xdma, XDMA_CHANNEL_RANGE * i);

	if (!xdma->h2c.channel_num) {
		xrt_err(xdma->xdev, "Not find h2c channel");
		goto failed;
	}
	if (!xdma->c2h.channel_num) {
		xrt_err(xdma->xdev, "Not find c2h channel");
		goto failed;
	}

	for (i = 0; i < xdma->h2c.channel_num + xdma->c2h.channel_num; i++) {
		val <<= 8;
		val |= i;
		if (i % 4 == 3) {
			ret = regmap_write(xdma->regmap, XDMA_IRQ_CHANNEL_VEC + rounddown(i, 4),
					   cpu_to_be32(val));
			if (ret) {
				xrt_err(xdma->xdev, "Init channel vector failed, %d", ret);
				goto failed;
			}
			val = 0;
		}
		ret = regmap_write(xdma->regmap, XDMA_IRQ_CHANNEL_ENABLE_W1S, 1 << i);
		if (ret) {
			xrt_err(xdma->xdev, "Enable channel interrupt failed, %d", ret);
			goto failed;
		}
	}
	sema_init(&xdma->h2c.channel_sem, xdma->h2c.channel_num);
	sema_init(&xdma->c2h.channel_sem, xdma->c2h.channel_num);
	xdma->c2h.start_index = xdma->h2c.channel_num;

	return 0;

failed:
	xdma_cleanup_channel_all(xdma);
	return ret;
}

static inline void xdma_desc_set(struct xdma_channel *channel, struct xdma_desc *desc,
				 dma_addr_t addr, u64 endpoint_addr, u32 len)
{
	desc->bytes = cpu_to_le32(len);
	if (channel->type == XDMA_TARGET_H2C_CHANNEL) {
		desc->src_addr_lo = cpu_to_le32(XDMA_DMA_L(addr));
		desc->src_addr_hi = cpu_to_le32(XDMA_DMA_H(addr));
		desc->dst_addr_lo = cpu_to_le32(XDMA_DMA_L(endpoint_addr));
		desc->dst_addr_hi = cpu_to_le32(XDMA_DMA_H(endpoint_addr));
	} else {
		desc->src_addr_lo = cpu_to_le32(XDMA_DMA_L(endpoint_addr));
		desc->src_addr_hi = cpu_to_le32(XDMA_DMA_H(endpoint_addr));
		desc->dst_addr_lo = cpu_to_le32(XDMA_DMA_L(addr));
		desc->dst_addr_hi = cpu_to_le32(XDMA_DMA_H(addr));
	}
}

static inline void xdma_desc_set_last(struct xdma_channel *channel, u32 desc_num)
{
	struct xdma_desc *block_desc = NULL, *last_desc;
	u32 adjacent;

	adjacent = desc_num & (XDMA_DESC_ADJACENT - 1);
	if (desc_num > XDMA_DESC_ADJACENT && adjacent > 0)
		block_desc = channel->descs + (desc_num & (~(XDMA_DESC_ADJACENT - 1))) - 1;

	last_desc = channel->descs + desc_num - 1;
	if (block_desc)
		block_desc->control = cpu_to_le32(XRT_DESC_CONTROL(adjacent, 0));
	last_desc->control |= cpu_to_le32(XDMA_DESC_STOPPED | XDMA_DESC_COMPLETED);
}

static inline void xdma_desc_clear_last(struct xdma_channel *channel, u32 desc_num)
{
	struct xdma_desc *block_desc = NULL, *last_desc;
	u32 adjacent;

	adjacent = desc_num & (XDMA_DESC_ADJACENT - 1);
	if (desc_num > XDMA_DESC_ADJACENT && adjacent > 0)
		block_desc = channel->descs + (desc_num & (~(XDMA_DESC_ADJACENT - 1))) - 1;

	last_desc = channel->descs + desc_num - 1;
	if (block_desc)
		block_desc->control = cpu_to_le32(XRT_DESC_CONTROL(XDMA_DESC_ADJACENT, 0));
	last_desc->control &= cpu_to_le32(~(XDMA_DESC_STOPPED | XDMA_DESC_COMPLETED));
}

static u64 xrt_xdma_start(struct xrt_xdma *xdma, struct xdma_channel *channel,
			  u64 endpoint_addr, struct scatterlist **sg, u32 *sg_off)
{
	dma_addr_t addr;
	int i, ret = 0;
	u32 len, rest;
	u64 total = 0;

	for (i = 0; i < XRT_DESC_NUM && *sg; i++) {
		addr = sg_dma_address(*sg) + *sg_off;
		rest = sg_dma_len(*sg) - *sg_off;
		if (XDMA_DESC_BLEN_MAX < rest) {
			len = XDMA_DESC_BLEN_MAX;
			*sg_off += XDMA_DESC_BLEN_MAX;
		} else {
			len = rest;
			*sg_off = 0;
			*sg = sg_next(*sg);
		}

		xdma_desc_set(channel, channel->descs + i, addr, endpoint_addr, len);
		endpoint_addr += len;
		total += len;
	}
	xdma_desc_set_last(channel, i);
	channel->submitted_desc_count = i;

	ret = regmap_write(xdma->regmap, XDMA_CHANNEL_INTERRUPT_EN(channel->base),
			   XDMA_IE_DEFAULT);
	if (ret) {
		xrt_err(xdma->xdev, "failed to set interrupt enable reg");
		return ret;
	}

	if (i >= XDMA_DESC_ADJACENT) {
		ret = regmap_write(xdma->regmap, XDMA_DMA_DESC_ADJACENT(channel->base),
				   XDMA_DESC_ADJACENT - 1);
	} else {
		ret = regmap_write(xdma->regmap, XDMA_DMA_DESC_ADJACENT(channel->base), i - 1);
	}
	if (ret) {
		xrt_err(xdma->xdev, "failed to set DMA descriptor adjacent");
		return ret;
	}

	ret = regmap_write(xdma->regmap, XDMA_CHANNEL_CONTROL(channel->base), XDMA_CTRL_START);
	if (ret) {
		xrt_err(xdma->xdev, "failed to start DMA");
		return ret;
	}

	return total;
}

static int xdma_acquire_channel(struct xrt_xdma *xdma, u32 dir)
{
	struct xdma_chan_info *chan_info;
	int channel_index;

	if (dir == DMA_TO_DEVICE)
		chan_info = &xdma->h2c;
	else
		chan_info = &xdma->c2h;

	if (down_killable(&chan_info->channel_sem))
		return -ERESTARTSYS;

	for (channel_index = 0; channel_index < chan_info->channel_num; channel_index++) {
		if (test_and_clear_bit(channel_index, &chan_info->channel_bitmap))
			return channel_index + chan_info->start_index;
	}

	up(&chan_info->channel_sem);

	return -ENOENT;
}

static void xdma_release_channel(struct xrt_xdma *xdma, u32 dir, u32 channel_index)
{
	struct xdma_chan_info *chan_info;

	if (dir == DMA_TO_DEVICE)
		chan_info = &xdma->h2c;
	else
		chan_info = &xdma->c2h;

	channel_index -= chan_info->start_index;
	set_bit(channel_index, &chan_info->channel_bitmap);
	up(&chan_info->channel_sem);
}

#if 0
static void xdma_dump_descriptor(struct xdma_channel *channel)
{
	struct xdma_desc *desc = channel->descs;
	int i;

	for (i = 0; i < channel->submitted_desc_count; i++) {
		pr_info("[%d]: ctrl %x, bytes %x, src %x.%x, dst %x.%x next %x.%x\n",
			i, desc->control, desc->bytes, desc->src_addr_hi, desc->src_addr_lo,
			desc->dst_addr_hi, desc->dst_addr_lo, desc->next_hi, desc->next_lo);
		desc++;
	}
}
#endif

static int xdma_request_submit(struct xrt_xdma *xdma, struct xrt_xdma_request *req)
{
	struct scatterlist *sg = req->sgt.sgl;
	int nents, channel_index, ret = 0;
	struct xdma_channel *channel;
	u32 val, sg_off = 0;
	u64 done_bytes = 0;

	if (!req->dma_mapped) {
		nents = dma_map_sg(xdma->dma_dev, sg, req->sgt.orig_nents, req->direction);
		req->sgt.nents = nents;
	}

	if (!req->sgt.nents) {
		xrt_err(xdma->xdev, "empty sg table");
		return -EINVAL;
	}

	channel_index = xdma_acquire_channel(xdma, req->direction);
	if (channel_index < 0) {
		xrt_err(xdma->xdev, "failed to acquire channel, ret %d", channel_index);
		return channel_index;
	}

	channel = &xdma->channels[channel_index];

	sg = req->sgt.sgl;
	while (sg && !ret) {
		done_bytes += xrt_xdma_start(xdma, channel, req->endpoint_addr + done_bytes,
					     &sg, &sg_off);
		if (!wait_for_completion_timeout(&channel->req_compl,
						 msecs_to_jiffies(XDMA_REQUEST_MAX_WAIT))) {
			xrt_err(xdma->xdev, "Wait for request timed out");
			xdma_channel_reg_dump(xdma, channel);
			ret = -EIO;
		}
	//	xdma_dump_descriptor(channel);
		ret = regmap_read(xdma->regmap, XDMA_CHANNEL_COMPL_COUNT(channel->base), &val);
		if (ret || val != channel->submitted_desc_count) {
			xrt_err(xdma->xdev, "Invalid completed count %d, expected %d",
				val, channel->submitted_desc_count);
			ret = -EINVAL;
		}
		xdma_desc_clear_last(channel, channel->submitted_desc_count);
		ret = regmap_read(xdma->regmap, XDMA_CHANNEL_STATUS_RC(channel->base), &val);
		if (ret)
			xrt_err(xdma->xdev, "failed read status register, ret %d", ret);

		ret = regmap_write(xdma->regmap, XDMA_CHANNEL_CONTROL_W1C(channel->base),
				   XDMA_CTRL_RUN_STOP);
		if (ret)
			xrt_err(xdma->xdev, "failed to write control_w1c, ret %d", ret);
	}
	xdma_release_channel(xdma, req->direction, channel_index);

	return ret;
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
		ret = xdma_request_submit(xdma, arg);
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

	xdma_cleanup_channel_all(xdma);
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

	xrt_info(xdev, "probing...");
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

	xdma->dma_dev = xleaf_get_root_dev(xdev);
	if (!xdma->dma_dev) {
		xrt_err(xdev, "get root device failed");
		ret = -EINVAL;
		goto failed;
	}

	ret = xdma_init_channels(xdma);
	if (ret) {
		xrt_err(xdev, "init channels failed %d", ret);
		goto failed;
	}

	return 0;

failed:
	return ret;
}

static int xdma_open(struct inode *inode, struct file *file)
{
	struct xrt_device *xdev = xleaf_devnode_open(inode);

	if (!xdev)
		return -ENODEV;

	xrt_info(xdev, "opened");
	file->private_data = xrt_get_drvdata(xdev);
	return 0;
}

static int xdma_close(struct inode *inode, struct file *file)
{
	struct xrt_xdma *xdma = file->private_data;

	xleaf_devnode_close(inode);

	xrt_info(xdma->xdev, "closed");
	return 0;
}

static long xdma_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct xrt_xdma *xdma = filp->private_data;
	struct xdma_ioc_read_write ioc_arg;
	struct xrt_xdma_request req = { 0 };
	struct page **pages;
	u32 gup_flags = 0;
	int nr_pages;
	int ret;

	switch (cmd) {
	case XDMA_IOC_READ:
		req.direction = DMA_FROM_DEVICE;
		gup_flags = FOLL_WRITE;
		break;
	case XDMA_IOC_WRITE:
		req.direction = DMA_TO_DEVICE;
		break;
	default:
		xrt_err(xdma->xdev, "invalid command");
		return -EINVAL;
	}

	if (copy_from_user((void *)&ioc_arg, (void __user *)arg, sizeof(ioc_arg)))
		return -EFAULT;

	nr_pages = (roundup(ioc_arg.data_ptr + ioc_arg.size, PAGE_SIZE) -
		    rounddown(ioc_arg.data_ptr, PAGE_SIZE)) >> PAGE_SHIFT;

	pages = kvzalloc(nr_pages * sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	ret = pin_user_pages_fast(ioc_arg.data_ptr, nr_pages, gup_flags, pages);
	if (ret < 0) {
		xrt_err(xdma->xdev, "pin %d pages failed, ret %d", nr_pages, ret);
		nr_pages = 0;
		goto out;
	} else if (ret != nr_pages) {
		xrt_err(xdma->xdev, "pined %d pages less than requested %d", ret, nr_pages);
		nr_pages = ret;
		goto out;
	}

	ret = sg_alloc_table_from_pages(&req.sgt, pages, nr_pages,
					ioc_arg.data_ptr & (~PAGE_MASK), ioc_arg.size,
					GFP_KERNEL);
	if (ret) {
		xrt_err(xdma->xdev, "alloc sgt failed, ret %d", ret);
		goto out;
	}
	req.endpoint_addr = ioc_arg.paddr;

	ret = xdma_request_submit(xdma, &req);
out:
	sg_free_table(&req.sgt);
	if (nr_pages)
		unpin_user_pages(pages, nr_pages);
	kvfree(pages);

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

static struct xrt_driver xrt_xdma_driver = {
	.driver = {
		.name = XRT_XDMA,
	},
	.file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = xdma_open,
			.release = xdma_close,
			.unlocked_ioctl = xdma_ioctl,
		},
		.xsf_dev_name = "xdma",
	},
	.subdev_id = XRT_SUBDEV_XDMA,
	.endpoints = xrt_xdma_endpoints,
	.probe = xrt_xdma_probe,
	.remove = xrt_xdma_remove,
	.leaf_call = xrt_xdma_leaf_call,
};

XRT_LEAF_INIT_FINI_FUNC(xdma);
