/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Scheduler event boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_sched.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"
#include "nvgpu_proc.h"

/* Initialize scheduler state embedded in one nvgpu_proc. */
void
nvgpu_sched_init(struct nvgpu_sched *sched)
{
	TAILQ_INIT(&sched->active_futures);
}

/* Poll every active future once from the owning proc LWKT. */
void
nvgpu_sched_run(struct nvgpu_proc *proc)
{
	struct nvgpu_sched *sched;
	struct nvgpu_future *future;
	struct nvgpu_future_result result;

	sched = nvgpu_proc_get_sched(proc);
	if (sched == NULL)
		return;

	TAILQ_FOREACH(future, &sched->active_futures, link) {
		if (future->poll == NULL)
			continue;
		result = future->poll(proc, future);
		if (result.ready) {
			nvgpu_log(NVGPU_LOG_DEBUG,
			    "sched future ready proc=%p result=%d\n",
			    proc, result.result);
		}
	}
}

/* Post an event to scheduler state.  Must be MPSAFE. */
void
nvgpu_sched_post_event(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "sched post event\n");
}
