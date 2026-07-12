/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private proc access used while VM and channel ownership are being localized.
 */

#ifndef _NVGPU_PROC_INTERNAL_H_
#define _NVGPU_PROC_INTERNAL_H_

#include <sys/stdint.h>

struct nvgpu_channel_list;
struct nvgpu_device;
struct nvgpu_fence;
struct nvgpu_proc;
struct nvgpu_vm;
struct reservation_object;

void nvgpu_proc_lock(struct nvgpu_proc *proc);
void nvgpu_proc_unlock(struct nvgpu_proc *proc);
struct nvgpu_device *nvgpu_proc_get_device(struct nvgpu_proc *proc);
struct nvgpu_channel_list *nvgpu_proc_get_channels(struct nvgpu_proc *proc);
struct nvgpu_vm *nvgpu_proc_get_vm(struct nvgpu_proc *proc);
void nvgpu_proc_set_vm(struct nvgpu_proc *proc, struct nvgpu_vm *vm);
struct reservation_object *nvgpu_proc_get_vm_resv(struct nvgpu_proc *proc);

int nvgpu_proc_register_bind(struct nvgpu_proc *proc,
	struct nvgpu_fence *done, struct nvgpu_fence **waits,
	uint32_t capacity, uint32_t *wait_count);
void nvgpu_proc_complete_bind(struct nvgpu_proc *proc,
	struct nvgpu_fence *done);

#endif /* _NVGPU_PROC_INTERNAL_H_ */
