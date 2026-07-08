/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Scheduler event boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_sched.h"
#include "nvgpu_debug.h"
#include "nvgpu_proc.h"

#include <sys/kernel.h>
#include <sys/malloc.h>

static MALLOC_DEFINE(M_NVGPU_FUTURE, "nvgpu_future", "nvgpu scheduler future");

/* Poll every active future once from the owning proc LWKT. */
void
nvgpu_sched_run(struct nvgpu_proc *proc)
{
	struct nvgpu_future *future;
	struct nvgpu_future_result result;

	for (;;) {
		lwkt_gettoken(&proc->token);
		future = TAILQ_FIRST(&proc->active_tasks);
		if (future != NULL)
			TAILQ_REMOVE(&proc->active_tasks, future, link);
		lwkt_reltoken(&proc->token);
		if (future == NULL)
			break;

		if (future->poll == NULL) {
			nvgpu_log(NVGPU_LOG_INFO,
			    "sched future without poll proc=%p future=%p\n",
			    proc, future);
			_kfree(future, M_NVGPU_FUTURE);
			continue;
		}

		result = future->poll(proc, future);
		if (result.ready) {
			nvgpu_log(NVGPU_LOG_DEBUG,
			    "sched future ready proc=%p result=%d\n",
			    proc, result.result);
			_kfree(future, M_NVGPU_FUTURE);
		} else {
			lwkt_gettoken(&proc->token);
			TAILQ_INSERT_TAIL(&proc->parked_tasks, future, link);
			lwkt_reltoken(&proc->token);
		}
	}
}

/* Wake scheduler work from external GPU events.  The proc fanout is not wired yet. */
void
nvgpu_sched_post_event(struct nvgpu_device *gpu __unused)
{
}
