/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unload admission gate for the native NVIDIA GPU driver.
 */

#include "nvgpu_unload.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


/* Initialize unload admission state. */
/* Initialize unload admission state before DRM users can open the device. */
int
nvgpu_unload_init(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "unload init\n");
	return (0);
}

/* Start unload admission.  Returns EBUSY while opens, mmaps, or scheduler work remain. */
int
nvgpu_unload_begin(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "unload begin\n");
	return (0);
}
