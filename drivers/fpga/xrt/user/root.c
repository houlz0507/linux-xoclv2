// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <lizhih@xilinx.com>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/xrt/metadata.h>
#include <linux/xrt/xleaf.h>
#include "xroot.h"
#include "xuser.h"

#define XUSER_MODULE_NAME	"xrt-user"

#define XUSER_PDEV(xu)		((xu)->pdev)
#define XUSER_DEV(xu)		(&(XUSER_PDEV(xu)->dev))
#define xuser_err(xu, fmt, args...)	\
	dev_err(XUSER_DEV(xu), "%s: " fmt, __func__, ##args)
#define xuser_warn(xu, fmt, args...)	\
	dev_warn(XUSER_DEV(xu), "%s: " fmt, __func__, ##args)
#define xuser_info(xu, fmt, args...)	\
	dev_info(XUSER_DEV(xu), "%s: " fmt, __func__, ##args)
#define xuser_dbg(xu, fmt, args...)	\
	dev_dbg(XUSER_DEV(xu), "%s: " fmt, __func__, ##args)

#define XRT_VSEC_ID		0x20
#define XRT_MAX_MRRS		512

static struct class *xuser_class;

/* PCI Device IDs */
#define PCI_DEVICE_ID_U50	0x5021
static const struct pci_device_id xuser_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, PCI_DEVICE_ID_U50), }, /* Alveo U50 */
	{ 0, }
};

struct xuser {
	struct pci_dev *pdev;
	void *root;

	bool ready;
};

static int xuser_config_pci(struct xuser *xu)
{
	struct pci_dev *pdev = XUSER_PDEV(xu);
	int rc, nvec;

	/* enable relaxed ordering */
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN);

	/* enable extended tag */
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_EXT_TAG);

	/* set MRRS */
	rc = pcie_get_readrq(pdev);
	if (rc < 0) {
		xuser_err(xu, "failed to read mrrs: %d", rc);
		return rc;
	}

	if (rc > XRT_MAX_MRRS) {
		rc = pcie_set_readrq(pdev, XRT_MAX_MRRS);
		if (rc < 0) {
			xuser_err(xu, "failed to set mrrs: %d", rc);
			return rc;
		}
	}

	rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (rc) {
		xuser_err(xu, "failed to set dma mask: %d", rc);
		return rc;
	}

	rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
	if (rc) {
		xuser_err(xu, "failed to set consistent_dma_mask: %d", rc);
		return rc;
	}

	pci_set_master(pdev);
	rc = pci_enable_device(pdev);
	if (rc) {
		xuser_err(xu, "failed to enable device: %d", rc);
		return rc;
	}

	nvec = pci_msix_vec_count(pdev);
	if (nvec <= 0) {
		xuser_err(xu, "msix is not supported: %d", nvec);
		return -EINVAL;
	}
	rc = pci_alloc_irq_vectors(pdev, nvec, nvec, PCI_IRQ_MSIX);
	if (rc < 0)
		return rc;

	return 0;
}

static int xuser_create_root_metadata(struct xuser *xu, char **root_dtb)
{
	struct pci_dev *pdev = XUSER_PDEV(xu);
	u32 off_low, off_high, header;
	int cap = 0, ret = 0;

	while ((cap = pci_find_next_ext_capability(pdev, cap, PCI_EXT_CAP_ID_VNDR))) {
		pci_read_config_dword(pdev, cap + PCI_VNDR_HEADER, &header);
		if (PCI_VNDR_HEADER_ID(header) == XRT_VSEC_ID)
			break;
	}
	if (!cap) {
		xuser_err(xu, "No Vendor Specific Capability.");
		return -ENOENT;
	}

	if (pci_read_config_dword(pdev, cap + 8, &off_low) ||
	    pci_read_config_dword(pdev, cap + 12, &off_high)) {
		xuser_err(xu, "pci_read vendor specific failed.");
		return -EINVAL;
	}

	ret = xroot_create_root_metadata(xu->root, ((u64)off_high << 32) | (off_low & ~0xfU),
					 off_low & 0xf, XRT_MD_NODE_USER_MAIN, root_dtb);

	return ret;
}

static ssize_t ready_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xuser *xu;

	xu = pci_get_drvdata(pdev);

	return sprintf(buf, "%d\n", xu->ready);
}
static DEVICE_ATTR_RO(ready);

static struct attribute *xuser_root_attrs[] = {
	&dev_attr_ready.attr,
	NULL
};

static struct attribute_group xuser_root_attr_group = {
	.attrs = xuser_root_attrs,
};

static void xuser_root_get_id(struct device *dev, struct xrt_root_get_id *rid)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	rid->xpigi_vendor_id = pdev->vendor;
	rid->xpigi_device_id = pdev->device;
	rid->xpigi_sub_vendor_id = pdev->subsystem_vendor;
	rid->xpigi_sub_device_id = pdev->subsystem_device;
}

static int xuser_root_get_resource(struct device *dev, struct xrt_root_get_res *res)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xuser *xu;

	xu = pci_get_drvdata(pdev);
	if (res->xpigr_region_id > PCI_STD_RESOURCE_END) {
		xuser_err(xu, "Invalid bar idx %d", res->xpigr_region_id);
		return -EINVAL;
	}

	res->xpigr_res = &pdev->resource[res->xpigr_region_id];
	return 0;
}

static int xuser_root_default(struct device *dev, enum xrt_root_cmd cmd, void *arg)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct xuser *xu;
	int ret = 0;

	xu = pci_get_drvdata(pdev);

	switch (cmd) {
	case XRT_ROOT_REQUEST_IRQ: {
		struct xrt_root_irq_req *req;

		req = (struct xrt_root_irq_req *)arg;
		if (req->xpiir_handler) {
			ret = pci_request_irq(pdev, req->xpiir_vec_idx, req->xpiir_handler, NULL,
					      req->xpiir_dev_id, req->xpiir_name);
			if (ret)
				xuser_err(xu, "failed to alloc vector: %d", ret);
		} else {
			pci_free_irq(pdev, req->xpiir_vec_idx, req->xpiir_dev_id);
		}
		break;
	}
	default:
		xuser_err(xu, "Invalid command %d", cmd);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct xroot_physical_function_callback xuser_xroot_pf_cb = {
	.xpc_get_id = xuser_root_get_id,
	.xpc_get_resource = xuser_root_get_resource,
	.xpc_default_cb = xuser_root_default,
};

static int xuser_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	char *dtb = NULL;
	struct xuser *xu;
	int ret;

	xu = devm_kzalloc(dev, sizeof(*xu), GFP_KERNEL);
	if (!xu)
		return -ENOMEM;

	xu->pdev = pdev;
	pci_set_drvdata(pdev, xu);
	ret = xuser_config_pci(xu);
	if (ret)
		goto failed;

	ret = xroot_probe(&pdev->dev, &xuser_xroot_pf_cb, &xu->root);
	if (ret)
		goto failed;

	ret = xuser_create_root_metadata(xu, &dtb);
	if (ret)
		goto failed_metadata;

	ret = xroot_create_group(xu->root, dtb);
	vfree(dtb);
	if (ret < 0)
		xuser_err(xu, "failed to create root group: %d", ret);

	if (!xroot_wait_for_bringup(xu->root))
		xuser_err(xu, "failed to bringup all groups");
	else
		xu->ready = true;

	ret = sysfs_create_group(&pdev->dev.kobj, &xuser_root_attr_group);
	if (ret) {
		/* Warning instead of failing the probe. */
		xuser_warn(xu, "create xuser root attrs failed: %d", ret);
	}

	xroot_broadcast(xu->root, XRT_EVENT_POST_CREATION);
	xuser_info(xu, "%s started successfully", XUSER_MODULE_NAME);
	return 0;

failed_metadata:
	xroot_remove(xu->root);
failed:
	pci_set_drvdata(pdev, NULL);
	return ret;
}

static void xuser_remove(struct pci_dev *pdev)
{
	struct xuser *xu = pci_get_drvdata(pdev);

	xroot_broadcast(xu->root, XRT_EVENT_PRE_REMOVAL);
	sysfs_remove_group(&pdev->dev.kobj, &xuser_root_attr_group);
	xroot_remove(xu->root);
	pci_free_irq_vectors(pdev);
	xuser_info(xu, "%s cleaned up successfully", XUSER_MODULE_NAME);
}

static struct pci_driver xuser_driver = {
	.name = XUSER_MODULE_NAME,
	.id_table = xuser_pci_ids,
	.probe = xuser_probe,
	.remove = xuser_remove,
};

static int __init xuser_init(void)
{
	int res = 0;

	res = xuser_register_leaf();
	if (res)
		return res;

	xuser_class = class_create(THIS_MODULE, XUSER_MODULE_NAME);
	if (IS_ERR(xuser_class))
		return PTR_ERR(xuser_class);

	res = pci_register_driver(&xuser_driver);
	if (res) {
		class_destroy(xuser_class);
		return res;
	}

	return 0;
}

static __exit void xuser_exit(void)
{
	pci_unregister_driver(&xuser_driver);
	class_destroy(xuser_class);
	xuser_unregister_leaf();
}

module_init(xuser_init);
module_exit(xuser_exit);

MODULE_DEVICE_TABLE(pci, xuser_pci_ids);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo user function driver");
MODULE_LICENSE("GPL v2");
