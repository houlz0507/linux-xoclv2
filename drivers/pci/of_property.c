// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022, Advanced Micro Devices, Inc.
 */

#include <linux/pci.h>
#include <linux/of.h>
#include <linux/bitfield.h>
#include <linux/bits.h>

struct of_pci_prop {
	char *name;
	int (*prop_val)(struct pci_dev *pdev, void **val, u32 *len);
};

struct of_pci_addr_pair {
	__be32		phys_hi;
	__be32		phys_mid;
	__be32		phys_lo;
	__be32		size_hi;
	__be32		size_lo;
};

#define OF_PCI_ADDR_SPACE_CONFIG	0x0
#define OF_PCI_ADDR_SPACE_IO		0x1
#define OF_PCI_ADDR_SPACE_MEM32		0x2
#define OF_PCI_ADDR_SPACE_MEM64		0x3

#define OF_PCI_ADDR_FIELD_SS		GENMASK(25, 24)
#define OF_PCI_ADDR_FIELD_PREFETCH	BIT(30)
#define OF_PCI_ADDR_FIELD_BUS		GENMASK(23, 16)
#define OF_PCI_ADDR_FIELD_DEV		GENMASK(15, 11)
#define OF_PCI_ADDR_FIELD_FUNC		GENMASK(10, 8)
#define OF_PCI_ADDR_FIELD_REG		GENMASK(7, 0)

#define OF_PCI_SIZE_HI			GENMASK_ULL(63, 32)
#define OF_PCI_SIZE_LO			GENMASK_ULL(31, 0)

#define OF_PCI_PROP_COMPAT_LEN_MAX	256
static int of_pci_prop_device_type(struct pci_dev *pdev, void **val, u32 *len)
{
	if (!pci_is_bridge(pdev))
		return 0;

	*val = kasprintf(GFP_KERNEL, "pci");
	if (!*val)
		return -ENOMEM;

	*len = strlen(*val) + 1;

	return 0;
}

static int of_pci_prop_reg(struct pci_dev *pdev, void **val, u32 *len)
{
	struct of_pci_addr_pair *reg;
	u32 reg_val, base_addr, ss;
	resource_size_t sz;
	int i = 1, resno;

	reg = kzalloc(sizeof(*reg) * (PCI_STD_NUM_BARS + 1), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;

	reg_val = FIELD_PREP(OF_PCI_ADDR_FIELD_SS, OF_PCI_ADDR_SPACE_CONFIG) |
		FIELD_PREP(OF_PCI_ADDR_FIELD_BUS, pdev->bus->number) |
		FIELD_PREP(OF_PCI_ADDR_FIELD_DEV, PCI_SLOT(pdev->devfn)) |
		FIELD_PREP(OF_PCI_ADDR_FIELD_FUNC, PCI_FUNC(pdev->devfn));
	reg[0].phys_hi = cpu_to_be32(reg_val);

	base_addr = PCI_BASE_ADDRESS_0;
	for (resno = PCI_STD_RESOURCES; resno <= PCI_STD_RESOURCE_END;
	     resno++, base_addr += 4) {
		sz = pci_resource_len(pdev, resno);
		if (!sz)
			continue;

		if (pci_resource_flags(pdev, resno) & IORESOURCE_IO)
			ss = OF_PCI_ADDR_SPACE_IO;
		else if (pci_resource_flags(pdev, resno) & IORESOURCE_MEM_64)
			ss = OF_PCI_ADDR_SPACE_MEM64;
		else
			ss = OF_PCI_ADDR_SPACE_MEM32;

		reg_val &= ~(OF_PCI_ADDR_FIELD_SS | OF_PCI_ADDR_FIELD_PREFETCH |
				OF_PCI_ADDR_FIELD_REG);
		reg_val |= FIELD_PREP(OF_PCI_ADDR_FIELD_SS, ss) |
			FIELD_PREP(OF_PCI_ADDR_FIELD_REG, base_addr);
		if (pci_resource_flags(pdev, resno) & IORESOURCE_PREFETCH)
			reg_val |= OF_PCI_ADDR_FIELD_PREFETCH;
		reg[i].phys_hi = cpu_to_be32(reg_val);
		reg[i].size_hi = cpu_to_be32(FIELD_GET(OF_PCI_SIZE_HI, sz));
		reg[i].size_lo = cpu_to_be32(FIELD_GET(OF_PCI_SIZE_LO, sz));
		i++;
	}

	*val = reg;
	*len = i * sizeof(*reg);

	return 0;
}

static int of_pci_prop_compatible(struct pci_dev *pdev, void **val, u32 *len)
{
	char *compat;

	compat = kzalloc(OF_PCI_PROP_COMPAT_LEN_MAX, GFP_KERNEL);
	if (!compat)
		return -ENOMEM;

	*val = compat;
	if (pdev->subsystem_vendor) {
		compat += sprintf(compat, "pci%x,%x.%x.%x.%x",
				  pdev->vendor, pdev->device,
				  pdev->subsystem_vendor,
				  pdev->subsystem_device,
				  pdev->revision) + 1;
		compat += sprintf(compat, "pci%x,%x.%x.%x",
				  pdev->vendor, pdev->device,
				  pdev->subsystem_vendor,
				  pdev->subsystem_device) + 1;
		compat += sprintf(compat, "pci%x,%x",
				  pdev->subsystem_vendor,
				  pdev->subsystem_device) + 1;
	}
	compat += sprintf(compat, "pci%x,%x.%x",
			  pdev->vendor, pdev->device, pdev->revision) + 1;
	compat += sprintf(compat, "pci%x,%x", pdev->vendor, pdev->device) + 1;
	compat += sprintf(compat, "pciclass,%06x", pdev->class) + 1;
	compat += sprintf(compat, "pciclass,%04x", pdev->class >> 8) + 1;

	*len = (u32)(compat - (char *)*val);

	return 0;
}

struct of_pci_prop of_pci_props[] = {
	{ .name = "device_type", .prop_val = of_pci_prop_device_type },
	{ .name = "reg", .prop_val = of_pci_prop_reg },
	{ .name = "compatible", .prop_val = of_pci_prop_compatible },
	{},
};

struct property *of_pci_props_create(struct pci_dev *pdev)
{
	struct property *props, *pp;
	void *val;
	u32 len;
	int i;

	props = kcalloc(ARRAY_SIZE(of_pci_props), sizeof(*props), GFP_KERNEL);
	if (!props)
		return NULL;

	pp = props;
	for (i = 0; of_pci_props[i].name; i++) {
		len = 0;
		of_pci_props[i].prop_val(pdev, &val, &len);
		if (!len)
			continue;
		props->name = of_pci_props[i].name;
		props->value = val;
		props->length = len;
		props++;
	}

	return pp;
}

void of_pci_props_destroy(struct property *props)
{
	int i;

	for (i = 0; props[i].name; i++)
		kfree(props[i].value);
	kfree(props);
}
