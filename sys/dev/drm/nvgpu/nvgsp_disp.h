/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP display backend boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_DISP_H_
#define _NVGSP_DISP_H_

struct nvgpu_device;

/* Initialize display backend before DRM/KMS registration. */
int nvgsp_disp_init(struct nvgpu_device *gpu);
/* Release display backend after DRM/KMS users have stopped. */
void nvgsp_disp_fini(struct nvgpu_device *gpu);

#endif /* _NVGSP_DISP_H_ */
