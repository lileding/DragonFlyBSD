/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private dma_fence implementation for nvgpu futures.
 */

#include "nvgpu_fence.h"

#include <linux/dma-fence.h>
#include <sys/errno.h>
#include <sys/lock.h>
#include <sys/malloc.h>

static MALLOC_DEFINE(M_NVGPU_FENCE, "nvgpu_fence", "nvgpu future fence");

struct nvgpu_fence {
	struct dma_fence base;
	spinlock_t lock;
	const char *timeline_name;
};

static const char *nvgpu_fence_get_driver_name(struct dma_fence *fence);
static const char *nvgpu_fence_get_timeline_name(struct dma_fence *fence);
static void nvgpu_fence_release(struct dma_fence *fence);

static const struct dma_fence_ops nvgpu_fence_ops = {
	.get_driver_name = nvgpu_fence_get_driver_name,
	.get_timeline_name = nvgpu_fence_get_timeline_name,
	.release = nvgpu_fence_release,
};

static const char *
nvgpu_fence_get_driver_name(struct dma_fence *fence __unused)
{
	return ("nvgpu");
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
nvgpu_fence_release(struct dma_fence *fence)
{
	struct nvgpu_fence *nfence;

	nfence = container_of(fence, struct nvgpu_fence, base);
	lockuninit(&nfence->lock);
	_kfree(nfence, M_NVGPU_FENCE);
}

struct dma_fence *
nvgpu_fence_create(struct nvgpu_device *gpu __unused,
    const char *timeline_name)
{
	struct nvgpu_fence *nfence;
	u64 context;

	nfence = kmalloc(sizeof(*nfence), M_NVGPU_FENCE, M_WAITOK | M_ZERO);
	lockinit(&nfence->lock, "nvgpuf", 0, 0);
	nfence->timeline_name = timeline_name;
	context = dma_fence_context_alloc(1);
	dma_fence_init(&nfence->base, &nvgpu_fence_ops, &nfence->lock,
	    context, 1);
	return (&nfence->base);
}

int
nvgpu_fence_signal(struct dma_fence *fence, int error)
{
	if (fence == NULL)
		return (EINVAL);
	if (error != 0)
		dma_fence_set_error(fence, error);
	return (dma_fence_signal(fence));
}
