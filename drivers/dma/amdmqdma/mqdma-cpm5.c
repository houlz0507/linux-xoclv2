// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022, Advanced Micro Devices, Inc.
 */

#include "mqdma.h"

#define QDMA_CPM5_GLBL2_CHANNEL_CAP_ADDR	0x120
#define GLBL2_CHAN_CAP_MULTIQ_MAX_MASK		GENMASK(11, 0)

static int qdma_cpm5_get_attrs(struct qdma_device *qdev)
{
	u32 val;
	int ret;

	ret = qdma_read_reg(qdev, 0, QDMA_CPM5_GLBL2_CHANNEL_CAP_ADDR, &val);
	if (ret) {
		qdma_err(qdev, "read cpm5 channel cap failed, ret %d", ret);
		return ret;
	}

	qdev->dev_info.num_qs = FIELD_GET(GLBL2_CHAN_CAP_MULTIQ_MAX_MASK, val);

	return 0;
}

struct qdma_hw_access qdma_cpm5_access = {
	.qdma_hw_get_attrs = qdma_cpm5_get_attrs,
};
