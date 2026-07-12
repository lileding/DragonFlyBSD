/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * User channel lifetime for one GPU process.
 */

#include "nvdrm_nouveau_abi.h"
#include "nvgpu_channel.h"
#include "nvgpu_channel_internal.h"
#include "nvgpu_debug.h"
#include "nvgpu_device.h"
#include "nvgpu_fence.h"
#include "nvgpu_future.h"
#include "nvgpu_intr.h"
#include "nvgpu_intr_internal.h"
#include "nvgpu_proc.h"
#include "nvgpu_proc_internal.h"
#include "nvgpu_vm.h"
#include "nvgsp_channel.h"
#include "nvgsp_vmm.h"

#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <machine/atomic.h>

static MALLOC_DEFINE(M_NVGPU_CHANNEL, "nvgpu_channel", "nvgpu user channel");

#define NVGPU_MAX_CHANNELS	64u
#define NVGPU_MAX_CHANNEL_OBJECTS 16u

struct nvgpu_channel_object {
	uint32_t handle;
	uint32_t oclass;
	uint64_t nvif_object;
	struct nvgsp_channel_object *backend;
};

struct nvgpu_channel {
	TAILQ_ENTRY(nvgpu_channel) link;
	uint32_t id;
	uint32_t engine_type;
	uint32_t inflight_execs;
	bool closing;
	struct nvgpu_device *device;
	struct nvgsp_channel *backend;
	struct nvgpu_channel_object objects[NVGPU_MAX_CHANNEL_OBJECTS];
};

static volatile u_int nvgpu_channel_next_id = 1;

static void nvgpu_channel_finalize_objects(struct nvgpu_channel *chan);

/* Caller serializes the proc channel list. */
static uint32_t
nvgpu_channel_count(struct nvgpu_proc *proc)
{
	struct nvgpu_channel_list *channels;
	struct nvgpu_channel *chan;
	uint32_t count = 0;

	channels = nvgpu_proc_get_channels(proc);
	if (channels == NULL)
		return (0);
	TAILQ_FOREACH(chan, channels, link)
		count++;
	return (count);
}

/* Caller serializes the proc channel list. */
static struct nvgpu_channel *
nvgpu_channel_find(struct nvgpu_proc *proc, uint32_t id)
{
	struct nvgpu_channel_list *channels;
	struct nvgpu_channel *chan;

	channels = nvgpu_proc_get_channels(proc);
	if (channels == NULL)
		return (NULL);
	TAILQ_FOREACH(chan, channels, link) {
		if (chan->id == id)
			return (chan);
	}
	return (NULL);
}

struct nvgpu_channel *
nvgpu_channel_borrow_by_id_locked(struct nvgpu_proc *proc, uint32_t id)
{
	struct nvgpu_channel *chan;

	chan = nvgpu_channel_find(proc, id);
	if (chan == NULL || chan->closing)
		return (NULL);
	return (chan);
}

void
nvgpu_channel_register_exec_locked(struct nvgpu_channel *channel)
{
	KASSERT(channel != NULL && !channel->closing,
	    ("registering EXEC on an inactive nvgpu channel"));
	channel->inflight_execs++;
}

bool
nvgpu_channel_complete_exec_locked(struct nvgpu_proc *proc,
    struct nvgpu_channel *channel)
{
	struct nvgpu_channel_list *channels;

	KASSERT(proc != NULL && channel != NULL,
	    ("completing EXEC without proc channel"));
	KASSERT(channel->inflight_execs != 0,
	    ("nvgpu channel inflight EXEC underflow"));
	channel->inflight_execs--;
	if (!channel->closing || channel->inflight_execs != 0)
		return (false);
	channels = nvgpu_proc_get_channels(proc);
	TAILQ_REMOVE(channels, channel, link);
	return (true);
}

void
nvgpu_channel_destroy(struct nvgpu_channel *chan)
{
	if (chan == NULL)
		return;
	nvgpu_channel_finalize_objects(chan);
	if (chan->backend != NULL)
		nvgsp_channel_destroy_user(chan->backend);
	_kfree(chan, M_NVGPU_CHANNEL);
}

int
nvgpu_channel_submit(struct nvgpu_channel *channel,
    struct nvgpu_channel_submit_args *args, struct nvgpu_future *future)
{
	struct nvgsp_channel_completion completion;
	struct nvgsp_channel_submission *submission;
	int error;

	if (channel == NULL || args == NULL || future == NULL ||
	    args->submitted == NULL || args->sema == NULL ||
	    (args->push_count != 0 && args->pushes == NULL) ||
	    args->push_count > NVGPU_CHANNEL_GPFIFO_ENTRIES - 2)
		return (EINVAL);
	error = nvgsp_channel_prepare_submit(channel->backend,
	    (const struct nvgsp_channel_push *)args->pushes,
	    (uint32_t)args->push_count, &submission);
	if (error != 0)
		return (error);
	nvgsp_channel_describe_submit(submission, &completion);
	args->sema->device = channel->device;
	args->sema->address = completion.sema;
	args->sema->target = completion.payload;
	args->sema->chid = completion.chid;
	args->sema->error = 0;
	error = nvgpu_intr_park(args->sema, future);
	if (error != 0) {
		nvgsp_channel_abort_submit(submission);
		memset(args->sema, 0, sizeof(*args->sema));
		return (error);
	}
	error = nvgpu_fence_signal(args->submitted, 0);
	KASSERT(error == 0, ("signaling submitted fence failed: %d", error));
	nvgsp_channel_commit_submit(submission);
	return (0);
}

static struct nvgpu_channel_object *
nvgpu_channel_object_slot(struct nvgpu_channel *chan)
{
	uint32_t i;

	for (i = 0; i < NVGPU_MAX_CHANNEL_OBJECTS; i++) {
		if (chan->objects[i].oclass == 0)
			return (&chan->objects[i]);
	}
	return (NULL);
}

static void
nvgpu_channel_finalize_objects(struct nvgpu_channel *chan)
{
	uint32_t i;

	for (i = 0; i < NVGPU_MAX_CHANNEL_OBJECTS; i++) {
		struct nvgpu_channel_object *obj = &chan->objects[i];

		if (obj->oclass == 0)
			continue;
		if (obj->backend != NULL)
			nvgsp_channel_free_object(obj->backend);
		memset(obj, 0, sizeof(*obj));
	}
}

static int
nvgpu_channel_select_engine(const struct nvgpu_channel_create_args *args,
    uint32_t *engine_type)
{
	*engine_type = NVGSP_CHANNEL_ENGINE_GRAPHICS;
	if (args->fb_ctxdma_handle != ~0u)
		return (0);

	switch (args->tt_ctxdma_handle) {
	case NOUVEAU_FIFO_ENGINE_GR:
		*engine_type = NVGSP_CHANNEL_ENGINE_GRAPHICS;
		return (0);
	case NOUVEAU_FIFO_ENGINE_CE:
		*engine_type = NVGSP_CHANNEL_ENGINE_COPY0;
		return (0);
	default:
		return (ENOSYS);
	}
}

/* Create an NVIF engine object under a channel selected by the NVIF token. */
int
nvgpu_channel_new_object(struct nvgpu_proc *proc, uint64_t token,
    uint64_t nvif_object, uint32_t handle, uint32_t oclass,
    int needs_gr_context)
{
	struct nvgpu_channel *chan;
	struct nvgpu_channel_object *obj;
	struct nvgsp_channel_object *backend;
	int error;
	uint32_t rm_handle;
	uint32_t slot;

	if (proc == NULL)
		return (EINVAL);
	nvgpu_proc_lock(proc);
	chan = nvgpu_channel_borrow_by_id_locked(proc, (uint32_t)token);
	if (chan == NULL) {
		nvgpu_proc_unlock(proc);
		return (ENOENT);
	}
	if (chan->backend == NULL) {
		nvgpu_proc_unlock(proc);
		return (ENOENT);
	}
	obj = nvgpu_channel_object_slot(chan);
	if (obj == NULL) {
		nvgpu_proc_unlock(proc);
		return (ENOMEM);
	}
	slot = (uint32_t)(obj - chan->objects);
	rm_handle = handle;
	if (rm_handle == 0) {
		rm_handle = ((oclass & 0xffffu) << 16) |
		    ((chan->id + slot) & 0xffffu);
		nvgpu_log(NVGPU_LOG_DEBUG,
		    "channel object generated RM handle=0x%x class=0x%x channel=%u slot=%u\n",
		    rm_handle, oclass, chan->id, slot);
	}
	if (needs_gr_context) {
		error = nvgsp_channel_promote_graphics_context(chan->backend);
		if (error != 0) {
			nvgpu_proc_unlock(proc);
			return (error);
		}
	}
	error = nvgsp_channel_alloc_object(chan->backend, rm_handle, oclass,
	    &backend);
	if (error != 0) {
		nvgpu_proc_unlock(proc);
		return (error);
	}
	obj->handle = rm_handle;
	obj->oclass = oclass;
	obj->nvif_object = nvif_object;
	obj->backend = backend;
	nvgpu_proc_unlock(proc);
	return (0);
}

/* Delete an NVIF engine object if it is still live.  Unknown objects are ignored. */
int
nvgpu_channel_delete_object(struct nvgpu_proc *proc, uint64_t nvif_object)
{
	struct nvgpu_channel_list *channels;
	struct nvgpu_channel *chan, *found;
	uint32_t i;

	if (proc == NULL)
		return (EINVAL);
	channels = nvgpu_proc_get_channels(proc);
	if (channels == NULL)
		return (EINVAL);
	found = NULL;
	nvgpu_proc_lock(proc);
	TAILQ_FOREACH(chan, channels, link) {
		for (i = 0; i < NVGPU_MAX_CHANNEL_OBJECTS; i++) {
			struct nvgpu_channel_object *obj = &chan->objects[i];

			if (obj->oclass == 0 || obj->nvif_object != nvif_object)
				continue;
			found = chan;
			break;
		}
		if (found != NULL)
			break;
	}
	if (found == NULL) {
		nvgpu_proc_unlock(proc);
		return (0);
	}
	for (i = 0; i < NVGPU_MAX_CHANNEL_OBJECTS; i++) {
		struct nvgpu_channel_object *obj = &found->objects[i];

		if (obj->oclass == 0 || obj->nvif_object != nvif_object)
			continue;
		if (obj->backend != NULL)
			nvgsp_channel_free_object(obj->backend);
		memset(obj, 0, sizeof(*obj));
		break;
	}
	nvgpu_proc_unlock(proc);
	return (0);
}

int
nvgpu_channel_create(struct nvgpu_channel_create_args *args,
    struct nvgpu_channel **result)
{
	struct nvgpu_channel_list *channels;
	struct nvgpu_channel *chan;
	struct nvgpu_proc *proc;
	struct nvgsp_vmm *vmm;
	uint32_t engine_type;
	int error;

	if (args == NULL || args->proc == NULL || result == NULL)
		return (EINVAL);
	proc = args->proc;
	channels = nvgpu_proc_get_channels(proc);
	if (channels == NULL)
		return (EINVAL);
	nvgpu_proc_lock(proc);
	if (nvgpu_channel_count(proc) >= NVGPU_MAX_CHANNELS) {
		nvgpu_proc_unlock(proc);
		return (ENOMEM);
	}
	nvgpu_proc_unlock(proc);

	error = nvgpu_channel_select_engine(args, &engine_type);
	if (error != 0)
		return (error);
	error = nvgpu_vm_ensure(proc, &vmm);
	if (error != 0)
		return (error);

	chan = kmalloc(sizeof(*chan), M_NVGPU_CHANNEL, M_WAITOK | M_ZERO);
	chan->device = nvgpu_proc_get_device(proc);
	chan->id = atomic_fetchadd_int(&nvgpu_channel_next_id, 1);
	chan->engine_type = engine_type;
	error = nvgsp_channel_create_user(vmm, engine_type, &chan->backend);
	if (error != 0) {
		_kfree(chan, M_NVGPU_CHANNEL);
		return (error);
	}

	nvgpu_proc_lock(proc);
	if (nvgpu_channel_count(proc) >= NVGPU_MAX_CHANNELS) {
		nvgpu_proc_unlock(proc);
		nvgpu_channel_destroy(chan);
		return (ENOMEM);
	}
	TAILQ_INSERT_TAIL(channels, chan, link);
	nvgpu_proc_unlock(proc);
	args->channel_id = (int32_t)chan->id;
	args->pushbuf_domains = NOUVEAU_GEM_DOMAIN_VRAM;
	args->notifier_handle = 0;
	args->nr_subchan = 0;
	*result = chan;
	return (0);
}

/* Remove one channel by userspace id and release the list's reference. */
int
nvgpu_channel_release_by_id(struct nvgpu_proc *proc, int32_t channel)
{
	struct nvgpu_channel_list *channels;
	struct nvgpu_channel *chan;
	bool destroy;

	if (proc == NULL)
		return (EINVAL);
	channels = nvgpu_proc_get_channels(proc);
	nvgpu_proc_lock(proc);
	chan = nvgpu_channel_borrow_by_id_locked(proc, (uint32_t)channel);
	if (chan == NULL) {
		nvgpu_proc_unlock(proc);
		return (ENOENT);
	}
	chan->closing = true;
	destroy = chan->inflight_execs == 0;
	if (destroy)
		TAILQ_REMOVE(channels, chan, link);
	nvgpu_proc_unlock(proc);
	if (destroy)
		nvgpu_channel_destroy(chan);
	return (0);
}

void
nvgpu_channel_destroy_all(struct nvgpu_proc *proc)
{
	struct nvgpu_channel_list *channels;
	struct nvgpu_channel *chan;

	if (proc == NULL)
		return;
	channels = nvgpu_proc_get_channels(proc);
	if (channels == NULL)
		return;
	nvgpu_proc_lock(proc);
	while ((chan = TAILQ_FIRST(channels)) != NULL) {
		KASSERT(chan->inflight_execs == 0,
		    ("destroying nvgpu channel with in-flight EXEC"));
		TAILQ_REMOVE(channels, chan, link);
		nvgpu_proc_unlock(proc);
		nvgpu_channel_destroy(chan);
		nvgpu_proc_lock(proc);
	}
	nvgpu_proc_unlock(proc);
}
