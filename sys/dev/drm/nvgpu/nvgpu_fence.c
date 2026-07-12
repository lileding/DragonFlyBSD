/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private fence abstraction for nvgpu futures.
 */

#include "nvgpu_fence.h"
#include "nvgpu_exec.h"
#include "nvgpu_intr.h"

#include <linux/dma-fence.h>
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
	struct nvgpu_fence *exec_future_done_fence;
	uint32_t exec_channel;
	bool exec_producer;
};

static MALLOC_DEFINE(M_NVGPU_FENCE, "nvgpu_fence", "nvgpu future fence");

static const char *nvgpu_fence_get_driver_name(struct dma_fence *fence);
static const char *nvgpu_fence_get_timeline_name(struct dma_fence *fence);
static bool nvgpu_fence_dma_is_signaled(struct dma_fence *fence);
static signed long nvgpu_fence_dma_wait(struct dma_fence *fence, bool intr,
    signed long timeout);
static void nvgpu_fence_finalize(struct dma_fence *fence);

static const struct dma_fence_ops nvgpu_fence_ops = {
	.get_driver_name = nvgpu_fence_get_driver_name,
	.get_timeline_name = nvgpu_fence_get_timeline_name,
	.signaled = nvgpu_fence_dma_is_signaled,
	.wait = nvgpu_fence_dma_wait,
	.release = nvgpu_fence_finalize,
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
	unsigned long deadline;
	unsigned long now;
	signed long remaining, step;
	int error;

	/* The fence address is the native lksleep/wakeup interlock. */
	nfence = container_of(fence, struct nvgpu_fence, base);
	remaining = timeout;
	deadline = timeout == MAX_SCHEDULE_TIMEOUT ? 0 :
	    jiffies + (unsigned long)timeout;
	for (;;) {
		nvgpu_exec_harvest_completed(nfence->gpu);
		lockmgr(fence->lock, LK_EXCLUSIVE);
		if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags)) {
			lockmgr(fence->lock, LK_RELEASE);
			return (remaining > 0 ? remaining : 1);
		}
		if (timeout == 0) {
			lockmgr(fence->lock, LK_RELEASE);
			return (0);
		}
		step = MAX(hz / 10, 1);
		if (timeout != MAX_SCHEDULE_TIMEOUT && step > remaining)
			step = remaining;
		error = lksleep(fence, fence->lock, intr ? PCATCH : 0,
		    "nvgpuf", (int)step);
		lockmgr(fence->lock, LK_RELEASE);
		if (error == EINTR || error == ERESTART)
			return (-ERESTARTSYS);
		if (timeout != MAX_SCHEDULE_TIMEOUT) {
			now = jiffies;
			if (time_after_eq(now, deadline))
				return (0);
			remaining = (signed long)(deadline - now);
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
nvgpu_fence_finalize(struct dma_fence *fence)
{
	struct nvgpu_fence *nfence;

	nfence = container_of(fence, struct nvgpu_fence, base);
	nvgpu_fence_release(nfence->exec_future_done_fence);
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

struct dma_fence *
nvgpu_fence_addref_as_dma(struct nvgpu_fence *fence)
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
nvgpu_fence_set_exec_origin(struct nvgpu_fence *gpu_complete,
    uint32_t channel, struct nvgpu_fence *future_done)
{
	if (gpu_complete == NULL || future_done == NULL)
		return;
	KASSERT(!gpu_complete->exec_producer &&
	    gpu_complete->exec_future_done_fence == NULL,
	    ("nvgpu fence EXEC producer already set"));
	nvgpu_fence_addref(future_done);
	gpu_complete->exec_future_done_fence = future_done;
	gpu_complete->exec_channel = channel;
	gpu_complete->exec_producer = true;
}

struct dma_fence *
nvgpu_fence_addref_for_exec_wait(struct dma_fence *dma,
    uint32_t channel)
{
	struct nvgpu_fence *gpu_complete;
	struct nvgpu_fence *wait;

	if (dma == NULL)
		return (NULL);
	if (dma->ops != &nvgpu_fence_ops)
		return (dma_fence_get(dma));
	gpu_complete = container_of(dma, struct nvgpu_fence, base);
	wait = gpu_complete;
	if (gpu_complete->exec_producer &&
	    gpu_complete->exec_channel == channel &&
	    gpu_complete->exec_future_done_fence != NULL)
		wait = gpu_complete->exec_future_done_fence;
	return (dma_fence_get(&wait->base));
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
nvgpu_fence_signal(struct nvgpu_fence *fence, int error)
{
	int result;

	if (fence == NULL)
		return (EINVAL);
	if (error != 0)
		dma_fence_set_error(&fence->base, error > 0 ? -error : error);
	result = dma_fence_signal(&fence->base);
	wakeup(&fence->base);
	return (result);
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
