// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef _AMD_XDMA_H
#define _AMD_XDMA_H

#include <linux/interrupt.h>
#include <linux/platform_device.h>

int xdma_user_isr_register(struct platform_device *pdev, u32 user_irq_index,
			   irq_handler_t handler, void *arg);

#endif /* _AMD_XDMA_H */
