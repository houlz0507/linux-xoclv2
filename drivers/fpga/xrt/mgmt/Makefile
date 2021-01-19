# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2021 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

FULL_XRT_PATH=$(srctree)/$(src)/..
FULL_DTC_PATH=$(srctree)/scripts/dtc/libfdt

obj-$(CONFIG_FPGA_XRT_XMGMT)	+= xmgmt.o

xmgmt-objs := root.o		\
	   main.o		\
	   fmgr-drv.o		\
	   main-region.o

ccflags-y := -I$(FULL_XRT_PATH)/include		\
	-I$(FULL_DTC_PATH)
