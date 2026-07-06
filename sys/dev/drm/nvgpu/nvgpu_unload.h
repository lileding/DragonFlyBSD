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
/* Start unload admission.  Returns EBUSY while opens, mmaps, or scheduler work remain. */
int nvgpu_unload_begin(struct nvgpu_device *gpu);

#endif /* _NVGPU_UNLOAD_H_ */
