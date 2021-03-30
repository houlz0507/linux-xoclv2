/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *    Lizhi Hou <lizhi.hou@xilinx.com>
 */

#ifndef _XRT_DEVICE_H_
#define _XRT_DEVICE_H_

#define XRT_MAX_DEVICE_NODES		128
#define XRT_INVALID_DEVICE_INST		(XRT_MAX_DEVICE_NODES + 1)

enum {
	XRT_DEVICE_STATE_NONE = 0,
	XRT_DEVICE_STATE_ADDED
};

/*
 * struct xrt_device - represent an xrt device on xrt bus
 *
 * dev: generic device interface.
 * id: id of the xrt device.
 * id_entry: matched id entry in xrt driver's id table.
 */
struct xrt_device {
	struct device dev;
	u32 subdev_id;
	const char *name;
	u32 instance;
	const struct xrt_device_id *id_entry;
	u32 state;
	u32 num_resources;
	struct resource *resource;
	void *sdev_data;
};

/*
 * struct xrt_driver - represent a xrt device driver
 *
 * drv: driver model structure.
 * id_table: pointer to table of device IDs the driver is interested in.
 *           { } member terminated.
 * probe: mandatory callback for device binding.
 * remove: callback for device unbinding.
 */
struct xrt_driver {
	struct device_driver driver;
	const struct xrt_device_id *id_table;

	int (*probe)(struct xrt_device *xrt_dev);
	void (*remove)(struct xrt_device *xrt_dev);
};

#define to_xrt_dev(d) container_of(d, struct xrt_device, dev)
#define to_xrt_drv(d) container_of(d, struct xrt_driver, driver)

#define xrt_get_device_id(xdev)	((xdev)->id_entry)

static inline void *xrt_get_drvdata(const struct xrt_device *xdev)
{
	return dev_get_drvdata(&xdev->dev);
}

static inline void xrt_set_drvdata(struct xrt_device *xdev, void *data)
{
	dev_set_drvdata(&xdev->dev, data);
}

static inline void *xrt_get_xdev_data(struct device *dev)
{
	struct xrt_device *xdev = to_xrt_dev(dev);

	return xdev->sdev_data;
}

struct xrt_device *
xrt_device_register(struct device *parent, u32 id,
		    struct resource *res, u32 res_num,
		    void *pdata, size_t data_sz);
void xrt_device_unregister(struct xrt_device *xdev);
int xrt_driver_register(struct xrt_driver *drv);
void xrt_driver_unregister(struct xrt_driver *drv);
void *xrt_get_xdev_data(struct device *dev);
struct resource *xrt_get_resource(struct xrt_device *xdev, u32 type, u32 num);

#endif /* _XRT_DEVICE_H_ */
