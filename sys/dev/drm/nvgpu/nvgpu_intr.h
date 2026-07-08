/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Interrupt ingress boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_INTR_H_
#define _NVGPU_INTR_H_

struct nvgpu_device;

/* Prepare interrupt state for gpu.  Called from boot before interrupts are enabled. */
int nvgpu_intr_init(struct nvgpu_device *gpu);
/* Enable interrupt delivery for gpu after nvgpu_intr_init(). */
int nvgpu_intr_enable(struct nvgpu_device *gpu);
/* Disable interrupt delivery before interrupt state teardown. */
void nvgpu_intr_disable(struct nvgpu_device *gpu);
/* Release interrupt state after delivery has been disabled. */
void nvgpu_intr_fini(struct nvgpu_device *gpu);
/* Top-level interrupt entry.  Must stay short and hand slow work to subsystem workers. */
void nvgpu_intr_handle(struct nvgpu_device *gpu);

#endif /* _NVGPU_INTR_H_ */
