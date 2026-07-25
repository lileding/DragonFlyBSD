#ifndef _DRM_SYSFS_H_
#define _DRM_SYSFS_H_

struct drm_device;
struct device;


void drm_sysfs_hotplug_event(struct drm_device *dev);
void drm_sysfs_driver_event(struct drm_device *dev, const char *event,
    const char *data);

#endif
