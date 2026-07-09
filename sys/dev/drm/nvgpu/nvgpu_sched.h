/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Device-global future scheduler for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_SCHED_H_
#define _NVGPU_SCHED_H_

struct nvgpu_device;

struct nvgpu_sched;

/* Start the device-global future scheduler. */
int nvgpu_sched_start(struct nvgpu_device *gpu, struct nvgpu_sched **out);

/* Stop the scheduler after DRM users have been rejected and drained. */
void nvgpu_sched_stop(struct nvgpu_sched *sched);

#endif /* _NVGPU_SCHED_H_ */
