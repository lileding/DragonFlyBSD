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
#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/systm.h>

#include "nvmm.h"
#include "nvmm_internal.h"
#include "nvmm_ioctl.h"
#include "x86/nvmm_x86_internal.h"

CTASSERT(NVMM_X64_STATE_SEGS == VMM_X64_STATE_SEGS);
CTASSERT(NVMM_X64_STATE_GPRS == VMM_X64_STATE_GPRS);
CTASSERT(NVMM_X64_STATE_CRS == VMM_X64_STATE_CRS);
CTASSERT(NVMM_X64_STATE_DRS == VMM_X64_STATE_DRS);
CTASSERT(NVMM_X64_STATE_MSRS == VMM_X64_STATE_MSRS);
CTASSERT(NVMM_X64_STATE_INTR == VMM_X64_STATE_INTR);
CTASSERT(NVMM_X64_STATE_FPU == VMM_X64_STATE_FPU);
CTASSERT(sizeof(struct nvmm_x64_state) == sizeof(struct vmm_cpustate));
CTASSERT(sizeof(struct nvmm_vcpu_exit) == sizeof(struct vmm_cpuexit));

static void
nvmm_syscall_state_to_vmm(struct vmm_cpustate *dst,
	const struct nvmm_x64_state *src, uint64_t flags)
{
	if (flags & NVMM_X64_STATE_SEGS)
		bcopy(src->segs, dst->segs, sizeof(src->segs));
	if (flags & NVMM_X64_STATE_GPRS)
		bcopy(src->gprs, dst->gprs, sizeof(src->gprs));
	if (flags & NVMM_X64_STATE_CRS)
		bcopy(src->crs, dst->crs, sizeof(src->crs));
	if (flags & NVMM_X64_STATE_DRS)
		bcopy(src->drs, dst->drs, sizeof(src->drs));
	if (flags & NVMM_X64_STATE_MSRS)
		bcopy(src->msrs, dst->msrs, sizeof(src->msrs));
	if (flags & NVMM_X64_STATE_INTR)
		bcopy(&src->intr, &dst->intr, sizeof(src->intr));
	if (flags & NVMM_X64_STATE_FPU)
		bcopy(&src->fpu, &dst->fpu, sizeof(src->fpu));
}

static void
nvmm_syscall_state_from_vmm(struct nvmm_x64_state *dst,
	const struct vmm_cpustate *src, uint64_t flags)
{
	if (flags & NVMM_X64_STATE_SEGS)
		bcopy(src->segs, dst->segs, sizeof(dst->segs));
	if (flags & NVMM_X64_STATE_GPRS)
		bcopy(src->gprs, dst->gprs, sizeof(dst->gprs));
	if (flags & NVMM_X64_STATE_CRS)
		bcopy(src->crs, dst->crs, sizeof(dst->crs));
	if (flags & NVMM_X64_STATE_DRS)
		bcopy(src->drs, dst->drs, sizeof(dst->drs));
	if (flags & NVMM_X64_STATE_MSRS)
		bcopy(src->msrs, dst->msrs, sizeof(dst->msrs));
	if (flags & NVMM_X64_STATE_INTR)
		bcopy(&src->intr, &dst->intr, sizeof(dst->intr));
	if (flags & NVMM_X64_STATE_FPU)
		bcopy(&src->fpu, &dst->fpu, sizeof(dst->fpu));
}

static void
nvmm_syscall_state_commit(struct nvmm_cpu *vcpu)
{
	uint64_t flags;

	flags = vcpu->comm->state_commit;
	vcpu->comm->state_commit = 0;
	nvmm_syscall_state_to_vmm(&vcpu->state, &vcpu->comm->state, flags);
	vcpu->comm->state_cached |= flags;
}

static void
nvmm_syscall_state_provide(struct nvmm_cpu *vcpu, uint64_t flags)
{
	nvmm_syscall_state_from_vmm(&vcpu->comm->state, &vcpu->state, flags);
	vcpu->comm->state_wanted = 0;
	vcpu->comm->state_cached |= flags;
}

static int
nvmm_syscall_event_from_comm(const struct nvmm_cpu *vcpu,
	struct vmm_cpuevent *event)
{
	const struct nvmm_vcpu_event *src = &vcpu->comm->event;

	switch (src->type) {
	case NVMM_VCPU_EVENT_EXCP:
		event->type = VMM_CPUEVENT_EXCP;
		event->vector = src->vector;
		event->error = src->u.excp.error;
		return 0;
	case NVMM_VCPU_EVENT_INTR:
		event->type = VMM_CPUEVENT_INTR;
		event->vector = src->vector;
		event->error = 0;
		return 0;
	default:
		return EINVAL;
	}
}

static void
nvmm_syscall_exit_from_vmm(struct nvmm_vcpu_exit *dst,
	const struct vmm_cpuexit *src)
{
	dst->reason = src->reason;
	bcopy(&src->u, &dst->u, sizeof(dst->u));
	dst->exitstate.rflags = src->exitstate.rflags;
	dst->exitstate.cr8 = src->exitstate.cr8;
	dst->exitstate.int_shadow = src->exitstate.int_shadow;
	dst->exitstate.int_window_exiting = src->exitstate.int_window_exiting;
	dst->exitstate.nmi_window_exiting = src->exitstate.nmi_window_exiting;
	dst->exitstate.evt_pending = src->exitstate.evt_pending;
}

int
nvmm_syscall_capability(struct nvmm_owner *owner __unused,
	struct nvmm_ioc_capability *args)
{
	struct vmm_x64_capability capability;
	int error;

	error = vmm_capability(&capability);
	if (error != 0)
		return error;

	args->cap.version = NVMM_KERN_VERSION;
	args->cap.state_size = sizeof(struct nvmm_x64_state);
	args->cap.comm_size = NVMM_COMM_PAGE_SIZE;
	args->cap.max_machines = NVMM_MAX_MACHINES;
	args->cap.max_vcpus = NVMM_MAX_VCPUS;
	args->cap.max_ram = NVMM_MAX_RAM;
	args->cap.arch.mach_conf_support = 0;
	args->cap.arch.vcpu_conf_support = 0;
	args->cap.arch.xcr0_mask = capability.xcr0_mask;
	args->cap.arch.mxcsr_mask = capability.mxcsr_mask;
	args->cap.arch.conf_cpuid_maxops = capability.cpuid_mask_max;
	return 0;
}

int
nvmm_syscall_machine_create(struct nvmm_owner *owner,
	struct nvmm_ioc_machine_create *args)
{
	struct nvmm_machine *mach;
	int error;

	error = nvmm_machine_alloc(&mach);
	if (error != 0)
		return error;

	mach->owner = owner;
	memset(mach->hmap, 0, sizeof(mach->hmap));
	mach->gpa_begin = 0;
	mach->gpa_end = NVMM_MAX_RAM;
	mach->vm = os_vmspace_create(mach->gpa_begin, mach->gpa_end);
	if (mach->vm == NULL) {
		error = ENOMEM;
		goto fail_machine;
	}

	error = vmm_machine_create(mach->vm, &mach->vmm_machine);
	if (error != 0)
		goto fail_vmspace;

	mach->commvmobj = os_vmobj_create(NVMM_MAX_VCPUS *
	    NVMM_COMM_PAGE_SIZE);
	if (mach->commvmobj == NULL) {
		error = ENOMEM;
		goto fail_vmm;
	}

	args->machid = mach->machid;
	nvmm_machine_put(mach);
	return 0;

fail_vmm:
	(void)vmm_machine_destroy(mach->vmm_machine);
	mach->vmm_machine = NULL;
fail_vmspace:
	os_vmspace_destroy(mach->vm);
	mach->vm = NULL;
fail_machine:
	nvmm_machine_free(mach);
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_machine_destroy_locked(struct nvmm_machine *mach)
{
	struct nvmm_cpu *vcpu;
	size_t i;
	int error;

	OS_ASSERT(os_rwl_wheld(&mach->lock));

	for (i = 0; i < NVMM_MAX_VCPUS; i++) {
		error = nvmm_vcpu_get(mach, i, &vcpu);
		if (error == ENOENT)
			continue;
		if (error != 0)
			return error;

		error = vmm_vcpu_destroy(vcpu->vmm_vcpu);
		if (error == 0) {
			vcpu->vmm_vcpu = NULL;
			nvmm_vcpu_free(mach, vcpu);
			os_atomic_dec_uint(&mach->ncpus);
		}
		nvmm_vcpu_put(vcpu);
		if (error != 0)
			return error;
	}

	error = vmm_machine_destroy(mach->vmm_machine);
	if (error != 0)
		return error;
	mach->vmm_machine = NULL;

	os_vmspace_destroy(mach->vm);
	mach->vm = NULL;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		if (!mach->hmap[i].present)
			continue;
		os_vmobj_rel(mach->hmap[i].vmobj);
		mach->hmap[i].vmobj = NULL;
		mach->hmap[i].present = false;
	}

	nvmm_machine_free(mach);
	return 0;
}

int
nvmm_syscall_machine_destroy(struct nvmm_owner *owner,
	struct nvmm_ioc_machine_destroy *args)
{
	struct nvmm_machine *mach;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error != 0)
		return error;
	error = nvmm_syscall_machine_destroy_locked(mach);
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_machine_configure(struct nvmm_owner *owner __unused,
	struct nvmm_ioc_machine_configure *args __unused)
{
	return EINVAL;
}

int
nvmm_syscall_vcpu_create(struct nvmm_owner *owner,
	struct nvmm_ioc_vcpu_create *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error != 0)
		return error;

	error = nvmm_vcpu_alloc(mach, args->cpuid, &vcpu);
	if (error != 0)
		goto out_machine;

	error = os_vmobj_map(os_kernel_map, (vaddr_t *)&vcpu->comm,
	    NVMM_COMM_PAGE_SIZE, mach->commvmobj,
	    args->cpuid * NVMM_COMM_PAGE_SIZE, true, false, true,
	    PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
	if (error != 0)
		goto fail_vcpu;

	memset(vcpu->comm, 0, NVMM_COMM_PAGE_SIZE);
	error = os_vmobj_map(os_curproc_map, (vaddr_t *)&args->comm,
	    NVMM_COMM_PAGE_SIZE, mach->commvmobj,
	    args->cpuid * NVMM_COMM_PAGE_SIZE, false, false, true,
	    PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
	if (error != 0)
		goto fail_vcpu;

	nvmm_syscall_state_to_vmm(&vcpu->state, &nvmm_x86_reset_state,
	    NVMM_X64_STATE_ALL);
	error = vmm_vcpu_create(mach->vmm_machine, &vcpu->state, NULL, 0,
	    &vcpu->vmm_vcpu);
	if (error != 0)
		goto fail_vcpu;

	nvmm_syscall_state_provide(vcpu, NVMM_X64_STATE_ALL);
	nvmm_vcpu_put(vcpu);
	os_atomic_inc_uint(&mach->ncpus);
	nvmm_machine_put(mach);
	return 0;

fail_vcpu:
	nvmm_vcpu_free(mach, vcpu);
	nvmm_vcpu_put(vcpu);
out_machine:
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_vcpu_destroy(struct nvmm_owner *owner,
	struct nvmm_ioc_vcpu_destroy *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error != 0)
		return error;
	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error != 0)
		goto out_machine;

	error = vmm_vcpu_destroy(vcpu->vmm_vcpu);
	if (error == 0) {
		vcpu->vmm_vcpu = NULL;
		nvmm_vcpu_free(mach, vcpu);
		os_atomic_dec_uint(&mach->ncpus);
	}
	nvmm_vcpu_put(vcpu);
out_machine:
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_vcpu_configure(struct nvmm_owner *owner __unused,
	struct nvmm_ioc_vcpu_configure *args __unused)
{
	return ENOTSUP;
}

int
nvmm_syscall_vcpu_setstate(struct nvmm_owner *owner,
	struct nvmm_ioc_vcpu_setstate *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error != 0)
		return error;
	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error == 0) {
		nvmm_syscall_state_commit(vcpu);
		nvmm_vcpu_put(vcpu);
	}
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_vcpu_getstate(struct nvmm_owner *owner,
	struct nvmm_ioc_vcpu_getstate *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error != 0)
		return error;
	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error == 0) {
		nvmm_syscall_state_provide(vcpu, vcpu->comm->state_wanted);
		nvmm_vcpu_put(vcpu);
	}
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_vcpu_inject(struct nvmm_owner *owner,
	struct nvmm_ioc_vcpu_inject *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	struct vmm_cpuevent event;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error != 0)
		return error;
	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error == 0) {
		error = nvmm_syscall_event_from_comm(vcpu, &event);
		if (error == 0)
			error = vmm_vcpu_inject(vcpu->vmm_vcpu, &event);
		nvmm_vcpu_put(vcpu);
	}
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_vcpu_run(struct nvmm_owner *owner,
	struct nvmm_ioc_vcpu_run *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	struct vmm_cpuexit *exit;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error != 0)
		return error;
	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error != 0)
		goto out_machine;

	nvmm_syscall_state_commit(vcpu);
	error = vmm_vcpu_run(vcpu->vmm_vcpu, &exit);
	nvmm_syscall_state_provide(vcpu, NVMM_X64_STATE_ALL);
	if (error == 0 && exit != NULL)
		nvmm_syscall_exit_from_vmm(&args->exit, exit);
	nvmm_vcpu_put(vcpu);
out_machine:
	nvmm_machine_put(mach);
	return error;
}

static os_vmobj_t *
nvmm_syscall_hmapping_getvmobj(struct nvmm_machine *mach, uintptr_t hva,
	size_t size, size_t *off)
{
	struct nvmm_hmapping *hmapping;
	size_t i;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];
		if (!hmapping->present)
			continue;
		if (hva >= hmapping->hva &&
		    hva + size <= hmapping->hva + hmapping->size) {
			*off = hva - hmapping->hva;
			return hmapping->vmobj;
		}
	}
	return NULL;
}

static int
nvmm_syscall_hmapping_validate(struct nvmm_machine *mach, uintptr_t hva,
	size_t size)
{
	struct nvmm_hmapping *hmapping;
	uintptr_t hva_end;
	uintptr_t hmap_end;
	size_t i;

	if ((hva % PAGE_SIZE) != 0 || (size % PAGE_SIZE) != 0 || hva == 0)
		return EINVAL;
	hva_end = hva + size;
	if (hva_end <= hva)
		return EINVAL;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];
		if (!hmapping->present)
			continue;
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
nvmm_syscall_hmapping_alloc(struct nvmm_machine *mach)
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
nvmm_syscall_hmapping_free(struct nvmm_machine *mach, uintptr_t hva,
	size_t size)
{
	struct nvmm_hmapping *hmapping;
	size_t i;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];
		if (!hmapping->present || hmapping->hva != hva ||
		    hmapping->size != size)
			continue;

		os_vmobj_unmap(os_curproc_map, hmapping->hva,
		    hmapping->hva + hmapping->size, false);
		os_vmobj_rel(hmapping->vmobj);
		hmapping->vmobj = NULL;
		hmapping->present = false;
		return 0;
	}
	return ENOENT;
}

int
nvmm_syscall_hva_map(struct nvmm_owner *owner, struct nvmm_ioc_hva_map *args)
{
	struct nvmm_machine *mach;
	struct nvmm_hmapping *hmapping;
	vaddr_t uva;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error != 0)
		return error;
	error = nvmm_syscall_hmapping_validate(mach, args->hva, args->size);
	if (error != 0)
		goto out;
	hmapping = nvmm_syscall_hmapping_alloc(mach);
	if (hmapping == NULL) {
		error = ENOBUFS;
		goto out;
	}
	hmapping->hva = args->hva;
	hmapping->size = args->size;
	hmapping->vmobj = os_vmobj_create(hmapping->size);
	uva = hmapping->hva;
	error = os_vmobj_map(os_curproc_map, &uva, hmapping->size,
	    hmapping->vmobj, 0, false, true, true,
	    PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
out:
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_hva_unmap(struct nvmm_owner *owner,
	struct nvmm_ioc_hva_unmap *args)
{
	struct nvmm_machine *mach;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error != 0)
		return error;
	error = nvmm_syscall_hmapping_free(mach, args->hva, args->size);
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_gpa_map(struct nvmm_owner *owner, struct nvmm_ioc_gpa_map *args)
{
	struct nvmm_machine *mach;
	os_vmobj_t *vmobj;
	gpaddr_t gpa;
	gpaddr_t gpa_end;
	size_t off;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error != 0)
		return error;
	if ((args->prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
		error = EINVAL;
		goto out;
	}
	gpa = args->gpa;
	gpa_end = gpa + args->size;
	if (gpa_end <= gpa || (gpa % PAGE_SIZE) != 0 ||
	    (args->size % PAGE_SIZE) != 0 || (args->hva % PAGE_SIZE) != 0 ||
	    args->hva == 0) {
		error = EINVAL;
		goto out;
	}
	if (gpa < mach->gpa_begin || gpa >= mach->gpa_end ||
	    gpa_end > mach->gpa_end) {
		error = EINVAL;
		goto out;
	}
	vmobj = nvmm_syscall_hmapping_getvmobj(mach, args->hva, args->size,
	    &off);
	if (vmobj == NULL) {
		error = EINVAL;
		goto out;
	}
	error = os_vmobj_map(&mach->vm->vm_map, &gpa, args->size, vmobj, off,
	    false, true, false, args->prot,
	    PROT_READ | PROT_WRITE | PROT_EXEC);
out:
	nvmm_machine_put(mach);
	return error;
}

int
nvmm_syscall_gpa_unmap(struct nvmm_owner *owner,
	struct nvmm_ioc_gpa_unmap *args)
{
	struct nvmm_machine *mach;
	gpaddr_t gpa;
	gpaddr_t gpa_end;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error != 0)
		return error;
	gpa = args->gpa;
	gpa_end = gpa + args->size;
	if (gpa_end <= gpa || (gpa % PAGE_SIZE) != 0 ||
	    (args->size % PAGE_SIZE) != 0 || gpa < mach->gpa_begin ||
	    gpa >= mach->gpa_end || gpa_end >= mach->gpa_end) {
		error = EINVAL;
		goto out;
	}
	os_vmobj_unmap(&mach->vm->vm_map, gpa, gpa + args->size, false);
	error = 0;
out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_syscall_ctl_mach_info(struct nvmm_owner *owner,
	struct nvmm_ioc_ctl *args)
{
	struct nvmm_ctl_mach_info ctl;
	struct nvmm_machine *mach;
	int error;
	size_t i;

	if (args->size != sizeof(ctl))
		return EINVAL;
	error = copyin(args->data, &ctl, sizeof(ctl));
	if (error != 0)
		return error;
	error = nvmm_machine_get(owner, ctl.machid, &mach, true);
	if (error != 0)
		return error;
	ctl.nvcpus = mach->ncpus;
	ctl.nram = 0;
	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		if (mach->hmap[i].present)
			ctl.nram += mach->hmap[i].size;
	}
	ctl.pid = mach->owner->pid;
	ctl.time = mach->time;
	nvmm_machine_put(mach);
	return copyout(&ctl, args->data, sizeof(ctl));
}

int
nvmm_syscall_ctl(struct nvmm_owner *owner, struct nvmm_ioc_ctl *args)
{
	switch (args->op) {
	case NVMM_CTL_MACH_INFO:
		return nvmm_syscall_ctl_mach_info(owner, args);
	default:
		return EINVAL;
	}
}
