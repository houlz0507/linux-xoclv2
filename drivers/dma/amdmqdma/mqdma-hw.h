/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022, Advanced Micro Devices, Inc.
 */
#ifndef __DMA_MQDMA_HW_H
#define __DMA_MQDMA_HW_H

#define QDMA_GLBL2_CHANNEL_FUNC_RET	0x12c
#define QDMA_GLBL2_FUNC_ID_MASK		GENMASK(7, 0)

#define QDMA_GLBL2_MISC_CAP		0x134
#define QDMA_GLBL2_DEV_TYPE_MASK	GENMASK(31, 28)
#define QDMA_DEV_CPM5		0x2

#endif
