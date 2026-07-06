/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP VRAM allocation boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_vram.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


int
nvgsp_vram_init(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vram init\n");
	return (0);
}

void
nvgsp_vram_fini(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vram fini\n");
}

/* Allocate VRAM for channel instance memory. */
int
nvgsp_vram_alloc_channel_inst(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vram alloc channel inst\n");
	return (0);
}

/* Allocate VRAM for USERD submission pages. */
int
nvgsp_vram_alloc_userd(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vram alloc userd\n");
	return (0);
}

/* Allocate VRAM for the golden channel. */
/* Allocate VRAM for golden channel state. */
int
nvgsp_vram_alloc_golden(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vram alloc golden\n");
	return (0);
}
