/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP BAR aperture boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_BAR_H_
#define _NVGSP_BAR_H_

struct nvgpu_device;

/* Initialize BAR2 backend state during boot; gpu is borrowed. */
int nvgsp_bar_init_bar2(struct nvgpu_device *gpu);
/* Release BAR2 state after all BAR2 users have stopped. */
void nvgsp_bar_fini_bar2(struct nvgpu_device *gpu);
/* Initialize BAR1 backend state during boot; gpu is borrowed. */
int nvgsp_bar_init_bar1(struct nvgpu_device *gpu);
/* Release BAR1 state after CPU mappings have drained. */
void nvgsp_bar_fini_bar1(struct nvgpu_device *gpu);
/* Map channel instance memory through BAR1.  Caller serializes BAR1 updates. */
int nvgsp_bar_map_bar1_inst(struct nvgpu_device *gpu);
/* Map USERD pages through BAR1 before channel publication. */
int nvgsp_bar_map_bar1_userd(struct nvgpu_device *gpu);

#endif /* _NVGSP_BAR_H_ */
