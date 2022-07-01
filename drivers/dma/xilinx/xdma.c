// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DMA driver for Xilinx DMA/Bridge Subsystem
 *
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
#include <linux/regmap.h>
#include <linux/dmaengine.h>
#include <linux/platform_device.h>
#include <linux/platform_data/xdma.h>
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
 * @desc_num: Number of descriptors in the block
 */
struct xdma_desc_block {
	void		*virt_addr;
	dma_addr_t	dma_addr;
	u32		desc_num;
};

/**
 * struct xdma_chan - Driver specific DMA channel structure
 * @vchan: Virtual channel
 * @xdev_hdl: Pointer to DMA device structure
 * @base: Offset of channel registers
 * @desc_blocks: Descriptor blocks
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
	struct xdma_desc_block		desc_blocks[XDMA_DESC_BLOCK_NUM];
	bool				busy;
	enum dma_transfer_direction	dir;
	struct dma_slave_config		cfg;
	u32				irq;
	struct tasklet_struct		tasklet;
};

/**
 * struct xdma_request - DMA request structure
 * @vdesc: Virtual DMA descriptor
 * @dir: Transferring direction of the request
 * @slave_addr: Physical address on DMA slave side
 * @sgl: Scatter gather list on host side
 * @sg_off: Start offset of the first sgl segment
 * @nents: Number of sgl segments to transfer
 */
struct xdma_request {
	struct virt_dma_desc		vdesc;
	enum dma_transfer_direction	dir;
	u64				slave_addr;
	struct scatterlist		*sgl;
	u32				sg_off;
	u32				nents;
};

/**
 * struct xdma_user_irq - User IRQ structure
 * @xdev_hdl: DMA device structure pointer
 * @handler: Interrupt handler routine
 * @arg: Argument of interrupt handler
 * @irq_lock: Spinlock for user IRQ structure
 * @irq: IRQ number assigned
 */
struct xdma_user_irq {
	void				*xdev_hdl;
	irq_handler_t			handler;
	void				*arg;
	spinlock_t			irq_lock; /* user irq lock */
	u32				irq;
	struct tasklet_struct		tasklet;
};

enum xdma_dev_status {
	XDMA_DEV_STATUS_INIT,
	XDMA_DEV_STATUS_REG_DMA,
	XDMA_DEV_STATUS_INIT_MSIX,
};

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
 * @user_irq: User IRQ structures
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
	struct xdma_user_irq	user_irq[XDMA_MAX_USER_IRQS];
	u32			status;
};

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

/* Write specified word of DMA descriptor */
static inline void xdma_desc_set_field(void *desc, enum xdma_desc_field field,
				       u32 val)
{
	*((__le32 *)desc + field) = cpu_to_le32(val);
}

/**
 * xdma_desc_set - fill DMA descriptor
 * @xdma_chan: DMA channel pointer
 * @desc: Descriptor pointer
 * @addr: Physical address on host side
 * @slave_addr: Physical address on DMA slave side
 * @len: Data length
 */
static void xdma_desc_set(struct xdma_chan *xdma_chan, void *desc,
			  dma_addr_t addr, u64 slave_addr, u32 len)
{
	xdma_desc_set_field(desc, XDMA_DF_CONTROL, XDMA_DESC_CONTROL(1, 0));
	xdma_desc_set_field(desc, XDMA_DF_BYTES, len);
	xdma_desc_set_field(desc, XDMA_DF_NEXT_LO, 0);
	xdma_desc_set_field(desc, XDMA_DF_NEXT_HI, 0);

	if (xdma_chan->dir == DMA_MEM_TO_DEV) {
		xdma_desc_set_field(desc, XDMA_DF_SADDR_LO, XDMA_ADDR_L(addr));
		xdma_desc_set_field(desc, XDMA_DF_SADDR_HI, XDMA_ADDR_H(addr));
		xdma_desc_set_field(desc, XDMA_DF_DADDR_LO,
				    XDMA_ADDR_L(slave_addr));
		xdma_desc_set_field(desc, XDMA_DF_DADDR_HI,
				    XDMA_ADDR_H(slave_addr));
	} else {
		xdma_desc_set_field(desc, XDMA_DF_DADDR_LO, XDMA_ADDR_L(addr));
		xdma_desc_set_field(desc, XDMA_DF_DADDR_HI, XDMA_ADDR_H(addr));
		xdma_desc_set_field(desc, XDMA_DF_SADDR_LO,
				    XDMA_ADDR_L(slave_addr));
		xdma_desc_set_field(desc, XDMA_DF_SADDR_HI,
				    XDMA_ADDR_H(slave_addr));
	}
}

/**
 * xdma_link_desc_blocks - Link descriptor blocks for DMA transfer
 * @num_blocks: Number of descriptors to link
 */
static void xdma_link_desc_blocks(struct xdma_chan *xdma_chan, u32 num_blocks)
{
	struct xdma_desc_block *block, *next_block;
	void *desc;
	int i;

	for (i = 0; i < num_blocks - 1; i++) {
		block = &xdma_chan->desc_blocks[i];
		next_block = &xdma_chan->desc_blocks[i + 1];

		desc = block->virt_addr + (block->desc_num - 1) *
			XDMA_DESC_SIZE;

		xdma_desc_set_field(desc, XDMA_DF_CONTROL,
				    XDMA_DESC_CONTROL(next_block->desc_num, 0));
		xdma_desc_set_field(desc, XDMA_DF_NEXT_LO,
				    XDMA_ADDR_L(next_block->dma_addr));
		xdma_desc_set_field(desc, XDMA_DF_NEXT_HI,
				    XDMA_ADDR_H(next_block->dma_addr));
	}
}

static inline struct xdma_chan *to_xdma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct xdma_chan, vchan.chan);
}

static inline struct xdma_request *to_xdma_req(struct virt_dma_desc *vdesc)
{
	return container_of(vdesc, struct xdma_request, vdesc);
}

static int xdma_enable_intr(struct xdma_device *xdev)
{
	int ret;

	ret = xdma_write_reg(xdev, XDMA_IRQ_BASE, XDMA_IRQ_CHAN_INT_EN_W1S, ~0);
	if (ret) {
		dev_err(&xdev->pdev->dev, "set channel intr mask failed: %d",
			ret);
		return ret;
	}

	ret = xdma_write_reg(xdev, XDMA_IRQ_BASE, XDMA_IRQ_USER_INT_EN_W1S, ~0);
	if (ret) {
		dev_err(&xdev->pdev->dev, "set user intr mask failed: %d",
			ret);
	}

	return ret;
}

static int xdma_disable_intr(struct xdma_device *xdev)
{
	int ret;

	ret = xdma_write_reg(xdev, XDMA_IRQ_BASE, XDMA_IRQ_CHAN_INT_EN_W1C, ~0);
	if (ret) {
		dev_err(&xdev->pdev->dev, "clear channel intr mask failed: %d",
			ret);
		return ret;
	}

	ret = xdma_write_reg(xdev, XDMA_IRQ_BASE, XDMA_IRQ_USER_INT_EN_W1C, ~0);
	if (ret) {
		dev_err(&xdev->pdev->dev, "clear user intr mask failed: %d",
			ret);
	}

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
		dev_err(&xdev->pdev->dev, "failed to clear non incr addr: %d",
			ret);
		return ret;
	}

	ret = xdma_write_reg(xdev, chan->base, XDMA_CHAN_INTR_ENABLE,
			     CHAN_IM_ALL);
	if (ret) {
		dev_err(&xdev->pdev->dev, "failed to set interrupt mask: %d",
			ret);
		return ret;
	}

	return 0;
}

static void xdma_free_tx_desc(struct virt_dma_desc *vdesc)
{
	struct xdma_request *req;

	req = to_xdma_req(vdesc);
	kfree(req);
}

/**
 * xdma_xfer_start - Start DMA transfer
 * @xdma_chan: DMA channel pointer
 */
static int xdma_xfer_start(struct xdma_chan *xdma_chan)
{
	struct virt_dma_desc *vd = vchan_next_desc(&xdma_chan->vchan);
	struct xdma_device *xdev = xdma_chan->xdev_hdl;
	u32 i, rest, len, sg_off, desc_num = 0;
	struct xdma_desc_block *block;
	struct xdma_request *req;
	struct scatterlist *sg;
	void *desc, *last_desc;
	u64 slave_addr;
	dma_addr_t addr;
	int ret;

	/* check if there is not any submitted descriptor or channel is busy */
	if (!vd || xdma_chan->busy)
		return -EINVAL;

	/* clear run stop bit to get ready for transfer */
	ret = xdma_write_reg(xdev, xdma_chan->base, XDMA_CHAN_CONTROL_W1C,
			     CHAN_CTRL_RUN_STOP);
	if (ret) {
		dev_err(&xdev->pdev->dev, "write control failed: %d", ret);
		return ret;
	}

	req = to_xdma_req(vd);
	if (req->dir != xdma_chan->dir) {
		dev_err(&xdev->pdev->dev, "incorrect request direction");
		return -EINVAL;
	}

	sg = req->sgl;
	sg_off = req->sg_off;
	slave_addr = req->slave_addr;

	/*
	 * submit sgl segments to hardware. complete submission either
	 * all descriptors are used or all segments are submitted.
	 */
	for (i = 0; i < XDMA_DESC_BLOCK_NUM && req->nents && sg; i++) {
		block = &xdma_chan->desc_blocks[i];
		desc = block->virt_addr;

		/* set descriptor block */
		while (desc_num < XDMA_DESC_ADJACENT && sg) {
			addr = sg_dma_address(sg) + sg_off;
			rest = sg_dma_len(sg) - sg_off;

			/*
			 * if the rest of segment greater than the maximum
			 * data block can be sent by a hardware descriptor
			 */
			if (rest > XDMA_DESC_BLEN_MAX) {
				len = XDMA_DESC_BLEN_MAX;
				sg_off += XDMA_DESC_BLEN_MAX;
			} else {
				len = rest;
				sg_off = 0;
				sg = sg_next(sg);
				req->nents--;
			}

			if (!len)
				continue;

			/* set hardware descriptor */
			xdma_desc_set(xdma_chan, desc, addr, slave_addr,
				      len);
			last_desc = desc;
			desc += XDMA_DESC_SIZE;
			desc_num++;
		}
		block->desc_num = desc_num;
		desc_num = 0;
	}

	/* link all descriptors for transferring */
	xdma_link_desc_blocks(xdma_chan, i);

	/* record the rest of sgl */
	req->sgl = sg;
	req->sg_off = sg_off;

	/* set control word of last descriptor */
	xdma_desc_set_field(last_desc, XDMA_DF_CONTROL, XDMA_DESC_CONTROL_LAST);

	/* set DMA engine to the first descriptor block */
	block = &xdma_chan->desc_blocks[0];
	ret = xdma_write_reg(xdev, XDMA_SGDMA_BASE(xdma_chan->base),
			     XDMA_SGDMA_DESC_LO,
			     XDMA_ADDR_L(block->dma_addr));
	if (ret) {
		dev_err(&xdev->pdev->dev, "write hi addr failed: %d", ret);
		return ret;
	}

	ret = xdma_write_reg(xdev, XDMA_SGDMA_BASE(xdma_chan->base),
			     XDMA_SGDMA_DESC_HI,
			     XDMA_ADDR_H(block->dma_addr));
	if (ret) {
		dev_err(&xdev->pdev->dev, "write lo addr failed: %d", ret);
		return ret;
	}

	ret = xdma_write_reg(xdev, XDMA_SGDMA_BASE(xdma_chan->base),
			     XDMA_SGDMA_DESC_ADJ, block->desc_num - 1);
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

static void xdma_userirq_tasklet(struct tasklet_struct *t)
{
	struct xdma_user_irq *user_irq = from_tasklet(user_irq, t, tasklet);

	spin_lock(&user_irq->irq_lock);
	if (user_irq->handler)
		user_irq->handler(user_irq->irq, user_irq->arg);
	spin_unlock(&user_irq->irq_lock);
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
	struct xdma_chan *chans;
	u32 chan_num = 0;
	int i, j, ret;

	if (dir == DMA_MEM_TO_DEV) {
		base = XDMA_CHAN_H2C_OFFSET;
		target = XDMA_CHAN_H2C_TARGET;
	} else {
		base = XDMA_CHAN_C2H_OFFSET;
		target = XDMA_CHAN_C2H_TARGET;
	}

	/* detect number of available DMA channels */
	for (i = 0; i < pdata->max_dma_channels; i++) {
		ret = xdma_read_reg(xdev, base + i * XDMA_CHAN_STRIDE,
				    XDMA_CHAN_IDENTIFIER, &identifier);
		if (ret) {
			dev_err(&xdev->pdev->dev,
				"failed to read channel id: %d", ret);
			return ret;
		}

		/* check if it is available DMA channel */
		if (XDMA_CHAN_CHECK_TARGET(identifier, target))
			chan_num++;
	}

	chans = devm_kzalloc(&xdev->pdev->dev, sizeof(*chans) * chan_num,
			     GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	if (dir == DMA_MEM_TO_DEV) {
		xdev->h2c_chans = chans;
		xdev->h2c_chan_num = chan_num;
	} else {
		xdev->c2h_chans = chans;
		xdev->c2h_chan_num = chan_num;
	}

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

		if (j == chan_num) {
			dev_err(&xdev->pdev->dev, "invalid channel number");
			return -EIO;
		}

		/* init channel structure and hardware */
		chans[j].xdev_hdl = xdev;
		chans[j].base = base + i * XDMA_CHAN_STRIDE;
		chans[j].dir = dir;

		ret = xdma_channel_init(&chans[j]);
		if (ret)
			return ret;
		chans[j].vchan.desc_free = xdma_free_tx_desc;
		vchan_init(&chans[j].vchan, &xdev->dma_dev);

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
 * xdma_prep_slave_sg - prepare a descriptor for a
 *	DMA_SLAVE transaction
 * @chan: DMA channel pointer
 * @sgl: Transfer scatter gather list
 * @sg_len: Length of scatter gather list
 * @dir: Transfer direction
 * @flags: transfer ack flags
 * @context: APP words of the descriptor
 */
static struct dma_async_tx_descriptor *
xdma_prep_slave_sg(struct dma_chan *chan, struct scatterlist *sgl,
		   unsigned int sg_len, enum dma_transfer_direction dir,
		   unsigned long flags, void *context)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	struct xdma_request *req;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return NULL;

	req->sgl = sgl;
	req->dir = dir;
	req->nents = sg_len;
	if (dir == DMA_MEM_TO_DEV)
		req->slave_addr = xdma_chan->cfg.dst_addr;
	else
		req->slave_addr = xdma_chan->cfg.src_addr;

	return vchan_tx_prep(&xdma_chan->vchan, &req->vdesc, flags);
}

/**
 * xdma_slave_config - Configure the DMA channel
 * @chan: DMA channel
 * @cfg: channel configuration
 */
int xdma_slave_config(struct dma_chan *chan, struct dma_slave_config *cfg)
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
	struct xdma_device *xdev = xdma_chan->xdev_hdl;

	dma_free_coherent(&xdev->pdev->dev,
			  XDMA_DESC_BLOCK_NUM * XDMA_DESC_BLOCK_SIZE,
			  xdma_chan->desc_blocks[0].virt_addr,
			  xdma_chan->desc_blocks[0].dma_addr);
}

/**
 * xdma_alloc_chan_resources - Allocate channel resources
 * @chan: DMA channel
 */
static int xdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	struct xdma_device *xdev = xdma_chan->xdev_hdl;
	dma_addr_t dma_addr;
	void *desc;
	int i;

	desc = dma_alloc_coherent(&xdev->pdev->dev,
				  XDMA_DESC_BLOCK_NUM * XDMA_DESC_BLOCK_SIZE,
				  &dma_addr, GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	for (i = 0; i < XDMA_DESC_BLOCK_NUM; i++) {
		xdma_chan->desc_blocks[i].virt_addr = desc;
		xdma_chan->desc_blocks[i].dma_addr = dma_addr;
		desc += XDMA_DESC_BLOCK_SIZE;
		dma_addr += XDMA_DESC_BLOCK_SIZE;
	}

	return 0;
}

/**
 * xdma_user_isr - XDMA user interrupt handler
 * @irq: IRQ number
 * @dev_id: Pointer to the user irq structure
 */
static irqreturn_t xdma_user_isr(int irq, void *dev_id)
{
	struct xdma_user_irq *user_irq = dev_id;

	tasklet_schedule(&user_irq->tasklet);

	return IRQ_HANDLED;
}

/**
 * xdma_channel_isr - XDMA channel interrupt handler
 * @irq: IRQ number
 * @dev_id: Pointer to the DMA channel structure
 */
static irqreturn_t xdma_channel_isr(int irq, void *dev_id)
{
	struct xdma_chan *xdma_chan = dev_id;
	struct virt_dma_desc *vd;
	struct xdma_request *req;

	spin_lock(&xdma_chan->vchan.lock);

	/* get submitted request */
	vd = vchan_next_desc(&xdma_chan->vchan);
	if (!vd)
		goto out;

	xdma_chan->busy = false;
	req = to_xdma_req(vd);

	/*
	 * if all data blocks are transferred, remove and complete the
	 * request
	 */
	if (!req->sgl) {
		list_del(&vd->node);
		vchan_cookie_complete(vd);
	} else {
		/* transfer the rest of data */
		tasklet_schedule(&xdma_chan->tasklet);
	}

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
	struct xdma_platdata *pdata = dev_get_platdata(&xdev->pdev->dev);
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
	for (i = 0; i < pdata->user_irqs; i++) {
		free_irq(xdev->user_irq[i].irq, &xdev->user_irq[i]);
		tasklet_kill(&xdev->user_irq[i].tasklet);
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

	for (i = 0; i < irq_num; i++) {
		/* Each IRQ register is 32 bit and contains 4 IRQs */
		shift = XDMA_IRQ_VEC_SHIFT * (i % 4);
		val |= irq_start << shift;
		irq_start++;
		if ((i + 1) % 4)
			continue;

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
	struct xdma_platdata *pdata = dev_get_platdata(&xdev->pdev->dev);
	u32 irq = xdev->irq_start;
	int i, ret;

	/* return failure if there are not enough IRQs */
	if (xdev->irq_num < xdev->h2c_chan_num + xdev->c2h_chan_num +
	    pdata->user_irqs) {
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
			goto failed;
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
			goto failed;
		}
		xdev->c2h_chans[i].irq = irq;
		tasklet_setup(&xdev->c2h_chans[i].tasklet,
			      xdma_channel_tasklet);
		irq++;
	}

	/* setup user interrupt handler */
	for (i = 0; i < pdata->user_irqs; i++) {
		ret = request_irq(irq, xdma_user_isr, 0, "xdma-user",
				  &xdev->user_irq[i]);
		if (ret) {
			dev_err(&xdev->pdev->dev,
				"request user irq%d failed: %d", irq, ret);
			goto failed;
		}
		xdev->user_irq[i].irq = irq;
		xdev->user_irq[i].xdev_hdl = xdev;
		spin_lock_init(&xdev->user_irq[i].irq_lock);
		tasklet_setup(&xdev->user_irq[i].tasklet, xdma_userirq_tasklet);
		irq++;
	}

	/* config hardware IRQ registers */
	ret = xdma_set_vector_reg(xdev, XDMA_IRQ_CHAN_VEC_NUM, 0,
				  xdev->h2c_chan_num + xdev->c2h_chan_num);
	if (ret) {
		dev_err(&xdev->pdev->dev, "failed to set channel vectors: %d",
			ret);
		return ret;
	}

	ret = xdma_set_vector_reg(xdev, XDMA_IRQ_USER_VEC_NUM,
				  xdev->h2c_chan_num + xdev->c2h_chan_num,
				  pdata->user_irqs);
	if (ret) {
		dev_err(&xdev->pdev->dev, "failed to set user vectors: %d",
			ret);
		return ret;
	}

	/* enable interrupt */
	ret = xdma_enable_intr(xdev);
	if (ret) {
		dev_err(&xdev->pdev->dev, "failed to enable interrupts: %d",
			ret);
		return ret;
	}

	return 0;

failed:
	for (i = 0; i < xdev->h2c_chan_num && irq > 0; i++, irq--)
		free_irq(xdev->h2c_chans[i].irq, &xdev->h2c_chans[i]);
	for (i = 0; i < xdev->c2h_chan_num && irq > 0; i++, irq--)
		free_irq(xdev->c2h_chans[i].irq, &xdev->c2h_chans[i]);
	for (i = 0; i < pdata->user_irqs && irq > 0; i++, irq--)
		free_irq(xdev->user_irq[i].irq, &xdev->user_irq[i]);

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
 * xdma_user_isr_register - Register user interrupt handler
 * @pdev: Pointer to the platform_device structure
 * @user_irq_index: User IRQ index
 * @handler: User interrupt handler
 * @arg: User interrupt handler argument
 */
int xdma_user_isr_register(struct platform_device *pdev, u32 user_irq_index,
			   irq_handler_t handler, void *arg)
{
	struct xdma_platdata *pdata = dev_get_platdata(&pdev->dev);
	struct xdma_device *xdev = platform_get_drvdata(pdev);
	unsigned long flags;

	if (user_irq_index >= pdata->user_irqs) {
		dev_err(&pdev->dev, "invalid user irq index");
		return -EINVAL;
	}

	spin_lock_irqsave(&xdev->user_irq[user_irq_index].irq_lock, flags);

	xdev->user_irq[user_irq_index].handler = handler;
	xdev->user_irq[user_irq_index].arg = arg;

	spin_unlock_irqrestore(&xdev->user_irq[user_irq_index].irq_lock, flags);

	return 0;
}
EXPORT_SYMBOL(xdma_user_isr_register);

/**
 * xdma_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 */
static int xdma_remove(struct platform_device *pdev)
{
	struct xdma_device *xdev = platform_get_drvdata(pdev);

	if (xdev->status >= XDMA_DEV_STATUS_INIT_MSIX)
		xdma_irq_fini(xdev);

	if (xdev->status >= XDMA_DEV_STATUS_REG_DMA)
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
	struct resource *res;
	void *reg_base;
	int ret;

	if (pdata->max_dma_channels > XDMA_MAX_CHANNELS) {
		dev_err(&pdev->dev, "invalid max dma channels %d",
			pdata->max_dma_channels);
		return -EINVAL;
	}
	if (pdata->user_irqs > XDMA_MAX_USER_IRQS) {
		dev_err(&pdev->dev, "invalid max user irqs %d",
			pdata->user_irqs);
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
	xdev->dma_dev.device_prep_slave_sg = xdma_prep_slave_sg;
	xdev->dma_dev.device_config = xdma_slave_config;
	xdev->dma_dev.device_issue_pending = xdma_issue_pending;
	xdev->dma_dev.filter.map = pdata->slave_map;
	xdev->dma_dev.filter.mapcnt = pdata->slave_map_cnt;
	xdev->dma_dev.filter.fn = xdma_filter_fn;

	ret = dma_async_device_register(&xdev->dma_dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register Xilinx XDMA: %d", ret);
		goto failed;
	}
	xdev->status = XDMA_DEV_STATUS_REG_DMA;

	ret = xdma_irq_init(xdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to init msix: %d", ret);
		goto failed;
	}
	xdev->status = XDMA_DEV_STATUS_INIT_MSIX;

	return 0;

failed:
	xdma_remove(pdev);

	return ret;
}

static const struct platform_device_id xdma_id_table[] = {
	{ "xdma", },
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
