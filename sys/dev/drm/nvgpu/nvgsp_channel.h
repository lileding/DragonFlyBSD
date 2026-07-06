/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP channel backend boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_CHANNEL_H_
#define _NVGSP_CHANNEL_H_

struct nvgpu_device;

/* Create the bootstrap channel during boot. */
int nvgsp_channel_create_bootstrap(struct nvgpu_device *gpu);
/* Destroy the bootstrap channel after submissions have stopped. */
void nvgsp_channel_destroy_bootstrap(struct nvgpu_device *gpu);
/* Create the golden channel used as a template for user channels. */
int nvgsp_channel_create_golden(struct nvgpu_device *gpu);
/* Destroy golden channel state after user channel creation has stopped. */
void nvgsp_channel_destroy_golden(struct nvgpu_device *gpu);

#endif /* _NVGSP_CHANNEL_H_ */
