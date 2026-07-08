/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-process GPU virtual address space owner.
 */

#ifndef _NVGPU_VM_H_
#define _NVGPU_VM_H_

#include <sys/stdint.h>

struct nvgpu_proc;
struct nvgsp_vmm;

/* Store Mesa's kernel-managed VA window on the process VM.  proc is borrowed. */
int nvgpu_vm_set_kernel_managed(struct nvgpu_proc *proc, uint64_t addr,
    uint64_t size);

/* Ensure a GSP VMM exists for channel/VM work.  Returns borrowed storage owned by proc. */
int nvgpu_vm_ensure(struct nvgpu_proc *proc, struct nvgsp_vmm **vmm);

/* Destroy the process VM and backend VMM after channels are gone. */
void nvgpu_vm_destroy(struct nvgpu_proc *proc);

#endif /* _NVGPU_VM_H_ */
