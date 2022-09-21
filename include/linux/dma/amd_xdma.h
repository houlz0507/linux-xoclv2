/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef _DMAENGINE_AMD_XDMA_H
#define _DMAENGINE_AMD_XDMA_H

#include <linux/interrupt.h>
#include <linux/platform_device.h>

int xdma_request_user_irq(struct platform_device *pdev, u32 user_irq_index,
			  irq_handler_t handler, void *arg, ulong flags);
void xdma_free_user_irq(struct platform_device *pdev, u32 user_irq_index,
			void *arg);

#endif /* _DMAENGINE_AMD_XDMA_H */
