/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private proc/channel operations used by syscall and NVIF adaptation.
 */

#ifndef _NVGPU_CHANNEL_INTERNAL_H_
#define _NVGPU_CHANNEL_INTERNAL_H_

#include <stdbool.h>
#include <sys/queue.h>
#include <sys/stdint.h>

struct nvgpu_channel;
struct nvgpu_proc;

TAILQ_HEAD(nvgpu_channel_list, nvgpu_channel);

struct nvgpu_channel *nvgpu_channel_borrow_by_id_locked(
	struct nvgpu_proc *proc, uint32_t id);
void nvgpu_channel_register_exec_locked(struct nvgpu_channel *channel);
bool nvgpu_channel_complete_exec_locked(struct nvgpu_proc *proc,
	struct nvgpu_channel *channel);
int nvgpu_channel_release_by_id(struct nvgpu_proc *proc, int32_t channel_id);
int nvgpu_channel_new_object(struct nvgpu_proc *proc, uint64_t token,
	uint64_t nvif_object, uint32_t handle, uint32_t oclass,
	int needs_gr_context);
int nvgpu_channel_delete_object(struct nvgpu_proc *proc,
	uint64_t nvif_object);
void nvgpu_channel_destroy_all(struct nvgpu_proc *proc);

#endif /* _NVGPU_CHANNEL_INTERNAL_H_ */
