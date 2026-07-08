/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM registration boundary for the native NVIDIA driver.
 */

#include "nvdrm_drv.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"
#include "nvgpu_unload.h"

#include <drm/drmP.h>
#include <drm/drm_drv.h>
#include <linux/err.h>

#define NVDRM_DRM_NAME		"nouveau"
#define NVDRM_DRM_DESC		"nVidia Riva/TNT/GeForce (dfly native GSP-RM)"
#define NVDRM_DRM_DATE		"20260708"
#define NVDRM_DRM_MAJOR		1
#define NVDRM_DRM_MINOR		3
#define NVDRM_DRM_PATCH		1

static int nvdrm_open(struct drm_device *ddev, struct drm_file *file_priv);
static void nvdrm_postclose(struct drm_device *ddev, struct drm_file *file_priv);
static void nvdrm_lastclose(struct drm_device *ddev);

static const struct file_operations nvdrm_fops = {
	.owner = THIS_MODULE,
};

static struct drm_driver nvdrm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ |
	    DRIVER_SYNCOBJ_TIMELINE | DRIVER_PRIME,
	.fops = &nvdrm_fops,
	.name = NVDRM_DRM_NAME,
	.desc = NVDRM_DRM_DESC,
	.date = NVDRM_DRM_DATE,
	.major = NVDRM_DRM_MAJOR,
	.minor = NVDRM_DRM_MINOR,
	.patchlevel = NVDRM_DRM_PATCH,
	.open = nvdrm_open,
	.postclose = nvdrm_postclose,
	.lastclose = nvdrm_lastclose,
};

static int
nvdrm_open(struct drm_device *ddev, struct drm_file *file_priv)
{
	(void)file_priv;
	return (nvgpu_unload_file_open(ddev->dev_private));
}

static void
nvdrm_postclose(struct drm_device *ddev, struct drm_file *file_priv)
{
	(void)file_priv;
	nvgpu_unload_file_close(ddev->dev_private);
}

static void
nvdrm_lastclose(struct drm_device *ddev)
{
	(void)ddev;
	nvgpu_log(NVGPU_LOG_DEBUG, "lastclose\n");
}

/* Register DRM after GPU boot.  gpu is borrowed; may sleep and must not hold GSP/VM tokens. */
int
nvdrm_register(struct nvgpu_device *gpu)
{
	struct pci_dev *pdev = NULL;
	struct drm_device *ddev;
	int error;

	drm_init_pdev(nvgpu_device_dev(gpu), &pdev);
	if (pdev == NULL) {
		nvgpu_log(NVGPU_LOG_INFO, "drm_init_pdev failed\n");
		return (ENOMEM);
	}

	ddev = drm_dev_alloc(&nvdrm_driver, &pdev->dev);
	if (IS_ERR(ddev)) {
		error = -PTR_ERR(ddev);
		nvgpu_log(NVGPU_LOG_INFO, "drm_dev_alloc failed error=%d\n", error);
		drm_fini_pdev(&pdev);
		return (error);
	}

	ddev->dev_private = gpu;
	ddev->pdev = pdev;
	nvgpu_device_set_drm(gpu, ddev, pdev);

	error = drm_dev_register(ddev, 0);
	if (error != 0) {
		nvgpu_log(NVGPU_LOG_INFO, "drm_dev_register failed error=%d\n", error);
		nvgpu_device_set_drm(gpu, NULL, NULL);
		if (ddev->sysctl != NULL)
			drm_sysctl_cleanup(ddev);
		drm_dev_put(ddev);
		drm_fini_pdev(&pdev);
		return (error);
	}

	nvgpu_log(NVGPU_LOG_INFO, "drm registered as %s\n", NVDRM_DRM_NAME);
	return (0);
}

/* Unregister DRM before backend teardown.  gpu is borrowed; callers must have rejected new users. */
void
nvdrm_unregister(struct nvgpu_device *gpu)
{
	struct drm_device *ddev;
	struct pci_dev *pdev;

	ddev = nvgpu_device_drm_dev(gpu);
	pdev = nvgpu_device_drm_pdev(gpu);
	if (ddev != NULL) {
		drm_dev_unregister(ddev);
		/* drm_dev_fini() does not run DragonFly's per-device sysctl cleanup. */
		if (ddev->sysctl != NULL)
			drm_sysctl_cleanup(ddev);
		drm_dev_put(ddev);
	}
	drm_fini_pdev(&pdev);
	nvgpu_device_set_drm(gpu, NULL, NULL);
	nvgpu_log(NVGPU_LOG_DEBUG, "unregister\n");
}

struct drm_device *
nvdrm_device(struct nvgpu_device *gpu)
{
	return (nvgpu_device_drm_dev(gpu));
}
