/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022, Advanced Micro Devices, Inc.
 */

#ifndef _DMAENGINE_AMD_QDMA_H
#define _DMAENGINE_AMD_QDMA_H

#include <linux/interrupt.h>
#include <linux/platform_device.h>

int qdma_enable_user_irq(struct platform_device *pdev, u32 irq_num);
void qdma_disable_user_irq(struct platform_device *pdev, u32 irq_num);
int qdma_get_user_irq(struct platform_device *pdev, u32 user_irq_index);

#endif /* _DMAENGINE_AMD_QDMA_H */
