/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Stackless future primitive used by the nvgpu scheduler.
 */

#include "nvgpu_future.h"
#include "nvgpu_fence.h"
#include "nvgpu_proc.h"
#include "nvgpu_sched.h"

#include <linux/dma-fence.h>
#include <machine/atomic.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

static MALLOC_DEFINE(M_NVGPU_FUTURE, "nvgpu_future",
    "nvgpu future callbacks");

struct nvgpu_future_wait_callback {
	struct dma_fence_cb cb;
	struct dma_fence *fence;
	struct nvgpu_future *future;
};

static void nvgpu_future_wait_cb(struct dma_fence *fence,
    struct dma_fence_cb *cb);

int
nvgpu_future_spawn(struct nvgpu_future *future, struct nvgpu_proc *proc,
    struct nvgpu_fence *done_fence, struct dma_fence **wait_fences,
    uint32_t wait_count,
    struct nvgpu_future_result (*poll)(struct nvgpu_future *future),
    void (*destroy)(struct nvgpu_future *future))
{
	struct nvgpu_future_wait_callback *callback;
	int error;

	if (future == NULL || proc == NULL || done_fence == NULL || poll == NULL ||
	    destroy == NULL || (wait_count != 0 && wait_fences == NULL))
		return (EINVAL);
	for (uint32_t i = 0; i < wait_count; i++) {
		if (wait_fences[i] == NULL)
			return (EINVAL);
	}

	future->poll = poll;
	future->destroy = destroy;
	future->proc = proc;
	future->done_fence = done_fence;
	future->wait_count = 1;
	future->error = 0;
	nvgpu_proc_hold(proc);

	for (uint32_t i = 0; i < wait_count; i++) {
		callback = kmalloc(sizeof(*callback), M_NVGPU_FUTURE,
		    M_WAITOK | M_ZERO);
		callback->fence = dma_fence_get(wait_fences[i]);
		callback->future = future;
		atomic_fetchadd_int(&future->wait_count, 1);
		error = dma_fence_add_callback(callback->fence, &callback->cb,
		    nvgpu_future_wait_cb);
		if (error != 0) {
			error = error < 0 ? -error : error;
			if (error == ENOENT) {
				error = callback->fence->error;
				if (error < 0)
					error = -error;
				if (error != 0)
					(void)atomic_cmpset_int(&future->error, 0,
					    (u_int)error);
			} else {
				(void)atomic_cmpset_int(&future->error, 0,
				    (u_int)error);
			}
			dma_fence_put(callback->fence);
			_kfree(callback, M_NVGPU_FUTURE);
			atomic_fetchadd_int(&future->wait_count, -1);
		}
	}
	if (atomic_fetchadd_int(&future->wait_count, -1) == 1) {
		nvgpu_future_wake(future);
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
nvgpu_future_wait_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct nvgpu_future_wait_callback *callback;
	struct nvgpu_future *future;
	int error;
	u_int old;

	callback = container_of(cb, struct nvgpu_future_wait_callback, cb);
	future = callback->future;
	error = fence->error;
	if (error < 0)
		error = -error;
	if (error != 0)
		(void)atomic_cmpset_int(&future->error, 0, (u_int)error);
	dma_fence_put(callback->fence);
	_kfree(callback, M_NVGPU_FUTURE);
	old = atomic_fetchadd_int(&future->wait_count, -1);
	if (old == 1)
		nvgpu_future_wake(future);
}
