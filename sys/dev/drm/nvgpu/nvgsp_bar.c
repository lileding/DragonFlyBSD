/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP BAR aperture boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_bar.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


int
nvgsp_bar2_init(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "bar2 init\n");
	return (0);
}

void
nvgsp_bar2_fini(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "bar2 fini\n");
}

int
nvgsp_bar1_init(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "bar1 init\n");
	return (0);
}

void
nvgsp_bar1_fini(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "bar1 fini\n");
}

int
nvgsp_bar1_map_inst(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "bar1 map inst\n");
	return (0);
}

int
nvgsp_bar1_map_userd(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "bar1 map userd\n");
	return (0);
}
