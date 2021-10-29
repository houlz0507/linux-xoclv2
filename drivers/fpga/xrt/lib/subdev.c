// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 *	Lizhi Hou <lizhih@xilinx.com>
 */

// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/xrt/xleaf.h>
#include <linux/xrt/metadata.h>
#include "subdev_pool.h"
#include "lib-drv.h"

extern struct bus_type xrt_bus_type;

#define IS_ROOT_DEV(dev)	((dev)->bus != &xrt_bus_type)
#define XRT_HOLDER_BUF_SZ	1024

static inline struct device *find_root(struct xrt_device *xdev)
{
	struct device *d = DEV(xdev);

	while (!IS_ROOT_DEV(d))
		d = d->parent;
	return d;
}

/*
 * It represents a holder of a subdev. One holder can repeatedly hold a subdev
 * as long as there is a unhold corresponding to a hold.
 */
struct xrt_subdev_holder {
	struct list_head xsh_holder_list;
	struct device *xsh_holder;
	int xsh_count;
	struct kref xsh_kref;
};

/*
 * It represents a specific instance of platform driver for a subdev, which
 * provides services to its clients (another subdev driver or root driver).
 */
struct xrt_subdev {
	struct list_head xs_dev_list;
	struct list_head xs_holder_list;
	enum xrt_subdev_id xs_id;		/* type of subdev */
	struct xrt_device *xs_xdev;
	struct completion xs_holder_comp;
};

static struct xrt_subdev *xrt_subdev_alloc(void)
{
	struct xrt_subdev *sdev = kzalloc(sizeof(*sdev), GFP_KERNEL);

	if (!sdev)
		return NULL;

	INIT_LIST_HEAD(&sdev->xs_dev_list);
	INIT_LIST_HEAD(&sdev->xs_holder_list);
	init_completion(&sdev->xs_holder_comp);
	return sdev;
}

int xrt_subdev_root_request(struct xrt_device *self, u32 cmd, void *arg)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(self);
	struct device *dev = DEV(self);
	xrt_subdev_root_cb_t root_cb;

	if (!pdata->xsp_root_cb) {
		dev_err(dev, "invalid root callback");
		return -EINVAL;
	}
	root_cb = pdata->xsp_root_cb;
	return (*root_cb)(dev->parent, pdata->xsp_root_cb_arg, cmd, arg);
}

static int
xrt_subdev_getres(struct device *parent, void *md, struct resource **res, int *res_num)
{
	struct xrt_subdev_platdata *pdata;
	struct resource *pci_res = NULL;
	int count1 = 0, count2 = 0, ret;
	u64 bar_off, reg_sz, bar_idx;
	const char *ep_name = NULL;

	if (!md)
		return -EINVAL;

	pdata = DEV_PDATA(to_xrt_dev(parent));

	/* go through metadata and count endpoints in it */
	xrt_md_get_next_endpoint(parent, md, NULL, &ep_name);
	while (ep_name) {
		ret = xrt_md_get_prop(parent, md, ep_name, XRT_MD_PROP_REG_BAR_OFF,
				      &bar_off, 0);
		if (!ret)
			count1++;
		xrt_md_get_next_endpoint(parent, md, ep_name, &ep_name);
	}
	if (!count1)
		return 0;

	*res = vzalloc(sizeof(**res) * count1);
	if (!*res)
		return -ENOMEM;

	ep_name = NULL;
	xrt_md_get_next_endpoint(parent, md, NULL, &ep_name);
	while (ep_name) {
		ret = xrt_md_get_prop(parent, md, ep_name, XRT_MD_PROP_REG_BAR_OFF,
				      &bar_off, 0);
		if (ret)
			continue;
		ret = xrt_md_get_prop(parent, md, ep_name, XRT_MD_PROP_REG_SIZE,
				      &reg_sz, 0);
		if (ret) {
			dev_err(parent, "Can not get reg size for %s", ep_name);
			goto failed;
		}
		ret = xrt_md_get_prop(parent, md, ep_name, XRT_MD_PROP_REG_BAR_IDX,
				      &bar_idx, 0);
		bar_idx = ret ? 0 : bar_idx;
		xleaf_get_root_res(to_xrt_dev(parent), bar_idx, &pci_res);
		if (!pci_res)
			continue;

		(*res)[count2].start = pci_res->start + bar_off;
		(*res)[count2].end = pci_res->start + reg_sz - 1;
		(*res)[count2].flags = IORESOURCE_MEM;
		(*res)[count2].name = ep_name;
		(*res)[count2].parent = pci_res;
		/* check if there is conflicted resource */
		ret = request_resource(pci_res, *res + count2);
		if (ret) {
			dev_err(parent, "Conflict resource %pR\n", *res + count2);
			goto failed;
		}
		release_resource(*res + count2);

		count2++;
		xrt_md_get_next_endpoint(parent, md, ep_name, &ep_name);
	}

	WARN_ON(count1 != count2);
	*res_num = count2;

	return 0;

failed:
	vfree(*res);
	*res_num = 0;
	*res = NULL;
	return ret;
}

static struct xrt_subdev *
xrt_subdev_create(struct device *parent, enum xrt_subdev_id id,
		  xrt_subdev_root_cb_t pcb, void *pcb_arg, void *md)
{
	struct xrt_subdev_platdata *pdata = NULL;
	struct xrt_subdev *sdev = NULL;
	struct xrt_device *xdev = NULL;
	struct resource *res = NULL;
	int res_num = 0, ret;
	size_t pdata_sz;
	u32 md_len = 0;

	sdev = xrt_subdev_alloc();
	if (!sdev) {
		dev_err(parent, "failed to alloc subdev for ID %d", id);
		return NULL;
	}
	sdev->xs_id = id;

	if (md)
		md_len = xrt_md_size(md);

	pdata_sz = sizeof(struct xrt_subdev_platdata) + md_len;
	pdata = vzalloc(pdata_sz);
	if (!pdata)
		goto fail1;

	pdata->xsp_root_cb = pcb;
	pdata->xsp_root_cb_arg = pcb_arg;
	if (md_len)
		memcpy(pdata->xsp_data, md, md_len);
	if (id == XRT_SUBDEV_GRP) {
		/* Group can only be created by root driver. */
		pdata->xsp_root_name = dev_name(parent);
	} else {
		struct xrt_device *grp = to_xrt_dev(parent);

		/* Leaf can only be created by group driver. */
		WARN_ON(to_xrt_drv(parent->driver)->subdev_id != XRT_SUBDEV_GRP);
		pdata->xsp_root_name = DEV_PDATA(grp)->xsp_root_name;
	}
	
	/* Create subdev. */
	if (id != XRT_SUBDEV_GRP) {
		int rc = xrt_subdev_getres(parent, md, &res, &res_num);

		if (rc) {
			dev_err(parent, "failed to get resource for %s: %d",
				xrt_drv_name(id), rc);
			goto fail2;
		}
	}

	ret = xrt_drv_get(id);
	if (ret) {
		xrt_err(xdev, "failed to load driver module for dev %d", id);
		goto fail2;
	}

	xdev = xrt_device_register(parent, id, res, res_num, pdata, pdata_sz);
	vfree(res);
	if (!xdev) {
		dev_err(parent, "failed to create subdev for %s", xrt_drv_name(id));
		goto fail3;
	}
	sdev->xs_xdev = xdev;

	if (device_attach(DEV(xdev)) != 1) {
		xrt_err(xdev, "failed to attach");
		goto fail4;
	}
	
	vfree(pdata);
	return sdev;

fail4:
	xrt_device_unregister(sdev->xs_xdev);
fail3:
	xrt_drv_put(id);
fail2:
	vfree(pdata);
fail1:
	kfree(sdev);
	return NULL;
}

static void xrt_subdev_destroy(struct xrt_subdev *sdev)
{
	struct xrt_device *xdev = sdev->xs_xdev;

	xrt_device_unregister(xdev);
	xrt_drv_put(sdev->xs_id);
	kfree(sdev);
}

static ssize_t
xrt_subdev_get_holders(struct xrt_subdev *sdev, char *buf, size_t len)
{
	const struct list_head *ptr;
	struct xrt_subdev_holder *h;
	ssize_t n = 0;

	list_for_each(ptr, &sdev->xs_holder_list) {
		h = list_entry(ptr, struct xrt_subdev_holder, xsh_holder_list);
		n += snprintf(buf + n, len - n, "%s:%d ",
			      dev_name(h->xsh_holder), kref_read(&h->xsh_kref));
		/* Truncation is fine here. Buffer content is only for debugging. */
		if (n >= (len - 1))
			break;
	}
	return n;
}

void xrt_subdev_pool_init(struct device *dev, struct xrt_subdev_pool *spool)
{
	INIT_LIST_HEAD(&spool->xsp_dev_list);
	spool->xsp_owner = dev;
	mutex_init(&spool->xsp_lock);
	spool->xsp_closing = false;
}

static void xrt_subdev_free_holder(struct xrt_subdev_holder *holder)
{
	list_del(&holder->xsh_holder_list);
	vfree(holder);
}

static void xrt_subdev_pool_wait_for_holders(struct xrt_subdev_pool *spool, struct xrt_subdev *sdev)
{
	const struct list_head *ptr, *next;
	char holders[128];
	struct xrt_subdev_holder *holder;
	struct mutex *lk = &spool->xsp_lock;

	while (!list_empty(&sdev->xs_holder_list)) {
		int rc;

		/* It's most likely a bug if we ever enters this loop. */
		xrt_subdev_get_holders(sdev, holders, sizeof(holders));
		xrt_err(sdev->xs_xdev, "awaits holders: %s", holders);
		mutex_unlock(lk);
		rc = wait_for_completion_killable(&sdev->xs_holder_comp);
		mutex_lock(lk);
		if (rc == -ERESTARTSYS) {
			xrt_err(sdev->xs_xdev, "give up on waiting for holders, clean up now");
			list_for_each_safe(ptr, next, &sdev->xs_holder_list) {
				holder = list_entry(ptr, struct xrt_subdev_holder, xsh_holder_list);
				xrt_subdev_free_holder(holder);
			}
		}
	}
}

void xrt_subdev_pool_fini(struct xrt_subdev_pool *spool)
{
	struct list_head *dl = &spool->xsp_dev_list;
	struct mutex *lk = &spool->xsp_lock;

	mutex_lock(lk);
	if (spool->xsp_closing) {
		mutex_unlock(lk);
		return;
	}
	spool->xsp_closing = true;
	mutex_unlock(lk);

	/* Remove subdev in the reverse order of added. */
	while (!list_empty(dl)) {
		struct xrt_subdev *sdev = list_first_entry(dl, struct xrt_subdev, xs_dev_list);

		xrt_subdev_pool_wait_for_holders(spool, sdev);
		list_del(&sdev->xs_dev_list);
		xrt_subdev_destroy(sdev);
	}
}

static struct xrt_subdev_holder *xrt_subdev_find_holder(struct xrt_subdev *sdev,
							struct device *holder_dev)
{
	struct list_head *hl = &sdev->xs_holder_list;
	struct xrt_subdev_holder *holder;
	const struct list_head *ptr;

	list_for_each(ptr, hl) {
		holder = list_entry(ptr, struct xrt_subdev_holder, xsh_holder_list);
		if (holder->xsh_holder == holder_dev)
			return holder;
	}
	return NULL;
}

static int xrt_subdev_hold(struct xrt_subdev *sdev, struct device *holder_dev)
{
	struct xrt_subdev_holder *holder = xrt_subdev_find_holder(sdev, holder_dev);
	struct list_head *hl = &sdev->xs_holder_list;

	if (!holder) {
		holder = vzalloc(sizeof(*holder));
		if (!holder)
			return -ENOMEM;
		holder->xsh_holder = holder_dev;
		kref_init(&holder->xsh_kref);
		list_add_tail(&holder->xsh_holder_list, hl);
	} else {
		kref_get(&holder->xsh_kref);
	}

	return 0;
}

static void xrt_subdev_free_holder_kref(struct kref *kref)
{
	struct xrt_subdev_holder *holder = container_of(kref, struct xrt_subdev_holder, xsh_kref);

	xrt_subdev_free_holder(holder);
}

static int
xrt_subdev_release(struct xrt_subdev *sdev, struct device *holder_dev)
{
	struct xrt_subdev_holder *holder = xrt_subdev_find_holder(sdev, holder_dev);
	struct list_head *hl = &sdev->xs_holder_list;

	if (!holder) {
		dev_err(holder_dev, "can't release, %s did not hold %s",
			dev_name(holder_dev), dev_name(DEV(sdev->xs_xdev)));
		return -EINVAL;
	}
	kref_put(&holder->xsh_kref, xrt_subdev_free_holder_kref);

	/* kref_put above may remove holder from list. */
	if (list_empty(hl))
		complete(&sdev->xs_holder_comp);
	return 0;
}

int xrt_subdev_pool_add(struct xrt_subdev_pool *spool, enum xrt_subdev_id id,
			xrt_subdev_root_cb_t pcb, void *pcb_arg, void *md)
{
	struct list_head *dl = &spool->xsp_dev_list;
	struct mutex *lk = &spool->xsp_lock;
	struct xrt_subdev *sdev;
	int ret = 0;

	sdev = xrt_subdev_create(spool->xsp_owner, id, pcb, pcb_arg, md);
	if (sdev) {
		mutex_lock(lk);
		if (spool->xsp_closing) {
			/* No new subdev when pool is going away. */
			xrt_err(sdev->xs_xdev, "pool is closing");
			ret = -ENODEV;
		} else {
			list_add(&sdev->xs_dev_list, dl);
		}
		mutex_unlock(lk);
		if (ret)
			xrt_subdev_destroy(sdev);
	} else {
		ret = -EINVAL;
	}

	ret = ret ? ret : sdev->xs_xdev->instance;
	return ret;
}


int xrt_subdev_pool_del(struct xrt_subdev_pool *spool, enum xrt_subdev_id id, int instance)
{
	struct list_head *dl = &spool->xsp_dev_list;
	struct mutex *lk = &spool->xsp_lock;
	const struct list_head *ptr;
	struct xrt_subdev *sdev;
	int ret = -ENOENT;

	mutex_lock(lk);
	if (spool->xsp_closing) {
		/* Pool is going away, all subdevs will be gone. */
		mutex_unlock(lk);
		return 0;
	}
	list_for_each(ptr, dl) {
		sdev = list_entry(ptr, struct xrt_subdev, xs_dev_list);
		if (sdev->xs_id != id || sdev->xs_xdev->instance != instance)
			continue;
		xrt_subdev_pool_wait_for_holders(spool, sdev);
		list_del(&sdev->xs_dev_list);
		ret = 0;
		break;
	}
	mutex_unlock(lk);
	if (ret)
		return ret;

	xrt_subdev_destroy(sdev);
	return 0;
}

static int xrt_subdev_pool_get_impl(struct xrt_subdev_pool *spool, xrt_subdev_match_t match,
				    void *arg, struct device *holder_dev, struct xrt_subdev **sdevp)
{
	struct xrt_device *xdev = (struct xrt_device *)arg;
	struct list_head *dl = &spool->xsp_dev_list;
	struct mutex *lk = &spool->xsp_lock;
	struct xrt_subdev *sdev = NULL;
	const struct list_head *ptr;
	struct xrt_subdev *d = NULL;
	int ret = -ENOENT;

	mutex_lock(lk);

	if (!xdev) {
		if (match == XRT_SUBDEV_MATCH_PREV) {
			sdev = list_empty(dl) ? NULL :
				list_last_entry(dl, struct xrt_subdev, xs_dev_list);
		} else if (match == XRT_SUBDEV_MATCH_NEXT) {
			sdev = list_first_entry_or_null(dl, struct xrt_subdev, xs_dev_list);
		}
	}

	list_for_each(ptr, dl) {
		d = list_entry(ptr, struct xrt_subdev, xs_dev_list);
		if (match == XRT_SUBDEV_MATCH_PREV || match == XRT_SUBDEV_MATCH_NEXT) {
			if (d->xs_xdev != xdev)
				continue;
		} else {
			if (!match(d->xs_id, d->xs_xdev, arg))
				continue;
		}

		if (match == XRT_SUBDEV_MATCH_PREV)
			sdev = !list_is_first(ptr, dl) ? list_prev_entry(d, xs_dev_list) : NULL;
		else if (match == XRT_SUBDEV_MATCH_NEXT)
			sdev = !list_is_last(ptr, dl) ? list_next_entry(d, xs_dev_list) : NULL;
		else
			sdev = d;
	}

	if (sdev)
		ret = xrt_subdev_hold(sdev, holder_dev);

	mutex_unlock(lk);

	if (!ret)
		*sdevp = sdev;
	return ret;
}

int xrt_subdev_pool_get(struct xrt_subdev_pool *spool, xrt_subdev_match_t match, void *arg,
			struct device *holder_dev, struct xrt_device **xdevp)
{
	int rc;
	struct xrt_subdev *sdev;

	rc = xrt_subdev_pool_get_impl(spool, match, arg, holder_dev, &sdev);
	if (rc) {
		if (rc != -ENOENT)
			dev_err(holder_dev, "failed to hold device: %d", rc);
		return rc;
	}

	if (!IS_ROOT_DEV(holder_dev)) {
		xrt_dbg(to_xrt_dev(holder_dev), "%s <<==== %s",
			dev_name(holder_dev), dev_name(DEV(sdev->xs_xdev)));
	}

	*xdevp = sdev->xs_xdev;
	return 0;
}

static int xrt_subdev_pool_put_impl(struct xrt_subdev_pool *spool, struct xrt_device *xdev,
				    struct device *holder_dev)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xsp_lock;
	struct list_head *dl = &spool->xsp_dev_list;
	struct xrt_subdev *sdev;
	int ret = -ENOENT;

	mutex_lock(lk);
	list_for_each(ptr, dl) {
		sdev = list_entry(ptr, struct xrt_subdev, xs_dev_list);
		if (sdev->xs_xdev != xdev)
			continue;
		ret = xrt_subdev_release(sdev, holder_dev);
		break;
	}
	mutex_unlock(lk);

	return ret;
}

int xrt_subdev_pool_put(struct xrt_subdev_pool *spool, struct xrt_device *xdev,
			struct device *holder_dev)
{
	int ret = xrt_subdev_pool_put_impl(spool, xdev, holder_dev);

	if (ret)
		return ret;

	if (!IS_ROOT_DEV(holder_dev)) {
		xrt_dbg(to_xrt_dev(holder_dev), "%s <<==X== %s",
			dev_name(holder_dev), dev_name(DEV(xdev)));
	}
	return 0;
}

void xrt_subdev_pool_trigger_event(struct xrt_subdev_pool *spool, enum xrt_events e)
{
	/* place holder */
}

void xrt_subdev_pool_handle_event(struct xrt_subdev_pool *spool, struct xrt_event *evt)
{
	/* place holder */
}

ssize_t xrt_subdev_pool_get_holders(struct xrt_subdev_pool *spool,
				    struct xrt_device *xdev, char *buf, size_t len)
{
	const struct list_head *ptr;
	struct mutex *lk = &spool->xsp_lock;
	struct list_head *dl = &spool->xsp_dev_list;
	struct xrt_subdev *sdev;
	ssize_t ret = 0;

	mutex_lock(lk);
	list_for_each(ptr, dl) {
		sdev = list_entry(ptr, struct xrt_subdev, xs_dev_list);
		if (sdev->xs_xdev != xdev)
			continue;
		ret = xrt_subdev_get_holders(sdev, buf, len);
		break;
	}
	mutex_unlock(lk);

	return ret;
}
EXPORT_SYMBOL_GPL(xrt_subdev_pool_get_holders);

void xleaf_get_root_res(struct xrt_device *xdev, u32 region_id, struct resource **res)
{
	struct xrt_root_get_res arg = { 0 };

	arg.xpigr_region_id = region_id;
	xrt_subdev_root_request(xdev, XRT_ROOT_GET_RESOURCE, &arg);
	*res = arg.xpigr_res;
}
EXPORT_SYMBOL_GPL(xleaf_get_root_res);
