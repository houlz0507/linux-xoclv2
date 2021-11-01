// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 *	Lizhi Hou <lizhih@xilinx.com>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/fpga-xrt.h>
#include "xroot.h"
#include "xmgmt.h"

#define XMGMT_MODULE_NAME	"xrt-mgmt"
#define XMGMT_DRIVER_VERSION	"4.0.0"

#define XMGMT_PDEV(xm)		((xm)->pdev)
#define XMGMT_DEV(xm)		(&(XMGMT_PDEV(xm)->dev))
#define xmgmt_err(xm, fmt, args...)	\
	dev_err(XMGMT_DEV(xm), "%s: " fmt, __func__, ##args)
#define xmgmt_warn(xm, fmt, args...)	\
	dev_warn(XMGMT_DEV(xm), "%s: " fmt, __func__, ##args)
#define xmgmt_info(xm, fmt, args...)	\
	dev_info(XMGMT_DEV(xm), "%s: " fmt, __func__, ##args)
#define xmgmt_dbg(xm, fmt, args...)	\
	dev_dbg(XMGMT_DEV(xm), "%s: " fmt, __func__, ##args)
#define XMGMT_DEV_ID(_pcidev)			\
	({ typeof(_pcidev) (pcidev) = (_pcidev);	\
	((pci_domain_nr((pcidev)->bus) << 16) |	\
	PCI_DEVID((pcidev)->bus->number, 0)); })

/* PCI Device IDs */
/*
 * Golden image is preloaded on the device when it is shipped to customer.
 * Then, customer can load other shells (from Xilinx or some other vendor).
 * If something goes wrong with the shell, customer can always go back to
 * golden and start over again.
 */
#define PCI_DEVICE_ID_U50_GOLDEN	0xD020
#define PCI_DEVICE_ID_U50		0x5020
static const struct pci_device_id xmgmt_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_U50), }, /* Alveo U50 */
	{ 0, }
};

struct xmgmt {
	struct pci_dev *pdev;
	void *root;

	bool ready;
};

static int xmgmt_config_pci(struct xmgmt *xm)
{
	struct pci_dev *pdev = XMGMT_PDEV(xm);
	int rc;

	rc = pcim_enable_device(pdev);
	if (rc < 0) {
		xmgmt_err(xm, "failed to enable device: %d", rc);
		return rc;
	}

	rc = pci_enable_pcie_error_reporting(pdev);
	if (rc)
		xmgmt_warn(xm, "failed to enable AER: %d", rc);

	pci_set_master(pdev);

	rc = pcie_get_readrq(pdev);
	if (rc > XRT_MAX_READRQ)
		pcie_set_readrq(pdev, XRT_MAX_READRQ);
	return 0;
}

static int xmgmt_add_vsec_node(struct xmgmt *xm, void *md)
{
	struct pci_dev *pdev = XMGMT_PDEV(xm);
	u32 vsec_data[XRT_VSEC_DATA_SZ];
	int cap = 0, i, ret;
	u32 header;

	while ((cap = pci_find_next_ext_capability(pdev, cap, PCI_EXT_CAP_ID_VNDR))) {
		pci_read_config_dword(pdev, cap + PCI_VNDR_HEADER, &header);
		if (PCI_VNDR_HEADER_ID(header) == XRT_VSEC_ID)
			break;
	}
	if (!cap) {
		xmgmt_info(xm, "No Vendor Specific Capability.");
		return -ENOENT;
	}

	cap += PCI_VNDR_HEADER + sizeof(header);
	for (i = 0; i < XRT_VSEC_DATA_SZ; i++) {
		if (pci_read_config_dword(pdev, cap + i * sizeof(u32), &vsec_data[i])) {
			xmgmt_err(xm, "pci_read vendor specific failed.");
			return -EINVAL;
		}
	}

	ret = xrt_md_add_endpoint(DEV(pdev), md, XRT_MD_NODE_VSEC);
	if (ret) {
		xmgmt_err(xm, "add vsec metadata failed, ret %d", ret);
		goto failed;
	}

	ret = xrt_md_set_prop(DEV(pdev), md, XRT_MD_NODE_VSEC, XRT_MD_PROP_DEVICE_ID,
			      XRT_SUBDEV_VSEC, 0);
	if (ret) {
		xmgmt_err(xm, "set vsec device id failed, ret %d", ret);
		goto failed;
	}

	ret = xrt_md_set_prop(DEV(pdev), md, XRT_MD_NODE_VSEC, XRT_MD_PROP_PRIV_DATA,
			      (u64)vsec_data, sizeof(vsec_data));
	if (ret)
		xmgmt_err(xm, "set vsec data failed, ret %d", ret);

failed:
	return ret;
}

static int xmgmt_create_root_metadata(struct xmgmt *xm, void **root_md)
{
	void *md;
	int ret;

	ret = xrt_md_create(XMGMT_DEV(xm), 2, XRT_VSEC_DATA_SZ * sizeof(u32), &md);
	if (ret) {
		xmgmt_err(xm, "create metadata failed, ret %d", ret);
		goto failed;
	}

	ret = xmgmt_add_vsec_node(xm, md);
	if (ret)
		goto failed;

	*root_md = md;
	return 0;

failed:
	vfree(md);
	return ret;
}

static void xmgmt_root_get_id(struct device *dev, struct xrt_root_get_id *rid)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	rid->xpigi_vendor_id = pdev->vendor;
	rid->xpigi_device_id = pdev->device;
	rid->xpigi_sub_vendor_id = pdev->subsystem_vendor;
	rid->xpigi_sub_device_id = pdev->subsystem_device;
}

static int xmgmt_root_get_resource(struct device *dev, struct xrt_root_get_res *res)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xmgmt *xm;

	xm = pci_get_drvdata(pdev);
	if (res->xpigr_region_id > PCI_STD_RESOURCE_END) {
		xmgmt_err(xm, "Invalid bar idx %d", res->xpigr_region_id);
		return -EINVAL;
	}

	res->xpigr_res = &pdev->resource[res->xpigr_region_id];
	return 0;
}

static struct xroot_physical_function_callback xmgmt_xroot_pf_cb = {
	.xpc_get_id = xmgmt_root_get_id,
	.xpc_get_resource = xmgmt_root_get_resource,
};

static int xmgmt_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct xmgmt *xm;
	void *md = NULL;
	int ret;

	xm = devm_kzalloc(dev, sizeof(*xm), GFP_KERNEL);
	if (!xm)
		return -ENOMEM;

	xm->pdev = pdev;
	pci_set_drvdata(pdev, xm);

	ret = xmgmt_config_pci(xm);
	if (ret)
		goto failed;

	ret = xroot_probe(&pdev->dev, &xmgmt_xroot_pf_cb, &xm->root);
	if (ret)
		goto failed;

	ret = xmgmt_create_root_metadata(xm, &md);
	if (ret)
		goto failed_metadata;

	ret = xroot_create_group(xm->root, md);
	vfree(md);
	if (ret) {
		xmgmt_err(xm, "failed to create root group: %d", ret);
		goto failed;
	}

	xmgmt_info(xm, "%s started successfully", XMGMT_MODULE_NAME);
	return 0;

failed_metadata:
	xroot_remove(xm->root);

failed:
	pci_set_drvdata(pdev, NULL);
	return ret;
}

static void xmgmt_remove(struct pci_dev *pdev)
{
	struct xmgmt *xm = pci_get_drvdata(pdev);

	xroot_remove(xm->root);
	pci_disable_pcie_error_reporting(xm->pdev);
	xmgmt_info(xm, "%s cleaned up successfully", XMGMT_MODULE_NAME);
}

static struct pci_driver xmgmt_driver = {
	.name = XMGMT_MODULE_NAME,
	.id_table = xmgmt_pci_ids,
	.probe = xmgmt_probe,
	.remove = xmgmt_remove,
};

static int __init xmgmt_init(void)
{
	int res = 0;

	res = pci_register_driver(&xmgmt_driver);
	if (res)
		return res;

	return 0;
}

static __exit void xmgmt_exit(void)
{
	pci_unregister_driver(&xmgmt_driver);
}

module_init(xmgmt_init);
module_exit(xmgmt_exit);

MODULE_DEVICE_TABLE(pci, xmgmt_pci_ids);
MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo management function driver");
MODULE_LICENSE("GPL v2");
