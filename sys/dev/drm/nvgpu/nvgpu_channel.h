/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * User channel lifetime for one GPU process.
 */

#ifndef _NVGPU_CHANNEL_H_
#define _NVGPU_CHANNEL_H_

#include <sys/queue.h>
#include <sys/stdint.h>

#define NVGPU_CHANNEL_GPFIFO_ENTRIES	512u

struct nvgpu_channel;
struct nvgpu_proc;

TAILQ_HEAD(nvgpu_channel_list, nvgpu_channel);

struct nvgpu_channel_alloc_args {
	uint32_t fb_ctxdma_handle;
	uint32_t tt_ctxdma_handle;
};

struct nvgpu_channel_alloc_reply {
	int32_t channel;
	uint32_t pushbuf_domains;
	uint32_t notifier_handle;
	uint32_t nr_subchan;
};

/* Allocate one user channel and insert it into proc's channel list. */
int nvgpu_channel_alloc(struct nvgpu_proc *proc,
    const struct nvgpu_channel_alloc_args *args,
    struct nvgpu_channel_alloc_reply *reply);

/* Free one channel by userspace id. */
int nvgpu_channel_free(struct nvgpu_proc *proc, int32_t channel);

/* Create an NVIF engine object under a channel selected by the NVIF token. */
int nvgpu_channel_new_object(struct nvgpu_proc *proc, uint64_t token,
    uint64_t nvif_object, uint32_t handle, uint32_t oclass,
    int needs_gr_context);

/* Delete an NVIF engine object if it is still live.  Unknown objects are ignored. */
int nvgpu_channel_delete_object(struct nvgpu_proc *proc, uint64_t nvif_object);

/* Destroy every remaining channel during proc teardown. */
void nvgpu_channel_destroy_all(struct nvgpu_proc *proc);

#endif /* _NVGPU_CHANNEL_H_ */
