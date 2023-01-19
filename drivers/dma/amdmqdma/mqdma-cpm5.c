// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023, Advanced Micro Devices, Inc.
 */

#include "mqdma.h"

#define QDMA_CPM5_FMAP_NUM_WORDS               2

#define QDMA_OFFSET_GLBL2_PF_BARLITE_INT       0x104
#define QDMA_GLBL2_PF3_BAR_MAP_MASK            GENMASK(23, 18)
#define QDMA_GLBL2_PF2_BAR_MAP_MASK            GENMASK(17, 12)
#define QDMA_GLBL2_PF1_BAR_MAP_MASK            GENMASK(11, 6)
#define QDMA_GLBL2_PF0_BAR_MAP_MASK            GENMASK(5, 0)
#define QDMA_CPM5_GLBL2_CHANNEL_MDMA_ADDR      0x118
#define GLBL2_CHANNEL_MDMA_C2H_ENG_MASK        GENMASK(11, 8)
#define GLBL2_CHANNEL_MDMA_H2C_ENG_MASK        GENMASK(3, 0)
#define QDMA_CPM5_GLBL2_CHANNEL_CAP_ADDR       0x120
#define GLBL2_CHAN_CAP_MULTIQ_MAX_MASK         GENMASK(11, 0)
#define QDMA_CPM5_GLBL2_MISC_CAP_ADDR          0x134
#define QDMA_CPM5_GLBL2_FLR_PRESENT_MASK       BIT(1)

/** QDMA_CPM5_IND_REG_SEL_FMAP */
#define QDMA_CPM5_FMAP_CTXT_W1_QID_MAX_MASK    GENMASK(11, 0)
#define QDMA_CPM5_FMAP_CTXT_W0_QID_MASK        GENMASK(10, 0)

static int qdma_cpm5_fmap_context_read(struct qdma_device *qdev, u16 func_id,
				struct qdma_fmap_cfg *fmap)
{
	qdma_info(qdev, "TODO: need to handle this function\n");
	return QDMA_SUCCESS;
}

static int qdma_cpm5_fmap_context_clear(struct qdma_device *qdev, u16 func_id)
{
	qdma_info(qdev, "TODO: need to handle this function\n");
	return QDMA_SUCCESS;
}

/*
 * qdma_cpm5_indirect_reg_write() - helper function to write indirect
 *              context registers.
 *
 * return -QDMA_ERR_HWACC_BUSY_TIMEOUT if register
 *  value didn't match, QDMA_SUCCESS other wise
 */
static int qdma_cpm5_indirect_reg_write(struct qdma_device *qdev,
					enum ind_ctxt_cmd_sel sel,
					u16 func_id, u32 *data, u16 cnt)
{
	struct qdma_indirect_ctxt_regs regs;
	u32 *wr_data = (u32 *)&regs;
	u32 index, reg_addr;

	spin_lock(&qdev->hw_prg_lock);

	/* write the context data */
	for (index = 0; index < QDMA_IND_CTXT_DATA_NUM_REGS; index++) {
		if (index < cnt)
			regs.qdma_ind_ctxt_data[index] = data[index];
		else
			regs.qdma_ind_ctxt_data[index] = 0;
		regs.qdma_ind_ctxt_mask[index] = 0xFFFFFFFF;
	}

	regs.cmd.word = 0;
	regs.cmd.bits.qid = func_id;
	regs.cmd.bits.op = QDMA_CTXT_CMD_WR;
	regs.cmd.bits.sel = sel;
	reg_addr = QDMA_REG_IND_CTXT_DATA_BASE;

	for (index = 0; index < ((2 * QDMA_IND_CTXT_DATA_NUM_REGS) + 1);
			index++, reg_addr += sizeof(u32))
		qdma_write_reg(qdev, reg_addr, wr_data[index]);

	/* check if the operation went through well */
	if (hw_monitor_reg(qdev, QDMA_REG_IND_CTXT_CMD,
			QDMA_REG_IND_CTXT_CMD_BUSY_MASK, 0,
			QDMA_REG_POLL_DFLT_INTERVAL_US,
			QDMA_REG_POLL_DFLT_TIMEOUT_US)) {
		spin_unlock(&qdev->hw_prg_lock);
		qdma_err(qdev, "hw_monitor_reg failed with err:%d\n",
				-QDMA_ERR_HWACC_BUSY_TIMEOUT);
		return -QDMA_ERR_HWACC_BUSY_TIMEOUT;
	}

	spin_unlock(&qdev->hw_prg_lock);

	return QDMA_SUCCESS;
}

static int qdma_cpm5_fmap_context_write(struct qdma_device *qdev, u16 func_id,
					struct qdma_fmap_cfg *config)
{
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_FMAP;
	u32 fmap[QDMA_CPM5_FMAP_NUM_WORDS] = {0};
	u16 words_count = 0;
	int ret;

	if (!qdev || !config) {
		ret = -QDMA_ERR_INV_PARAM;
		qdma_err(qdev, "qdev or fmap_cfg is NULL, err:%d\n", ret);
		return ret;
	}

	qdma_info(qdev, "[Debug]func_id=%hu, qbase=%hu, qmax=%hu\n", func_id,
			config->qbase, config->qmax);
	fmap[words_count++] = FIELD_SET(QDMA_CPM5_FMAP_CTXT_W0_QID_MASK, 0,
			config->qbase);
	fmap[words_count++] = FIELD_SET(QDMA_CPM5_FMAP_CTXT_W1_QID_MAX_MASK, 0,
			config->qmax);

	return qdma_cpm5_indirect_reg_write(qdev, sel, func_id, fmap, words_count);
}

static int qdma_cpm5_fmap_conf(struct qdma_device *qdev, u16 func_id,
			struct qdma_fmap_cfg *config,
			enum qdma_hw_access_type access_type)
{
	int ret = QDMA_SUCCESS;

	switch (access_type) {
	case QDMA_HW_ACCESS_READ:
		ret = qdma_cpm5_fmap_context_read(qdev, func_id, config);
		break;
	case QDMA_HW_ACCESS_WRITE:
		ret = qdma_cpm5_fmap_context_write(qdev, func_id, config);
		break;
	case QDMA_HW_ACCESS_CLEAR:
		ret = qdma_cpm5_fmap_context_clear(qdev, func_id);
		break;
	case QDMA_HW_ACCESS_INVALIDATE:
	default:
		ret = -QDMA_ERR_INV_PARAM;
		qdma_err(qdev, "access_type(%d) invalid, err:%d\n",
				access_type, ret);
		break;
	}

	return ret;
}

static int qdma_cpm5_get_attrs(struct qdma_device *qdev)
{
	u32 val = 0;

	/* number of PFs */
	qdma_read_reg(qdev, QDMA_OFFSET_GLBL2_PF_BARLITE_INT, &val);
	qdev->dev_info.num_pfs = FIELD_GET(QDMA_GLBL2_PF0_BAR_MAP_MASK, val);

	qdma_read_reg(qdev, QDMA_CPM5_GLBL2_CHANNEL_CAP_ADDR, &val);
	qdev->dev_info.num_qs = FIELD_GET(GLBL2_CHAN_CAP_MULTIQ_MAX_MASK, val);
	/* There are 12 bits assigned in QDMA_CPM5_GLBL2_CHANNEL_CAP_ADDR
	 * to represent the num_qs. For CPM5, max queues can be 4096 which needs
	 * 13 bits(0x1000). Adding a hack in driver to represent 4096 queues
	 * when HW sets the num_qs to 0xFFF
	 */
	if (qdev->dev_info.num_qs == 0xFFF)
		qdev->dev_info.num_qs++;

	/* MM enabled */
	qdma_read_reg(qdev, QDMA_CPM5_GLBL2_CHANNEL_MDMA_ADDR, &val);
	qdev->dev_info.mm_en = (FIELD_GET(GLBL2_CHANNEL_MDMA_C2H_ENG_MASK,
			val) && FIELD_GET(GLBL2_CHANNEL_MDMA_H2C_ENG_MASK,
			val)) ? 1 : 0;

	/* num of mm channels */
	qdev->dev_info.mm_channel_max = 2;
	qdma_info(qdev, "pfs:%d, qs:%d, mm_en:%d, mm_chan_max:%d\n",
			qdev->dev_info.num_pfs, qdev->dev_info.num_qs,
			qdev->dev_info.mm_en, qdev->dev_info.mm_channel_max);

	return 0;
}

struct qdma_hw_access qdma_cpm5_access = {
	.qdma_hw_get_attrs = qdma_cpm5_get_attrs,
	.qdma_fmap_conf = qdma_cpm5_fmap_conf,
};
