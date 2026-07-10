/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private fence abstraction for nvgpu futures.
 */

#include "nvgpu_fence.h"
#include "nvgpu_exec.h"
#include "nvgpu_intr.h"

#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <sys/errno.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/systm.h>

struct nvgpu_fence {
	struct dma_fence base;
	spinlock_t lock;
	struct nvgpu_device *gpu;
	const char *timeline_name;
	struct nvgpu_fence *exec_submit_fence;
	uint32_t exec_channel;
	bool exec_producer;
};

static MALLOC_DEFINE(M_NVGPU_FENCE, "nvgpu_fence", "nvgpu future fence");

struct nvgpu_fence_callback {
	struct dma_fence_cb cb;
	nvgpu_fence_callback_t func;
	void *arg;
};

static const char *nvgpu_fence_get_driver_name(struct dma_fence *fence);
static const char *nvgpu_fence_get_timeline_name(struct dma_fence *fence);
static bool nvgpu_fence_dma_is_signaled(struct dma_fence *fence);
static signed long nvgpu_fence_dma_wait(struct dma_fence *fence, bool intr,
    signed long timeout);
static void nvgpu_fence_drop(struct dma_fence *fence);
static void nvgpu_fence_dma_callback(struct dma_fence *fence,
    struct dma_fence_cb *cb);

static const struct dma_fence_ops nvgpu_fence_ops = {
	.get_driver_name = nvgpu_fence_get_driver_name,
	.get_timeline_name = nvgpu_fence_get_timeline_name,
	.signaled = nvgpu_fence_dma_is_signaled,
	.wait = nvgpu_fence_dma_wait,
	.release = nvgpu_fence_drop,
};

static const char *
nvgpu_fence_get_driver_name(struct dma_fence *fence __unused)
{
	return ("nvgpu");
}

static bool
nvgpu_fence_dma_is_signaled(struct dma_fence *fence)
{
	struct nvgpu_fence *nfence;

	nfence = container_of(fence, struct nvgpu_fence, base);
	nvgpu_intr_request_exec_harvest(nfence->gpu);
	return (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags));
}

static signed long
nvgpu_fence_dma_wait(struct dma_fence *fence, bool intr, signed long timeout)
{
	struct nvgpu_fence *nfence;
	signed long remaining, result, step;

	nfence = container_of(fence, struct nvgpu_fence, base);
	remaining = timeout;
	for (;;) {
		nvgpu_exec_harvest_completed(nfence->gpu);
		if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
			return (remaining > 0 ? remaining : 1);
		if (timeout == 0)
			return (0);
		step = MAX(hz / 10, 1);
		if (timeout != MAX_SCHEDULE_TIMEOUT && step > remaining)
			step = remaining;
		result = dma_fence_default_wait(fence, intr, step);
		if (result < 0)
			return (result);
		if (timeout != MAX_SCHEDULE_TIMEOUT) {
			remaining -= step;
			if (remaining <= 0)
				return (0);
		}
	}
}

static const char *
nvgpu_fence_get_timeline_name(struct dma_fence *fence)
{
	struct nvgpu_fence *nfence;

	nfence = container_of(fence, struct nvgpu_fence, base);
	return (nfence->timeline_name != NULL ? nfence->timeline_name :
	    "future");
}

static void
nvgpu_fence_drop(struct dma_fence *fence)
{
	struct nvgpu_fence *nfence;

	nfence = container_of(fence, struct nvgpu_fence, base);
	nvgpu_fence_release(nfence->exec_submit_fence);
	lockuninit(&nfence->lock);
	_kfree(nfence, M_NVGPU_FENCE);
}

struct nvgpu_fence *
nvgpu_fence_create(struct nvgpu_device *gpu,
    const char *timeline_name)
{
	struct nvgpu_fence *fence;
	u64 context;

	fence = kmalloc(sizeof(*fence), M_NVGPU_FENCE, M_WAITOK | M_ZERO);
	lockinit(&fence->lock, "nvgpuf", 0, 0);
	fence->gpu = gpu;
	fence->timeline_name = timeline_name;
	context = dma_fence_context_alloc(1);
	dma_fence_init(&fence->base, &nvgpu_fence_ops, &fence->lock,
	    context, 1);
	return (fence);
}

struct nvgpu_fence *
nvgpu_fence_import_dma_ref(struct dma_fence *dma)
{
	struct dma_fence_chain *chain;
	struct nvgpu_fence *fence;

	if (dma == NULL)
		return (NULL);
	if (dma->ops == &nvgpu_fence_ops)
		return (container_of(dma, struct nvgpu_fence, base));
	chain = to_dma_fence_chain(dma);
	if (chain != NULL && chain->fence != NULL &&
	    chain->fence->ops == &nvgpu_fence_ops) {
		fence = container_of(chain->fence, struct nvgpu_fence, base);
		dma_fence_get(&fence->base);
		dma_fence_put(dma);
		return (fence);
	}
	dma_fence_put(dma);
	return (NULL);
}

struct dma_fence *
nvgpu_fence_get_dma_ref(struct nvgpu_fence *fence)
{
	if (fence == NULL)
		return (NULL);
	return (dma_fence_get(&fence->base));
}

void
nvgpu_fence_addref(struct nvgpu_fence *fence)
{
	if (fence != NULL)
		dma_fence_get(&fence->base);
}

void
nvgpu_fence_release(struct nvgpu_fence *fence)
{
	if (fence == NULL)
		return;
	dma_fence_put(&fence->base);
}

void
nvgpu_fence_set_exec_producer(struct nvgpu_fence *gpu_complete,
    uint32_t channel, struct nvgpu_fence *submitted)
{
	if (gpu_complete == NULL || submitted == NULL)
		return;
	KASSERT(!gpu_complete->exec_producer &&
	    gpu_complete->exec_submit_fence == NULL,
	    ("nvgpu fence EXEC producer already set"));
	nvgpu_fence_addref(submitted);
	gpu_complete->exec_submit_fence = submitted;
	gpu_complete->exec_channel = channel;
	gpu_complete->exec_producer = true;
}

struct nvgpu_fence *
nvgpu_fence_hold_exec_wait(struct nvgpu_fence *gpu_complete,
    uint32_t channel)
{
	struct nvgpu_fence *wait;

	if (gpu_complete == NULL)
		return (NULL);
	wait = gpu_complete;
	if (gpu_complete->exec_producer &&
	    gpu_complete->exec_channel == channel &&
	    gpu_complete->exec_submit_fence != NULL)
		wait = gpu_complete->exec_submit_fence;
	nvgpu_fence_addref(wait);
	return (wait);
}

bool
nvgpu_fence_is_signaled(struct nvgpu_fence *fence)
{
	if (fence == NULL)
		return (true);
	return (dma_fence_is_signaled(&fence->base));
}

int
nvgpu_fence_error(struct nvgpu_fence *fence)
{
	int error;

	if (fence == NULL)
		return (0);
	error = fence->base.error;
	return (error < 0 ? -error : error);
}

int
nvgpu_fence_add_callback(struct nvgpu_fence *fence,
    nvgpu_fence_callback_t func, void *arg)
{
	struct nvgpu_fence_callback *cb;
	int error;

	if (fence == NULL || func == NULL)
		return (EINVAL);
	cb = kmalloc(sizeof(*cb), M_NVGPU_FENCE, M_WAITOK | M_ZERO);
	cb->func = func;
	cb->arg = arg;
	error = dma_fence_add_callback(&fence->base, &cb->cb,
	    nvgpu_fence_dma_callback);
	if (error != 0) {
		_kfree(cb, M_NVGPU_FENCE);
	}
	return (error < 0 ? -error : error);
}

int
nvgpu_fence_signal(struct nvgpu_fence *fence, int error)
{
	if (fence == NULL)
		return (EINVAL);
	if (error != 0)
		dma_fence_set_error(&fence->base, error > 0 ? -error : error);
	return (dma_fence_signal(&fence->base));
}

int
nvgpu_fence_wait(struct nvgpu_fence *fence, bool interruptible)
{
	signed long result;
	int error;

	if (fence == NULL)
		return (EINVAL);
	result = dma_fence_wait(&fence->base, interruptible);
	if (result < 0)
		return ((int)-result);
	error = nvgpu_fence_error(fence);
	return (error);
}

static void
nvgpu_fence_dma_callback(struct dma_fence *dma __unused,
    struct dma_fence_cb *cb)
{
	struct nvgpu_fence_callback *callback;
	struct nvgpu_fence *fence;

	fence = container_of(dma, struct nvgpu_fence, base);
	callback = container_of(cb, struct nvgpu_fence_callback, cb);
	callback->func(fence, callback->arg);
	_kfree(callback, M_NVGPU_FENCE);
}
