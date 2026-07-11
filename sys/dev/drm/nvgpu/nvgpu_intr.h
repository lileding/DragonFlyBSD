/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Interrupt ingress boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_INTR_H_
#define _NVGPU_INTR_H_

#include <sys/stdint.h>

struct nvgpu_device;
struct nvgpu_intr_state;

/* Prepare interrupt state for gpu.  Called from boot before interrupts are enabled. */
int nvgpu_intr_init(struct nvgpu_device *gpu);
/* Enable interrupt delivery for gpu after nvgpu_intr_init(). */
int nvgpu_intr_enable(struct nvgpu_device *gpu);
/* Disable interrupt delivery before interrupt state teardown. */
void nvgpu_intr_disable(struct nvgpu_device *gpu);
/* Admit display IRQ fanout after KMS callbacks are installed. */
void nvgpu_intr_enable_display_dispatch(struct nvgpu_device *gpu);
/* Stop new display fanout and wait for an in-progress callback to return. */
void nvgpu_intr_disable_display_dispatch(struct nvgpu_device *gpu);
/* Release interrupt state after delivery has been disabled. */
void nvgpu_intr_fini(struct nvgpu_device *gpu);
/* Top-level interrupt entry.  Must stay short and hand slow work to subsystem workers. */
void nvgpu_intr_handle(struct nvgpu_device *gpu);

/* Queue one GSP-reported channel fault for process-context completion. */
void nvgpu_intr_report_channel_fault(struct nvgpu_device *gpu, uint32_t chid);

/* Request an EXEC semaphore harvest without sleeping or taking a token. */
void nvgpu_intr_request_exec_harvest(struct nvgpu_device *gpu);

#endif /* _NVGPU_INTR_H_ */
