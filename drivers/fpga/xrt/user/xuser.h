/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XUSER_H_
#define _XUSER_H_

void *xuser_mailbox_probe(struct xrt_device *xdev);
void xuser_mailbox_remove(void *handle);
void xuser_mailbox_event_cb(struct xrt_device *xdev, void *arg);
int xuser_peer_get_metadata(void *handle, char **dtb);

void *xuser_xdev2mailbox(struct xrt_device *xdev);
int xuser_register_leaf(void);
void xuser_unregister_leaf(void);

#endif /* _XUSER_H_ */
