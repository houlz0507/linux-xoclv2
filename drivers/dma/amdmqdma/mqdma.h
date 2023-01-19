/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023, Advanced Micro Devices, Inc.
 */

#ifndef __DMA_MQDMA_H
#define __DMA_MQDMA_H

#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <linux/dmaengine.h>
#include <linux/platform_device.h>
#include "../virt-dma.h"
#include "mqdma-hw.h"

/* m - bit mask
 * y - value to be written in the bitrange
 * x - input value whose bitrange to be modified
 */
#define FIELD_SET(m, y, x)	\
	(((x) & ~(m)) |		\
	FIELD_PREP((m), (y)))

/* QDMA HW version string array length */
#define QDMA_VERSION_STRING_LEN          32
#define QDMA_QUEUE_NAME_MAXLEN           20

#define QDMA_GLBL2_FLR_PRESENT_MASK      BIT(1)
#define QDMA_GLBL2_MAILBOX_EN_MASK       BIT(0)

/**
 * struct qdma_request - DMA request structure
 * @vdesc: Virtual DMA descriptor
 * @dir: Transferring direction of the request
 * @slave_addr: Physical address on DMA slave side
 * @sgl: Scatter gather list on host side
 * @sg_off: Start offset of the first sgl segment
 * @nents: Number of sgl segments to transfer
 */
struct qdma_request {
	struct virt_dma_desc		vdesc;
	enum dma_transfer_direction	dir;
	u64				slave_addr;
	struct scatterlist		*sgl;
	u32				sg_off;
	u32				nents;
};

/* Structure for H2C descriptor writeback */
struct qdma_h2c_wb {
	u16 pidx;
	u16 cidx;
	u32 rsvd;
};

struct qdma_queue_conf {
	u16 qidx;    /* 0 ~ (qdma_dev_conf.qsets_max - 1) */
	u8 st;
	u8 c2h;
	u8 filler;
	u8 st_c2h_wrb_desc_size;

	/* fill in by libqdma */
	char name[QDMA_QUEUE_NAME_MAXLEN + 1];
	u32 rngsz;     /* size of the descriptor queue */
};

struct qdma_descq {
	struct qdma_queue_conf conf;
	u64 h2c_desc_ring_base;
	u8 channel;
	u8 enabled;   /* taken/config'ed */
	u8 inited;    /* resource/context initialized */
	u8 color;     /* st c2h only */
	u8 func_id;
	u8 bypass;

	/* configuration for queue context updates */
	u32 irq_en;
	u32 irq_arm;
	u32 pfetch_en;
	u32 wrb_stat_desc_en;
	u32 wrb_trig_mode;
	u32 wrb_timer_idx;
	u32 qidx_hw;
	u32 qidx_soft;
	int intr_id;
	u8 cmpl_cnt_th_idx;
	u8 cmpl_rng_sz_idx;
	u8 cmpl_stat_en;
	u8 cmpl_trig_mode;
	u8 c2h_buf_sz_idx;
	u8 cmpl_timer_idx;
	u32 avail;
	u32 pend;
	u32 pidx;
	u32 cidx;
	u32 credit;
	u8 *desc;
	u8 *desc_wb;
};

struct qdma_q_stats {
	u64 packets;
	u64 bytes;
};

struct qdma_h2c_buffer {
	dma_addr_t dma;
	u32 length;
	u32 time_stamp;
	u16 next_to_watch;
	bool mapped_as_page;
};

struct qdma_c2h_buffer {
	dma_addr_t dma;
};

struct qdma_h2c_desc {
	u32 rsv1;    /* reserved */
	u32 length;    /* Length of the data to dma */
	dma_addr_t src_addr;  /*  64 bit source address*/
};

struct qdma_c2h_desc {
	dma_addr_t dst_addr; /* Destination Address */
};

/* Structure for completion descriptor */
struct qdma_c2h_cmpl {
	u8 data_format;
	u8 color;
	u8 err;
	u8 desc_used;
	u32 len;
	u64 rsvd;
};

/* Structure for Completion descriptor writeback */
struct qdma_cmpl_wb {
	u16 pidx;
	u16 cidx;
	u32 color_isr_status;
};

struct qdma_h2c_ring {
	/* pointer to the h2c descriptor ring memory */
	struct qdma_h2c_desc *h2c_baseaddr;
	/* physical address of the h2c descriptor ring */
	dma_addr_t h2c_phaddr;
	/* length of h2c descriptor ring in bytes */
	u32 size;
	/* number of descriptors in the ring */
	u32 count;
	/* next descriptor to associate a buffer with */
	u32 next_to_use;
	/* next descriptor to free skb*/
	u32 next_to_remove;
	/*count for bulk dma*/
	u32 bulk_count;
	/* array of buffer information structs */
	struct qdma_h2c_buffer *buffer_info;
	struct qdma_q_stats stats;

	struct qdma_descq *h2c_q;
	struct qdma_h2c_wb *h2c_wb;

};

struct qdma_c2h_ring {
	/* pointer to the c2h descriptor ring memory */
	struct qdma_c2h_desc *c2h_baseaddr;
	/* physical address of the c2h descriptor ring */
	dma_addr_t c2h_phaddr;
	/* length of c2h descriptor ring in bytes */
	u32 size;
	/* number of descriptors in the ring */
	u32 count;
	/* next descriptor to associate a buffer with */
	u32 next_to_use;
	/* array of buffer information structs */
	struct qdma_c2h_buffer *buffer_info;
	struct qdma_descq *c2h_q;

	/* pointer to the completion descriptor ring memory */
	struct qdma_c2h_cmpl *cmpl_baseaddr;
	/* physical address of the completion descriptor ring */
	dma_addr_t cmpl_phaddr;
	/* length of ccompletion descriptor ring in bytes */
	u32 cmpl_size;
	struct qdma_q_stats stats;

	struct qdma_c2h_cmpl *cmpl;
	struct qdma_cmpl_wb *cmpl_wb;
};

/**
 * struct qdma_chan - Driver specific DMA channel structure
 * @vchan: Virtual channel
 * @qdev_hdl: Pointer to DMA device structure
 * @busy: Busy flag of the channel
 * @dir: Transferring direction of the channel
 * @cfg: Transferring config of the channel
 * @tasklet: Channel tasklet
 */
struct qdma_chan {
	struct virt_dma_chan		vchan;
	void				*qdev_hdl;
	bool				busy;
	enum dma_transfer_direction	dir;
	struct dma_slave_config		cfg;
	struct tasklet_struct		tasklet;
};

/*
 * struct qdma_version - DMA version information
 * @ip_type: QDMA IP type
 * @device_type: QDMA Device Type
 * @device_type_str: QDMA device type string
 * @ip_type_str: QDMA IP type string
 */
struct qdma_version {
	u32	ip_type;
	u32	device_type;
	char	device_type_str[QDMA_VERSION_STRING_LEN];
	char	ip_type_str[QDMA_VERSION_STRING_LEN];
};

/*
 * struct qdma_dev_info- - QDMA device attributes
 * @dev_type: QDMA Device Type
 * @func_id:
 * @num_pfs: Num of PFs
 * @num_qs: Maximum number of queues per device
 * @mm_channel_max: Num of MM channels
 * @flr_present: FLR resent or not
 * @mm_en: MM mode supported or not
 */
struct qdma_dev_info {
	u32	dev_type;
	u32	func_id;
	u8	num_pfs;
	u16	num_qs;
	u8	mm_channel_max;
	u8	flr_present;
	u8	mm_en;
	u8	mailbox_en;
};

struct qdma_device;
struct qdma_fmap_cfg;

/*
 * struct qdma_hw_access - HW access callback functions
 */
struct qdma_hw_access {
	int (*qdma_hw_get_attrs)(struct qdma_device *qdev);
	int (*qdma_fmap_conf)(struct qdma_device *qdev, u16 func_id,
			struct qdma_fmap_cfg *config,
			enum qdma_hw_access_type access_type);
};

/*
 * struct qdma_device - DMA device structure
 * @qdma_dev_info: Defines per device qdma property
 * @qdma_hw_access: qdma_hw_access structure
 * @qdma_version: qdma_version structure
 * @ioaddr: PCIe config. bar
 * @hw_prog_lock: DMA device hardware program lock
 */
struct qdma_device {
	struct platform_device	*pdev;
	struct dma_device	dma_dev;
	u32			qbase;
	u32			max_queues;
	u32			h2c_chan_num;
	u32			c2h_chan_num;
	u32			func_id;
	u32			flags;
	struct regmap           *regmap;
	struct qdma_chan	*h2c_chans;
	struct qdma_chan	*c2h_chans;
	struct qdma_h2c_ring	*h2c_ring;
	struct qdma_c2h_ring	*c2h_ring;
	struct qdma_dev_info	dev_info;
	struct qdma_hw_access	*hw_access;
	struct qdma_version	version_info;
	void __iomem		*ioaddr;
	spinlock_t              hw_prg_lock;
};

/* ------------------------ indirect register context fields -----------*/
union qdma_ind_ctxt_cmd {
	u32 word;
	struct {
		u32 busy;
		u32 sel;
		u32 op;
		u32 qid;
		u32 rsvd;
	} bits;
};

#define QDMA_IND_CTXT_DATA_NUM_REGS                 8

/**
 * struct qdma_indirect_ctxt_regs - Inirect Context programming registers
 */
struct qdma_indirect_ctxt_regs {
	u32 qdma_ind_ctxt_data[QDMA_IND_CTXT_DATA_NUM_REGS];
	u32 qdma_ind_ctxt_mask[QDMA_IND_CTXT_DATA_NUM_REGS];
	union qdma_ind_ctxt_cmd cmd;
};

/*
 * struct qdma_fmap_cfg - fmap config data structure
 * @qbase - queue base for the function
 * @qmax - maximum queues in the function
 */
struct qdma_fmap_cfg {
	u16 qbase;
	u16 qmax;
};

extern struct qdma_hw_access qdma_cpm5_access;
u32 set_initial_regs(struct qdma_device *qdev);
int hw_monitor_reg(void *dev_hndl, u32 reg, u32 mask, u32 val,
		u32 interval_us, u32 timeout_us);

#define qdma_err(qdev, fmt, args...)					\
	dev_err(&(qdev)->pdev->dev, fmt, ##args)
#define qdma_info(qdev, fmt, args...)					\
	dev_info(&(qdev)->pdev->dev, fmt, ##args)

static inline void qdma_read_reg(struct qdma_device *qdev, u32 reg, u32 *val)
{
	int ret;

	ret = regmap_read(qdev->regmap, reg, val);
	if (ret)
		qdma_err(qdev, "Failed to read reg 0x%X, err: %d\n", reg, ret);
}

static inline void qdma_write_reg(struct qdma_device *qdev, u32 reg, u32 val)
{
	int ret;

	ret = regmap_write(qdev->regmap, reg, val);
	if (ret)
		qdma_err(qdev, "Failed to write reg 0x%X, err: %d\n", reg, ret);
}

static inline void qdma_write_csr_values(struct qdma_device *qdev, u32 reg,
					u32 cnt, const u32 *values)
{
	u32 index, reg_addr;

	for (index = 0; index < cnt; index++) {
		reg_addr = reg + (index * sizeof(u32));
		qdma_write_reg(qdev, reg_addr, values[index]);
	}
}

#endif
