/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023, Advanced Micro Devices, Inc.
 */
#ifndef __DMA_MQDMA_HW_H
#define __DMA_MQDMA_HW_H

/* polling a register */
#define QDMA_REG_POLL_DFLT_INTERVAL_US    10          /* 10us per poll */
#define QDMA_REG_POLL_DFLT_TIMEOUT_US     (500*1000)  /* 500ms */

#define QDMA_GLBL2_CHANNEL_FUNC_RET       0x12c
#define QDMA_GLBL2_FUNC_ID_MASK           GENMASK(7, 0)

#define QDMA_GLBL2_MISC_CAP               0x134
#define QDMA_GLBL2_DEV_TYPE_MASK          GENMASK(31, 28)
#define QDMA_DEV_CPM5                     0x2

#define QDMA_REG_FUNC_ID                  0x12C
#define QDMA_REG_GLBL_WB_ACC              0x250
#define GLBL_DSC_CFG_RSVD_1_MASK          GENMASK(31, 10)
#define GLBL_DSC_CFG_UNC_OVR_COR_MASK     BIT(9)
#define GLBL_DSC_CFG_CTXT_FER_DIS_MASK    BIT(8)
#define GLBL_DSC_CFG_RSVD_2_MASK          GENMASK(7, 6)
#define GLBL_DSC_CFG_MAXFETCH_MASK        GENMASK(5, 3)
#define GLBL_DSC_CFG_WB_ACC_INT_MASK      GENMASK(2, 0)

/* QDMA Context array size */
#define QDMA_FMAP_NUM_WORDS               2

#define QDMA_MM_CONTROL_RUN               0x1
#define QDMA_MM_CONTROL_STEP              0x100

#define REG_COUNT                         16
#define REG_SIZE                          4

#define CTXT_REG_COUNT                    8
#define IND_CTXT_CMD_QID_SHIFT            7
#define IND_CTXT_CMD_OP_SHIFT             5
#define IND_CTXT_CMD_SEL_SHIFT            1

#define QDMA_REG_IND_CTXT_DATA_BASE       0x804
#define QDMA_REG_IND_CTXT_MASK_BASE       0x824
#define QDMA_REG_IND_CTXT_CMD             0x844
#define QDMA_REG_IND_CTXT_CMD_BUSY_MASK   BIT(0)

#define QDMA_REG_GLBL_RNG_SZ_BASE         0x204
#define QDMA_REG_C2H_BUF_SZ_BASE          0xAB0
#define QDMA_REG_C2H_TIMER_CNT_BASE       0xA00
#define QDMA_REG_C2H_CNT_TH_BASE          0xA40

#define QDMA_OFFSET_C2H_MM_CONTROL        0x1004

#define QDMA_OFFSET_H2C_MM_CONTROL        0x1204

/**
 * @enum qdma_wrb_interval - writeback update interval
 */
enum qdma_wrb_interval {
	/** @QDMA_WRB_INTERVAL_4 - writeback update interval of 4 */
	QDMA_WRB_INTERVAL_4,
	/** @QDMA_WRB_INTERVAL_8 - writeback update interval of 8 */
	QDMA_WRB_INTERVAL_8,
	/** @QDMA_WRB_INTERVAL_16 - writeback update interval of 16 */
	QDMA_WRB_INTERVAL_16,
	/** @QDMA_WRB_INTERVAL_32 - writeback update interval of 32 */
	QDMA_WRB_INTERVAL_32,
	/** @QDMA_WRB_INTERVAL_64 - writeback update interval of 64 */
	QDMA_WRB_INTERVAL_64,
	/** @QDMA_WRB_INTERVAL_128 - writeback update interval of 128 */
	QDMA_WRB_INTERVAL_128,
	/** @QDMA_WRB_INTERVAL_256 - writeback update interval of 256 */
	QDMA_WRB_INTERVAL_256,
	/** @QDMA_WRB_INTERVAL_512 - writeback update interval of 512 */
	QDMA_WRB_INTERVAL_512,
	/** @QDMA_NUM_WRB_INTERVALS - total number of writeback intervals */
	QDMA_NUM_WRB_INTERVALS
};

/* CSR Default values */
#define DEFAULT_MAX_DSC_FETCH             6
#define DEFAULT_WRB_INT                   QDMA_WRB_INTERVAL_128

/*
 * Q Context programming (indirect)
 */
enum ind_ctxt_cmd_op {
	QDMA_CTXT_CMD_CLR,
	QDMA_CTXT_CMD_WR,
	QDMA_CTXT_CMD_RD,
	QDMA_CTXT_CMD_INV
};

enum ind_ctxt_cmd_sel {
	QDMA_CTXT_SEL_SW_C2H,
	QDMA_CTXT_SEL_SW_H2C,
	QDMA_CTXT_SEL_HW_C2H,
	QDMA_CTXT_SEL_HW_H2C,
	QDMA_CTXT_SEL_CR_C2H,
	QDMA_CTXT_SEL_CR_H2C,
	QDMA_CTXT_SEL_CMPT,
	QDMA_CTXT_SEL_PFTCH,
	QDMA_CTXT_SEL_INT_COAL,
	QDMA_CTXT_SEL_PASID_RAM_LOW,
	QDMA_CTXT_SEL_PASID_RAM_HIGH,
	QDMA_CTXT_SEL_TIMER,
	QDMA_CTXT_SEL_FMAP,
};

enum qdma_access_error_codes {
	QDMA_SUCCESS = 0,
	QDMA_ERR_INV_PARAM,
	QDMA_ERR_NO_MEM,
	QDMA_ERR_HWACC_BUSY_TIMEOUT,
	QDMA_ERR_HWACC_INV_CONFIG_BAR,
	QDMA_ERR_HWACC_NO_PEND_LEGCY_INTR,
	QDMA_ERR_HWACC_BAR_NOT_FOUND,
	QDMA_ERR_HWACC_FEATURE_NOT_SUPPORTED,   /* 7 */
	QDMA_ERR_RM_RES_EXISTS,             /* 8 */
	QDMA_ERR_RM_RES_NOT_EXISTS,
	QDMA_ERR_RM_DEV_EXISTS,
	QDMA_ERR_RM_DEV_NOT_EXISTS,
	QDMA_ERR_RM_NO_QUEUES_LEFT,
	QDMA_ERR_RM_QMAX_CONF_REJECTED,     /* 13 */
	QDMA_ERR_MBOX_FMAP_WR_FAILED,       /* 14 */
	QDMA_ERR_MBOX_NUM_QUEUES,
	QDMA_ERR_MBOX_INV_QID,
	QDMA_ERR_MBOX_INV_RINGSZ,
	QDMA_ERR_MBOX_INV_BUFSZ,
	QDMA_ERR_MBOX_INV_CNTR_TH,
	QDMA_ERR_MBOX_INV_TMR_TH,
	QDMA_ERR_MBOX_INV_MSG,
	QDMA_ERR_MBOX_SEND_BUSY,
	QDMA_ERR_MBOX_NO_MSG_IN,
	QDMA_ERR_MBOX_REG_READ_FAILED,
	QDMA_ERR_MBOX_ALL_ZERO_MSG,         /* 25 */
};

/**
 * enum qdma_hw_access_type - To hold hw access type
 */
enum qdma_hw_access_type {
	QDMA_HW_ACCESS_READ,
	QDMA_HW_ACCESS_WRITE,
	QDMA_HW_ACCESS_CLEAR,
	QDMA_HW_ACCESS_INVALIDATE,
	QDMA_HW_ACCESS_MAX
};

#endif
