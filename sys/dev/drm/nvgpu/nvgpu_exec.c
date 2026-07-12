/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * EXEC future and GPU completion boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_exec.h"
#include "nvgpu_channel.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_fence.h"
#include "nvgpu_future.h"
#include "nvgpu_proc.h"
#include "nvgpu_sched.h"
#include "nvgsp_channel.h"

#include <linux/dma-fence.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/spinlock.h>
#include <sys/spinlock2.h>
#include <sys/systm.h>

static MALLOC_DEFINE(M_NVGPU_EXEC, "nvgpu_exec", "nvgpu exec state");

struct nvgpu_exec_pending {
	TAILQ_ENTRY(nvgpu_exec_pending) link;
	struct nvgpu_fence *gpu_complete_fence;
	volatile uint32_t *sema;
	uint32_t payload;
	uint32_t chid;
};
TAILQ_HEAD(nvgpu_exec_pending_list, nvgpu_exec_pending);

struct nvgpu_exec_state {
	struct nvgpu_device *gpu;
	struct spinlock pending_spin;
	struct nvgpu_exec_pending_list pending;
};

struct nvgpu_exec_future {
	struct nvgpu_future base;
	struct nvgpu_fence *gpu_complete_fence;
	struct nvgpu_channel *channel;
	struct nvgpu_exec_pending *pending;
	struct nvgsp_channel_push *pushes;
	uint64_t seqno;
	uint32_t push_count;
};

static volatile uint64_t nvgpu_exec_seqno;

static struct nvgpu_future_result nvgpu_exec_future_poll(
    struct nvgpu_future *future);
static void nvgpu_exec_future_destroy(struct nvgpu_future *future);

int
nvgpu_exec_init(struct nvgpu_device *gpu)
{
	struct nvgpu_exec_state *state;

	if (gpu == NULL)
		return (EINVAL);
	state = kmalloc(sizeof(*state), M_NVGPU_EXEC, M_WAITOK | M_ZERO);
	state->gpu = gpu;
	spin_init(&state->pending_spin, "nvgpu pending fences");
	TAILQ_INIT(&state->pending);
	nvgpu_device_set_exec_state(gpu, state);
	return (0);
}

void
nvgpu_exec_fini(struct nvgpu_device *gpu)
{
	struct nvgpu_exec_state *state;

	if (gpu == NULL)
		return;
	state = nvgpu_device_get_exec_state(gpu);
	if (state == NULL)
		return;
	spin_lock(&state->pending_spin);
	KASSERT(TAILQ_EMPTY(&state->pending),
	    ("destroying EXEC state with pending GPU submissions"));
	spin_unlock(&state->pending_spin);
	nvgpu_device_set_exec_state(gpu, NULL);
	spin_uninit(&state->pending_spin);
	_kfree(state, M_NVGPU_EXEC);
}

int
nvgpu_exec_submit(struct nvgpu_proc *proc,
    struct nvgpu_exec_submit_args *args)
{
	struct nvgpu_exec_future *exec;
	struct dma_fence **waits;
	struct nvgpu_fence *bind_wait;
	struct nvgpu_fence *gpu_complete_fence;
	struct nvgpu_fence *done_fence;
	uint32_t wait_count;
	int error;

	if (args == NULL)
		return (EINVAL);
	gpu_complete_fence = args->gpu_complete_fence;
	args->gpu_complete_fence = NULL;
	if (proc == NULL || gpu_complete_fence == NULL ||
	    (args->push_count != 0 && args->pushes == NULL) ||
	    (args->wait_count != 0 && args->wait_fences == NULL) ||
	    args->push_count > NVGPU_CHANNEL_GPFIFO_ENTRIES - 2) {
		error = EINVAL;
		goto fail_fence;
	}
	for (uint32_t i = 0; i < args->push_count; i++) {
		if ((args->pushes[i].flags & ~NVGPU_EXEC_PUSH_NO_PREFETCH) != 0 ||
		    ((args->pushes[i].va | args->pushes[i].va_len) & 3) != 0 ||
		    args->pushes[i].va_len == 0 ||
		    args->pushes[i].va_len >= (1u << 23)) {
			error = EINVAL;
			goto fail_fence;
		}
	}
	for (uint32_t i = 0; i < args->wait_count; i++) {
		if (args->wait_fences[i] == NULL) {
			error = EINVAL;
			goto fail_fence;
		}
	}

	exec = kmalloc(sizeof(*exec), M_NVGPU_EXEC, M_WAITOK | M_ZERO);
	exec->pending = kmalloc(sizeof(*exec->pending), M_NVGPU_EXEC,
	    M_WAITOK | M_ZERO);
	if (args->push_count != 0) {
		exec->pushes = kmalloc((size_t)args->push_count *
		    sizeof(*exec->pushes), M_NVGPU_EXEC, M_WAITOK);
		for (uint32_t i = 0; i < args->push_count; i++) {
			exec->pushes[i].va = args->pushes[i].va;
			exec->pushes[i].va_len = args->pushes[i].va_len;
			exec->pushes[i].flags = args->pushes[i].flags;
		}
	}
	exec->push_count = args->push_count;
	exec->seqno = atomic_fetchadd_64(&nvgpu_exec_seqno, 1) + 1;
	done_fence = nvgpu_fence_create(nvgpu_proc_get_device(proc),
	    "exec-future");
	if (done_fence == NULL) {
		error = ENOMEM;
		goto fail_exec;
	}
	nvgpu_fence_set_exec_origin(gpu_complete_fence, args->channel,
	    done_fence);

	bind_wait = NULL;
	error = nvgpu_proc_register_exec(proc, gpu_complete_fence, args->channel,
	    &bind_wait, &exec->channel);
	if (error != 0) {
		nvgpu_fence_release(done_fence);
		goto fail_exec;
	}
	exec->gpu_complete_fence = gpu_complete_fence;
	gpu_complete_fence = NULL;
	wait_count = args->wait_count + (bind_wait != NULL ? 1 : 0);
	waits = NULL;
	if (wait_count != 0) {
		waits = kmalloc((size_t)wait_count * sizeof(*waits),
		    M_NVGPU_EXEC, M_WAITOK | M_ZERO);
		for (uint32_t i = 0; i < args->wait_count; i++)
			waits[i] = nvgpu_fence_addref_for_exec_wait(args->wait_fences[i],
			    args->channel);
		if (bind_wait != NULL)
			waits[args->wait_count] =
			    nvgpu_fence_addref_as_dma(bind_wait);
	}
	error = nvgpu_future_spawn(&exec->base, proc, done_fence, waits,
	    wait_count, nvgpu_exec_future_poll, nvgpu_exec_future_destroy);
	if (waits != NULL) {
		for (uint32_t i = 0; i < wait_count; i++)
			dma_fence_put(waits[i]);
		_kfree(waits, M_NVGPU_EXEC);
	}
	nvgpu_fence_release(bind_wait);
	if (error != 0)
		nvgpu_future_finish(&exec->base, error);
	return (error);

fail_exec:
	if (exec->pushes != NULL)
		_kfree(exec->pushes, M_NVGPU_EXEC);
	_kfree(exec->pending, M_NVGPU_EXEC);
	_kfree(exec, M_NVGPU_EXEC);
fail_fence:
	if (gpu_complete_fence != NULL) {
		(void)nvgpu_fence_signal(gpu_complete_fence, error);
		nvgpu_fence_release(gpu_complete_fence);
	}
	return (error);
}

void
nvgpu_exec_harvest_completed(struct nvgpu_device *gpu)
{
	struct nvgpu_exec_pending_list completed;
	struct nvgpu_exec_pending *pending, *next;
	struct nvgpu_exec_state *state;

	state = nvgpu_device_get_exec_state(gpu);
	if (state == NULL)
		return;
	TAILQ_INIT(&completed);
	spin_lock(&state->pending_spin);
	for (pending = TAILQ_FIRST(&state->pending); pending != NULL;
	    pending = next) {
		uint32_t value;

		next = TAILQ_NEXT(pending, link);
		cpu_lfence();
		value = *pending->sema;
		if ((int32_t)(value - pending->payload) < 0)
			continue;
		TAILQ_REMOVE(&state->pending, pending, link);
		TAILQ_INSERT_TAIL(&completed, pending, link);
	}
	spin_unlock(&state->pending_spin);

	while ((pending = TAILQ_FIRST(&completed)) != NULL) {
		TAILQ_REMOVE(&completed, pending, link);
		(void)nvgpu_fence_signal(pending->gpu_complete_fence, 0);
		nvgpu_fence_release(pending->gpu_complete_fence);
		_kfree(pending, M_NVGPU_EXEC);
	}
}

void
nvgpu_exec_fail_channel(struct nvgpu_device *gpu, uint32_t chid, int error)
{
	struct nvgpu_exec_pending_list failed;
	struct nvgpu_exec_pending *pending, *next;
	struct nvgpu_exec_state *state;

	state = nvgpu_device_get_exec_state(gpu);
	if (state == NULL)
		return;
	nvgsp_channel_mark_fault(gpu, chid, error);
	TAILQ_INIT(&failed);
	spin_lock(&state->pending_spin);
	for (pending = TAILQ_FIRST(&state->pending); pending != NULL;
	    pending = next) {
		next = TAILQ_NEXT(pending, link);
		if (pending->chid != chid)
			continue;
		TAILQ_REMOVE(&state->pending, pending, link);
		TAILQ_INSERT_TAIL(&failed, pending, link);
	}
	spin_unlock(&state->pending_spin);

	while ((pending = TAILQ_FIRST(&failed)) != NULL) {
		TAILQ_REMOVE(&failed, pending, link);
		(void)nvgpu_fence_signal(pending->gpu_complete_fence, error);
		nvgpu_fence_release(pending->gpu_complete_fence);
		_kfree(pending, M_NVGPU_EXEC);
	}
}

static struct nvgpu_future_result
nvgpu_exec_future_poll(struct nvgpu_future *future)
{
	struct nvgpu_exec_future *exec;
	struct nvgpu_exec_state *state;
	struct nvgsp_channel_completion completion;
	struct nvgsp_channel_submission *submission;
	struct nvgpu_future_result result;
	int error;

	exec = (struct nvgpu_exec_future *)future;
	state = nvgpu_device_get_exec_state(nvgpu_proc_get_device(future->proc));
	if (state == NULL) {
		error = ENODEV;
		goto fail;
	}
	error = nvgsp_channel_prepare_submit(
	    nvgpu_channel_get_backend(exec->channel), exec->pushes,
	    exec->push_count, &submission);
	if (error == EAGAIN) {
		nvgpu_future_wake(future);
		result.ready = false;
		result.result = 0;
		return (result);
	}
	if (error != 0)
		goto fail;

	nvgsp_channel_describe_submit(submission, &completion);
	exec->pending->gpu_complete_fence = exec->gpu_complete_fence;
	exec->pending->sema = completion.sema;
	exec->pending->payload = completion.payload;
	exec->pending->chid = completion.chid;
	exec->gpu_complete_fence = NULL;
	spin_lock(&state->pending_spin);
	TAILQ_INSERT_TAIL(&state->pending, exec->pending, link);
	spin_unlock(&state->pending_spin);
	exec->pending = NULL;
	nvgsp_channel_commit_submit(submission);
	nvgpu_log(NVGPU_LOG_DEBUG, "exec submitted seq=%ju pushes=%u\n",
	    (uintmax_t)exec->seqno, exec->push_count);
	result.ready = true;
	result.result = 0;
	return (result);

fail:
	result.ready = true;
	result.result = error;
	return (result);
}

static void
nvgpu_exec_future_destroy(struct nvgpu_future *future)
{
	struct nvgpu_exec_future *exec;

	exec = (struct nvgpu_exec_future *)future;
	if (exec->gpu_complete_fence != NULL) {
		int error = future->error != 0 ? (int)future->error : ENODEV;

		(void)nvgpu_fence_signal(exec->gpu_complete_fence, error);
		nvgpu_fence_release(exec->gpu_complete_fence);
	}
	if (exec->pushes != NULL)
		_kfree(exec->pushes, M_NVGPU_EXEC);
	if (exec->pending != NULL)
		_kfree(exec->pending, M_NVGPU_EXEC);
	_kfree(exec, M_NVGPU_EXEC);
}
