// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 *	Lizhi Hou <lizhi.hou@xilinx.com>
 */

#include <linux/module.h>
#include <linux/vmalloc.h>
#include "xleaf.h"
#include "xroot.h"
#include "lib-drv.h"

#define XRT_IPLIB_MODULE_NAME		"xrt-lib"
#define XRT_IPLIB_MODULE_VERSION	"4.0.0"
#define XRT_DRVNAME(drv)		((drv)->driver.name)

/*
 * Subdev driver is known by it's ID to others. We map the ID to it's
 * struct xrt_driver, which contains it's binding name and driver/file ops.
 * We also map it to the endpoint name in DTB as well, if it's different
 * than the driver's binding name.
 */
struct xrt_drv_map {
	struct list_head list;
	enum xrt_subdev_id id;
	struct xrt_driver *drv;
	struct xrt_subdev_endpoints *eps;
	struct ida ida; /* manage driver instance and char dev minor */
};

static DEFINE_MUTEX(xrt_lib_lock); /* global lock protecting xrt_drv_maps list */
static LIST_HEAD(xrt_drv_maps);
struct class *xrt_class;

static struct xrt_drv_map *
__xrt_drv_find_map_by_id(enum xrt_subdev_id id)
{
	struct xrt_drv_map *tmap;

	list_for_each_entry(tmap, &xrt_drv_maps, list) {
		if (tmap->id == id)
			return tmap;
	}
	return NULL;
}

static struct xrt_drv_map *
xrt_drv_find_map_by_id(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map;

	mutex_lock(&xrt_lib_lock);
	map = __xrt_drv_find_map_by_id(id);
	mutex_unlock(&xrt_lib_lock);
	/*
	 * map should remain valid even after the lock is dropped since a registered
	 * driver should only be unregistered when driver module is being unloaded,
	 * which means that the driver should not be used by then.
	 */
	return map;
}

static int xrt_drv_register_driver(struct xrt_drv_map *map)
{
	const char *drvname = XRT_DRVNAME(map->drv);
	int rc = 0;

	rc = xrt_driver_register(map->drv);
	if (rc) {
		pr_err("register %s xrt driver failed\n", drvname);
		return rc;
	}

	/* Initialize dev_t for char dev node. */
	if (map->drv->file_ops.xsf_ops.open) {
		rc = alloc_chrdev_region(&map->drv->file_ops.xsf_dev_t, 0,
					 XRT_MAX_DEVICE_NODES, drvname);
		if (rc) {
			xrt_driver_unregister(map->drv);
			pr_err("failed to alloc dev minor for %s: %d\n", drvname, rc);
			return rc;
		}
	} else {
		map->drv->file_ops.xsf_dev_t = (dev_t)-1;
	}

	ida_init(&map->ida);

	pr_info("%s registered successfully\n", drvname);

	return 0;
}

static void xrt_drv_unregister_driver(struct xrt_drv_map *map)
{
	const char *drvname = XRT_DRVNAME(map->drv);

	ida_destroy(&map->ida);

	if (map->drv->file_ops.xsf_dev_t != (dev_t)-1) {
		unregister_chrdev_region(map->drv->file_ops.xsf_dev_t,
					 XRT_MAX_DEVICE_NODES);
	}

	xrt_driver_unregister(map->drv);

	pr_info("%s unregistered successfully\n", drvname);
}

int xleaf_register_driver(enum xrt_subdev_id id,
			  struct xrt_driver *drv,
			  struct xrt_subdev_endpoints *eps)
{
	struct xrt_drv_map *map;
	int rc;

	mutex_lock(&xrt_lib_lock);

	map = __xrt_drv_find_map_by_id(id);
	if (map) {
		mutex_unlock(&xrt_lib_lock);
		pr_err("Id %d already has a registered driver, 0x%p\n",
		       id, map->drv);
		return -EEXIST;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		mutex_unlock(&xrt_lib_lock);
		return -ENOMEM;
	}
	map->id = id;
	map->drv = drv;
	map->eps = eps;

	rc = xrt_drv_register_driver(map);
	if (rc) {
		kfree(map);
		mutex_unlock(&xrt_lib_lock);
		return rc;
	}

	list_add(&map->list, &xrt_drv_maps);

	mutex_unlock(&xrt_lib_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(xleaf_register_driver);

void xleaf_unregister_driver(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map;

	mutex_lock(&xrt_lib_lock);

	map = __xrt_drv_find_map_by_id(id);
	if (!map) {
		mutex_unlock(&xrt_lib_lock);
		pr_err("Id %d has no registered driver\n", id);
		return;
	}

	list_del(&map->list);

	mutex_unlock(&xrt_lib_lock);

	xrt_drv_unregister_driver(map);
	kfree(map);
}
EXPORT_SYMBOL_GPL(xleaf_unregister_driver);

const char *xrt_drv_name(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);

	if (map)
		return XRT_DRVNAME(map->drv);
	return NULL;
}

int xrt_drv_get_instance(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);

	return ida_alloc_range(&map->ida, 0, XRT_MAX_DEVICE_NODES, GFP_KERNEL);
}

void xrt_drv_put_instance(enum xrt_subdev_id id, int instance)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);

	ida_free(&map->ida, instance);
}

struct xrt_subdev_endpoints *xrt_drv_get_endpoints(enum xrt_subdev_id id)
{
	struct xrt_drv_map *map = xrt_drv_find_map_by_id(id);
	struct xrt_subdev_endpoints *eps;

	eps = map ? map->eps : NULL;
	return eps;
}

static int xrt_bus_match(struct device *dev, struct device_driver *drv)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xrt_driver *xdrv = to_xrt_drv(drv);

	if (xdev->subdev_id == xdrv->subdev_id)
		return 1;

	return 0;
}

static int xrt_bus_probe(struct device *dev)
{
	struct xrt_driver *xdrv = to_xrt_drv(dev->driver);
	struct xrt_device *xdev = to_xrt_dev(dev);

	return xdrv->probe(xdev);
}

static int xrt_bus_remove(struct device *dev)
{
	struct xrt_driver *xdrv = to_xrt_drv(dev->driver);
	struct xrt_device *xdev = to_xrt_dev(dev);

	if (xdrv->remove)
		xdrv->remove(xdev);

	return 0;
}

static struct bus_type xrt_bus_type = {
	.name		= "xrt",
	.match		= xrt_bus_match,
	.probe		= xrt_bus_probe,
	.remove		= xrt_bus_remove,
};

static void xrt_device_release(struct device *dev)
{
	struct xrt_device *xdev = container_of(dev, struct xrt_device, dev);

	kfree(xdev);
}

void xrt_device_unregister(struct xrt_device *xdev)
{
	if (xdev->state == XRT_DEVICE_STATE_ADDED)
		device_del(&xdev->dev);

	vfree(xdev->sdev_data);
	kfree(xdev->resource);

	if (xdev->instance != XRT_INVALID_DEVICE_INST)
		xrt_drv_put_instance(xdev->subdev_id, xdev->instance);

	if (xdev->dev.release == xrt_device_release)
		put_device(&xdev->dev);
}

struct xrt_device *
xrt_device_register(struct device *parent, u32 id,
		    struct resource *res, u32 res_num,
		    void *pdata, size_t data_sz)
{
	struct xrt_device *xdev = NULL;
	int ret;

	xdev = kzalloc(sizeof(*xdev), GFP_KERNEL);
	if (!xdev)
		return NULL;
	xdev->instance = XRT_INVALID_DEVICE_INST;

	/* Obtain dev instance number. */
	ret = xrt_drv_get_instance(id);
	if (ret < 0) {
		dev_err(parent, "failed get instance, ret %d", ret);
		goto fail;
	}

	xdev->instance = ret;
	xdev->name = xrt_drv_name(id);
	xdev->subdev_id = id;
	device_initialize(&xdev->dev);
	xdev->dev.release = xrt_device_release;
	xdev->dev.parent = parent;

	xdev->dev.bus = &xrt_bus_type;
	dev_set_name(&xdev->dev, "%s.%d", xdev->name, xdev->instance);

	xdev->num_resources = res_num;
	xdev->resource = kmemdup(res, sizeof(*res) * res_num, GFP_KERNEL);
	if (!xdev->resource)
		goto fail;

	xdev->sdev_data = vzalloc(data_sz);
	if (!xdev->sdev_data)
		goto fail;

	memcpy(xdev->sdev_data, pdata, data_sz);

	ret = device_add(&xdev->dev);
	if (ret) {
		dev_err(parent, "failed add device, ret %d", ret);
		goto fail;
	}
	xdev->state = XRT_DEVICE_STATE_ADDED;

	return xdev;

fail:
	xrt_device_unregister(xdev);
	kfree(xdev);

	return NULL;
}

int xrt_driver_register(struct xrt_driver *drv)
{
	drv->driver.owner = THIS_MODULE;
	drv->driver.bus = &xrt_bus_type;

	return driver_register(&drv->driver);
}

void xrt_driver_unregister(struct xrt_driver *drv)
{
	driver_unregister(&drv->driver);
}

struct resource *xrt_get_resource(struct xrt_device *xdev, u32 type, u32 num)
{
	u32 i;

	for (i = 0; i < xdev->num_resources; i++) {
		struct resource *r = &xdev->resource[i];

		if (type == resource_type(r) && num-- == 0)
			return r;
	}
	return NULL;
}

/* Leaf driver's module init/fini callbacks. */
static void (*leaf_init_fini_cbs[])(bool) = {
	group_leaf_init_fini,
	vsec_leaf_init_fini,
	devctl_leaf_init_fini,
	axigate_leaf_init_fini,
	icap_leaf_init_fini,
	calib_leaf_init_fini,
	clkfreq_leaf_init_fini,
	clock_leaf_init_fini,
	ucs_leaf_init_fini,
};

static __init int xrt_lib_init(void)
{
	int ret;
	int i;

	ret = bus_register(&xrt_bus_type);
	if (ret)
		return ret;

	xrt_class = class_create(THIS_MODULE, XRT_IPLIB_MODULE_NAME);
	if (IS_ERR(xrt_class)) {
		bus_unregister(&xrt_bus_type);
		return PTR_ERR(xrt_class);
	}

	for (i = 0; i < ARRAY_SIZE(leaf_init_fini_cbs); i++)
		leaf_init_fini_cbs[i](true);
	return 0;
}

static __exit void xrt_lib_fini(void)
{
	struct xrt_drv_map *map;
	int i;

	for (i = 0; i < ARRAY_SIZE(leaf_init_fini_cbs); i++)
		leaf_init_fini_cbs[i](false);

	mutex_lock(&xrt_lib_lock);

	while (!list_empty(&xrt_drv_maps)) {
		map = list_first_entry_or_null(&xrt_drv_maps, struct xrt_drv_map, list);
		pr_err("Unloading module with %s still registered\n", XRT_DRVNAME(map->drv));
		list_del(&map->list);
		mutex_unlock(&xrt_lib_lock);
		xrt_drv_unregister_driver(map);
		kfree(map);
		mutex_lock(&xrt_lib_lock);
	}

	mutex_unlock(&xrt_lib_lock);

	class_destroy(xrt_class);
	bus_unregister(&xrt_bus_type);
}

module_init(xrt_lib_init);
module_exit(xrt_lib_fini);

MODULE_VERSION(XRT_IPLIB_MODULE_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo IP Lib driver");
MODULE_LICENSE("GPL v2");
