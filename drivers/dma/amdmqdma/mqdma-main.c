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

/**
 * qdma_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 */
static int amdmqdma_remove(struct platform_device *pdev)
{
	struct qdma_device *qdev = platform_get_drvdata(pdev);

	qdma_info(qdev, "[Debug]%s invoked\n", __func__);
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
	struct qdma_device *qdev;
	void __iomem *reg_base;
	struct resource *res;
	int ret;

	qdev = devm_kzalloc(&pdev->dev, sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return -ENOMEM;

	qdev->pdev = pdev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(reg_base))
		return PTR_ERR(reg_base);

	qdev->regmap = devm_regmap_init_mmio(&pdev->dev, reg_base,
					     &qdma_regmap_config);
	if (IS_ERR(qdev->regmap))
		return PTR_ERR(qdev->regmap);

	INIT_LIST_HEAD(&qdev->dma_dev.channels);
	spin_lock_init(&qdev->ind_ctxt_lock);
	platform_set_drvdata(pdev, qdev);

	ret = qdma_cpm5_init(qdev);
	if (ret)
		goto failed;

	dma_cap_set(DMA_SLAVE, qdev->dma_dev.cap_mask);
	dma_cap_set(DMA_PRIVATE, qdev->dma_dev.cap_mask);

	qdev->dma_dev.dev = &pdev->dev;
	qdev->dma_dev.device_free_chan_resources = qdma_free_chan_resources;
	qdev->dma_dev.device_alloc_chan_resources = qdma_alloc_chan_resources;
	qdev->dma_dev.device_tx_status = dma_cookie_status;
	qdev->dma_dev.device_issue_pending = qdma_issue_pending;

	ret = dma_async_device_register(&qdev->dma_dev);
	if (ret) {
		qdma_err(qdev, "failed to register AMD QDMA %d", ret);
		goto failed;
	}

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
