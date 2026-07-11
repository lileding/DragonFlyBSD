/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * User channel lifetime for one GPU process.
 */

#include "nvdrm_nouveau_abi.h"
#include "nvgpu_channel.h"
#include "nvgpu_debug.h"
#include "nvgpu_proc.h"
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
	volatile u_int refs;
	uint32_t id;
	uint32_t engine_type;
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
nvgpu_channel_hold_by_id(struct nvgpu_proc *proc, uint32_t id)
{
	struct nvgpu_channel *chan;

	if (proc == NULL)
		return (NULL);
	nvgpu_proc_lock(proc);
	chan = nvgpu_channel_find(proc, id);
	if (chan != NULL)
		atomic_add_int(&chan->refs, 1);
	nvgpu_proc_unlock(proc);
	return (chan);
}

void
nvgpu_channel_release(struct nvgpu_channel *chan)
{
	u_int refs;

	if (chan == NULL)
		return;
	refs = atomic_fetchadd_int(&chan->refs, -1);
	KASSERT(refs != 0, ("nvgpu channel refs underflow"));
	if (refs != 1)
		return;
	nvgpu_channel_finalize_objects(chan);
	if (chan->backend != NULL)
		nvgsp_channel_destroy_user(chan->backend);
	_kfree(chan, M_NVGPU_CHANNEL);
}

struct nvgsp_channel *
nvgpu_channel_get_backend(struct nvgpu_channel *chan)
{
	return (chan != NULL ? chan->backend : NULL);
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
nvgpu_channel_select_engine(const struct nvgpu_channel_alloc_args *args,
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
	chan = nvgpu_channel_hold_by_id(proc, (uint32_t)token);
	if (chan == NULL)
		return (ENOENT);
	if (chan->backend == NULL) {
		nvgpu_channel_release(chan);
		return (ENOENT);
	}
	obj = nvgpu_channel_object_slot(chan);
	if (obj == NULL) {
		nvgpu_channel_release(chan);
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
			nvgpu_channel_release(chan);
			return (error);
		}
	}
	error = nvgsp_channel_alloc_object(chan->backend, rm_handle, oclass,
	    &backend);
	if (error != 0) {
		nvgpu_channel_release(chan);
		return (error);
	}
	obj->handle = rm_handle;
	obj->oclass = oclass;
	obj->nvif_object = nvif_object;
	obj->backend = backend;
	nvgpu_channel_release(chan);
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
			atomic_add_int(&chan->refs, 1);
			found = chan;
			break;
		}
		if (found != NULL)
			break;
	}
	nvgpu_proc_unlock(proc);
	if (found == NULL)
		return (0);
	for (i = 0; i < NVGPU_MAX_CHANNEL_OBJECTS; i++) {
		struct nvgpu_channel_object *obj = &found->objects[i];

		if (obj->oclass == 0 || obj->nvif_object != nvif_object)
			continue;
		if (obj->backend != NULL)
			nvgsp_channel_free_object(obj->backend);
		memset(obj, 0, sizeof(*obj));
		break;
	}
	nvgpu_channel_release(found);
	return (0);
}

/* Allocate one user channel and insert it into proc's channel list. */
int
nvgpu_channel_alloc(struct nvgpu_proc *proc,
    const struct nvgpu_channel_alloc_args *args,
    struct nvgpu_channel_alloc_reply *reply)
{
	struct nvgpu_channel_list *channels;
	struct nvgpu_channel *chan;
	struct nvgsp_vmm *vmm;
	uint32_t engine_type;
	int error;

	if (proc == NULL || args == NULL || reply == NULL)
		return (EINVAL);
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
	chan->refs = 1;
	chan->id = atomic_fetchadd_int(&nvgpu_channel_next_id, 1);
	chan->engine_type = engine_type;
	error = nvgsp_channel_create_user(vmm, engine_type, &chan->backend);
	if (error != 0) {
		_kfree(chan, M_NVGPU_CHANNEL);
		return (error);
	}

	nvgpu_proc_lock(proc);
	TAILQ_INSERT_TAIL(channels, chan, link);
	nvgpu_proc_unlock(proc);
	reply->channel = chan->id;
	reply->pushbuf_domains = NOUVEAU_GEM_DOMAIN_VRAM;
	reply->notifier_handle = 0;
	reply->nr_subchan = 0;
	return (0);
}

/* Remove one channel by userspace id and release the list's reference. */
int
nvgpu_channel_release_by_id(struct nvgpu_proc *proc, int32_t channel)
{
	struct nvgpu_channel_list *channels;
	struct nvgpu_channel *chan;

	if (proc == NULL)
		return (EINVAL);
	channels = nvgpu_proc_get_channels(proc);
	if (channels == NULL)
		return (EINVAL);
	nvgpu_proc_lock(proc);
	chan = nvgpu_channel_find(proc, (uint32_t)channel);
	if (chan == NULL) {
		nvgpu_proc_unlock(proc);
		return (ENOENT);
	}
	TAILQ_REMOVE(channels, chan, link);
	nvgpu_proc_unlock(proc);
	nvgpu_channel_release(chan);
	return (0);
}

/* Remove every remaining channel and release the list's references. */
void
nvgpu_channel_release_all(struct nvgpu_proc *proc)
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
		TAILQ_REMOVE(channels, chan, link);
		nvgpu_proc_unlock(proc);
		nvgpu_channel_release(chan);
		nvgpu_proc_lock(proc);
	}
	nvgpu_proc_unlock(proc);
}
