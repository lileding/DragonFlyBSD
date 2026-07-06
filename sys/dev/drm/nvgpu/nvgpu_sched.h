/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Scheduler event boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_SCHED_H_
#define _NVGPU_SCHED_H_

struct nvgpu_device;

/* Post an event to scheduler state.  Must be MPSAFE. */
void nvgpu_sched_post_event(struct nvgpu_device *gpu);

#endif /* _NVGPU_SCHED_H_ */
