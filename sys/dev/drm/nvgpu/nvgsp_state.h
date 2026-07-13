/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP backend state boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_STATE_H_
#define _NVGSP_STATE_H_

#include <sys/stdint.h>

struct nvgpu_device;

struct nvgsp_intr_masks {
	uint32_t nonstall;
	uint32_t stall;
	uint32_t engine;
	uint32_t display;
};

/* Return usable VRAM base parsed from static GSP info. */
uint64_t nvgsp_state_get_fb_usable_base(struct nvgpu_device *gpu);
/* Return usable VRAM bytes parsed from static GSP info. */
uint64_t nvgsp_state_get_fb_usable_size(struct nvgpu_device *gpu);
/* Initialize CPU-side GSP backend state. */
int nvgsp_state_init(struct nvgpu_device *gpu);
/* Release CPU-side GSP state after GSP shutdown. */
void nvgsp_state_fini(struct nvgpu_device *gpu);
/* Query static GPU info from GSP during boot. */
int nvgsp_state_query_static_info(struct nvgpu_device *gpu);
/* Query method-buffer size before channel creation. */
int nvgsp_state_query_mthdbuf_size(struct nvgpu_device *gpu);
/* Retrieve interrupt routing metadata before nvgpu_intr_init(). */
int nvgsp_state_get_intr_table(struct nvgpu_device *gpu);
/* Enable the finite CPU leaf vectors returned by GSP after the ithread is wired. */
int nvgsp_state_enable_intr(struct nvgpu_device *gpu);
/* Return GSP's classification for one CPU interrupt leaf. */
void nvgsp_state_get_intr_masks(struct nvgpu_device *gpu, uint32_t leaf,
    struct nvgsp_intr_masks *masks);
/* Enable doorbells before channel submission. */
int nvgsp_state_enable_doorbell(struct nvgpu_device *gpu);

#endif /* _NVGSP_STATE_H_ */
