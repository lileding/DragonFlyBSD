/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM registration boundary for the native NVIDIA driver.
 */

#include "nvdrm_drv.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


/* Register the DRM device after GPU boot has completed. */
/* Register DRM after GPU boot.  gpu is borrowed; may sleep and must not hold GSP/VM tokens. */
int
nvdrm_register(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "register\n");
	return (0);
}

/* Unregister the DRM device before backend teardown. */
/* Unregister DRM before backend teardown.  gpu is borrowed; callers must have rejected new users. */
void
nvdrm_unregister(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "unregister\n");
}

struct drm_device *
nvdrm_device(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "device\n");
	return (NULL);
}
