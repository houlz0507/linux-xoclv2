// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA VSEC Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/firmware.h>
#include <linux/fpga-xrt.h>

#define XRT_VSEC "xrt_vsec"

#define VSEC_TYPE_UUID		0x50
#define VSEC_TYPE_FLASH		0x51
#define VSEC_TYPE_PLATINFO	0x52
#define VSEC_TYPE_MAILBOX	0x53
#define VSEC_TYPE_END		0xff

#define VSEC_REG_FORMAT		0x0
#define VSEC_REG_LENGTH		0x4
#define VSEC_REG_ENTRY		0x8

#define VSEC_BAR_OFFSET(pd)	(((u64)(pd)->addr_hi << 32) + ((pd)->addr_lo & ~0xf))
#define VSEC_BAR_INDEX(pd)	((pd)->addr_lo & 0xf)

struct xrt_vsec_priv_data {
	u32		addr_lo;
	u32		addr_hi;
};

struct xrt_vsec_header {
	u32		format;
	u32		length;
	u32		entry_sz;
	u32		rsvd;
} __packed;

struct xrt_vsec_entry {
	u8		type;
	u8		bar_rev;
	u16		off_lo;
	u32		off_hi;
	u8		ver_type;
	u8		minor;
	u8		major;
	u8		rsvd0;
	u32		rsvd1;
} __packed;

struct vsec_device {
	u8		type;
	char		*ep_name;
	ulong		size;
	char		*compat;
};

XRT_DEFINE_REGMAP_CONFIG(vsec_regmap_config);

struct xrt_vsec {
	struct xrt_device	*xdev;
	struct regmap		*regmap;
	struct regmap		*uuid_regmap;
	u32			length;

	int			group;
};

static inline int vsec_read_entry(struct xrt_vsec *vsec, u32 index, struct xrt_vsec_entry *entry)
{
	int ret;

	ret = regmap_bulk_read(vsec->regmap, sizeof(struct xrt_vsec_header) +
			       index * sizeof(struct xrt_vsec_entry), entry,
			       sizeof(struct xrt_vsec_entry) /
			       vsec_regmap_config.reg_stride);

	return ret;
}

static inline u32 vsec_get_bar(struct xrt_vsec_entry *entry)
{
	return (entry->bar_rev >> 4) & 0xf;
}

static inline u64 vsec_get_bar_off(struct xrt_vsec_entry *entry)
{
	return entry->off_lo | ((u64)entry->off_hi << 16);
}

static inline u32 vsec_get_rev(struct xrt_vsec_entry *entry)
{
	return entry->bar_rev & 0xf;
}

static int xrt_vsec_leaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	int ret = 0;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	default:
		ret = -EINVAL;
		xrt_err(xdev, "should never been called");
		break;
	}

	return ret;
}

static int xrt_vsec_create_regmap(struct xrt_vsec *vsec, u32 bar_idx, u64 bar_off,
				  struct regmap **regmap)
{
	struct resource *res = NULL;
	void __iomem *base = NULL;

	xleaf_get_root_res(vsec->xdev, bar_idx, &res);
	if (!res) {
		xrt_err(vsec->xdev, "failed to get bar addr");
		return -EINVAL;
	}

	base = devm_ioremap(&vsec->xdev->dev, res->start + bar_off,
			    vsec_regmap_config.max_register);
	if (!base) {
		xrt_err(vsec->xdev, "Map failed");
		return -EIO;
	}

	*regmap = devm_regmap_init_mmio(&vsec->xdev->dev, base, &vsec_regmap_config);
	if (IS_ERR(*regmap)) {
		xrt_err(vsec->xdev, "regmap %pR failed", res);
		return PTR_ERR(*regmap);
	}

	return 0;
}

static int xrt_vsec_create_metadata(struct xrt_vsec *vsec, void **metadata)
{
	char uuid_buf[UUID_STRING_LEN];
	const struct firmware *fw;
	char fw_name[256];
	char *dtb = NULL;
	void *md = NULL;
	uuid_t uuid;
	u32 len;
	int ret;

	ret = regmap_bulk_read(vsec->uuid_regmap, 0, uuid_buf,
			       UUID_SIZE / vsec_regmap_config.reg_stride);
	if (ret) {
		xrt_err(vsec->xdev, "failed to read uuid %d", ret);
		return ret;
	}
	import_uuid(&uuid, uuid_buf);
	xrt_md_trans_uuid2str(&uuid, uuid_buf);

	snprintf(fw_name, sizeof(fw_name), "xilinx/%s/partition.xsabin", uuid_buf);
	xrt_info(vsec->xdev, "try loading fw: %s", fw_name);

	ret = request_firmware(&fw, fw_name, DEV(vsec->xdev));
	if (ret)
		return ret;

	ret = xrt_xclbin_get_metadata(DEV(vsec->xdev), (struct axlf *)fw->data, &dtb, &len);
	if (ret)
		goto failed;

	ret = xrt_md_create(DEV(vsec->xdev), 1, fw->size, &md);
	if (ret)
		goto failed;

	ret = xrt_md_add_endpoint(DEV(vsec->xdev), md, XRT_MD_NODE_DTB);
	if (ret)
		goto failed;

	ret = xrt_md_set_prop(DEV(vsec->xdev), md, XRT_MD_NODE_DTB, XRT_MD_PROP_PRIV_DATA,
			      (u64)dtb, len);
	if (ret)
		goto failed;

	release_firmware(fw);
	*metadata = md;

	return 0;
failed:
	release_firmware(fw);
	vfree(md);

	return ret;
}

static int xrt_vsec_mapio(struct xrt_vsec *vsec)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(vsec->xdev);
	struct xrt_vsec_priv_data *pd;
	struct xrt_vsec_entry entry;
	u32 len, bar_idx;
	u64 bar_off;
	int ret, i;

	if (!pdata || !xrt_md_size(pdata->xsp_data)) {
		xrt_err(vsec->xdev, "empty metadata");
		return -EINVAL;
	}

	ret = xrt_md_get_prop(DEV(vsec->xdev), pdata->xsp_data, XRT_MD_NODE_VSEC,
			      XRT_MD_PROP_PRIV_DATA, (u64 *)&pd, &len);
	if (ret) {
		xrt_err(vsec->xdev, "failed to get bar idx, ret %d", ret);
		return -EINVAL;
	}
	if (len != sizeof(*pd)) {
		xrt_err(vsec->xdev, "invalid private data");
		return -EINVAL;
	}

	bar_off = VSEC_BAR_OFFSET(pd);
	bar_idx = VSEC_BAR_INDEX(pd);

	xrt_info(vsec->xdev, "Map vsec at bar %d, offset 0x%llx", bar_idx, bar_off);
	ret = xrt_vsec_create_regmap(vsec, bar_idx, bar_off, &vsec->regmap);
	if (ret) {
		xrt_err(vsec->xdev, "failed to create regmap %d", ret);
		return ret;
	}

	ret = regmap_read(vsec->regmap, VSEC_REG_LENGTH, &vsec->length);
	if (ret) {
		xrt_err(vsec->xdev, "failed to read length %d", ret);
		return ret;
	}

	/* Map and read base partition UUID */
	for (i = 0; i * sizeof(entry) < vsec->length -
	    sizeof(struct xrt_vsec_header); i++) {
		ret = vsec_read_entry(vsec, i, &entry);
		if (ret) {
			xrt_err(vsec->xdev, "failed read entry %d, ret %d", i, ret);
			return ret;
		}
		if (entry.type == VSEC_TYPE_UUID || entry.type == VSEC_TYPE_END)
			break;
	}
	if (entry.type != VSEC_TYPE_UUID) {
		xrt_err(vsec->xdev, "Did not get uuid");
		return -EINVAL;
	}

	bar_idx = vsec_get_bar(&entry);
        bar_off = vsec_get_bar_off(&entry);
	ret = xrt_vsec_create_regmap(vsec, bar_idx, bar_off, &vsec->uuid_regmap);
	if (ret) {
		xrt_err(vsec->xdev, "failed to create uuid regmap %d", ret);
		return ret;
	}

	return 0;
}

static void xrt_vsec_remove(struct xrt_device *xdev)
{
	struct xrt_vsec	*vsec;

	vsec = xrt_get_drvdata(xdev);

	if (vsec->group >= 0)
		xleaf_destroy_group(xdev, vsec->group);
}

static int xrt_vsec_probe(struct xrt_device *xdev)
{
	struct xrt_vsec	*vsec;
	void *metadata = NULL;
	int ret = 0;

	vsec = devm_kzalloc(&xdev->dev, sizeof(*vsec), GFP_KERNEL);
	if (!vsec)
		return -ENOMEM;

	vsec->xdev = xdev;
	vsec->group = -1;
	xrt_set_drvdata(xdev, vsec);

	ret = xrt_vsec_mapio(vsec);
	if (ret)
		goto failed;

	ret = xrt_vsec_create_metadata(vsec, &metadata);
	if (ret) {
		xrt_err(xdev, "create metadata failed, ret %d", ret);
		goto failed;
	}

	ret = xleaf_create_group(xdev, metadata);
	if (ret < 0) {
		xrt_err(xdev, "create group failed, ret %d", vsec->group);
		goto failed;
	}
	vfree(metadata);
	vsec->group = ret;

	return 0;

failed:
	vfree(metadata);
	xrt_vsec_remove(xdev);

	return ret;
}

static const struct xrt_device_id xrt_vsec_ids[] = {
	{ XRT_SUBDEV_VSEC },
	{ }
};
MODULE_DEVICE_TABLE(xrt, xrt_vsec_ids);

static struct xrt_driver xrt_vsec_driver = {
	.driver = {
		.name = XRT_VSEC,
		.owner = THIS_MODULE,
	},
	.id_table = xrt_vsec_ids,
	.subdev_id = XRT_SUBDEV_VSEC,
	.probe = xrt_vsec_probe,
	.remove = xrt_vsec_remove,
	.leaf_call = xrt_vsec_leaf_call,
};
module_xrt_driver(xrt_vsec_driver);

MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx XRT VSEC driver");
MODULE_LICENSE("GPL v2");
