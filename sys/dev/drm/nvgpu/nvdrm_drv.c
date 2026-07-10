/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM registration boundary for the native NVIDIA driver.
 */

#include "nvdrm_drv.h"
#include "nvdrm_file.h"
#include "nvdrm_ioctl.h"
#include "nvdrm_prime.h"
#include "nvgpu_bo.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"
#include "nvgpu_ttm.h"

#include <drm/drmP.h>
#include <drm/drm_drv.h>
#include <linux/err.h>

#define NVDRM_DRM_NAME		"nouveau"
#define NVDRM_DRM_DESC		"nVidia Riva/TNT/GeForce (dfly native GSP-RM)"
#define NVDRM_DRM_DATE		"20260708"
#define NVDRM_DRM_MAJOR		1
#define NVDRM_DRM_MINOR		3
#define NVDRM_DRM_PATCH		1

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
	.ioctls = nvdrm_ioctl_descs,
	.num_ioctls = NVDRM_IOCTL_COUNT,
	.open = nvdrm_file_open,
	.postclose = nvdrm_file_postclose,
	.lastclose = nvdrm_file_lastclose,
	.mmap_single = nvgpu_ttm_mmap_single,
	.gem_free_object_unlocked = nvgpu_bo_free,
	.prime_handle_to_fd = nvdrm_prime_handle_to_fd,
	.prime_fd_to_handle = nvdrm_prime_fd_to_handle,
	.gem_prime_export = nvdrm_prime_export,
	.gem_prime_res_obj = nvdrm_prime_get_resv,
	.gem_prime_import = drm_gem_prime_import,
};

/* Register DRM after GPU boot.  gpu is borrowed; may sleep and must not hold GSP/VM tokens. */
int
nvdrm_register(struct nvgpu_device *gpu)
{
	struct pci_dev *pdev = NULL;
	struct drm_device *ddev;
	int error;

	drm_init_pdev(nvgpu_device_get_newbus_dev(gpu), &pdev);
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

	error = nvgpu_ttm_init(gpu, ddev);
	if (error != 0) {
		nvgpu_log(NVGPU_LOG_INFO, "nvgpu_ttm_init failed error=%d\n", error);
		nvgpu_ttm_fini(gpu);
		nvgpu_device_set_drm(gpu, NULL, NULL);
		if (ddev->sysctl != NULL)
			drm_sysctl_cleanup(ddev);
		drm_dev_put(ddev);
		drm_fini_pdev(&pdev);
		return (error);
	}

	error = drm_dev_register(ddev, 0);
	if (error != 0) {
		nvgpu_log(NVGPU_LOG_INFO, "drm_dev_register failed error=%d\n", error);
		nvgpu_ttm_fini(gpu);
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

	ddev = nvgpu_device_get_drm_dev(gpu);
	pdev = nvgpu_device_get_drm_pdev(gpu);
	if (ddev != NULL) {
		drm_dev_unregister(ddev);
		nvgpu_ttm_fini(gpu);
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
nvdrm_get_device(struct nvgpu_device *gpu)
{
	return (nvgpu_device_get_drm_dev(gpu));
}
