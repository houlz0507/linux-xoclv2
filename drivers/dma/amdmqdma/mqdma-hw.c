// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023, Advanced Micro Devices, Inc.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "mqdma.h"
#include "mqdma-hw.h"

static u32 g_ring_sz[REG_COUNT] = {2049, 65, 129, 193, 257, 385, 513, 769,
	1025, 1537, 3073, 4097, 6145, 8193, 12289, 16385};

/*
 * set_initial_regs	- initialise global csr registers
 * @qdev  : qdma_device handle
 *
 * get function id of the device, set csr registers with
 * global ring size, c2h buffer size, c2h timer
 * count c2h count threshold , qbase and qmax values.
 */

u32 set_initial_regs(struct qdma_device *qdev)
{
	u32 id, func_id = 0, err = 0;
	struct qdma_fmap_cfg fmap;
	u32 offset;

	qdma_read_reg(qdev, QDMA_REG_FUNC_ID, &func_id);

	/* Configuring CSR registers */
	/* Global ring sizes */
	qdma_write_csr_values(qdev, QDMA_REG_GLBL_RNG_SZ_BASE, REG_SIZE,
			g_ring_sz);

	/* Enable MM channel */
	if (qdev->dev_info.mm_en) {
		for (id = 0; id < qdev->dev_info.mm_channel_max; id++) {
			offset = id * QDMA_MM_CONTROL_STEP;
			qdma_write_reg(qdev, QDMA_OFFSET_C2H_MM_CONTROL + offset,
					QDMA_MM_CONTROL_RUN);
			qdma_write_reg(qdev, QDMA_OFFSET_H2C_MM_CONTROL + offset,
					QDMA_MM_CONTROL_RUN);
		}
	}

	/* Configure fmap context */
	memset(&fmap, 0, sizeof(struct qdma_fmap_cfg));
	fmap.qbase = qdev->qbase;
	fmap.qmax = qdev->max_queues;

	if (qdev->hw_access && qdev->hw_access->qdma_fmap_conf) {
		err = qdev->hw_access->qdma_fmap_conf(qdev, func_id, &fmap,
				QDMA_HW_ACCESS_WRITE);
		if (err) {
			qdma_err(qdev, "qdma_fmap_conf() err %d\n", err);
			return -1;
		}
	}

	return func_id;
}

/*
 * hw_monitor_reg() - polling a register repeatly until
 *  (the register value & mask) == val or time is up
 *
 * return -QDMA_BUSY_IIMEOUT_ERR if register value didn't match, 0 other wise
 */
int hw_monitor_reg(void *dev_hndl, u32 reg, u32 mask, u32 val,
		u32 interval_us, u32 timeout_us)
{
	struct qdma_device *qdev = (struct qdma_device *)dev_hndl;
	int count;
	u32 v = 0;

	if (!interval_us)
		interval_us = QDMA_REG_POLL_DFLT_INTERVAL_US;
	if (!timeout_us)
		timeout_us = QDMA_REG_POLL_DFLT_TIMEOUT_US;

	count = timeout_us / interval_us;
	do {
		qdma_read_reg(qdev, reg, &v);
		if ((v & mask) == val)
			return QDMA_SUCCESS;
		udelay(interval_us);
	} while (--count);

	qdma_read_reg(qdev, reg, &v);

	if ((v & mask) == val)
		return QDMA_SUCCESS;

	qdma_err(qdev, "%s: Reg read=%u Expected=%u, err:%d\n", __func__, v,
			val, -QDMA_ERR_HWACC_BUSY_TIMEOUT);
	return -QDMA_ERR_HWACC_BUSY_TIMEOUT;
}
