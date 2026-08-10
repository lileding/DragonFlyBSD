/*
 * Copyright (c) 2018-2021 Maxime Villard, m00nbsd.net
 * All rights reserved.
 *
 * This code is part of the NVMM hypervisor.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <sys/kernel.h>
#include <sys/mman.h>

#include "nvmm.h"
#include "nvmm_internal.h"
#include "nvmm_ioctl.h"

static void
nvmm_native_vmspace_destroy(os_vmspace_t *vm)
{
	pmap_del_all_cpus(vm);
	vmspace_rel(vm);
}

static int
nvmm_native_vmspace_fault(os_vmspace_t *vm, vaddr_t va, vm_prot_t prot)
{
	int fault_flags;

	if (prot & VM_PROT_WRITE)
		fault_flags = VM_FAULT_DIRTY;
	else
		fault_flags = VM_FAULT_NORMAL;
	return vm_fault(&vm->vm_map, trunc_page(va), prot, fault_flags);
}

static int
nvmm_native_capability(struct nvmm_owner *owner, struct nvmm_ioc_capability *args)
{
	args->cap.version = NVMM_KERN_VERSION;
	args->cap.state_size = nvmm_impl->state_size;
	args->cap.comm_size = NVMM_COMM_PAGE_SIZE;
	args->cap.max_machines = NVMM_MAX_MACHINES;
	args->cap.max_vcpus = NVMM_MAX_VCPUS;
	args->cap.max_ram = NVMM_MAX_RAM;

	(*nvmm_impl->capability)(&args->cap);

	return 0;
}

static int
nvmm_native_machine_create(struct nvmm_owner *owner,
    struct nvmm_ioc_machine_create *args)
{
	struct nvmm_machine *mach;
	int error;

	error = nvmm_machine_alloc(&mach);
	if (error)
		return error;

	/* Curproc owns the machine. */
	mach->owner = owner;
	mach->use_vmm = false;

	/* Zero out the host mappings. */
	memset(&mach->hmap, 0, sizeof(mach->hmap));

	/* Create the machine vmspace. */
	mach->gpa_begin = 0;
	mach->gpa_end = NVMM_MAX_RAM;
	mach->vm = os_vmspace_create(mach->gpa_begin, mach->gpa_end);

	/* Create the comm vmobj. */
	mach->commvmobj = os_vmobj_create(
	    NVMM_MAX_VCPUS * NVMM_COMM_PAGE_SIZE);

	(*nvmm_impl->machine_create)(mach);

	args->machid = mach->machid;
	nvmm_machine_put(mach);

	return 0;
}

static int
nvmm_native_machine_destroy(struct nvmm_owner *owner,
    struct nvmm_ioc_machine_destroy *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;
	size_t i;

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error)
		return error;

	for (i = 0; i < NVMM_MAX_VCPUS; i++) {
		error = nvmm_vcpu_get(mach, i, &vcpu);
		if (error)
			continue;

		(*nvmm_impl->vcpu_destroy)(mach, vcpu);
		nvmm_vcpu_free(mach, vcpu);
		nvmm_vcpu_put(vcpu);
		os_atomic_dec_uint(&mach->ncpus);
	}

	(*nvmm_impl->machine_destroy)(mach);

	/* Free the machine vmspace. */
	nvmm_native_vmspace_destroy(mach->vm);

	/* Drop the kernel vmobj refs. */
	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		if (!mach->hmap[i].present)
			continue;
		os_vmobj_rel(mach->hmap[i].vmobj);
	}

	nvmm_machine_free(mach);
	nvmm_machine_put(mach);

	return 0;
}

int
nvmm_native_machine_destroy_locked(struct nvmm_machine *mach)
{
	struct nvmm_cpu *vcpu;
	size_t i;

	OS_ASSERT(os_rwl_wheld(&mach->lock));

	for (i = 0; i < NVMM_MAX_VCPUS; i++) {
		if (nvmm_vcpu_get(mach, i, &vcpu) != 0)
			continue;
		(*nvmm_impl->vcpu_destroy)(mach, vcpu);
		nvmm_vcpu_free(mach, vcpu);
		nvmm_vcpu_put(vcpu);
		os_atomic_dec_uint(&mach->ncpus);
	}
	(*nvmm_impl->machine_destroy)(mach);
	nvmm_native_vmspace_destroy(mach->vm);
	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		if (mach->hmap[i].present)
			os_vmobj_rel(mach->hmap[i].vmobj);
	}
	nvmm_machine_free(mach);
	return 0;
}

static int
nvmm_native_machine_configure(struct nvmm_owner *owner,
    struct nvmm_ioc_machine_configure *args)
{
	struct nvmm_machine *mach;
	size_t allocsz;
	uint64_t op;
	void *data;
	int error;

	op = NVMM_MACH_CONF_MD(args->op);
	if (__predict_false(op >= nvmm_impl->mach_conf_max)) {
		return EINVAL;
	}

	allocsz = nvmm_impl->mach_conf_sizes[op];
	data = os_mem_alloc(allocsz);

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error) {
		os_mem_free(data, allocsz);
		return error;
	}

	error = copyin(args->conf, data, allocsz);
	if (error) {
		goto out;
	}

	error = (*nvmm_impl->machine_configure)(mach, op, data);

out:
	nvmm_machine_put(mach);
	os_mem_free(data, allocsz);
	return error;
}

static int
nvmm_native_vcpu_create(struct nvmm_owner *owner, struct nvmm_ioc_vcpu_create *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_alloc(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	/* Map the comm page on the kernel side, as wired. */
	error = os_vmobj_map(os_kernel_map, (vaddr_t *)&vcpu->comm,
	    NVMM_COMM_PAGE_SIZE, mach->commvmobj,
	    args->cpuid * NVMM_COMM_PAGE_SIZE, true /* wired */,
	    false /* !fixed */, true /* shared */, PROT_READ | PROT_WRITE,
	    PROT_READ | PROT_WRITE);
	if (error) {
		nvmm_vcpu_free(mach, vcpu);
		nvmm_vcpu_put(vcpu);
		goto out;
	}

	memset(vcpu->comm, 0, NVMM_COMM_PAGE_SIZE);

	/* Map the comm page on the user side, as pageable. */
	error = os_vmobj_map(os_curproc_map, (vaddr_t *)&args->comm,
	    NVMM_COMM_PAGE_SIZE, mach->commvmobj,
	    args->cpuid * NVMM_COMM_PAGE_SIZE, false /* !wired */,
	    false /* !fixed */, true /* shared */, PROT_READ | PROT_WRITE,
	    PROT_READ | PROT_WRITE);
	if (error) {
		nvmm_vcpu_free(mach, vcpu);
		nvmm_vcpu_put(vcpu);
		goto out;
	}

	error = (*nvmm_impl->vcpu_create)(mach, vcpu);
	if (error) {
		nvmm_vcpu_free(mach, vcpu);
		nvmm_vcpu_put(vcpu);
		goto out;
	}

	nvmm_vcpu_put(vcpu);
	os_atomic_inc_uint(&mach->ncpus);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_native_vcpu_destroy(struct nvmm_owner *owner, struct nvmm_ioc_vcpu_destroy *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	(*nvmm_impl->vcpu_destroy)(mach, vcpu);
	nvmm_vcpu_free(mach, vcpu);
	nvmm_vcpu_put(vcpu);
	os_atomic_dec_uint(&mach->ncpus);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_native_vcpu_configure(struct nvmm_owner *owner,
    struct nvmm_ioc_vcpu_configure *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	size_t allocsz;
	uint64_t op;
	void *data;
	int error;

	op = NVMM_VCPU_CONF_MD(args->op);
	if (__predict_false(op >= nvmm_impl->vcpu_conf_max))
		return EINVAL;

	allocsz = nvmm_impl->vcpu_conf_sizes[op];
	data = os_mem_alloc(allocsz);

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error) {
		os_mem_free(data, allocsz);
		return error;
	}

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error) {
		nvmm_machine_put(mach);
		os_mem_free(data, allocsz);
		return error;
	}

	error = copyin(args->conf, data, allocsz);
	if (error) {
		goto out;
	}

	error = (*nvmm_impl->vcpu_configure)(vcpu, op, data);

out:
	nvmm_vcpu_put(vcpu);
	nvmm_machine_put(mach);
	os_mem_free(data, allocsz);
	return error;
}

static int
nvmm_native_vcpu_setstate(struct nvmm_owner *owner,
    struct nvmm_ioc_vcpu_setstate *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	(*nvmm_impl->vcpu_setstate)(vcpu);
	nvmm_vcpu_put(vcpu);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_native_vcpu_getstate(struct nvmm_owner *owner,
    struct nvmm_ioc_vcpu_getstate *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	(*nvmm_impl->vcpu_getstate)(vcpu);
	nvmm_vcpu_put(vcpu);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_native_do_vcpu_run(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	struct vmspace *vm = mach->vm;
	int ret;

	while (1) {
		/* Got a signal? Or pending resched? Leave. */
		if (__predict_false(os_return_needed())) {
			exit->reason = NVMM_VCPU_EXIT_NONE;
			return 0;
		}

		/* Run the VCPU. */
		ret = (*nvmm_impl->vcpu_run)(mach, vcpu, exit);
		if (__predict_false(ret != 0)) {
			return ret;
		}

		/* Process nested page faults. */
		if (__predict_true(exit->reason != NVMM_VCPU_EXIT_MEMORY)) {
			break;
		}
		if (exit->u.mem.gpa >= mach->gpa_end) {
			break;
		}
		if (nvmm_native_vmspace_fault(vm, exit->u.mem.gpa,
		    exit->u.mem.prot)) {
			break;
		}
	}

	return 0;
}

static int
nvmm_native_vcpu_run(struct nvmm_owner *owner, struct nvmm_ioc_vcpu_run *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	error = nvmm_native_do_vcpu_run(mach, vcpu, &args->exit);
	nvmm_vcpu_put(vcpu);

out:
	nvmm_machine_put(mach);
	return error;
}

/* -------------------------------------------------------------------------- */

static os_vmobj_t *
nvmm_native_hmapping_getvmobj(struct nvmm_machine *mach, uintptr_t hva, size_t size,
   size_t *off)
{
	struct nvmm_hmapping *hmapping;
	size_t i;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];
		if (!hmapping->present) {
			continue;
		}
		if (hva >= hmapping->hva &&
		    hva + size <= hmapping->hva + hmapping->size) {
			*off = hva - hmapping->hva;
			return hmapping->vmobj;
		}
	}

	return NULL;
}

static int
nvmm_native_hmapping_validate(struct nvmm_machine *mach, uintptr_t hva, size_t size)
{
	struct nvmm_hmapping *hmapping;
	size_t i;
	uintptr_t hva_end;
	uintptr_t hmap_end;

	if ((hva % PAGE_SIZE) != 0 || (size % PAGE_SIZE) != 0) {
		return EINVAL;
	}
	if (hva == 0) {
		return EINVAL;
	}

	/*
	 * Overflow tests MUST be done very carefully to avoid compiler
	 * optimizations from effectively deleting the test.
	 */
	hva_end = hva + size;
	if (hva_end <= hva)
		return EINVAL;

	/*
	 * Overlap tests
	 */
	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];

		if (!hmapping->present) {
			continue;
		}
		hmap_end = hmapping->hva + hmapping->size;

		if (hva >= hmapping->hva && hva_end <= hmap_end)
			break;
		if (hva >= hmapping->hva && hva < hmap_end)
			return EEXIST;
		if (hva_end > hmapping->hva && hva_end <= hmap_end)
			return EEXIST;
		if (hva <= hmapping->hva && hva_end >= hmap_end)
			return EEXIST;
	}

	return 0;
}

static struct nvmm_hmapping *
nvmm_native_hmapping_alloc(struct nvmm_machine *mach)
{
	struct nvmm_hmapping *hmapping;
	size_t i;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];
		if (!hmapping->present) {
			hmapping->present = true;
			return hmapping;
		}
	}

	return NULL;
}

static int
nvmm_native_hmapping_free(struct nvmm_machine *mach, uintptr_t hva, size_t size)
{
	struct nvmm_hmapping *hmapping;
	size_t i;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];
		if (!hmapping->present || hmapping->hva != hva ||
		    hmapping->size != size) {
			continue;
		}

		os_vmobj_unmap(os_curproc_map, hmapping->hva,
		    hmapping->hva + hmapping->size, false);
		os_vmobj_rel(hmapping->vmobj);

		hmapping->vmobj = NULL;
		hmapping->present = false;

		return 0;
	}

	return ENOENT;
}

static int
nvmm_native_hva_map(struct nvmm_owner *owner, struct nvmm_ioc_hva_map *args)
{
	struct nvmm_machine *mach;
	struct nvmm_hmapping *hmapping;
	vaddr_t uva;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error)
		return error;

	error = nvmm_native_hmapping_validate(mach, args->hva, args->size);
	if (error)
		goto out;

	hmapping = nvmm_native_hmapping_alloc(mach);
	if (hmapping == NULL) {
		error = ENOBUFS;
		goto out;
	}

	hmapping->hva = args->hva;
	hmapping->size = args->size;
	hmapping->vmobj = os_vmobj_create(hmapping->size);
	uva = hmapping->hva;

	/* Map the vmobj into the user address space, as pageable. */
	error = os_vmobj_map(os_curproc_map, &uva, hmapping->size,
	    hmapping->vmobj, 0, false /* !wired */, true /* fixed */,
	    true /* shared */, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_native_hva_unmap(struct nvmm_owner *owner, struct nvmm_ioc_hva_unmap *args)
{
	struct nvmm_machine *mach;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error)
		return error;

	error = nvmm_native_hmapping_free(mach, args->hva, args->size);

	nvmm_machine_put(mach);
	return error;
}

/* -------------------------------------------------------------------------- */

static int
nvmm_native_gpa_map(struct nvmm_owner *owner, struct nvmm_ioc_gpa_map *args)
{
	struct nvmm_machine *mach;
	os_vmobj_t *vmobj;
	gpaddr_t gpa;
	gpaddr_t gpa_end;
	size_t off;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	if ((args->prot & ~(PROT_READ|PROT_WRITE|PROT_EXEC)) != 0) {
		error = EINVAL;
		goto out;
	}

	/*
	 * Overflow tests MUST be done very carefully to avoid compiler
	 * optimizations from effectively deleting the test.
	 */
	gpa = args->gpa;
	gpa_end = gpa + args->size;
	if (gpa_end <= gpa) {
		error = EINVAL;
		goto out;
	}

	if ((gpa % PAGE_SIZE) != 0 || (args->size % PAGE_SIZE) != 0 ||
	    (args->hva % PAGE_SIZE) != 0) {
		error = EINVAL;
		goto out;
	}
	if (args->hva == 0) {
		error = EINVAL;
		goto out;
	}

	if (gpa < mach->gpa_begin || gpa >= mach->gpa_end) {
		error = EINVAL;
		goto out;
	}
	if (gpa_end  > mach->gpa_end) {
		error = EINVAL;
		goto out;
	}

	vmobj = nvmm_native_hmapping_getvmobj(mach, args->hva, args->size, &off);
	if (vmobj == NULL) {
		error = EINVAL;
		goto out;
	}

	/* Map the vmobj into the machine address space, as pageable. */
	error = os_vmobj_map(&mach->vm->vm_map, &gpa, args->size, vmobj, off,
	    false /* !wired */, true /* fixed */, false /* !shared */,
	    args->prot, PROT_READ | PROT_WRITE | PROT_EXEC);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_native_gpa_unmap(struct nvmm_owner *owner, struct nvmm_ioc_gpa_unmap *args)
{
	struct nvmm_machine *mach;
	gpaddr_t gpa;
	gpaddr_t gpa_end;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	/*
	 * Overflow tests MUST be done very carefully to avoid compiler
	 * optimizations from effectively deleting the test.
	 */
	gpa = args->gpa;
	gpa_end = gpa + args->size;
	if (gpa_end <= gpa) {
		error = EINVAL;
		goto out;
	}

	if ((gpa % PAGE_SIZE) != 0 || (args->size % PAGE_SIZE) != 0) {
		error = EINVAL;
		goto out;
	}
	if (gpa < mach->gpa_begin || gpa >= mach->gpa_end) {
		error = EINVAL;
		goto out;
	}
	if (gpa_end >= mach->gpa_end) {
		error = EINVAL;
		goto out;
	}

	/* Unmap the memory from the machine. */
	os_vmobj_unmap(&mach->vm->vm_map, gpa, gpa + args->size, false);

out:
	nvmm_machine_put(mach);
	return error;
}

/* -------------------------------------------------------------------------- */

static int
nvmm_native_ctl_mach_info(struct nvmm_owner *owner, struct nvmm_ioc_ctl *args)
{
	struct nvmm_ctl_mach_info ctl;
	struct nvmm_machine *mach;
	int error;
	size_t i;

	if (args->size != sizeof(ctl))
		return EINVAL;
	error = copyin(args->data, &ctl, sizeof(ctl));
	if (error)
		return error;

	error = nvmm_machine_get(owner, ctl.machid, &mach, true);
	if (error)
		return error;

	ctl.nvcpus = mach->ncpus;

	ctl.nram = 0;
	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		if (!mach->hmap[i].present)
			continue;
		ctl.nram += mach->hmap[i].size;
	}

	ctl.pid = mach->owner->pid;
	ctl.time = mach->time;

	nvmm_machine_put(mach);

	error = copyout(&ctl, args->data, sizeof(ctl));
	if (error)
		return error;

	return 0;
}

static int
nvmm_native_ctl(struct nvmm_owner *owner, struct nvmm_ioc_ctl *args)
{
	switch (args->op) {
	case NVMM_CTL_MACH_INFO:
		return nvmm_native_ctl_mach_info(owner, args);
	default:
		return EINVAL;
	}
}

/* -------------------------------------------------------------------------- */

int
nvmm_native_ioctl(struct nvmm_owner *owner, unsigned long cmd, void *data)
{
	switch (cmd) {
	case NVMM_IOC_CAPABILITY:
		return nvmm_native_capability(owner, data);
	case NVMM_IOC_MACHINE_CREATE:
		return nvmm_native_machine_create(owner, data);
	case NVMM_IOC_MACHINE_DESTROY:
		return nvmm_native_machine_destroy(owner, data);
	case NVMM_IOC_MACHINE_CONFIGURE:
		return nvmm_native_machine_configure(owner, data);
	case NVMM_IOC_VCPU_CREATE:
		return nvmm_native_vcpu_create(owner, data);
	case NVMM_IOC_VCPU_DESTROY:
		return nvmm_native_vcpu_destroy(owner, data);
	case NVMM_IOC_VCPU_CONFIGURE:
		return nvmm_native_vcpu_configure(owner, data);
	case NVMM_IOC_VCPU_SETSTATE:
		return nvmm_native_vcpu_setstate(owner, data);
	case NVMM_IOC_VCPU_GETSTATE:
		return nvmm_native_vcpu_getstate(owner, data);
	case NVMM_IOC_VCPU_RUN:
		return nvmm_native_vcpu_run(owner, data);
	case NVMM_IOC_GPA_MAP:
		return nvmm_native_gpa_map(owner, data);
	case NVMM_IOC_GPA_UNMAP:
		return nvmm_native_gpa_unmap(owner, data);
	case NVMM_IOC_HVA_MAP:
		return nvmm_native_hva_map(owner, data);
	case NVMM_IOC_HVA_UNMAP:
		return nvmm_native_hva_unmap(owner, data);
	case NVMM_IOC_CTL:
		return nvmm_native_ctl(owner, data);
	default:
		return EINVAL;
	}
}
