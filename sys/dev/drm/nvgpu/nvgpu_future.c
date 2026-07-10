/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Stackless future primitive used by the nvgpu scheduler.
 */

#include "nvgpu_future.h"
#include "nvgpu_fence.h"
#include "nvgpu_proc.h"
#include "nvgpu_sched.h"

#include <machine/atomic.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

static void nvgpu_future_wait_cb(struct nvgpu_fence *fence, void *arg);

int
nvgpu_future_spawn(struct nvgpu_future *future, struct nvgpu_proc *proc,
    struct nvgpu_fence *done_fence, struct nvgpu_fence **wait_fences,
    uint32_t wait_count,
    struct nvgpu_future_result (*poll)(struct nvgpu_future *future),
    void (*destroy)(struct nvgpu_future *future))
{
	int error;
	u_int old;

	future->poll = poll;
	future->destroy = destroy;
	future->proc = proc;
	future->done_fence = done_fence;
	future->wait_count = 1;
	future->error = 0;
	nvgpu_proc_hold(proc);

	if (wait_count != 0 && wait_fences == NULL)
		return (EINVAL);
	for (uint32_t i = 0; i < wait_count; i++) {
		if (wait_fences[i] == NULL)
			return (EINVAL);
	}
	for (uint32_t i = 0; i < wait_count; i++) {
		atomic_fetchadd_int(&future->wait_count, 1);

		error = nvgpu_fence_add_callback(wait_fences[i],
		    nvgpu_future_wait_cb, future);
		if (error != 0) {
			if (error == ENOENT) {
				error = nvgpu_fence_error(wait_fences[i]);
				if (error != 0)
					future->error = (u_int)error;
			} else {
				future->error = (u_int)error;
			}
			atomic_fetchadd_int(&future->wait_count, -1);
		}
	}
	if (atomic_fetchadd_int(&future->wait_count, -1) == 1) {
		if (future->error != 0) {
			nvgpu_future_finish(future, (int)future->error);
		} else {
			nvgpu_future_wake(future);
		}
	}
	return (0);
}

void
nvgpu_future_finish(struct nvgpu_future *future, int result)
{
	struct nvgpu_fence *done;

	if (future == NULL)
		return;
	future->error = (u_int)result;
	done = future->done_fence;
	future->done_fence = NULL;
	if (done != NULL) {
		(void)nvgpu_fence_signal(done, result);
		nvgpu_fence_release(done);
	}
	{
		struct nvgpu_proc *proc = future->proc;
		if (future->destroy != NULL)
			future->destroy(future);
		nvgpu_proc_release(proc);
	}
}


static void
nvgpu_future_wait_cb(struct nvgpu_fence *fence, void *arg)
{
	struct nvgpu_future *future;
	int error;
	u_int old;

	future = arg;
	error = nvgpu_fence_error(fence);
	if (error != 0)
		future->error = (u_int)error;
	old = atomic_fetchadd_int(&future->wait_count, -1);
	if (old == 1) {
		if (future->error != 0) {
			nvgpu_future_finish(future, (int)future->error);
		} else {
			nvgpu_future_wake(future);
		}
	}
}
