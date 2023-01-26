// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023, Advanced Micro Devices, Inc.
 */

#include <linux/platform_data/amd_mqdma.h>
#include "mqdma.h"

#define QDMA_OFFSET_GLBL2_PF_BARLITE_INT	0x104
#define QDMA_GLBL2_PF0_BAR_MAP_MASK		GENMASK(5, 0)
#define QDMA_CPM5_GLBL2_CHANNEL_MDMA		0x118
#define GLBL2_CHANNEL_MDMA_C2H_ENG_MASK		GENMASK(11, 8)
#define GLBL2_CHANNEL_MDMA_H2C_ENG_MASK		GENMASK(3, 0)
#define QDMA_CPM5_GLBL2_CHANNEL_CAP		0x120
#define GLBL2_CHAN_CAP_MULTIQ_MAX_MASK		GENMASK(11, 0)
#define QDMA_CPM5_FMAP_CTXT_QID_MAX_MASK	GENMASK(11, 0)
#define QDMA_CPM5_FMAP_CTXT_QID_BASE_MASK	GENMASK(10, 0)

struct qdma_cpm5_fmap {
	u32	qbase;
	u32	qmax;
	u32	resv[6];
};

static int qdma_cpm5_get_attrs(struct qdma_device *qdev)
{
	u32 val;
	int ret;

	/* number of PFs */
	ret = regmap_read(qdev->regmap, QDMA_OFFSET_GLBL2_PF_BARLITE_INT, &val);
	if (ret)
		return ret;
	qdev->dev_info.num_pfs = FIELD_GET(QDMA_GLBL2_PF0_BAR_MAP_MASK, val);

	ret = regmap_read(qdev->regmap, QDMA_CPM5_GLBL2_CHANNEL_CAP, &val);
	if (ret)
		return ret;
	qdev->dev_info.num_qs = FIELD_GET(GLBL2_CHAN_CAP_MULTIQ_MAX_MASK, val);
	/*
	 * There are 12 bits assigned in QDMA_CPM5_GLBL2_CHANNEL_CAP
	 * to represent the num_qs. For CPM5, max queues can be 4096 which needs
	 * 13 bits(0x1000). Adding a hack in driver to represent 4096 queues
	 * when HW sets the num_qs to 0xFFF
	 */
	if (qdev->dev_info.num_qs == 0xFFF)
		qdev->dev_info.num_qs++;

	/* MM enabled */
	ret = regmap_read(qdev->regmap, QDMA_CPM5_GLBL2_CHANNEL_MDMA, &val);
	if (ret)
		return ret;

	if (FIELD_GET(GLBL2_CHANNEL_MDMA_C2H_ENG_MASK, val) &&
	    FIELD_GET(GLBL2_CHANNEL_MDMA_H2C_ENG_MASK, val))
		qdev->dev_info.mm_en = true;

	/* num of mm channels */
	qdev->dev_info.mm_channel_max = 2;
	qdma_dbg(qdev, "pfs:%d, qs:%d, mm_en:%d, mm_chan_max:%d\n",
		 qdev->dev_info.num_pfs, qdev->dev_info.num_qs,
		 qdev->dev_info.mm_en, qdev->dev_info.mm_channel_max);

	return 0;
}

/*
 * qdma_cpm5_ind_reg_write - helper function to write indirect context
 */
static int qdma_cpm5_ind_reg_write(struct qdma_device *qdev,
				   struct qdma_ind_ctxt *ctxt)
{
	int ret, i;
	u32 val;

	spin_lock(&qdev->ind_ctxt_lock);
	ret = regmap_bulk_write(qdev->regmap, QDMA_IND_CTXT_DATA_BASE,
				ctxt, sizeof(*ctxt));
	if (ret)
		goto failed;

	for (i = 0; i < QDMA_IND_CTXT_POLL_COUNT; i++) {
		ret = regmap_read(qdev->regmap, QDMA_IND_CTXT_CMD, &val);
		if (ret)
			goto failed;

		if (!FIELD_GET(QDMA_CTXT_CMD_BUSY, val))
			break;

		ret = -ETIME;
		udelay(QDMA_IND_CTXT_POLL_INTERVAL);
	}

failed:
	spin_unlock(&qdev->ind_ctxt_lock);

	return ret;
}

/*
 * qdma_set_global_regs - initialise global csr registers
 * @qdev  : qdma_device handle
 *
 * Get function id of the device, set csr registers with
 * global ring size, qbase and qmax values.
 */
static int qdma_set_global_regs(struct qdma_device *qdev)
{
	u32 ring_sz[] = {2049, 65, 129, 193, 257, 385, 513, 769, 1025, 1537,
			 3073, 4097, 6145, 8193, 12289, 16385};
	struct qdma_ind_ctxt fmap_ctxt = { 0 };
	struct qdma_cpm5_fmap *fmap;
	u32 i, reg;
	int ret;

	ret = regmap_bulk_write(qdev->regmap, QDMA_REG_GLBL_RNG_SZ_BASE,
				ring_sz, ARRAY_SIZE(ring_sz));
	if (ret)
		return ret;

	/* Enable MM channel */
	for (i = 0; i < qdev->dev_info.mm_channel_max; i++) {
		reg = QDMA_OFFSET_C2H_MM_CONTROL + i * QDMA_MM_CONTROL_STEP;
		ret = regmap_write(qdev->regmap, reg, QDMA_MM_CONTROL_RUN);
		if (ret)
			return ret;

		reg = QDMA_OFFSET_H2C_MM_CONTROL + i * QDMA_MM_CONTROL_STEP;
		ret = regmap_write(qdev->regmap, reg, QDMA_MM_CONTROL_RUN);
		if (ret)
			return ret;
	}

	/* config function map */
	fmap = (struct qdma_cpm5_fmap *)fmap_ctxt.data;
	fmap->qmax = FIELD_PREP(QDMA_CPM5_FMAP_CTXT_QID_MAX_MASK,
				qdev->max_queues);
	fmap_ctxt.cmd  = QDMA_CTX_CMD(QDMA_CTXT_SEL_FMAP, QDMA_CTXT_CMD_WR,
				      qdev->func_id);
	memset(fmap_ctxt.mask, ~0, sizeof(fmap_ctxt.mask));

	ret = qdma_cpm5_ind_reg_write(qdev, &fmap_ctxt);
	if (ret)
		qdma_err(qdev, "Write fmap registers failed, ret: %d", ret);

	return ret;
}

int qdma_cpm5_init(struct qdma_device *qdev)
{
	struct amdmqdma_platdata *pdata = dev_get_platdata(&qdev->pdev->dev);
	u32 val;
	int ret;

	/* read function id */
	ret = regmap_read(qdev->regmap, QDMA_GLBL2_CHANNEL_FUNC_RET, &val);
	if (ret)
		return ret;

	qdev->dev_info.func_id = FIELD_GET(QDMA_GLBL2_FUNC_ID_MASK, val);
	ret = regmap_read(qdev->regmap, QDMA_GLBL2_MISC_CAP, &val);
	if (ret)
		return ret;

	qdev->dev_info.dev_type = FIELD_GET(QDMA_GLBL2_DEV_TYPE_MASK, val);
	if (qdev->dev_info.dev_type != QDMA_DEV_CPM5) {
		qdma_err(qdev, "Device is not supported");
		return -ENODEV;
	}

	ret = qdma_cpm5_get_attrs(qdev);
	if (ret) {
		qdma_err(qdev, "get attributes failed, ret: %d", ret);
		return ret;
	}

	qdma_dbg(qdev, "func id: 0x%x, dev type: 0x%x, queue num: %d",
		 qdev->dev_info.func_id, qdev->dev_info.dev_type,
		 qdev->dev_info.num_qs);

	if (!qdev->dev_info.mm_en) {
		qdma_err(qdev, "does not support memory mapped queue");
		return -ENODEV;
	}

	if (pdata->max_dma_channels &&
	    pdata->max_dma_channels < qdev->dev_info.num_qs)
		qdev->max_queues = pdata->max_dma_channels;
	else
		qdev->max_queues = qdev->dev_info.num_qs;

	qdma_info(qdev, "max queues %d", qdev->max_queues);

	ret = qdma_set_global_regs(qdev);
	if (ret)
		qdma_err(qdev, "failed to set global regs, ret: %d", ret);

	return ret;
}
