/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Scheduler event boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_sched.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


/* Post an external event to scheduler state. */
/* Post an event to scheduler state.  Must be MPSAFE. */
void
nvgpu_sched_post_event(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "sched post event\n");
}
