/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Display policy boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_DISPLAY_H_
#define _NVGPU_DISPLAY_H_

struct nvgpu_device;

/* Handle one vblank event.  gpu is borrowed; called from display/interrupt fanout. */
void nvgpu_display_handle_vblank(struct nvgpu_device *gpu);

#endif /* _NVGPU_DISPLAY_H_ */
