// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __DMA_XDMA_REGS_H
#define __DMA_XDMA_REGS_H

/* The length of register space exposed to host */
#define XDMA_REG_SPACE_LEN	65536

/*
 * maximium number of DMA channels for each direction:
 * Host to Card (H2C) or Card to Host (C2H)
 */
#define XDMA_MAX_CHANNELS	4

/* maximium number of user IRQs */
#define XDMA_MAX_USER_IRQS	16

/* macros to get higher and lower 32-bit address */
#define XDMA_ADDR_H(addr)	((u64)(addr) >> 32)
#define XDMA_ADDR_L(addr)	((u64)(addr) & 0xffffffffUL)

/*
 * macros to define the number of descriptor blocks can be used in one
 * DMA transfer request.
 * the DMA engine uses a linked list of descriptor blocks that specify the
 * source, destination, and length of the DMA transfers.
 */
#define XDMA_DESC_BLOCK_NUM		BIT(7)
#define XDMA_DESC_BLOCK_MASK		(XDMA_DESC_BLOCK_NUM - 1)

/* descriptor definitions */
#define XDMA_DESC_ADJACENT		BIT(5)
#define XDMA_DESC_MAGIC			0xad4bUL
#define XDMA_DESC_MAGIC_SHIFT		16
#define XDMA_DESC_ADJACENT_SHIFT	8
#define XDMA_DESC_STOPPED		BIT(0)
#define XDMA_DESC_COMPLETED		BIT(1)
#define XDMA_DESC_BLEN_BITS		28
#define XDMA_DESC_BLEN_MAX		(BIT(XDMA_DESC_BLEN_BITS) - PAGE_SIZE)
#define XDMA_DESC_NUM	(XDMA_DESC_BLOCK_NUM * XDMA_DESC_ADJACENT)
#define XDMA_DESC_CONTROL(adjacent, flag)				\
	((XDMA_DESC_MAGIC << 16) |					\
	 (((adjacent) - 1) << XDMA_DESC_ADJACENT_SHIFT) | (flag))
#define XDMA_DESC_CONTROL_LAST						\
	XDMA_DESC_CONTROL(1, XDMA_DESC_STOPPED | XDMA_DESC_COMPLETED)

/*
 * Descriptor for a single contiguous memory block transfer.
 *
 * Multiple descriptors are linked by means of the next pointer. An additional
 * extra adjacent number gives the amount of extra contiguous descriptors.
 *
 * The descriptors are in root complex memory, and the bytes in the 32-bit
 * words must be in little-endian byte ordering.
 */
enum xdma_desc_field {
	XDMA_DF_CONTROL,
	XDMA_DF_BYTES,		/* transfer length in bytes */
	XDMA_DF_SADDR_LO,	/* source address (low 32-bit) */
	XDMA_DF_SADDR_HI,	/* source address (high 32-bit) */
	XDMA_DF_DADDR_LO,	/* destination address (low 32-bit) */
	XDMA_DF_DADDR_HI,	/* destination address (high 32-bit) */
	/*
	 * next descriptor in the single-linked list of descriptors;
	 * this is the PCIe (bus) address of the next descriptor in the
	 * root complex memory
	 */
	XDMA_DF_NEXT_LO,	/* next desc address (low 32-bit) */
	XDMA_DF_NEXT_HI,	/* next desc address (high 32-bit) */
	XDMA_DF_NUM
};
#define XDMA_DF_SZ		4
#define XDMA_DESC_SIZE		(XDMA_DF_NUM * XDMA_DF_SZ)
#define XDMA_DESC_BLOCK_SIZE	(XDMA_DESC_SIZE * XDMA_DESC_ADJACENT)

/*
 * Channel registers
 */
enum xdma_chan_reg {
	XDMA_CHAN_IDENTIFIER		= 0x0,
	XDMA_CHAN_CONTROL		= 0x4,
	XDMA_CHAN_CONTROL_W1S		= 0x8,
	XDMA_CHAN_CONTROL_W1C		= 0xc,
	XDMA_CHAN_STATUS		= 0x40,
	XDMA_CHAN_COMPLETED_DESC	= 0x48,
	XDMA_CHAN_ALIGNMENTS		= 0x4c,
	XDMA_CHAN_INTR_ENABLE		= 0x90,
	XDMA_CHAN_INTR_ENABLE_W1S	= 0x94,
	XDMA_CHAN_INTR_ENABLE_W1C	= 0x9c,
};

#define XDMA_CHAN_STRIDE	0x100
#define XDMA_CHAN_H2C_OFFSET	0x0
#define XDMA_CHAN_C2H_OFFSET	0x1000
#define XDMA_CHAN_H2C_TARGET	0x0
#define XDMA_CHAN_C2H_TARGET	0x1
#define XDMA_CHAN_CHECK_TARGET(id, target)		\
	(((u32)(id) >> 16) == 0x1fc0 + (target))

/* bits of the channel control register */
#define CHAN_CTRL_RUN_STOP			(1UL << 0)
#define CHAN_CTRL_IE_DESC_STOPPED		(1UL << 1)
#define CHAN_CTRL_IE_DESC_COMPLETED		(1UL << 2)
#define CHAN_CTRL_IE_DESC_ALIGN_MISMATCH	(1UL << 3)
#define CHAN_CTRL_IE_MAGIC_STOPPED		(1UL << 4)
#define CHAN_CTRL_IE_IDLE_STOPPED		(1UL << 6)
#define CHAN_CTRL_IE_READ_ERROR			(0x1FUL << 9)
#define CHAN_CTRL_IE_DESC_ERROR			(0x1FUL << 19)
#define CHAN_CTRL_NON_INCR_ADDR			(1UL << 25)
#define CHAN_CTRL_POLL_MODE_WB			(1UL << 26)

#define CHAN_CTRL_START	(CHAN_CTRL_RUN_STOP | CHAN_CTRL_IE_READ_ERROR |	\
			 CHAN_CTRL_IE_DESC_ERROR |			\
			 CHAN_CTRL_IE_DESC_ALIGN_MISMATCH |		\
			 CHAN_CTRL_IE_MAGIC_STOPPED |			\
			 CHAN_CTRL_IE_DESC_STOPPED |			\
			 CHAN_CTRL_IE_DESC_COMPLETED)

/* bits of the channel interrupt enable mask */
#define CHAN_IM_DESC_ERROR			(1UL << 19)
#define CHAN_IM_READ_ERROR			(1UL << 9)
#define CHAN_IM_IDLE_STOPPED			(1UL << 6)
#define CHAN_IM_MAGIC_STOPPED			(1UL << 4)
#define CHAN_IM_DESC_COMPLETED			(1UL << 2)
#define CHAN_IM_DESC_STOPPED			(1UL << 1)

#define CHAN_IM_ALL	(CHAN_IM_DESC_ERROR | CHAN_IM_READ_ERROR |	\
			 CHAN_IM_IDLE_STOPPED | CHAN_IM_MAGIC_STOPPED | \
			 CHAN_IM_DESC_COMPLETED | CHAN_IM_DESC_STOPPED)

/*
 * Channel SGDMA registers
 */
enum xdma_chan_sgdma_reg {
	XDMA_SGDMA_IDENTIFIER	= 0x0,
	XDMA_SGDMA_DESC_LO	= 0x80,
	XDMA_SGDMA_DESC_HI	= 0x84,
	XDMA_SGDMA_DESC_ADJ	= 0x88,
	XDMA_SGDMA_DESC_CREDIT	= 0x8c,
};

#define XDMA_SGDMA_BASE(chan_base)	((chan_base) + 0x4000)

/* bits of the SG DMA control register */
#define XDMA_CTRL_RUN_STOP			(1UL << 0)
#define XDMA_CTRL_IE_DESC_STOPPED		(1UL << 1)
#define XDMA_CTRL_IE_DESC_COMPLETED		(1UL << 2)
#define XDMA_CTRL_IE_DESC_ALIGN_MISMATCH	(1UL << 3)
#define XDMA_CTRL_IE_MAGIC_STOPPED		(1UL << 4)
#define XDMA_CTRL_IE_IDLE_STOPPED		(1UL << 6)
#define XDMA_CTRL_IE_READ_ERROR			(0x1FUL << 9)
#define XDMA_CTRL_IE_DESC_ERROR			(0x1FUL << 19)
#define XDMA_CTRL_NON_INCR_ADDR			(1UL << 25)
#define XDMA_CTRL_POLL_MODE_WB			(1UL << 26)

/*
 * interrupt registers
 */
enum xdma_irq_reg {
	XDMA_IRQ_IDENTIFIER		= 0x0,
	XDMA_IRQ_USER_INT_EN		= 0x04,
	XDMA_IRQ_USER_INT_EN_W1S	= 0x08,
	XDMA_IRQ_USER_INT_EN_W1C	= 0x0c,
	XDMA_IRQ_CHAN_INT_EN		= 0x10,
	XDMA_IRQ_CHAN_INT_EN_W1S	= 0x14,
	XDMA_IRQ_CHAN_INT_EN_W1C	= 0x18,
	XDMA_IRQ_USER_INT_REQ		= 0x40,
	XDMA_IRQ_CHAN_INT_REQ		= 0x44,
	XDMA_IRQ_USER_INT_PEND		= 0x48,
	XDMA_IRQ_CHAN_INT_PEND		= 0x4c,
	XDMA_IRQ_USER_VEC_NUM		= 0x80,
	XDMA_IRQ_CHAN_VEC_NUM		= 0xa0,
};

#define XDMA_IRQ_BASE			0x2000
#define XDMA_IRQ_VEC_SHIFT		8

#endif /* __DMA_HDMA_REGS_H */
