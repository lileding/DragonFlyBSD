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
#include "nvgpu_unload.h"
#include "nvgpu_vm.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <stdbool.h>

#include <linux/dma-fence.h>
#include <linux/reservation.h>

static MALLOC_DEFINE(M_NVGPU_PROC, "nvgpu_proc", "nvgpu process state");

struct nvgpu_proc_exec {
	TAILQ_ENTRY(nvgpu_proc_exec) link;
	struct nvgpu_proc *proc;
	struct nvgpu_fence *gpu_complete_fence;
};
TAILQ_HEAD(nvgpu_proc_exec_list, nvgpu_proc_exec);

struct nvgpu_proc {
	struct nvgpu_device *gpu;
	struct lwkt_token token;
	uint32_t refs;
	struct nvgpu_channel_list channels;
	struct nvgpu_proc_exec_list inflight_execs;
	struct nvgpu_fence *last_bind_fence;
	struct reservation_object vm_resv;
	struct nvgpu_vm *vm;
	bool shutdown;
};

static void nvgpu_proc_finalize(struct nvgpu_proc *proc);

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
	TAILQ_INIT(&proc->inflight_execs);
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
    struct nvgpu_fence *gpu_complete_fence,
    struct nvgpu_fence **bind_wait_fence,
    struct nvgpu_proc_exec **execp)
{
	struct dma_fence *dma;
	struct nvgpu_proc_exec *exec;

	if (proc == NULL || gpu_complete_fence == NULL ||
	    bind_wait_fence == NULL || execp == NULL)
		return (EINVAL);
	*bind_wait_fence = NULL;
	*execp = NULL;
	exec = kmalloc(sizeof(*exec), M_NVGPU_PROC, M_WAITOK | M_ZERO);
	exec->proc = proc;
	exec->gpu_complete_fence = gpu_complete_fence;
	nvgpu_fence_addref(gpu_complete_fence);
	nvgpu_proc_hold(proc);

	lwkt_gettoken(&proc->token);
	if (proc->shutdown) {
		lwkt_reltoken(&proc->token);
		nvgpu_fence_release(gpu_complete_fence);
		nvgpu_proc_release(proc);
		_kfree(exec, M_NVGPU_PROC);
		return (ENODEV);
	}
	if (proc->last_bind_fence != NULL) {
		nvgpu_fence_addref(proc->last_bind_fence);
		*bind_wait_fence = proc->last_bind_fence;
	}
	TAILQ_INSERT_TAIL(&proc->inflight_execs, exec, link);
	lwkt_reltoken(&proc->token);
	dma = nvgpu_fence_addref_as_dma(gpu_complete_fence);
	KASSERT(dma != NULL, ("EXEC completion fence has no dma fence"));
	reservation_object_lock(&proc->vm_resv, NULL);
	reservation_object_add_excl_fence(&proc->vm_resv, dma);
	reservation_object_unlock(&proc->vm_resv);
	dma_fence_put(dma);
	*execp = exec;
	return (0);
}

void
nvgpu_proc_complete_exec(struct nvgpu_proc_exec *exec, int error)
{
	struct nvgpu_proc *proc;
	struct nvgpu_fence *fence;

	if (exec == NULL)
		return;
	proc = exec->proc;
	fence = exec->gpu_complete_fence;
	lwkt_gettoken(&proc->token);
	TAILQ_REMOVE(&proc->inflight_execs, exec, link);
	lwkt_reltoken(&proc->token);
	(void)nvgpu_fence_signal(fence, error);
	nvgpu_fence_release(fence);
	_kfree(exec, M_NVGPU_PROC);
	nvgpu_proc_release(proc);
}

int
nvgpu_proc_register_bind(struct nvgpu_proc *proc,
    struct nvgpu_fence *done_fence, struct nvgpu_fence **wait_fences,
    uint32_t capacity, uint32_t *wait_count)
{
	struct nvgpu_proc_exec *exec;
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
	TAILQ_FOREACH(exec, &proc->inflight_execs, link)
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
	TAILQ_FOREACH(exec, &proc->inflight_execs, link) {
		nvgpu_fence_addref(exec->gpu_complete_fence);
		wait_fences[count++] = exec->gpu_complete_fence;
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
	KASSERT(TAILQ_EMPTY(&proc->inflight_execs),
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
