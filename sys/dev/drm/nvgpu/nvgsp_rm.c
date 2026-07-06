/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP RM object boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_rm.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


int
nvgsp_rm_client_create(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "rm client create\n");
	return (0);
}

int
nvgsp_rm_device_create(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "rm device create\n");
	return (0);
}

int
nvgsp_rm_subdevice_create(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "rm subdevice create\n");
	return (0);
}

/* Create an RM usermode object. */
/* Create the RM usermode object before channel use. */
int
nvgsp_rm_create_usermode_object(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "rm create usermode object\n");
	return (0);
}

/* Free an RM graphics object. */
/* Free an RM graphics object after GPU use has stopped. */
int
nvgsp_rm_free_graphics_object(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "rm free graphics object\n");
	return (0);
}
