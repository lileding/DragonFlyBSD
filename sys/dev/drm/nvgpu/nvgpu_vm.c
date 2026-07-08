/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-process GPU virtual address space owner.
 */

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
	vm->gpu = nvgpu_proc_get_gpu(proc);
	vm->client_handle = atomic_fetchadd_int(&nvgpu_vm_next_client, 1);
	proc->vm = vm;
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
	vm = proc->vm;
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
	vm = proc->vm;
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

/* Destroy the process VM and backend VMM after channels are gone. */
void
nvgpu_vm_destroy(struct nvgpu_proc *proc)
{
	struct nvgpu_vm *vm;

	if (proc == NULL || proc->vm == NULL)
		return;
	vm = proc->vm;
	proc->vm = NULL;
	if (vm->backend != NULL)
		nvgsp_vmm_destroy_user(vm->backend);
	_kfree(vm, M_NVGPU_VM);
}
