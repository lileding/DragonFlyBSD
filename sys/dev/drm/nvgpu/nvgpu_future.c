/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private dependency and ownership state for native futures.
 */

#include "nvgpu_fence.h"
#include "nvgpu_future.h"
#include "nvgpu_sched.h"

#include <machine/atomic.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/spinlock.h>
#include <sys/spinlock2.h>
#include <sys/systm.h>

#define NVGPU_FUTURE_BUCKETS 256u

struct nvgpu_future_wait {
	struct nvgpu_future_state *state;
	struct nvgpu_fence *fence;
};

LIST_HEAD(nvgpu_future_state_list, nvgpu_future_state);

static MALLOC_DEFINE(M_NVGPU_FUTURE, "nvgpu_future",
    "nvgpu private future state");

static struct spinlock nvgpu_future_spin =
    SPINLOCK_INITIALIZER(nvgpu_future_spin, "nvgpu future registry");
static struct nvgpu_future_state_list
    nvgpu_future_registry[NVGPU_FUTURE_BUCKETS];

static struct nvgpu_future_result nvgpu_future_poll(
	struct nvgpu_future *future);
static void nvgpu_future_wait_complete(void *argument);

static u_int
nvgpu_future_bucket(struct nvgpu_future *future)
{
	return (((uintptr_t)future >> 4) & (NVGPU_FUTURE_BUCKETS - 1));
}

struct nvgpu_future_state *
nvgpu_future_get_state(struct nvgpu_future *future)
{
	struct nvgpu_future_state *state;
	u_int bucket;

	if (future == NULL)
		return (NULL);
	bucket = nvgpu_future_bucket(future);
	spin_lock(&nvgpu_future_spin);
	LIST_FOREACH(state, &nvgpu_future_registry[bucket], registry_link) {
		if (state->future == future)
			break;
	}
	spin_unlock(&nvgpu_future_spin);
	return (state);
}

int
nvgpu_future_spawn(struct nvgpu_future *future, struct nvgpu_fence **waits,
	size_t wait_count)
{
	struct nvgpu_future_state *state;
	struct nvgpu_future_wait *wait;
	u_int bucket;
	int error;

	if (future == NULL || future->poll == NULL ||
	    (wait_count != 0 && waits == NULL))
		return (EINVAL);
	for (size_t i = 0; i < wait_count; i++) {
		if (waits[i] == NULL)
			return (EINVAL);
	}
	state = kmalloc(sizeof(*state), M_NVGPU_FUTURE, M_WAITOK | M_ZERO);
	state->future = future;
	state->poll = future->poll;
	state->wait_count = 1;
	future->poll = nvgpu_future_poll;
	bucket = nvgpu_future_bucket(future);
	spin_lock(&nvgpu_future_spin);
	LIST_INSERT_HEAD(&nvgpu_future_registry[bucket], state, registry_link);
	spin_unlock(&nvgpu_future_spin);

	for (size_t i = 0; i < wait_count; i++) {
		wait = kmalloc(sizeof(*wait), M_NVGPU_FUTURE, M_WAITOK | M_ZERO);
		wait->state = state;
		wait->fence = waits[i];
		nvgpu_fence_addref(wait->fence);
		atomic_fetchadd_int(&state->wait_count, 1);
		if (!nvgpu_fence_add_callback_unless_signaled(wait->fence,
		    nvgpu_future_wait_complete, wait)) {
			error = nvgpu_fence_get_error(wait->fence);
			if (error != 0 && error != EINPROGRESS)
				(void)atomic_cmpset_int(&state->wait_error, 0,
				    (u_int)error);
			nvgpu_fence_release(wait->fence);
			_kfree(wait, M_NVGPU_FUTURE);
			atomic_fetchadd_int(&state->wait_count, -1);
		}
	}
	if (atomic_fetchadd_int(&state->wait_count, -1) != 1)
		return (0);
	error = nvgpu_sched_put(future);
	if (error == 0)
		return (0);
	spin_lock(&nvgpu_future_spin);
	LIST_REMOVE(state, registry_link);
	spin_unlock(&nvgpu_future_spin);
	future->poll = state->poll;
	_kfree(state, M_NVGPU_FUTURE);
	return (error);
}

static void
nvgpu_future_wait_complete(void *argument)
{
	struct nvgpu_future_wait *wait;
	struct nvgpu_future_state *state;
	u_int old;
	int error;

	wait = argument;
	state = wait->state;
	error = nvgpu_fence_get_error(wait->fence);
	if (error != 0 && error != EINPROGRESS)
		(void)atomic_cmpset_int(&state->wait_error, 0, (u_int)error);
	nvgpu_fence_release(wait->fence);
	_kfree(wait, M_NVGPU_FUTURE);
	old = atomic_fetchadd_int(&state->wait_count, -1);
	if (old != 1)
		return;
	error = nvgpu_sched_put(state->future);
	KASSERT(error == 0, ("future wake after scheduler stop: %d", error));
}

int
nvgpu_future_get_wait_error(struct nvgpu_future *future)
{
	struct nvgpu_future_state *state;

	state = nvgpu_future_get_state(future);
	KASSERT(state != NULL, ("reading an unregistered nvgpu future"));
	return ((int)state->wait_error);
}

static struct nvgpu_future_result
nvgpu_future_poll(struct nvgpu_future *future)
{
	struct nvgpu_future_state *state;
	struct nvgpu_future_result result;

	state = nvgpu_future_get_state(future);
	KASSERT(state != NULL, ("polling unregistered nvgpu future"));
	result = state->poll(future);
	if (!result.ready)
		return (result);
	spin_lock(&nvgpu_future_spin);
	LIST_REMOVE(state, registry_link);
	spin_unlock(&nvgpu_future_spin);
	_kfree(state, M_NVGPU_FUTURE);
	return (result);
}

void
nvgpu_future_assert_empty(void)
{
	bool empty = true;

	spin_lock(&nvgpu_future_spin);
	for (u_int i = 0; i < NVGPU_FUTURE_BUCKETS; i++) {
		if (!LIST_EMPTY(&nvgpu_future_registry[i])) {
			empty = false;
			break;
		}
	}
	spin_unlock(&nvgpu_future_spin);
	KASSERT(empty, ("stopping scheduler with registered futures"));
}
