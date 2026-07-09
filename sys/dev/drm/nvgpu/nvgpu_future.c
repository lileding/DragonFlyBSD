/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Stackless future primitive used by the nvgpu scheduler.
 */

#include "nvgpu_future.h"
#include "nvgpu_fence.h"
#include "nvgpu_proc.h"
#include "nvgpu_sched.h"

#include <sys/errno.h>
#include <sys/malloc.h>

static MALLOC_DEFINE(M_NVGPU_FUTURE_WAIT, "nvgpu_fwait",
    "nvgpu future wait item");

struct nvgpu_future_wait {
	TAILQ_ENTRY(nvgpu_future_wait) link;
	struct dma_fence_cb cb;
	struct nvgpu_future *future;
	struct dma_fence *fence;
	bool linked;
	bool armed;
};

static void nvgpu_future_wait_cb(struct dma_fence *fence,
    struct dma_fence_cb *cb);
static void nvgpu_future_resolve_wait(struct nvgpu_future_wait *wait,
    int error);

void
nvgpu_future_init(struct nvgpu_future *future, struct nvgpu_proc *proc,
    struct dma_fence *done_fence,
    struct nvgpu_future_result (*poll)(struct nvgpu_future *future),
    void (*release)(struct nvgpu_future *future))
{
	future->poll = poll;
	future->release = release;
	future->proc = proc;
	future->done_fence = done_fence;
	TAILQ_INIT(&future->waits);
	future->wait_count = 0;
	future->wait_error = 0;
	future->submitted = false;
	future->queued = false;
	future->polling = false;
	future->wake_pending = false;
	future->done = false;
	nvgpu_proc_hold(proc);
}


int
nvgpu_future_add_wait(struct nvgpu_future *future, struct dma_fence *fence)
{
	struct nvgpu_future_wait *wait;
	int error;

	if (future == NULL || fence == NULL)
		return (EINVAL);
	if (dma_fence_is_signaled(fence)) {
		if (fence->error != 0 && future->wait_error == 0)
			future->wait_error = fence->error;
		return (0);
	}

	wait = kmalloc(sizeof(*wait), M_NVGPU_FUTURE_WAIT, M_WAITOK | M_ZERO);
	wait->future = future;
	wait->fence = dma_fence_get(fence);
	wait->linked = true;
	TAILQ_INSERT_TAIL(&future->waits, wait, link);
	future->wait_count++;

	wait->armed = true;
	error = dma_fence_add_callback(fence, &wait->cb,
	    nvgpu_future_wait_cb);
	if (error == -ENOENT) {
		nvgpu_future_resolve_wait(wait, fence->error);
		return (0);
	}
	if (error != 0) {
		nvgpu_future_resolve_wait(wait, error);
		return (error);
	}
	return (0);
}

void
nvgpu_future_finish(struct nvgpu_future *future,
    struct nvgpu_future_result result)
{
	struct dma_fence *done;

	if (future == NULL)
		return;
	future->done = true;
	done = future->done_fence;
	future->done_fence = NULL;
	if (done != NULL) {
		(void)nvgpu_fence_signal(done, result.result);
		dma_fence_put(done);
	}
	{
		struct nvgpu_proc *proc = future->proc;
		if (future->release != NULL)
			future->release(future);
		nvgpu_proc_release(proc);
	}
}


void
nvgpu_future_cancel(struct nvgpu_future *future, int error)
{
	struct nvgpu_future_wait *wait;
	struct nvgpu_future_wait *tmp;
	struct dma_fence *done;

	if (future == NULL)
		return;
	future->done = true;
	TAILQ_FOREACH_MUTABLE(wait, &future->waits, link, tmp) {
		if (wait->armed)
			(void)dma_fence_remove_callback(wait->fence, &wait->cb);
		TAILQ_REMOVE(&future->waits, wait, link);
		if (future->wait_count > 0)
			future->wait_count--;
		dma_fence_put(wait->fence);
		_kfree(wait, M_NVGPU_FUTURE_WAIT);
	}
	done = future->done_fence;
	future->done_fence = NULL;
	if (done != NULL) {
		if (future->submitted)
			(void)nvgpu_fence_signal(done, error);
		dma_fence_put(done);
	}
	{
		struct nvgpu_proc *proc = future->proc;
		if (future->release != NULL)
			future->release(future);
		nvgpu_proc_release(proc);
	}
}


static void
nvgpu_future_wait_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct nvgpu_future_wait *wait;

	wait = container_of(cb, struct nvgpu_future_wait, cb);
	nvgpu_future_resolve_wait(wait, fence->error);
}

static void
nvgpu_future_resolve_wait(struct nvgpu_future_wait *wait, int error)
{
	struct nvgpu_future *future;

	future = wait->future;
	if (future == NULL)
		return;
	nvgpu_sched_lock_future(future);
	if (wait->linked) {
		TAILQ_REMOVE(&future->waits, wait, link);
		wait->linked = false;
		if (future->wait_count > 0)
			future->wait_count--;
	}
	if (error != 0 && future->wait_error == 0)
		future->wait_error = error;
	dma_fence_put(wait->fence);
	_kfree(wait, M_NVGPU_FUTURE_WAIT);
	if (future->wait_count == 0)
		nvgpu_sched_wake_future_locked(future);
	nvgpu_sched_unlock_future(future);
}
