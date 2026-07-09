/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-process GPU virtual address space owner.
 */

#ifndef _NVGPU_VM_H_
#define _NVGPU_VM_H_

#include <sys/stdint.h>

struct drm_file;
struct nvgpu_proc;
struct nvgsp_vmm;

struct nvgpu_vm_bind_op {
	uint32_t op;
	uint32_t flags;
	uint32_t handle;
	uint64_t addr;
	uint64_t bo_offset;
	uint64_t range;
};

#define NVGPU_VM_BIND_OP_MAP	0u
#define NVGPU_VM_BIND_OP_UNMAP	1u
#define NVGPU_VM_BIND_SPARSE	(1u << 8)

/* Store Mesa's kernel-managed VA window on the process VM.  proc is borrowed. */
int nvgpu_vm_set_kernel_managed(struct nvgpu_proc *proc, uint64_t addr,
    uint64_t size);

/* Ensure a GSP VMM exists for channel/VM work.  Returns borrowed storage owned by proc. */
int nvgpu_vm_ensure(struct nvgpu_proc *proc, struct nvgsp_vmm **vmm);
/* Apply synchronous VM_BIND operations for one process. */
int nvgpu_vm_bind(struct nvgpu_proc *proc, struct drm_file *file,
    const struct nvgpu_vm_bind_op *ops, uint32_t op_count);

/* Destroy the process VM and backend VMM after channels are gone. */
void nvgpu_vm_destroy(struct nvgpu_proc *proc);

#endif /* _NVGPU_VM_H_ */
