/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Interrupt ingress boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_INTR_H_
#define _NVGPU_INTR_H_

struct nvgpu_device;
struct nvgpu_future;
struct nvgpu_sema;

/*
 * Start interrupt delivery and its process-context event worker for device.
 * Device owns the resulting state until nvgpu_intr_stop() returns.
 */
int nvgpu_intr_start(struct nvgpu_device *device);

/*
 * Synchronously disable interrupt delivery and release device interrupt state.
 * New channel submissions must already be blocked and no future may be parked.
 */
void nvgpu_intr_stop(struct nvgpu_device *device);

/*
 * Park one future on an initialized GPU semaphore without sleeping.
 *
 * This function is MPSAFE.  On zero interrupt state owns future until GPU
 * completion transfers it to the scheduler.  On error caller keeps ownership.
 */
int nvgpu_intr_park(struct nvgpu_sema *sema,
	struct nvgpu_future *future);

#endif /* _NVGPU_INTR_H_ */
