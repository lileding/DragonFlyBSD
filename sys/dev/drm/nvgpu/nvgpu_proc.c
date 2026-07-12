/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 */

#include "nvgpu_channel.h"
#include "nvgpu_channel_internal.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_fence.h"
#include "nvgpu_future.h"
#include "nvgpu_future_internal.h"
#include "nvgpu_intr_internal.h"
#include "nvgpu_proc.h"
#include "nvgpu_proc_internal.h"
#include "nvgpu_sched.h"
#include "nvgpu_unload.h"
#include "nvgpu_vm.h"

#include <linux/reservation.h>
#include <machine/atomic.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/systm.h>

static MALLOC_DEFINE(M_NVGPU_PROC, "nvgpu_proc", "nvgpu process state");
static MALLOC_DEFINE(M_NVGPU_EXEC, "nvgpu_exec", "nvgpu exec future");

struct nvgpu_proc_exec_record {
	TAILQ_ENTRY(nvgpu_proc_exec_record) link;
	struct nvgpu_fence *done;
	struct nvgpu_fence *submitted;
	u_int channel_id;
};
TAILQ_HEAD(nvgpu_proc_exec_list, nvgpu_proc_exec_record);

struct nvgpu_proc {
	struct nvgpu_device *device;
	struct lwkt_token token;
	volatile u_int refs;
	struct nvgpu_channel_list channels;
	struct nvgpu_proc_exec_list execs;
	struct nvgpu_fence *last_bind;
	struct reservation_object vm_resv;
	struct nvgpu_vm *vm;
};

struct nvgpu_exec_future {
	struct nvgpu_future base;
	struct nvgpu_proc *proc;
	struct nvgpu_channel *channel;
	struct nvgpu_proc_exec_record record;
	struct nvgpu_fence *done;
	struct nvgpu_fence *submitted;
	struct nvgpu_sema sema;
	struct nvgpu_channel_push *pushes;
	size_t push_count;
};

static void nvgpu_proc_finalize(struct nvgpu_proc *proc);
static struct nvgpu_future_result nvgpu_proc_poll_exec(
	struct nvgpu_future *future);

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
	return (proc != NULL ? proc->device : NULL);
}

struct nvgpu_channel_list *
nvgpu_proc_get_channels(struct nvgpu_proc *proc)
{
	return (proc != NULL ? &proc->channels : NULL);
}

struct nvgpu_vm *
nvgpu_proc_get_vm(struct nvgpu_proc *proc)
{
	return (proc != NULL ? proc->vm : NULL);
}

void
nvgpu_proc_set_vm(struct nvgpu_proc *proc, struct nvgpu_vm *vm)
{
	if (proc != NULL)
		proc->vm = vm;
}

struct reservation_object *
nvgpu_proc_get_vm_resv(struct nvgpu_proc *proc)
{
	return (proc != NULL ? &proc->vm_resv : NULL);
}

int
nvgpu_proc_create(struct nvgpu_device *device, struct nvgpu_proc **result)
{
	struct nvgpu_proc *proc;

	if (device == NULL || result == NULL)
		return (EINVAL);
	proc = kmalloc(sizeof(*proc), M_NVGPU_PROC, M_WAITOK | M_ZERO);
	proc->device = device;
	proc->refs = 1;
	lwkt_token_init(&proc->token, "nvgprc");
	TAILQ_INIT(&proc->channels);
	TAILQ_INIT(&proc->execs);
	reservation_object_init(&proc->vm_resv);
	*result = proc;
	nvgpu_log(NVGPU_LOG_DEBUG, "proc created proc=%p\n", proc);
	return (0);
}

void
nvgpu_proc_addref(struct nvgpu_proc *proc)
{
	if (proc != NULL)
		atomic_fetchadd_int(&proc->refs, 1);
}

void
nvgpu_proc_release(struct nvgpu_proc *proc)
{
	u_int refs;

	if (proc == NULL)
		return;
	refs = atomic_fetchadd_int(&proc->refs, -1);
	KASSERT(refs != 0, ("nvgpu proc refs underflow"));
	if (refs == 1)
		nvgpu_proc_finalize(proc);
}

int
nvgpu_proc_spawn(struct nvgpu_proc *proc, struct nvgpu_proc_exec *args)
{
	struct nvgpu_exec_future *exec;
	struct nvgpu_proc_exec_record *record;
	struct nvgpu_fence **waits;
	struct nvgpu_fence *wait;
	size_t wait_count;
	int error;

	if (proc == NULL || args == NULL || args->done == NULL ||
	    (args->push_count != 0 && args->pushes == NULL) ||
	    (args->wait_count != 0 && args->waits == NULL) ||
	    args->push_count > NVGPU_CHANNEL_GPFIFO_ENTRIES - 2)
		return (EINVAL);
	for (size_t i = 0; i < args->push_count; i++) {
		if ((args->pushes[i].flags & ~NVGPU_CHANNEL_PUSH_NO_PREFETCH) != 0 ||
		    ((args->pushes[i].va | args->pushes[i].va_len) & 3) != 0 ||
		    args->pushes[i].va_len == 0 ||
		    args->pushes[i].va_len >= (1u << 23))
			return (EINVAL);
	}
	for (size_t i = 0; i < args->wait_count; i++) {
		if (args->waits[i] == NULL)
			return (EINVAL);
	}

	exec = kmalloc(sizeof(*exec), M_NVGPU_EXEC, M_WAITOK | M_ZERO);
	if (args->push_count != 0) {
		exec->pushes = kmalloc(args->push_count * sizeof(*exec->pushes),
		    M_NVGPU_EXEC, M_WAITOK);
		memcpy(exec->pushes, args->pushes,
		    args->push_count * sizeof(*exec->pushes));
	}
	waits = kmalloc((args->wait_count + 1) * sizeof(*waits),
	    M_NVGPU_EXEC, M_WAITOK | M_ZERO);
	exec->base.poll = nvgpu_proc_poll_exec;
	exec->proc = proc;
	exec->done = args->done;
	exec->submitted = nvgpu_fence_create();
	if (exec->submitted == NULL) {
		_kfree(waits, M_NVGPU_EXEC);
		if (exec->pushes != NULL)
			_kfree(exec->pushes, M_NVGPU_EXEC);
		_kfree(exec, M_NVGPU_EXEC);
		return (ENOMEM);
	}
	exec->push_count = args->push_count;
	exec->record.done = exec->done;
	exec->record.submitted = exec->submitted;
	exec->record.channel_id = args->channel_id;
	nvgpu_proc_addref(proc);
	nvgpu_fence_addref(exec->done);

	nvgpu_proc_lock(proc);
	exec->channel = nvgpu_channel_borrow_by_id_locked(proc, args->channel_id);
	if (exec->channel == NULL) {
		nvgpu_proc_unlock(proc);
		error = ENOENT;
		goto fail;
	}
	wait_count = 0;
	for (size_t i = 0; i < args->wait_count; i++) {
		wait = args->waits[i];
		TAILQ_FOREACH(record, &proc->execs, link) {
			if (record->channel_id == args->channel_id &&
			    record->done == wait) {
				wait = record->submitted;
				break;
			}
		}
		waits[wait_count++] = wait;
	}
	if (proc->last_bind != NULL)
		waits[wait_count++] = proc->last_bind;
	nvgpu_channel_register_exec_locked(exec->channel);
	TAILQ_INSERT_TAIL(&proc->execs, &exec->record, link);
	error = nvgpu_future_spawn(&exec->base, waits, wait_count);
	if (error != 0) {
		TAILQ_REMOVE(&proc->execs, &exec->record, link);
		KASSERT(!nvgpu_channel_complete_exec_locked(proc, exec->channel),
		    ("failed EXEC spawn retired an active channel"));
	}
	nvgpu_proc_unlock(proc);
	_kfree(waits, M_NVGPU_EXEC);
	waits = NULL;
	if (error == 0) {
		reservation_object_lock(&proc->vm_resv, NULL);
		reservation_object_add_excl_fence(&proc->vm_resv,
		    &args->done->dma);
		reservation_object_unlock(&proc->vm_resv);
		return (0);
	}

fail:
	(void)nvgpu_fence_signal(exec->submitted, error);
	(void)nvgpu_fence_signal(exec->done, error);
	nvgpu_fence_release(exec->submitted);
	nvgpu_fence_release(exec->done);
	nvgpu_proc_release(proc);
	_kfree(waits, M_NVGPU_EXEC);
	if (exec->pushes != NULL)
		_kfree(exec->pushes, M_NVGPU_EXEC);
	_kfree(exec, M_NVGPU_EXEC);
	return (error);
}

static struct nvgpu_future_result
nvgpu_proc_poll_exec(struct nvgpu_future *future)
{
	struct nvgpu_channel_submit_args submit;
	struct nvgpu_exec_future *exec;
	struct nvgpu_proc *proc;
	bool destroy_channel;
	int error;

	exec = (struct nvgpu_exec_future *)future;
	error = nvgpu_future_get_wait_error(future);
	if (error != 0) {
		(void)nvgpu_fence_signal(exec->submitted, error);
		goto complete;
	}
	if (exec->sema.address == NULL) {
		submit.pushes = exec->pushes;
		submit.push_count = exec->push_count;
		submit.submitted = exec->submitted;
		submit.sema = &exec->sema;
		error = nvgpu_channel_submit(exec->channel, &submit, future);
		if (error == 0)
			return (NVGPU_FUTURE_PENDING);
		if (error == EAGAIN) {
			error = nvgpu_sched_put(future);
			if (error == 0)
				return (NVGPU_FUTURE_PENDING);
		}
		(void)nvgpu_fence_signal(exec->submitted, error);
	} else {
		error = exec->sema.error;
	}

complete:
	proc = exec->proc;
	nvgpu_proc_lock(proc);
	TAILQ_REMOVE(&proc->execs, &exec->record, link);
	destroy_channel = nvgpu_channel_complete_exec_locked(proc, exec->channel);
	nvgpu_proc_unlock(proc);
	(void)nvgpu_fence_signal(exec->done, error);
	if (destroy_channel)
		nvgpu_channel_destroy(exec->channel);
	nvgpu_fence_release(exec->submitted);
	nvgpu_fence_release(exec->done);
	nvgpu_proc_release(proc);
	if (exec->pushes != NULL)
		_kfree(exec->pushes, M_NVGPU_EXEC);
	_kfree(exec, M_NVGPU_EXEC);
	return (NVGPU_FUTURE_READY(error));
}

int
nvgpu_proc_register_bind(struct nvgpu_proc *proc, struct nvgpu_fence *done,
	struct nvgpu_fence **waits, uint32_t capacity, uint32_t *wait_count)
{
	struct nvgpu_fence *previous;
	uint32_t required;

	if (proc == NULL || done == NULL || wait_count == NULL ||
	    (capacity != 0 && waits == NULL))
		return (EINVAL);
	nvgpu_proc_lock(proc);
	required = proc->last_bind != NULL ? 1 : 0;
	if (capacity < required) {
		*wait_count = required;
		nvgpu_proc_unlock(proc);
		return (ENOSPC);
	}
	if (proc->last_bind != NULL) {
		nvgpu_fence_addref(proc->last_bind);
		waits[0] = proc->last_bind;
	}
	previous = proc->last_bind;
	nvgpu_fence_addref(done);
	proc->last_bind = done;
	*wait_count = required;
	nvgpu_proc_unlock(proc);
	nvgpu_fence_release(previous);
	return (0);
}

void
nvgpu_proc_complete_bind(struct nvgpu_proc *proc, struct nvgpu_fence *done)
{
	struct nvgpu_fence *last;

	if (proc == NULL || done == NULL)
		return;
	last = NULL;
	nvgpu_proc_lock(proc);
	if (proc->last_bind == done) {
		last = proc->last_bind;
		proc->last_bind = NULL;
	}
	nvgpu_proc_unlock(proc);
	nvgpu_fence_release(last);
}

static void
nvgpu_proc_finalize(struct nvgpu_proc *proc)
{
	KASSERT(TAILQ_EMPTY(&proc->execs),
	    ("finalizing proc with active EXEC futures"));
	KASSERT(proc->last_bind == NULL,
	    ("finalizing proc with active remap future"));
	nvgpu_channel_destroy_all(proc);
	nvgpu_vm_destroy(proc);
	reservation_object_fini(&proc->vm_resv);
	nvgpu_unload_release_by_drm(proc->device);
	nvgpu_log(NVGPU_LOG_DEBUG, "proc finalized proc=%p\n", proc);
	lwkt_token_uninit(&proc->token);
	_kfree(proc, M_NVGPU_PROC);
}
