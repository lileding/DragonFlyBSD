/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unload admission gate for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_UNLOAD_H_
#define _NVGPU_UNLOAD_H_

struct nvgpu_device;

/* Initialize unload admission state before DRM users can open the device. */
int nvgpu_unload_init(struct nvgpu_device *gpu);
/* Release unload admission state after DRM is unpublished and all users are gone. */
void nvgpu_unload_fini(struct nvgpu_device *gpu);
/* Admit one DRM file open unless unload has started. */
int nvgpu_unload_file_open(struct nvgpu_device *gpu);
/* Drop one DRM file reference previously admitted by nvgpu_unload_file_open(). */
void nvgpu_unload_file_close(struct nvgpu_device *gpu);
/* Start unload admission after proving the DRM core has no live users. */
int nvgpu_unload_begin(struct nvgpu_device *gpu);

#endif /* _NVGPU_UNLOAD_H_ */
