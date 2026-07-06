/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP backend state boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_STATE_H_
#define _NVGSP_STATE_H_

struct nvgpu_device;

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
/* Enable doorbells before channel submission. */
int nvgsp_state_enable_doorbell(struct nvgpu_device *gpu);

#endif /* _NVGSP_STATE_H_ */
