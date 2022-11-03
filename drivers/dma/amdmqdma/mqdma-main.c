// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DMA driver for Xilinx Queue-based DMA Subsystem
 *
 * Copyright (C) 2022, Advanced Micro Devices, Inc.
 */

#include <linux/mod_devicetable.h>
#include <linux/dmapool.h>
#include <linux/platform_data/amd_mqdma.h>
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
	u32 val, dev_type;
	int ret;

	/* read function id */
	ret = qdma_read_reg(qdev, 0, QDMA_GLBL2_CHANNEL_FUNC_RET, &val);
	if (ret)
		return ret;
	qdev->dev_info.func_id = FIELD_GET(QDMA_GLBL2_FUNC_ID_MASK, val);
	
	ret = qdma_read_reg(qdev, 0, QDMA_GLBL2_MISC_CAP, &val);
	if (ret)
		return ret;
	qdev->dev_info.dev_type = FIELD_GET(QDMA_GLBL2_DEV_TYPE_MASK, val);
	switch (qdev->dev_info.dev_type) {
	case QDMA_DEV_CPM5:
		qdev->hw_access = &qdma_cpm5_access;
		break;
	default:
		qdma_err(qdev, "Unknown device type: %x", dev_type);
		return -EINVAL;
	}

	ret = qdev->hw_access->qdma_hw_get_attrs(qdev);
	if (ret)
		return ret;

	return 0;
}

/**
 * qdma_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 */
static int qdma_remove(struct platform_device *pdev)
{
	return 0;
}

/**
 * qdma_probe - Driver probe function
 * @pdev: Pointer to the platform_device structure
 */
static int qdma_probe(struct platform_device *pdev)
{
	struct amdmqdma_platdata *pdata = dev_get_platdata(&pdev->dev);
	struct qdma_device *qdev;
	void __iomem *reg_base;
	struct resource *res;
	int ret = -ENODEV;

	qdev = devm_kzalloc(&pdev->dev, sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return -ENOMEM;

	platform_set_drvdata(pdev, qdev);
	qdev->pdev = pdev;

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		qdma_err(qdev, "failed to get irq resource");
		goto failed;
	}
	qdev->irq_start = res->start;
	qdev->irq_num = res->end - res->start + 1;

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
	qdev->regmap = devm_regmap_init_mmio(&pdev->dev, reg_base,
					     &qdma_regmap_config);
	if (!qdev->regmap) {
		qdma_err(qdev, "config regmap failed: %d", ret);
		goto failed;
	}
	INIT_LIST_HEAD(&qdev->dma_dev.channels);

	ret = qdma_hw_init(qdev);
	if (ret) {
		qdma_err(qdev, "failed to get version, ret %d", ret);
		goto failed;
	}

	return 0;

failed:
	qdma_remove(pdev);

	return ret;
}

static const struct platform_device_id qdma_id_table[] = {
	{ "amdmqdma", },
	{ },
};

static struct platform_driver qdma_driver = {
	.driver		= {
		.name = "amdmqdma",
	},
	.id_table	= qdma_id_table,
	.probe		= qdma_probe,
	.remove		= qdma_remove,
};

module_platform_driver(qdma_driver);

MODULE_DESCRIPTION("AMD QDMA driver");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_LICENSE("GPL");
