/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP display backend boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_disp.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


int
nvgsp_disp_init(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "disp init\n");
	return (0);
}

void
nvgsp_disp_fini(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "disp fini\n");
}
