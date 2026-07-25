/*
 * Copyright 2015-2018 François Tigeot <ftigeot@wolfpond.org>
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/device.h>

#include <sys/bus.h>

#include <drm/drm_sysfs.h>
#include <drm/drmP.h>
#include "drm_internal.h"

/*
 * Post a driver event to userland.  kobject_uevent_env() used to swallow
 * these; devctl is what DragonFly listens on.
 */
void
drm_sysfs_driver_event(struct drm_device *dev, const char *event,
    const char *data)
{
	char subsystem[16];

	if (dev == NULL || dev->primary == NULL)
		return;

	ksnprintf(subsystem, sizeof(subsystem), "card%d", dev->primary->index);
	devctl_notify("DRM", subsystem, event, data);
}

void drm_sysfs_hotplug_event(struct drm_device *dev)
{
	char data[96];
	char subsystem[16];
	int card_index;
	int render_index;

	if (dev == NULL || dev->primary == NULL)
		return;

	/*
	 * Ownership: drm core owns dev and its minors; this function only borrows
	 * them long enough to build one devctl payload.
	 * Lifetime: devctl_notify() copies the formatted payload before this
	 * function returns, so no pointer into dev or stack storage escapes.
	 * Threading: callers must already be in process context through
	 * drm_kms_helper_hotplug_event(); no modeset or device locks are taken
	 * here.
	 */
	card_index = dev->primary->index;
	render_index = dev->render != NULL ? dev->render->index : -1;
	ksnprintf(subsystem, sizeof(subsystem), "card%d", card_index);
	ksnprintf(data, sizeof(data), "HOTPLUG=1 card=%d render=%d",
	    card_index, render_index);
	devctl_notify("DRM", subsystem, "HOTPLUG", data);
}

extern struct dev_ops drm_cdevsw;

struct device *drm_sysfs_minor_alloc(struct drm_minor *minor)
{
	const char dev_str[12];
	struct device *kdev;
	int r;
	struct cdev *devnode;

	if (minor->type == DRM_MINOR_PRIMARY)
		ksnprintf(dev_str, sizeof(dev_str), "card%d", minor->index);
	else if (minor->type == DRM_MINOR_RENDER)
		ksnprintf(dev_str, sizeof(dev_str), "renderD%d", minor->index);
	else
		return NULL;

	kdev = kzalloc(sizeof(*kdev), GFP_KERNEL);
	if (!kdev)
		return ERR_PTR(-ENOMEM);

	devnode = make_dev(&drm_cdevsw, minor->index,
		DRM_DEV_UID, DRM_DEV_GID, DRM_DEV_MODE, "dri/%s", dev_str);
	/* drm_minor_free() destroys this node at device teardown. */
	minor->devnode = devnode;

	kdev->parent = minor->dev->dev;
	dev_set_drvdata(kdev, minor);

	r = dev_set_name(kdev, dev_str);
	if (r < 0)
		goto err_free;

	return kdev;

err_free:
	return ERR_PTR(r);
}
