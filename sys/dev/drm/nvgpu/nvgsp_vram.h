/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP VRAM allocation boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_VRAM_H_
#define _NVGSP_VRAM_H_

struct nvgpu_device;

/* Initialize backend VRAM allocation state during boot. */
int nvgsp_vram_init(struct nvgpu_device *gpu);
/* Release backend VRAM allocation state after users stop. */
void nvgsp_vram_fini(struct nvgpu_device *gpu);
/* Allocate VRAM for channel instance memory. */
int nvgsp_vram_alloc_channel_inst(struct nvgpu_device *gpu);
/* Allocate VRAM for USERD submission pages. */
int nvgsp_vram_alloc_userd(struct nvgpu_device *gpu);
/* Allocate VRAM for golden channel state. */
int nvgsp_vram_alloc_golden(struct nvgpu_device *gpu);

#endif /* _NVGSP_VRAM_H_ */
