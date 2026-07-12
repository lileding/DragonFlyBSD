/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 */

#include "nvgpu_proc.h"
#include "nvgpu_channel.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_fence.h"
#include "nvgpu_future.h"
#include "nvgpu_unload.h"
#include "nvgpu_vm.h"

#include <machine/atomic.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <stdbool.h>

#include <linux/dma-fence.h>
#include <linux/reservation.h>

static MALLOC_DEFINE(M_NVGPU_PROC, "nvgpu_proc", "nvgpu process state");

struct nvgpu_proc_gpu_fence {
	TAILQ_ENTRY(nvgpu_proc_gpu_fence) link;
	struct nvgpu_fence *fence;
	uint32_t channel_id;
};
TAILQ_HEAD(nvgpu_proc_gpu_fence_list, nvgpu_proc_gpu_fence);

struct nvgpu_gpu_complete_future {
	struct nvgpu_future base;
	struct nvgpu_proc_gpu_fence *proc_fence;
	int gpu_error;
};

struct nvgpu_gpu_complete_callback {
	struct dma_fence_cb cb;
	struct dma_fence *fence;
	struct nvgpu_gpu_complete_future *complete;
};

struct nvgpu_proc {
	struct nvgpu_device *gpu;
	struct lwkt_token token;
	uint32_t refs;
	struct nvgpu_channel_list channels;
	struct nvgpu_proc_gpu_fence_list gpu_fences;
	struct nvgpu_fence *last_bind_fence;
	struct reservation_object vm_resv;
	struct nvgpu_vm *vm;
	bool shutdown;
};

static void nvgpu_proc_finalize(struct nvgpu_proc *proc);
static struct nvgpu_future_result nvgpu_proc_gpu_complete_poll(
    struct nvgpu_future *future);
static void nvgpu_proc_gpu_complete_destroy(struct nvgpu_future *future);
static void nvgpu_proc_gpu_complete_cb(struct dma_fence *fence,
    struct dma_fence_cb *cb);

void
nvgpu_proc_lock(struct nvgpu_proc *proc)
{
	lwkt_gettoken(&proc->token);
}

void
nvgpu_proc_unlock(struct nvgpu_proc *proc)
{
	lwkt_reltoken(&proc->token);
}

struct nvgpu_device *
nvgpu_proc_get_device(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return (NULL);
	return (proc->gpu);
}

struct nvgpu_channel_list *
nvgpu_proc_get_channels(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return (NULL);
	return (&proc->channels);
}

struct nvgpu_vm *
nvgpu_proc_get_vm(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return (NULL);
	return (proc->vm);
}

void
nvgpu_proc_set_vm(struct nvgpu_proc *proc, struct nvgpu_vm *vm)
{
	if (proc == NULL)
		return;
	proc->vm = vm;
}

int
nvgpu_proc_create(struct nvgpu_device *gpu, struct nvgpu_proc **procp)
{
	struct nvgpu_proc *proc;

	if (gpu == NULL || procp == NULL)
		return (EINVAL);

	proc = kmalloc(sizeof(*proc), M_NVGPU_PROC, M_WAITOK | M_ZERO);

	proc->gpu = gpu;
	lwkt_token_init(&proc->token, "nvgprc");
	proc->refs = 1;
	TAILQ_INIT(&proc->channels);
	TAILQ_INIT(&proc->gpu_fences);
	proc->last_bind_fence = NULL;
	reservation_object_init(&proc->vm_resv);
	proc->vm = NULL;
	proc->shutdown = false;

	*procp = proc;
	nvgpu_log(NVGPU_LOG_DEBUG, "proc created proc=%p\n", proc);
	return (0);
}

struct reservation_object *
nvgpu_proc_get_vm_resv(struct nvgpu_proc *proc)
{
	return (proc != NULL ? &proc->vm_resv : NULL);
}

void
nvgpu_proc_stop(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return;

	lwkt_gettoken(&proc->token);
	proc->shutdown = true;
	lwkt_reltoken(&proc->token);
	nvgpu_log(NVGPU_LOG_DEBUG, "proc stop requested proc=%p\n", proc);
	nvgpu_proc_release(proc);
}

void
nvgpu_proc_hold(struct nvgpu_proc *proc)
{
	if (proc == NULL)
		return;
	lwkt_gettoken(&proc->token);
	proc->refs++;
	lwkt_reltoken(&proc->token);
}

void
nvgpu_proc_release(struct nvgpu_proc *proc)
{
	bool destroy = false;

	if (proc == NULL)
		return;
	lwkt_gettoken(&proc->token);
	KASSERT(proc->refs > 0, ("nvgpu proc refs underflow"));
	proc->refs--;
	if (proc->shutdown && proc->refs == 0)
		destroy = true;
	lwkt_reltoken(&proc->token);
	if (destroy)
		nvgpu_proc_finalize(proc);
}

int
nvgpu_proc_register_exec(struct nvgpu_proc *proc,
    struct nvgpu_fence *gpu_complete_fence, uint32_t channel_id,
    struct nvgpu_fence **bind_wait_fence, struct nvgpu_channel **channelp)
{
	struct nvgpu_gpu_complete_callback *callback;
	struct nvgpu_gpu_complete_future *complete;
	struct dma_fence *dma;
	struct nvgpu_proc_gpu_fence *proc_fence;
	struct nvgpu_channel *channel;
	struct nvgpu_fence *done_fence;
	int error;

	if (proc == NULL || gpu_complete_fence == NULL ||
	    bind_wait_fence == NULL || channelp == NULL)
		return (EINVAL);
	*bind_wait_fence = NULL;
	*channelp = NULL;
	proc_fence = kmalloc(sizeof(*proc_fence), M_NVGPU_PROC,
	    M_WAITOK | M_ZERO);
	complete = kmalloc(sizeof(*complete), M_NVGPU_PROC,
	    M_WAITOK | M_ZERO);
	callback = kmalloc(sizeof(*callback), M_NVGPU_PROC,
	    M_WAITOK | M_ZERO);
	done_fence = nvgpu_fence_create(proc->gpu, "gpu-complete-cleanup");
	if (done_fence == NULL) {
		_kfree(callback, M_NVGPU_PROC);
		_kfree(complete, M_NVGPU_PROC);
		_kfree(proc_fence, M_NVGPU_PROC);
		return (ENOMEM);
	}
	proc_fence->fence = gpu_complete_fence;
	proc_fence->channel_id = channel_id;
	nvgpu_fence_addref(gpu_complete_fence);
	complete->base.poll = nvgpu_proc_gpu_complete_poll;
	complete->base.destroy = nvgpu_proc_gpu_complete_destroy;
	complete->base.proc = proc;
	complete->base.done_fence = done_fence;
	complete->proc_fence = proc_fence;
	nvgpu_proc_hold(proc);
	callback->fence = nvgpu_fence_addref_as_dma(gpu_complete_fence);
	callback->complete = complete;

	lwkt_gettoken(&proc->token);
	if (proc->shutdown) {
		lwkt_reltoken(&proc->token);
		dma_fence_put(callback->fence);
		_kfree(callback, M_NVGPU_PROC);
		nvgpu_fence_release(gpu_complete_fence);
		complete->proc_fence = NULL;
		nvgpu_future_finish(&complete->base, ENODEV);
		_kfree(proc_fence, M_NVGPU_PROC);
		return (ENODEV);
	}
	channel = nvgpu_channel_borrow_by_id_locked(proc, channel_id);
	if (channel == NULL) {
		lwkt_reltoken(&proc->token);
		dma_fence_put(callback->fence);
		_kfree(callback, M_NVGPU_PROC);
		nvgpu_fence_release(gpu_complete_fence);
		complete->proc_fence = NULL;
		nvgpu_future_finish(&complete->base, ENOENT);
		_kfree(proc_fence, M_NVGPU_PROC);
		return (ENOENT);
	}
	if (proc->last_bind_fence != NULL) {
		nvgpu_fence_addref(proc->last_bind_fence);
		*bind_wait_fence = proc->last_bind_fence;
	}
	TAILQ_INSERT_TAIL(&proc->gpu_fences, proc_fence, link);
	*channelp = channel;
	lwkt_reltoken(&proc->token);
	error = dma_fence_add_callback(callback->fence, &callback->cb,
	    nvgpu_proc_gpu_complete_cb);
	if (error != 0) {
		complete->gpu_error = callback->fence->error;
		if (complete->gpu_error < 0)
			complete->gpu_error = -complete->gpu_error;
		dma_fence_put(callback->fence);
		_kfree(callback, M_NVGPU_PROC);
		nvgpu_future_wake(&complete->base);
	}
	dma = nvgpu_fence_addref_as_dma(gpu_complete_fence);
	KASSERT(dma != NULL, ("EXEC completion fence has no dma fence"));
	reservation_object_lock(&proc->vm_resv, NULL);
	reservation_object_add_excl_fence(&proc->vm_resv, dma);
	reservation_object_unlock(&proc->vm_resv);
	dma_fence_put(dma);
	return (0);
}

int
nvgpu_proc_register_channel_release(struct nvgpu_proc *proc,
    uint32_t channel_id, struct nvgpu_channel **channelp,
    struct nvgpu_fence **wait_fences, uint32_t capacity,
    uint32_t *wait_count)
{
	struct nvgpu_proc_gpu_fence *gpu_fence;
	struct nvgpu_channel *channel;
	uint32_t required;
	uint32_t count;

	if (proc == NULL || channelp == NULL || wait_count == NULL ||
	    (capacity != 0 && wait_fences == NULL))
		return (EINVAL);
	*channelp = NULL;
	lwkt_gettoken(&proc->token);
	if (proc->shutdown) {
		lwkt_reltoken(&proc->token);
		return (ENODEV);
	}
	channel = nvgpu_channel_borrow_by_id_locked(proc, channel_id);
	if (channel == NULL) {
		lwkt_reltoken(&proc->token);
		return (ENOENT);
	}
	required = 0;
	TAILQ_FOREACH(gpu_fence, &proc->gpu_fences, link) {
		if (gpu_fence->channel_id == channel_id)
			required++;
	}
	if (capacity < required) {
		*wait_count = required;
		lwkt_reltoken(&proc->token);
		return (ENOSPC);
	}
	count = 0;
	TAILQ_FOREACH(gpu_fence, &proc->gpu_fences, link) {
		if (gpu_fence->channel_id != channel_id)
			continue;
		nvgpu_fence_addref(gpu_fence->fence);
		wait_fences[count++] = gpu_fence->fence;
	}
	nvgpu_channel_close_locked(channel);
	*channelp = channel;
	*wait_count = count;
	lwkt_reltoken(&proc->token);
	return (0);
}

static struct nvgpu_future_result
nvgpu_proc_gpu_complete_poll(struct nvgpu_future *future)
{
	struct nvgpu_gpu_complete_future *complete;
	struct nvgpu_future_result result;
	struct nvgpu_proc_gpu_fence *proc_fence;
	struct nvgpu_proc *proc;

	complete = (struct nvgpu_gpu_complete_future *)future;
	proc = future->proc;
	proc_fence = complete->proc_fence;
	lwkt_gettoken(&proc->token);
	TAILQ_REMOVE(&proc->gpu_fences, proc_fence, link);
	lwkt_reltoken(&proc->token);
	complete->proc_fence = NULL;
	nvgpu_fence_release(proc_fence->fence);
	_kfree(proc_fence, M_NVGPU_PROC);
	result.ready = true;
	result.result = complete->gpu_error;
	return (result);
}

static void
nvgpu_proc_gpu_complete_destroy(struct nvgpu_future *future)
{
	struct nvgpu_gpu_complete_future *complete;

	complete = (struct nvgpu_gpu_complete_future *)future;
	KASSERT(complete->proc_fence == NULL,
	    ("destroying GPU-complete future before proc cleanup"));
	_kfree(complete, M_NVGPU_PROC);
}

static void
nvgpu_proc_gpu_complete_cb(struct dma_fence *fence,
    struct dma_fence_cb *cb)
{
	struct nvgpu_gpu_complete_callback *callback;
	struct nvgpu_gpu_complete_future *complete;
	int error;

	callback = container_of(cb, struct nvgpu_gpu_complete_callback, cb);
	complete = callback->complete;
	error = fence->error;
	complete->gpu_error = error < 0 ? -error : error;
	dma_fence_put(callback->fence);
	_kfree(callback, M_NVGPU_PROC);
	nvgpu_future_wake(&complete->base);
}

int
nvgpu_proc_register_bind(struct nvgpu_proc *proc,
    struct nvgpu_fence *done_fence, struct nvgpu_fence **wait_fences,
    uint32_t capacity, uint32_t *wait_count)
{
	struct nvgpu_proc_gpu_fence *gpu_fence;
	struct nvgpu_fence *previous;
	uint32_t required;
	uint32_t count;

	if (proc == NULL || done_fence == NULL || wait_count == NULL ||
	    (capacity != 0 && wait_fences == NULL))
		return (EINVAL);
	lwkt_gettoken(&proc->token);
	if (proc->shutdown) {
		lwkt_reltoken(&proc->token);
		return (ENODEV);
	}
	required = proc->last_bind_fence != NULL ? 1 : 0;
	TAILQ_FOREACH(gpu_fence, &proc->gpu_fences, link)
		required++;
	if (capacity < required) {
		*wait_count = required;
		lwkt_reltoken(&proc->token);
		return (ENOSPC);
	}
	count = 0;
	if (proc->last_bind_fence != NULL) {
		nvgpu_fence_addref(proc->last_bind_fence);
		wait_fences[count++] = proc->last_bind_fence;
	}
	TAILQ_FOREACH(gpu_fence, &proc->gpu_fences, link) {
		nvgpu_fence_addref(gpu_fence->fence);
		wait_fences[count++] = gpu_fence->fence;
	}
	previous = proc->last_bind_fence;
	nvgpu_fence_addref(done_fence);
	proc->last_bind_fence = done_fence;
	*wait_count = count;
	lwkt_reltoken(&proc->token);
	nvgpu_fence_release(previous);
	return (0);
}

void
nvgpu_proc_complete_bind(struct nvgpu_proc *proc,
    struct nvgpu_fence *done_fence)
{
	struct nvgpu_fence *last;

	if (proc == NULL || done_fence == NULL)
		return;
	last = NULL;
	lwkt_gettoken(&proc->token);
	if (proc->last_bind_fence == done_fence) {
		last = proc->last_bind_fence;
		proc->last_bind_fence = NULL;
	}
	lwkt_reltoken(&proc->token);
	nvgpu_fence_release(last);
}

static void
nvgpu_proc_finalize(struct nvgpu_proc *proc)
{
	KASSERT(TAILQ_EMPTY(&proc->gpu_fences),
	    ("finalizing proc with inflight EXECs"));
	KASSERT(proc->last_bind_fence == NULL,
	    ("finalizing proc with an unfinished VM_BIND"));
	nvgpu_channel_release_all(proc);
	nvgpu_vm_destroy(proc);
	reservation_object_fini(&proc->vm_resv);
	nvgpu_unload_release_by_drm(proc->gpu);
	nvgpu_log(NVGPU_LOG_DEBUG, "proc finalize proc=%p\n", proc);
	lwkt_token_uninit(&proc->token);
	_kfree(proc, M_NVGPU_PROC);
}
