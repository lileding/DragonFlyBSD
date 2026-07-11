/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-process GPU virtual address space owner.
 */

#ifndef _NVGPU_VM_H_
#define _NVGPU_VM_H_

#include <sys/stdint.h>

struct dma_fence;
struct nvgpu_bo;
struct nvgpu_fence;
struct nvgpu_proc;
struct nvgsp_vmm;

struct nvgpu_vm_bind_op {
	uint32_t op;
	uint32_t flags;
	uint32_t handle;
	uint64_t addr;
	uint64_t bo_offset;
	uint64_t range;
	struct nvgpu_bo *bo;
};

struct nvgpu_vm_bind_args {
	struct nvgpu_vm_bind_op *ops;
	uint32_t op_count;
	struct nvgpu_fence *done_fence;
	struct dma_fence **wait_fences;
	uint32_t wait_count;
};

#define NVGPU_VM_BIND_OP_MAP	0u
#define NVGPU_VM_BIND_OP_UNMAP	1u
#define NVGPU_VM_BIND_SPARSE	(1u << 8)

/* Store Mesa's kernel-managed VA window on the process VM.  proc is borrowed. */
int nvgpu_vm_set_kernel_managed(struct nvgpu_proc *proc, uint64_t addr,
    uint64_t size);

/* Ensure a GSP VMM exists for channel/VM work.  Returns borrowed storage owned by proc. */
int nvgpu_vm_ensure(struct nvgpu_proc *proc, struct nvgsp_vmm **vmm);
/*
 * Spawn one ordered VM_BIND future.  The op array, its BO references, and wait
 * fences are borrowed while the future copies state and installs callbacks.
 * args->done_fence is consumed on all paths.
 */
int nvgpu_vm_bind_spawn(struct nvgpu_proc *proc,
    struct nvgpu_vm_bind_args *args);

/* Destroy the process VM and backend VMM after channels are gone. */
void nvgpu_vm_destroy(struct nvgpu_proc *proc);

#endif /* _NVGPU_VM_H_ */
