/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-process GPU virtual address space owner.
 */

#include "nvgpu_bo.h"
#include "nvgpu_device.h"
#include "nvgpu_proc.h"
#include "nvgpu_vm.h"
#include "nvgsp_vmm.h"

#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <machine/atomic.h>

static MALLOC_DEFINE(M_NVGPU_VM, "nvgpu_vm", "nvgpu process VM");

struct nvgpu_vm {
	struct nvgpu_device *gpu;
	struct nvgsp_vmm *backend;
	uint32_t client_handle;
	uint64_t kernel_managed_addr;
	uint64_t kernel_managed_size;
};

static volatile u_int nvgpu_vm_next_client = 0xc1d10000u;

static int
nvgpu_vm_alloc(struct nvgpu_proc *proc, struct nvgpu_vm **vmp)
{
	struct nvgpu_vm *vm;

	vm = kmalloc(sizeof(*vm), M_NVGPU_VM, M_WAITOK | M_ZERO);
	vm->gpu = nvgpu_proc_get_device(proc);
	vm->client_handle = atomic_fetchadd_int(&nvgpu_vm_next_client, 1);
	nvgpu_proc_set_vm(proc, vm);
	*vmp = vm;
	return (0);
}

/* Store Mesa's kernel-managed VA window on the process VM.  proc is borrowed. */
int
nvgpu_vm_set_kernel_managed(struct nvgpu_proc *proc, uint64_t addr,
    uint64_t size)
{
	struct nvgpu_vm *vm;
	int error;

	if (proc == NULL)
		return (EINVAL);
	vm = nvgpu_proc_get_vm(proc);
	if (vm == NULL) {
		error = nvgpu_vm_alloc(proc, &vm);
		if (error != 0)
			return (error);
	}
	vm->kernel_managed_addr = addr;
	vm->kernel_managed_size = size;
	return (0);
}

/* Ensure a GSP VMM exists for channel/VM work.  Returns borrowed storage owned by proc. */
int
nvgpu_vm_ensure(struct nvgpu_proc *proc, struct nvgsp_vmm **vmm)
{
	struct nvgpu_vm *vm;
	int error;

	if (proc == NULL || vmm == NULL)
		return (EINVAL);
	vm = nvgpu_proc_get_vm(proc);
	if (vm == NULL) {
		error = nvgpu_vm_alloc(proc, &vm);
		if (error != 0)
			return (error);
	}
	if (vm->backend == NULL) {
		error = nvgsp_vmm_create_user(vm->gpu, vm->client_handle,
		    &vm->backend);
		if (error != 0)
			return (error);
	}
	*vmm = vm->backend;
	return (0);
}

static int
nvgpu_vm_bind_one(struct nvgpu_proc *proc, struct drm_file *file,
    struct nvgsp_vmm *vmm, const struct nvgpu_vm_bind_op *op)
{
	struct nvgpu_bo *bo;
	uint64_t size;
	vm_paddr_t paddr;
	int error;

	if (op->range == 0 || ((op->addr | op->bo_offset | op->range) &
	    (uint64_t)(PAGE_SIZE - 1)) != 0)
		return (EINVAL);
	if (op->op == NVGPU_VM_BIND_OP_UNMAP ||
	    (op->op == NVGPU_VM_BIND_OP_MAP && op->handle == 0))
		return (nvgsp_vmm_unmap(vmm, op->addr, op->range));
	if (op->op != NVGPU_VM_BIND_OP_MAP)
		return (EINVAL);
	if ((op->flags & NVGPU_VM_BIND_SPARSE) != 0)
		return (nvgsp_vmm_unmap(vmm, op->addr, op->range));

	error = nvgpu_bo_lookup(file, op->handle, &bo);
	if (error != 0)
		return (error);
	size = nvgpu_bo_get_size(bo);
	if (op->bo_offset > size || op->range > size - op->bo_offset) {
		nvgpu_bo_put(bo);
		return (EINVAL);
	}
	if (nvgpu_bo_is_vram(bo)) {
		error = nvgpu_bo_get_paddr_at(bo, op->bo_offset, &paddr);
		if (error == 0)
			error = nvgsp_vmm_map_vram(vmm, op->addr, paddr, op->range, 0);
	} else {
		uint64_t done = 0;

		error = nvgpu_bo_ensure_ttm_populated(bo);
		while (error == 0 && done < op->range) {
			uint64_t run_size;

			error = nvgpu_bo_get_paddr_run_at(bo, op->bo_offset + done,
			    op->range - done, &paddr, &run_size);
			if (error != 0)
				break;
			error = nvgsp_vmm_map_sysmem(vmm, op->addr + done, paddr,
			    run_size);
			if (error != 0)
				break;
			done += run_size;
		}
	}
	nvgpu_bo_put(bo);
	return (error);
}

/* Apply synchronous VM_BIND operations in order for one process. */
int
nvgpu_vm_bind(struct nvgpu_proc *proc, struct drm_file *file,
    const struct nvgpu_vm_bind_op *ops, uint32_t op_count)
{
	struct nvgsp_vmm *vmm;
	int error;

	if (proc == NULL || file == NULL || (op_count != 0 && ops == NULL))
		return (EINVAL);
	error = nvgpu_vm_ensure(proc, &vmm);
	if (error != 0)
		return (error);
	for (uint32_t i = 0; i < op_count; i++) {
		error = nvgpu_vm_bind_one(proc, file, vmm, &ops[i]);
		if (error != 0)
			return (error);
	}
	return (0);
}

/* Destroy the process VM and backend VMM after channels are gone. */
void
nvgpu_vm_destroy(struct nvgpu_proc *proc)
{
	struct nvgpu_vm *vm;

	if (proc == NULL)
		return;
	vm = nvgpu_proc_get_vm(proc);
	if (vm == NULL)
		return;
	nvgpu_proc_set_vm(proc, NULL);
	if (vm->backend != NULL)
		nvgsp_vmm_destroy_user(vm->backend);
	_kfree(vm, M_NVGPU_VM);
}
