// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DMA driver for Xilinx DMA/Bridge Subsystem
 *
 * Copyright (C) 2017-2020 Xilinx, Inc. All rights reserved.
 * Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
 */

/*
 * The DMA/Bridge Subsystem for PCI Express allows for the movement of data
 * between Host memory and the DMA subsystem. It does this by operating on
 * 'descriptors' that contain information about the source, destination and
 * amount of data to transfer. These direct memory transfers can be both in
 * the Host to Card (H2C) and Card to Host (C2H) transfers. The DMA can be
 * configured to have a single AXI4 Master interface shared by all channels
 * or one AXI4-Stream interface for each channel enabled. Memory transfers are
 * specified on a per-channel basis in descriptor linked lists, which the DMA
 * fetches from host memory and processes. Events such as descriptor completion
 * and errors are signaled using interrupts. The core also provides up to 16
 * user interrupt wires that generate interrupts to the host.
 */

#include <linux/mod_devicetable.h>
#include <linux/bitfield.h>
#include <linux/dmapool.h>
#include <linux/regmap.h>
#include <linux/dmaengine.h>
#include <linux/platform_device.h>
#include <linux/platform_data/amd_xdma.h>
#include <linux/dma-mapping.h>
#include "../virt-dma.h"
#include "xdma-regs.h"

/* mmio regmap config for all XDMA registers */
static const struct regmap_config xdma_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = XDMA_REG_SPACE_LEN,
};

/**
 * struct xdma_desc_block - Descriptor block
 * @virt_addr: Virtual address of block start
 * @dma_addr: DMA address of block start
 */
struct xdma_desc_block {
	void		*virt_addr;
	dma_addr_t	dma_addr;
};

/**
 * struct xdma_chan - Driver specific DMA channel structure
 * @vchan: Virtual channel
 * @xdev_hdl: Pointer to DMA device structure
 * @base: Offset of channel registers
 * @desc_pool: Descriptor pool
 * @busy: Busy flag of the channel
 * @dir: Transferring direction of the channel
 * @cfg: Transferring config of the channel
 * @irq: IRQ assigned to the channel
 * @tasklet: Channel tasklet
 */
struct xdma_chan {
	struct virt_dma_chan		vchan;
	void				*xdev_hdl;
	u32				base;
	struct dma_pool			*desc_pool;
	bool				busy;
	enum dma_transfer_direction	dir;
	struct dma_slave_config		cfg;
	u32				irq;
	struct tasklet_struct		tasklet;
};

/**
 * struct xdma_desc - DMA desc structure
 * @vdesc: Virtual DMA descriptor
 * @dir: Transferring direction of the request
 * @dev_addr: Physical address on DMA device side
 * @desc_blocks: Hardware descriptor blocks
 * @dblk_num: Number of hardware descriptor blocks
 * @desc_num: Number of hardware descriptors
 * @completed_desc_num: Completed hardware descriptors
 */
struct xdma_desc {
	struct virt_dma_desc		vdesc;
	struct xdma_chan		*chan;
	enum dma_transfer_direction	dir;
	u64				dev_addr;
	struct xdma_desc_block		*desc_blocks;
	u32				dblk_num;
	u32				desc_num;
	u32				completed_desc_num;
};

#define XDMA_DEV_STATUS_REG_DMA		BIT(0)
#define XDMA_DEV_STATUS_INIT_MSIX	BIT(1)

/**
 * struct xdma_device - DMA device structure
 * @pdev: Platform device pointer
 * @dma_dev: DMA device structure
 * @regmap: MMIO regmap for DMA registers
 * @h2c_chans: Host to Card channels
 * @c2h_chans: Card to Host channels
 * @h2c_chan_num: Number of H2C channels
 * @c2h_chan_num: Number of C2H channels
 * @irq_start: Start IRQ assigned to device
 * @irq_num: Number of IRQ assigned to device
 * @status: Initialization status
 */
struct xdma_device {
	struct platform_device	*pdev;
	struct dma_device	dma_dev;
	struct regmap		*regmap;
	struct xdma_chan	*h2c_chans;
	struct xdma_chan	*c2h_chans;
	u32			h2c_chan_num;
	u32			c2h_chan_num;
	u32			irq_start;
	u32			irq_num;
	u32			status;
};

#define xdma_err(xdev, fmt, args...)					\
	dev_err(&(xdev)->pdev->dev, fmt, ##args)

/* Read and Write DMA registers */
static inline int xdma_read_reg(struct xdma_device *xdev, u32 base, u32 reg,
				u32 *val)
{
	return regmap_read(xdev->regmap, base + reg, val);
}

static inline int xdma_write_reg(struct xdma_device *xdev, u32 base, u32 reg,
				 u32 val)
{
	return regmap_write(xdev->regmap, base + reg, val);
}

/* Get the last desc in a desc block */
static inline void *xdma_blk_last_desc(struct xdma_desc_block *block)
{
	return block->virt_addr + (XDMA_DESC_ADJACENT - 1) * XDMA_DESC_SIZE;
}

/**
 * xdma_link_desc_blocks - Link descriptor blocks for DMA transfer
 * @sw_desc: Tx descriptor pointer
 */
static void xdma_link_desc_blocks(struct xdma_desc *sw_desc)
{
	struct xdma_desc_block *block;
	u32 last_blk_desc_num, desc_control;
	struct xdma_hw_desc *desc;
	int i;

	desc_control = XDMA_DESC_CONTROL(XDMA_DESC_ADJACENT, 0);
	for (i = 1; i < sw_desc->dblk_num; i++) {
		block = &sw_desc->desc_blocks[i - 1];
		desc = xdma_blk_last_desc(block);

		if (!(i & XDMA_DESC_BLOCK_MASK)) {
			desc->control = cpu_to_le32(XDMA_DESC_CONTROL_LAST);
			continue;
		}
		desc->control = cpu_to_le32(desc_control);
		desc->next_desc = cpu_to_le64(block[1].dma_addr);
	}

	/* update the last block */
	last_blk_desc_num = sw_desc->desc_num & XDMA_DESC_ADJACENT_MASK;
	if ((sw_desc->dblk_num & XDMA_DESC_BLOCK_MASK) > 1 &&
	    last_blk_desc_num) {
		block = &sw_desc->desc_blocks[sw_desc->dblk_num - 2];
		desc = xdma_blk_last_desc(block);
		desc_control = XDMA_DESC_CONTROL(last_blk_desc_num, 0);
		desc->control = cpu_to_le32(desc_control);
	}

	block = &sw_desc->desc_blocks[sw_desc->dblk_num - 1];
	desc = block->virt_addr + (last_blk_desc_num - 1) * XDMA_DESC_SIZE;
	desc->control = cpu_to_le32(XDMA_DESC_CONTROL_LAST);
}

static inline struct xdma_chan *to_xdma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct xdma_chan, vchan.chan);
}

static inline struct xdma_desc *to_xdma_desc(struct virt_dma_desc *vdesc)
{
	return container_of(vdesc, struct xdma_desc, vdesc);
}

static int xdma_enable_intr(struct xdma_device *xdev)
{
	int ret;

	ret = xdma_write_reg(xdev, XDMA_IRQ_BASE, XDMA_IRQ_CHAN_INT_EN_W1S, ~0);
	if (ret)
		xdma_err(xdev, "enable channel intr failed: %d", ret);

	return ret;
}

static int xdma_disable_intr(struct xdma_device *xdev)
{
	int ret;

	ret = xdma_write_reg(xdev, XDMA_IRQ_BASE, XDMA_IRQ_CHAN_INT_EN_W1C, ~0);
	if (ret)
		xdma_err(xdev, "disable channel intr failed: %d", ret);

	return ret;
}

/**
 * xdma_channel_init - Initialize DMA channel registers
 * @chan: DMA channel pointer
 */
static int xdma_channel_init(struct xdma_chan *chan)
{
	struct xdma_device *xdev = chan->xdev_hdl;
	int ret;

	ret = xdma_write_reg(xdev, chan->base, XDMA_CHAN_CONTROL_W1C,
			     CHAN_CTRL_NON_INCR_ADDR);
	if (ret) {
		xdma_err(xdev, "clear non incr addr failed: %d", ret);
		return ret;
	}

	ret = xdma_write_reg(xdev, chan->base, XDMA_CHAN_INTR_ENABLE,
			     CHAN_IM_ALL);
	if (ret) {
		xdma_err(xdev, "failed to set interrupt mask: %d", ret);
		return ret;
	}

	return 0;
}

/**
 * xdma_free_desc - Free descriptor
 * @vdesc: Virtual DMA descriptor
 */
static void xdma_free_desc(struct virt_dma_desc *vdesc)
{
	struct xdma_desc *sw_desc;
	int i;

	sw_desc = to_xdma_desc(vdesc);
	for (i = 0; i < sw_desc->dblk_num; i++) {
		if (!sw_desc->desc_blocks[i].virt_addr)
			break;
		dma_pool_free(sw_desc->chan->desc_pool,
			      sw_desc->desc_blocks[i].virt_addr,
			      sw_desc->desc_blocks[i].dma_addr);
	}
	kfree(sw_desc->desc_blocks);
	kfree(sw_desc);
}

/**
 * xdma_alloc_desc - Allocate descriptor
 * @chan: DMA channel pointer
 * @desc_num: Number of hardware descriptors
 */
static struct xdma_desc *
xdma_alloc_desc(struct xdma_chan *chan, u32 desc_num)
{
	struct xdma_desc *sw_desc;
	struct xdma_hw_desc *desc;
	dma_addr_t dma_addr;
	u32 dblk_num;
	void *addr;
	int i, j;

	sw_desc = kzalloc(sizeof(*sw_desc), GFP_NOWAIT);
	if (!sw_desc)
		return NULL;

	sw_desc->chan = chan;
	sw_desc->desc_num = desc_num;
	dblk_num = DIV_ROUND_UP(desc_num, XDMA_DESC_ADJACENT);
	sw_desc->desc_blocks = kcalloc(dblk_num, sizeof(*sw_desc->desc_blocks),
				       GFP_NOWAIT);
	if (!sw_desc->desc_blocks)
		goto failed;

	sw_desc->dblk_num = dblk_num;
	for (i = 0; i < sw_desc->dblk_num; i++) {
		addr = dma_pool_alloc(chan->desc_pool, GFP_NOWAIT, &dma_addr);
		if (!addr)
			goto failed;

		sw_desc->desc_blocks[i].virt_addr = addr;
		sw_desc->desc_blocks[i].dma_addr = dma_addr;
		for (j = 0, desc = addr; j < XDMA_DESC_ADJACENT; j++)
			desc[j].control = cpu_to_le32(XDMA_DESC_CONTROL(1, 0));
	}

	xdma_link_desc_blocks(sw_desc);

	return sw_desc;

failed:
	xdma_free_desc(&sw_desc->vdesc);
	return NULL;
}

/**
 * xdma_xfer_start - Start DMA transfer
 * @xdma_chan: DMA channel pointer
 */
static int xdma_xfer_start(struct xdma_chan *xdma_chan)
{
	struct virt_dma_desc *vd = vchan_next_desc(&xdma_chan->vchan);
	struct xdma_device *xdev = xdma_chan->xdev_hdl;
	struct xdma_desc_block *block;
	u32 val, completed_blocks;
	struct xdma_desc *desc;
	int ret;

	/*
	 * check if there is not any submitted descriptor or channel is busy.
	 * vchan lock should be held where this function is called.
	 */
	if (!vd || xdma_chan->busy)
		return -EINVAL;

	/* clear run stop bit to get ready for transfer */
	ret = xdma_write_reg(xdev, xdma_chan->base, XDMA_CHAN_CONTROL_W1C,
			     CHAN_CTRL_RUN_STOP);
	if (ret) {
		dev_err(&xdev->pdev->dev, "write control failed: %d", ret);
		return ret;
	}

	desc = to_xdma_desc(vd);
	if (desc->dir != xdma_chan->dir) {
		dev_err(&xdev->pdev->dev, "incorrect request direction");
		return -EINVAL;
	}

	/* set DMA engine to the first descriptor block */
	completed_blocks = desc->completed_desc_num / XDMA_DESC_ADJACENT;
	block = &desc->desc_blocks[completed_blocks];
	val = FIELD_GET(XDMA_LO_ADDR_MASK, block->dma_addr);
	ret = xdma_write_reg(xdev, XDMA_SGDMA_BASE(xdma_chan->base),
			     XDMA_SGDMA_DESC_LO, val);
	if (ret) {
		dev_err(&xdev->pdev->dev, "write hi addr failed: %d", ret);
		return ret;
	}

	val = FIELD_GET(XDMA_HI_ADDR_MASK, block->dma_addr);
	ret = xdma_write_reg(xdev, XDMA_SGDMA_BASE(xdma_chan->base),
			     XDMA_SGDMA_DESC_HI, val);
	if (ret) {
		dev_err(&xdev->pdev->dev, "write lo addr failed: %d", ret);
		return ret;
	}

	if (completed_blocks + 1 == desc->dblk_num)
		val = (desc->desc_num - 1) & XDMA_DESC_ADJACENT_MASK;
	else
		val = XDMA_DESC_ADJACENT - 1;
	ret = xdma_write_reg(xdev, XDMA_SGDMA_BASE(xdma_chan->base),
			     XDMA_SGDMA_DESC_ADJ, val);
	if (ret) {
		dev_err(&xdev->pdev->dev, "write adjacent failed: %d", ret);
		return ret;
	}

	/* kick off DMA transfer */
	ret = xdma_write_reg(xdev, xdma_chan->base, XDMA_CHAN_CONTROL,
			     CHAN_CTRL_START);
	if (ret) {
		dev_err(&xdev->pdev->dev, "write control failed: %d", ret);
		return ret;
	}

	xdma_chan->busy = true;
	return 0;
}

static void xdma_channel_tasklet(struct tasklet_struct *t)
{
	struct xdma_chan *xdma_chan = from_tasklet(xdma_chan, t, tasklet);

	spin_lock(&xdma_chan->vchan.lock);
	xdma_xfer_start(xdma_chan);
	spin_unlock(&xdma_chan->vchan.lock);
}

/**
 * xdma_config_channels - Detect and config DMA channels
 * @xdev: DMA device pointer
 * @dir: Channel direction
 */
static int xdma_config_channels(struct xdma_device *xdev,
				enum dma_transfer_direction dir)
{
	struct xdma_platdata *pdata = dev_get_platdata(&xdev->pdev->dev);
	u32 base, identifier, target;
	struct xdma_chan **chans;
	u32 *chan_num;
	int i, j, ret;

	if (dir == DMA_MEM_TO_DEV) {
		base = XDMA_CHAN_H2C_OFFSET;
		target = XDMA_CHAN_H2C_TARGET;
		chans = &xdev->h2c_chans;
		chan_num = &xdev->h2c_chan_num;
	} else if (dir == DMA_DEV_TO_MEM) {
		base = XDMA_CHAN_C2H_OFFSET;
		target = XDMA_CHAN_C2H_TARGET;
		chans = &xdev->c2h_chans;
		chan_num = &xdev->c2h_chan_num;
	} else {
		dev_err(&xdev->pdev->dev, "invalid direction specified");
		return -EINVAL;
	}

	/* detect number of available DMA channels */
	for (i = 0, *chan_num = 0; i < pdata->max_dma_channels; i++) {
		ret = xdma_read_reg(xdev, base + i * XDMA_CHAN_STRIDE,
				    XDMA_CHAN_IDENTIFIER, &identifier);
		if (ret) {
			dev_err(&xdev->pdev->dev,
				"failed to read channel id: %d", ret);
			return ret;
		}

		/* check if it is available DMA channel */
		if (XDMA_CHAN_CHECK_TARGET(identifier, target))
			(*chan_num)++;
	}

	if (!*chan_num) {
		dev_err(&xdev->pdev->dev, "does not probe any channel");
		return -EINVAL;
	}

	*chans = devm_kzalloc(&xdev->pdev->dev, sizeof(**chans) * (*chan_num),
			      GFP_KERNEL);
	if (!*chans)
		return -ENOMEM;

	for (i = 0, j = 0; i < pdata->max_dma_channels; i++) {
		ret = xdma_read_reg(xdev, base + i * XDMA_CHAN_STRIDE,
				    XDMA_CHAN_IDENTIFIER, &identifier);
		if (ret) {
			dev_err(&xdev->pdev->dev,
				"failed to read channel id: %d", ret);
			return ret;
		}

		if (!XDMA_CHAN_CHECK_TARGET(identifier, target))
			continue;

		if (j == *chan_num) {
			dev_err(&xdev->pdev->dev, "invalid channel number");
			return -EIO;
		}

		/* init channel structure and hardware */
		(*chans)[j].xdev_hdl = xdev;
		(*chans)[j].base = base + i * XDMA_CHAN_STRIDE;
		(*chans)[j].dir = dir;

		ret = xdma_channel_init(&(*chans)[j]);
		if (ret)
			return ret;
		(*chans)[j].vchan.desc_free = xdma_free_desc;
		vchan_init(&(*chans)[j].vchan, &xdev->dma_dev);

		j++;
	}

	dev_info(&xdev->pdev->dev, "configured %d %s channels", j,
		 (dir == DMA_MEM_TO_DEV) ? "H2C" : "C2H");

	return 0;
}

/**
 * xdma_issue_pending - Issue pending transactions
 * @chan: DMA channel pointer
 */
static void xdma_issue_pending(struct dma_chan *chan)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&xdma_chan->vchan.lock, flags);
	if (vchan_issue_pending(&xdma_chan->vchan))
		xdma_xfer_start(xdma_chan);
	spin_unlock_irqrestore(&xdma_chan->vchan.lock, flags);
}

/**
 * xdma_prep_device_sg - prepare a descriptor for a
 *	DMA transaction
 * @chan: DMA channel pointer
 * @sgl: Transfer scatter gather list
 * @sg_len: Length of scatter gather list
 * @dir: Transfer direction
 * @flags: transfer ack flags
 * @context: APP words of the descriptor
 */
static struct dma_async_tx_descriptor *
xdma_prep_device_sg(struct dma_chan *chan, struct scatterlist *sgl,
		    unsigned int sg_len, enum dma_transfer_direction dir,
		    unsigned long flags, void *context)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	struct dma_async_tx_descriptor *tx_desc;
	u32 desc_num = 0, i, len, rest;
	struct xdma_desc_block *dblk;
	struct xdma_desc *sw_desc;
	struct scatterlist *sg;
	dma_addr_t addr;
	u64 dev_addr, *src, *dst;
	struct xdma_hw_desc *desc;

	for_each_sg(sgl, sg, sg_len, i)
		desc_num += DIV_ROUND_UP(sg_dma_len(sg), XDMA_DESC_BLEN_MAX);

	sw_desc = xdma_alloc_desc(xdma_chan, desc_num);
	if (!sw_desc)
		return NULL;
	sw_desc->dir = dir;

	if (dir == DMA_MEM_TO_DEV) {
		dev_addr = xdma_chan->cfg.dst_addr;
		src = &addr;
		dst = &dev_addr;
	} else {
		dev_addr = xdma_chan->cfg.src_addr;
		src = &dev_addr;
		dst = &addr;
	}

	dblk = sw_desc->desc_blocks;
	desc = dblk->virt_addr;
	desc_num = 1;
	for_each_sg(sgl, sg, sg_len, i) {
		addr = sg_dma_address(sg);
		rest = sg_dma_len(sg);

		do {
			len = min_t(u32, rest, XDMA_DESC_BLEN_MAX);
			/* set hardware descriptor */
			desc->bytes = cpu_to_le32(len);
			desc->src_addr = cpu_to_le64(*src);
			desc->dst_addr = cpu_to_le64(*dst);

			if (!(desc_num & XDMA_DESC_ADJACENT_MASK)) {
				dblk++;
				desc = dblk->virt_addr;
			} else {
				desc++;
			}

			desc_num++;
			dev_addr += len;
			addr += len;
			rest -= len;
		} while (rest);
	}

	tx_desc = vchan_tx_prep(&xdma_chan->vchan, &sw_desc->vdesc, flags);
	if (!tx_desc)
		goto failed;

	return tx_desc;

failed:
	xdma_free_desc(&sw_desc->vdesc);

	return NULL;
}

/**
 * xdma_device_config - Configure the DMA channel
 * @chan: DMA channel
 * @cfg: channel configuration
 */
static int xdma_device_config(struct dma_chan *chan,
			      struct dma_slave_config *cfg)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);

	memcpy(&xdma_chan->cfg, cfg, sizeof(*cfg));

	return 0;
}

/**
 * xdma_free_chan_resources - Free channel resources
 * @chan: DMA channel
 */
static void xdma_free_chan_resources(struct dma_chan *chan)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);

	vchan_free_chan_resources(&xdma_chan->vchan);
	dma_pool_destroy(xdma_chan->desc_pool);
	xdma_chan->desc_pool = NULL;
}

/**
 * xdma_alloc_chan_resources - Allocate channel resources
 * @chan: DMA channel
 */
static int xdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	struct xdma_device *xdev = xdma_chan->xdev_hdl;

	xdma_chan->desc_pool = dma_pool_create(dma_chan_name(chan),
					       xdev->dma_dev.dev,
					       XDMA_DESC_BLOCK_SIZE,
					       XDMA_DESC_BLOCK_ALIGN,
					       0);
	if (!xdma_chan->desc_pool) {
		dev_err(&chan->dev->device,
			"unable to allocate descriptor pool");
		return -ENOMEM;
	}

	return 0;
}

/**
 * xdma_channel_isr - XDMA channel interrupt handler
 * @irq: IRQ number
 * @dev_id: Pointer to the DMA channel structure
 */
static irqreturn_t xdma_channel_isr(int irq, void *dev_id)
{
	struct xdma_chan *xdma_chan = dev_id;
	u32 complete_desc_num = 0;
	struct virt_dma_desc *vd;
	struct xdma_desc *desc;
	int ret;

	spin_lock(&xdma_chan->vchan.lock);

	/* get submitted request */
	vd = vchan_next_desc(&xdma_chan->vchan);
	if (!vd)
		goto out;

	xdma_chan->busy = false;
	desc = to_xdma_desc(vd);

	ret = xdma_read_reg(xdma_chan->xdev_hdl, xdma_chan->base,
			    XDMA_CHAN_COMPLETED_DESC, &complete_desc_num);
	if (ret)
		goto out;

	desc->completed_desc_num += complete_desc_num;
	/*
	 * if all data blocks are transferred, remove and complete the request
	 */
	if (desc->completed_desc_num == desc->desc_num) {
		list_del(&vd->node);
		vchan_cookie_complete(vd);
		goto out;
	}

	if (desc->completed_desc_num > desc->desc_num ||
	    complete_desc_num != XDMA_DESC_BLOCK_NUM * XDMA_DESC_ADJACENT)
		goto out;

	/* transfer the rest of data */
	tasklet_schedule(&xdma_chan->tasklet);

out:
	spin_unlock(&xdma_chan->vchan.lock);
	return IRQ_HANDLED;
}

/**
 * xdma_irq_fini - Uninitialize IRQ
 * @xdev: DMA device pointer
 */
static void xdma_irq_fini(struct xdma_device *xdev)
{
	int ret, i;

	/* disable interrupt */
	ret = xdma_disable_intr(xdev);
	if (ret) {
		dev_err(&xdev->pdev->dev, "failed to disable interrupts: %d",
			ret);
	}

	/* free irq handler */
	for (i = 0; i < xdev->h2c_chan_num; i++) {
		free_irq(xdev->h2c_chans[i].irq, &xdev->h2c_chans[i]);
		tasklet_kill(&xdev->h2c_chans[i].tasklet);
	}
	for (i = 0; i < xdev->c2h_chan_num; i++) {
		free_irq(xdev->c2h_chans[i].irq, &xdev->c2h_chans[i]);
		tasklet_kill(&xdev->c2h_chans[i].tasklet);
	}
}

/**
 * xdma_set_vector_reg - configure hardware IRQ registers
 * @xdev: DMA device pointer
 * @vec_tbl_start: Start of IRQ registers
 * @irq_start: Start of IRQ
 * @irq_num: Number of IRQ
 */
static int xdma_set_vector_reg(struct xdma_device *xdev, u32 vec_tbl_start,
			       u32 irq_start, u32 irq_num)
{
	u32 shift, i, val = 0;
	int ret;

	/* Each IRQ register is 32 bit and contains 4 IRQs */
	while (irq_num > 0) {
		for (i = 0; i < 4; i++) {
			shift = XDMA_IRQ_VEC_SHIFT * i;
			val |= irq_start << shift;
			irq_start++;
			irq_num--;
		}

		/* write IRQ register */
		ret = xdma_write_reg(xdev, XDMA_IRQ_BASE, vec_tbl_start, val);
		if (ret) {
			dev_err(&xdev->pdev->dev, "failed to set vector: %d",
				ret);
			return ret;
		}
		vec_tbl_start += sizeof(u32);
		val = 0;
	}

	return 0;
}

/**
 * xdma_irq_init - initialize IRQs
 * @xdev: DMA device pointer
 */
static int xdma_irq_init(struct xdma_device *xdev)
{
	u32 irq = xdev->irq_start;
	int i, j, ret;

	/* return failure if there are not enough IRQs */
	if (xdev->irq_num < xdev->h2c_chan_num + xdev->c2h_chan_num) {
		dev_err(&xdev->pdev->dev, "not enough irq");
		return -EINVAL;
	}

	/* setup H2C interrupt handler */
	for (i = 0; i < xdev->h2c_chan_num; i++) {
		ret = request_irq(irq, xdma_channel_isr, 0,
				  "xdma-h2c-channel", &xdev->h2c_chans[i]);
		if (ret) {
			dev_err(&xdev->pdev->dev,
				"H2C channel%d request irq%d failed: %d",
				i, irq, ret);
			for (j = 0; j < i; j++) {
				free_irq(xdev->h2c_chans[j].irq,
					 &xdev->h2c_chans[j]);
			}
			return ret;
		}
		xdev->h2c_chans[i].irq = irq;
		tasklet_setup(&xdev->h2c_chans[i].tasklet,
			      xdma_channel_tasklet);
		irq++;
	}

	/* setup C2H interrupt handler */
	for (i = 0; i < xdev->c2h_chan_num; i++) {
		ret = request_irq(irq, xdma_channel_isr, 0,
				  "xdma-c2h-channel", &xdev->c2h_chans[i]);
		if (ret) {
			dev_err(&xdev->pdev->dev,
				"H2C channel%d request irq%d failed: %d",
				i, irq, ret);
			for (j = 0; j < i; j++) {
				free_irq(xdev->c2h_chans[j].irq,
					 &xdev->c2h_chans[j]);
			}
			goto failed_init_c2h;
		}
		xdev->c2h_chans[i].irq = irq;
		tasklet_setup(&xdev->c2h_chans[i].tasklet,
			      xdma_channel_tasklet);
		irq++;
	}

	/* config hardware IRQ registers */
	ret = xdma_set_vector_reg(xdev, XDMA_IRQ_CHAN_VEC_NUM, 0,
				  xdev->h2c_chan_num + xdev->c2h_chan_num);
	if (ret) {
		dev_err(&xdev->pdev->dev, "failed to set channel vectors: %d",
			ret);
		goto failed;
	}

	/* enable interrupt */
	ret = xdma_enable_intr(xdev);
	if (ret) {
		dev_err(&xdev->pdev->dev, "failed to enable interrupts: %d",
			ret);
		goto failed;
	}

	return 0;

failed:
	for (i = 0; i < xdev->c2h_chan_num; i++)
		free_irq(xdev->c2h_chans[i].irq, &xdev->c2h_chans[i]);
failed_init_c2h:
	for (i = 0; i < xdev->h2c_chan_num; i++)
		free_irq(xdev->h2c_chans[i].irq, &xdev->h2c_chans[i]);

	return ret;
}

static bool xdma_filter_fn(struct dma_chan *chan, void *param)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	struct xdma_chan_info *chan_info = param;

	if (chan_info->dir != xdma_chan->dir)
		return false;

	return true;
}

/**
 * xdma_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 */
static int xdma_remove(struct platform_device *pdev)
{
	struct xdma_device *xdev = platform_get_drvdata(pdev);

	if (xdev->status & XDMA_DEV_STATUS_INIT_MSIX)
		xdma_irq_fini(xdev);

	if (xdev->status & XDMA_DEV_STATUS_REG_DMA)
		dma_async_device_unregister(&xdev->dma_dev);

	return 0;
}

/**
 * xdma_probe - Driver probe function
 * @pdev: Pointer to the platform_device structure
 */
static int xdma_probe(struct platform_device *pdev)
{
	struct xdma_platdata *pdata = dev_get_platdata(&pdev->dev);
	struct xdma_device *xdev;
	void __iomem *reg_base;
	struct resource *res;
	int ret = -ENODEV;

	if (pdata->max_dma_channels > XDMA_MAX_CHANNELS) {
		dev_err(&pdev->dev, "invalid max dma channels %d",
			pdata->max_dma_channels);
		return -EINVAL;
	}

	xdev = devm_kzalloc(&pdev->dev, sizeof(*xdev), GFP_KERNEL);
	if (!xdev)
		return -ENOMEM;

	platform_set_drvdata(pdev, xdev);
	xdev->pdev = pdev;

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get irq resource");
		goto failed;
	}
	xdev->irq_start = res->start;
	xdev->irq_num = res->end - res->start + 1;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get io resource");
		goto failed;
	}

	reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (!reg_base) {
		dev_err(&pdev->dev, "ioremap failed");
		goto failed;
	}

	xdev->regmap = devm_regmap_init_mmio(&pdev->dev, reg_base,
					     &xdma_regmap_config);
	if (!xdev->regmap) {
		dev_err(&pdev->dev, "config regmap failed: %d", ret);
		goto failed;
	}
	INIT_LIST_HEAD(&xdev->dma_dev.channels);

	ret = xdma_config_channels(xdev, DMA_MEM_TO_DEV);
	if (ret) {
		dev_err(&pdev->dev, "config H2C channels failed: %d", ret);
		goto failed;
	}

	ret = xdma_config_channels(xdev, DMA_DEV_TO_MEM);
	if (ret) {
		dev_err(&pdev->dev, "config C2H channels failed: %d", ret);
		goto failed;
	}

	dma_cap_set(DMA_SLAVE, xdev->dma_dev.cap_mask);
	dma_cap_set(DMA_PRIVATE, xdev->dma_dev.cap_mask);

	xdev->dma_dev.dev = &pdev->dev;
	xdev->dma_dev.device_free_chan_resources = xdma_free_chan_resources;
	xdev->dma_dev.device_alloc_chan_resources = xdma_alloc_chan_resources;
	xdev->dma_dev.device_tx_status = dma_cookie_status;
	xdev->dma_dev.device_prep_slave_sg = xdma_prep_device_sg;
	xdev->dma_dev.device_config = xdma_device_config;
	xdev->dma_dev.device_issue_pending = xdma_issue_pending;
	xdev->dma_dev.filter.map = pdata->device_map;
	xdev->dma_dev.filter.mapcnt = pdata->device_map_cnt;
	xdev->dma_dev.filter.fn = xdma_filter_fn;

	ret = dma_async_device_register(&xdev->dma_dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register Xilinx XDMA: %d", ret);
		goto failed;
	}
	xdev->status |= XDMA_DEV_STATUS_REG_DMA;

	ret = xdma_irq_init(xdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to init msix: %d", ret);
		goto failed;
	}
	xdev->status |= XDMA_DEV_STATUS_INIT_MSIX;

	return 0;

failed:
	xdma_remove(pdev);

	return ret;
}

static const struct platform_device_id xdma_id_table[] = {
	{ "xdma", 0},
	{ },
};

static struct platform_driver xdma_driver = {
	.driver		= {
		.name = "xdma",
	},
	.id_table	= xdma_id_table,
	.probe		= xdma_probe,
	.remove		= xdma_remove,
};

module_platform_driver(xdma_driver);

MODULE_DESCRIPTION("AMD XDMA driver");
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_LICENSE("GPL");
