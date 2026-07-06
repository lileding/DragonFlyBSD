/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP BAR aperture boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_BAR_H_
#define _NVGSP_BAR_H_

struct nvgpu_device;

/* Initialize BAR2 backend state during boot; gpu is borrowed. */
int nvgsp_bar2_init(struct nvgpu_device *gpu);
/* Release BAR2 state after all BAR2 users have stopped. */
void nvgsp_bar2_fini(struct nvgpu_device *gpu);
/* Initialize BAR1 backend state during boot; gpu is borrowed. */
int nvgsp_bar1_init(struct nvgpu_device *gpu);
/* Release BAR1 state after CPU mappings have drained. */
void nvgsp_bar1_fini(struct nvgpu_device *gpu);
/* Map channel instance memory through BAR1.  Caller serializes BAR1 updates. */
int nvgsp_bar1_map_inst(struct nvgpu_device *gpu);
/* Map USERD pages through BAR1 before channel publication. */
int nvgsp_bar1_map_userd(struct nvgpu_device *gpu);

#endif /* _NVGSP_BAR_H_ */
