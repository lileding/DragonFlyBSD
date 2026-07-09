/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 */

#ifndef _NVGPU_PROC_H_
#define _NVGPU_PROC_H_

struct nvgpu_channel_list;
struct nvgpu_device;
struct nvgpu_proc;
struct nvgpu_vm;

/* Create per-open GPU state.  procp receives one owned file reference. */
int nvgpu_proc_create(struct nvgpu_device *gpu, struct nvgpu_proc **procp);

/* Drop the file reference; final teardown runs when all future users release proc. */
void nvgpu_proc_stop(struct nvgpu_proc *proc);

/* Hold or release one asynchronous user of proc-owned state. */
void nvgpu_proc_hold(struct nvgpu_proc *proc);
void nvgpu_proc_release(struct nvgpu_proc *proc);

/* Return the borrowed physical GPU for this proc. */
struct nvgpu_device *nvgpu_proc_get_device(struct nvgpu_proc *proc);

/* Return borrowed proc-owned storage for modules that own the contents. */
struct nvgpu_channel_list *nvgpu_proc_get_channels(struct nvgpu_proc *proc);
struct nvgpu_vm *nvgpu_proc_get_vm(struct nvgpu_proc *proc);
void nvgpu_proc_set_vm(struct nvgpu_proc *proc, struct nvgpu_vm *vm);

#endif /* _NVGPU_PROC_H_ */
