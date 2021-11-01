// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Driver XCLBIN parser
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors: David Zhang <davidzha@xilinx.com>
 */

#include <asm/errno.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <linux/fpga-xrt.h>

/* Used for parsing bitstream header */
#define BITSTREAM_EVEN_MAGIC_BYTE	0x0f
#define BITSTREAM_ODD_MAGIC_BYTE	0xf0

static inline u16 bitstream_read16(const char *data, u32 *offset)
{
	u16 val;

	val = be16_to_cpu(*(__be16 *)(data + *offset));
	*offset += sizeof(__be16);

	return val;
}

static inline u32 bitstream_read32(const char *data, u32 *offset)
{
	u32 val;

	val = be32_to_cpu(*(__be32 *)(data + *offset));
	*offset += sizeof(__be32);

	return val;
}

static int xrt_xclbin_get_section_hdr(const struct axlf *xclbin,
				      enum axlf_section_kind kind,
				      const struct axlf_section_header **header)
{
	const struct axlf_section_header *phead = NULL;
	u64 xclbin_len;
	int i;

	*header = NULL;
	for (i = 0; i < xclbin->header.num_sections; i++) {
		if (xclbin->sections[i].section_kind == kind) {
			phead = &xclbin->sections[i];
			break;
		}
	}

	if (!phead)
		return -ENOENT;

	xclbin_len = xclbin->header.length;
	if (xclbin_len > XCLBIN_MAX_SZ_1G || !phead->section_size ||
	    phead->section_offset + phead->section_size > xclbin_len)
		return -EINVAL;

	*header = phead;
	return 0;
}

static int xrt_xclbin_section_info(const struct axlf *xclbin,
				   enum axlf_section_kind kind,
				   u64 *offset, u64 *size)
{
	const struct axlf_section_header *mem_header = NULL;
	int rc;

	rc = xrt_xclbin_get_section_hdr(xclbin, kind, &mem_header);
	if (rc)
		return rc;

	*offset = mem_header->section_offset;
	*size = mem_header->section_size;

	return 0;
}

/* caller must free the allocated memory for **data */
int xrt_xclbin_get_section(struct device *dev,
			   const struct axlf *buf,
			   enum axlf_section_kind kind,
			   void **data, u64 *len)
{
	const struct axlf *xclbin = (const struct axlf *)buf;
	void *section = NULL;
	u64 offset;
	u64 size;
	int err;

	if (!data) {
		dev_err(dev, "invalid data pointer");
		return -EINVAL;
	}

	err = xrt_xclbin_section_info(xclbin, kind, &offset, &size);
	if (err) {
		dev_dbg(dev, "parsing section failed. kind %d, err = %d", kind, err);
		return err;
	}

	section = vzalloc(size);
	if (!section)
		return -ENOMEM;

	memcpy(section, ((const char *)xclbin) + offset, size);

	*data = section;
	if (len)
		*len = size;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_xclbin_get_section);

static inline int xclbin_bit_get_string(const unchar *data, u32 size,
					u32 offset, unchar prefix,
					const unchar **str)
{
	int len;
	u32 tmp;

	/* prefix and length will be 3 bytes */
	if (offset + 3  > size)
		return -EINVAL;

	/* Read prefix */
	tmp = data[offset++];
	if (tmp != prefix)
		return -EINVAL;

	/* Get string length */
	len = bitstream_read16(data, &offset);
	if (offset + len > size)
		return -EINVAL;

	if (data[offset + len - 1] != '\0')
		return -EINVAL;

	*str = data + offset;

	return len + 3;
}

/* parse bitstream header */
int xrt_xclbin_parse_bitstream_header(struct device *dev, const unchar *data,
				      u32 size, struct xclbin_bit_head_info *head_info)
{
	u32 offset = 0;
	int len, i;
	u16 magic;

	memset(head_info, 0, sizeof(*head_info));

	/* Get "Magic" length */
	if (size < sizeof(u16)) {
		dev_err(dev, "invalid size");
		return -EINVAL;
	}

	len = bitstream_read16(data, &offset);
	if (offset + len > size) {
		dev_err(dev, "invalid magic len");
		return -EINVAL;
	}
	head_info->magic_length = len;

	for (i = 0; i < head_info->magic_length - 1; i++) {
		magic = data[offset++];
		if (!(i % 2) && magic != BITSTREAM_EVEN_MAGIC_BYTE) {
			dev_err(dev, "invalid magic even byte at %d", offset);
			return -EINVAL;
		}

		if ((i % 2) && magic != BITSTREAM_ODD_MAGIC_BYTE) {
			dev_err(dev, "invalid magic odd byte at %d", offset);
			return -EINVAL;
		}
	}

	if (offset + 3 > size) {
		dev_err(dev, "invalid length of magic end");
		return -EINVAL;
	}
	/* Read null end of magic data. */
	if (data[offset++]) {
		dev_err(dev, "invalid magic end");
		return -EINVAL;
	}

	/* Read 0x01 (short) */
	magic = bitstream_read16(data, &offset);

	/* Check the "0x01" half word */
	if (magic != 0x01) {
		dev_err(dev, "invalid magic end");
		return -EINVAL;
	}

	len = xclbin_bit_get_string(data, size, offset, 'a', &head_info->design_name);
	if (len < 0) {
		dev_err(dev, "get design name failed");
		return -EINVAL;
	}

	head_info->version = strstr(head_info->design_name, "Version=") + strlen("Version=");
	offset += len;

	len = xclbin_bit_get_string(data, size, offset, 'b', &head_info->part_name);
	if (len < 0) {
		dev_err(dev, "get part name failed");
		return -EINVAL;
	}
	offset += len;

	len = xclbin_bit_get_string(data, size, offset, 'c', &head_info->date);
	if (len < 0) {
		dev_err(dev, "get data failed");
		return -EINVAL;
	}
	offset += len;

	len = xclbin_bit_get_string(data, size, offset, 'd', &head_info->time);
	if (len < 0) {
		dev_err(dev, "get time failed");
		return -EINVAL;
	}
	offset += len;

	if (offset + 5 >= size) {
		dev_err(dev, "can not get bitstream length");
		return -EINVAL;
	}

	/* Read 'e' */
	if (data[offset++] != 'e') {
		dev_err(dev, "invalid prefix of bitstream length");
		return -EINVAL;
	}

	/* Get byte length of bitstream */
	head_info->bitstream_length = bitstream_read32(data, &offset);

	head_info->header_length = offset;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_xclbin_parse_bitstream_header);

int xrt_xclbin_get_metadata(struct device *dev, const struct axlf *xclbin, char **dtb,
			    u32 *dtb_len)
{
	char *md = NULL;
	u64 len;
	int rc;

	*dtb = NULL;

	rc = xrt_xclbin_get_section(dev, xclbin, PARTITION_METADATA, (void **)&md, &len);
	if (rc)
		return rc;

	if (len > XRT_MD_MAX_LEN) {
		dev_err(dev, "Invalid dtb len %lld", len);
		return -EINVAL;
	}

	*dtb = md;
	*dtb_len = (u32)len;

	return 0;
}
EXPORT_SYMBOL_GPL(xrt_xclbin_get_metadata);
