// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DMA driver for Xilinx Queue-based DMA Subsystem
 *
 * Copyright (C) 2023, Advanced Micro Devices, Inc.
 */

#include <linux/dmapool.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_data/amd_mqdma.h>
#include <linux/slab.h>
#include "../virt-dma.h"
#include "mqdma.h"

/* mmio regmap config for all QDMA registers */
static struct regmap_config qdma_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int qdma_hw_init(struct qdma_device *qdev)
{
	u32 val = 0;
	int ret = 0;

	/* read function id */
	qdma_read_reg(qdev, QDMA_GLBL2_CHANNEL_FUNC_RET, &val);
	qdev->dev_info.func_id = FIELD_GET(QDMA_GLBL2_FUNC_ID_MASK, val);
	qdma_info(qdev, "%s: chan_fun reg: 0X%X, func_id: %d\n", __func__,
			val, qdev->dev_info.func_id);
	
	qdma_read_reg(qdev, QDMA_GLBL2_MISC_CAP, &val);
	qdev->dev_info.dev_type = FIELD_GET(QDMA_GLBL2_DEV_TYPE_MASK, val);
	qdma_info(qdev, "%s: misc_cap: 0X%X, dev_type: %d\n", __func__,
			val, qdev->dev_info.dev_type);
	switch (qdev->dev_info.dev_type) {
	case QDMA_DEV_CPM5:
		qdev->hw_access = &qdma_cpm5_access;
		break;
	default:
		qdma_err(qdev, "Unknown or non supported device type: %x received",
				qdev->dev_info.dev_type);
		return -EINVAL;
	}

	ret = qdev->hw_access->qdma_hw_get_attrs(qdev);
	if (ret) {
		qdma_err(qdev, "qdma_hw_get_attr() ret: %d\n", ret);
		return ret;
	}

	if (qdev->max_queues > qdev->dev_info.num_qs) {
		qdev->max_queues = qdev->dev_info.num_qs;
		qdma_info(qdev, "Set max queues to device supported queues: %d\n",
				qdev->max_queues);
	}

	return ret;
}

/**
 * qdma_config_channels - init qdma channels
 * @qdev : pointer to qdma device
 * @dir : dma data trnsfer direction
 */
static int qdma_config_channels(struct qdma_device *qdev,
				enum dma_transfer_direction dir)
{
	struct amdmqdma_platdata *pdata = dev_get_platdata(&qdev->pdev->dev);
	struct qdma_chan **chans;
	u32 chan_num;
	int i;

	if (dir == DMA_MEM_TO_DEV) {
		chans = &qdev->h2c_chans;
	} else if (dir == DMA_DEV_TO_MEM) {
		chans = &qdev->c2h_chans;
	} else {
		qdma_err(qdev, "invalid direction specified, dir: %d\n", dir);
		return -EINVAL;
	}

	chan_num = pdata->max_dma_channels;
	*chans = devm_kzalloc(&qdev->pdev->dev, sizeof(**chans) * (chan_num),
			GFP_KERNEL);
	if (!*chans)
		return -ENOMEM;

	for (i = 0; i < pdata->max_dma_channels; i++) {
		(*chans)[i].dir = dir;
		vchan_init(&(*chans)[i].vchan, &qdev->dma_dev);
	}

	qdma_info(qdev, "configured %d %s channels", i,
			(dir == DMA_MEM_TO_DEV) ? "H2C" : "C2H");

	return 0;
}

static inline struct qdma_chan *to_qdma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct qdma_chan, vchan.chan);
}

static void qdma_free_chan_resources(struct dma_chan *chan)
{
	//TODO: free qdma queue
	struct qdma_chan *qdma_chan = to_qdma_chan(chan);

	vchan_free_chan_resources(&qdma_chan->vchan);
	pr_info("free chan resources %s\n", __func__);
}

static int qdma_alloc_chan_resources(struct dma_chan *chan)
{
	//TODO: alloc qdma queue
	pr_info("alloc chan resources %s\n", __func__);

	return 0;
}

static int alloc_queues(struct qdma_device *qdev)
{
	qdev->h2c_ring = kcalloc(qdev->max_queues,
		sizeof(struct qdma_h2c_ring), GFP_KERNEL);
	if (!qdev->h2c_ring)
		return -ENOMEM;

	qdev->c2h_ring = kcalloc(qdev->max_queues,
		sizeof(struct qdma_c2h_ring), GFP_KERNEL);
	if (!qdev->c2h_ring) {
		kfree(qdev->h2c_ring);
		return -ENOMEM;
	}

	qdma_info(qdev, "Allocated memory for c2h & h2c queues successfully\n");
	return 0;
}

static int free_queues(struct qdma_device *qdev)
{
	kfree(qdev->h2c_ring);
	qdev->h2c_ring = NULL;
	kfree(qdev->c2h_ring);
	qdev->c2h_ring = NULL;
	return 0;
}

/*
 * qdma_init	- initialise qdma
 * @qdev : pointer to qdma device
 *
 * configure h2c and c2h channels, set up csr registers,
 * allocate memory for h2c and c2h rings
 */

int qdma_init(struct qdma_device *qdev)
{
	int ret = 0;

	ret = qdma_config_channels(qdev, DMA_MEM_TO_DEV);
	if (ret) {
		qdma_err(qdev, "config H2C channels failed: %d", ret);
		return -1;
	}
	ret = qdma_config_channels(qdev, DMA_DEV_TO_MEM);
	if (ret) {
		qdma_err(qdev, "config H2C channels failed: %d", ret);
		return -1;
	}
	qdev->func_id = set_initial_regs(qdev);
	if (qdev->func_id < 0) {
		qdma_err(qdev, "Unable to complete initial register config\n");
		return -1;
	}
	qdma_info(qdev, "[Debug]%s: func_id: %d\n", __func__, qdev->func_id);
	if (alloc_queues(qdev)) {
		qdma_err(qdev, "Unable to allocate memory for queues\n");
		return -ENOMEM;
	}

	return 0;
}

/**
 * qdma_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 */
static int amdmqdma_remove(struct platform_device *pdev)
{
	struct qdma_device *qdev = platform_get_drvdata(pdev);

	qdma_info(qdev, "[Debug]%s invoked\n", __func__);
	free_queues(qdev);
	dma_async_device_unregister(&qdev->dma_dev);
	qdma_info(qdev, "qdma platform device unbinded\n");
	return 0;
}

/**
 * qdma_issue_pending - Issue pending transactions
 * @chan: DMA channel pointer
 */
static void qdma_issue_pending(struct dma_chan *chan)
{
	pr_info("TODO: qdma platform device unbinded\n");
}

/**
 * amdmqdma_probe - Driver probe function
 * @pdev: Pointer to the platform_device structure
 */
static int amdmqdma_probe(struct platform_device *pdev)
{
	struct amdmqdma_platdata *pdata;
	struct qdma_device *qdev;
	void __iomem *reg_base;
	struct resource *res;
	int ret = -ENODEV;

	pr_info("%s: invoked\n", __func__);
	if (!pdev) {
		pr_err("%s: failed to get valid platform_device handle",
			__func__);
		return -ENODEV;
	}

	pdata = dev_get_platdata(&pdev->dev);
	if (!pdata) {
		pr_err("%s: failed to get valid platform data handle",
			__func__);
		return -ENODEV;
	}

	qdev = devm_kzalloc(&pdev->dev, sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return -ENOMEM;

	platform_set_drvdata(pdev, qdev);
	qdev->pdev = pdev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		qdma_err(qdev, "failed to get io resource");
		goto failed;
	}

	reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (!reg_base) {
		qdma_err(qdev, "ioremap failed");
		goto failed;
	}

	qdev->ioaddr = reg_base;
	qdev->max_queues = pdata->max_dma_channels;
	qdev->qbase = 0;

	qdma_info(qdev, "Received qdma resources, bar addr: 0X%llX, max_qs: %d",
			res->start, qdev->max_queues);
	qdev->regmap = devm_regmap_init_mmio(&pdev->dev, reg_base,
					     &qdma_regmap_config);
	if (!qdev->regmap) {
		qdma_err(qdev, "config regmap failed: %d", ret);
		goto failed;
	}
	INIT_LIST_HEAD(&qdev->dma_dev.channels);
	spin_lock_init(&qdev->hw_prg_lock);

	ret = qdma_hw_init(qdev);
	if (ret) {
		qdma_err(qdev, "failed to get valid qdma cpm version, ret %d",
				ret);
		goto failed;
	}

	ret = qdma_init(qdev);
	if (ret) {
		qdma_err(qdev, "Failed to initialise qdma\n");
		goto failed;
	}
	qdma_info(qdev, "amdmqdma init success\n");

	dma_cap_set(DMA_SLAVE, qdev->dma_dev.cap_mask);
	dma_cap_set(DMA_PRIVATE, qdev->dma_dev.cap_mask);

	qdev->dma_dev.dev = &pdev->dev;
	qdev->dma_dev.device_free_chan_resources = qdma_free_chan_resources;
	qdev->dma_dev.device_alloc_chan_resources = qdma_alloc_chan_resources;
	qdev->dma_dev.device_tx_status = dma_cookie_status;
	qdev->dma_dev.device_issue_pending = qdma_issue_pending;

	ret = dma_async_device_register(&qdev->dma_dev);
	if (ret) {
		qdma_err(qdev, "failed to register AMD QDMA driver, err: %d",
				ret);
		goto failed;
	}

	qdma_info(qdev, "amdmqdma platform device probed successfully\n");
	return 0;

failed:
	platform_set_drvdata(pdev, NULL);

	return ret;
}

static const struct platform_device_id amdmqdma_id_table[] = {
	{ "amdmqdma", },
	{ },
};

static struct platform_driver qdma_driver = {
	.driver		= {
		.name = "amdmqdma",
	},
	.id_table	= amdmqdma_id_table,
	.probe		= amdmqdma_probe,
	.remove		= amdmqdma_remove,
};

module_platform_driver(qdma_driver);

MODULE_DESCRIPTION("AMD QDMA driver");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_LICENSE("GPL");
