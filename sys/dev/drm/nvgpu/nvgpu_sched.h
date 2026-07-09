/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Device-global future scheduler for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_SCHED_H_
#define _NVGPU_SCHED_H_

#include "nvgpu_future.h"

struct nvgpu_device;

/* Start the device-global future scheduler. */
int nvgpu_sched_start(struct nvgpu_device *gpu);

/* Stop the scheduler after DRM users have been rejected and drained. */
void nvgpu_sched_stop(struct nvgpu_device *gpu);

/* Submit a future after all userspace-visible signal handles are published. */
void nvgpu_sched_submit_future(struct nvgpu_future *future);

/* Make a submitted future runnable; safe from callbacks and ioctl threads. */
void nvgpu_sched_wake_future(struct nvgpu_future *future);

/* Internal helpers for future wait callbacks that mutate wait_count. */
void nvgpu_sched_lock_future(struct nvgpu_future *future);
void nvgpu_sched_unlock_future(struct nvgpu_future *future);
void nvgpu_sched_wake_future_locked(struct nvgpu_future *future);

/* Wake scheduler work from external GPU events.  Real EXEC fanout is wired later. */
void nvgpu_sched_post_event(struct nvgpu_device *gpu);

#endif /* _NVGPU_SCHED_H_ */
