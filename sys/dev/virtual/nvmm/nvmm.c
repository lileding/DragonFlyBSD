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
#include <sys/systm.h>

#include "nvmm.h"
#include "nvmm_internal.h"
#include "nvmm_ioctl.h"

static struct nvmm_machine machines[NVMM_MAX_MACHINES];
volatile unsigned int nmachines __cacheline_aligned;

struct nvmm_owner nvmm_root_owner;

int
nvmm_machine_alloc(struct nvmm_machine **ret)
{
	struct nvmm_machine *mach;
	size_t i;

	for (i = 0; i < NVMM_MAX_MACHINES; i++) {
		mach = &machines[i];

		os_rwl_wlock(&mach->lock);
		if (mach->present) {
			os_rwl_unlock(&mach->lock);
			continue;
		}

		mach->present = true;
		mach->time = time_second;
		*ret = mach;
		os_atomic_inc_uint(&nmachines);
		return 0;
	}

	return ENOBUFS;
}

void
nvmm_machine_free(struct nvmm_machine *mach)
{
	OS_ASSERT(os_rwl_wheld(&mach->lock));
	OS_ASSERT(mach->present);
	mach->present = false;
	os_atomic_dec_uint(&nmachines);
}

int
nvmm_machine_get(struct nvmm_owner *owner, nvmm_machid_t machid,
	struct nvmm_machine **ret, bool writer)
{
	struct nvmm_machine *mach;

	if (__predict_false(machid >= NVMM_MAX_MACHINES))
		return EINVAL;
	mach = &machines[machid];

	if (__predict_false(writer)) {
		os_rwl_wlock(&mach->lock);
	} else {
		os_rwl_rlock(&mach->lock);
	}
	if (__predict_false(!mach->present)) {
		os_rwl_unlock(&mach->lock);
		return ENOENT;
	}
	if (__predict_false(mach->owner != owner &&
	    owner != &nvmm_root_owner)) {
		os_rwl_unlock(&mach->lock);
		return EPERM;
	}
	*ret = mach;
	return 0;
}

void
nvmm_machine_put(struct nvmm_machine *mach)
{
	os_rwl_unlock(&mach->lock);
}

int
nvmm_vcpu_alloc(struct nvmm_machine *mach, nvmm_cpuid_t cpuid,
	struct nvmm_cpu **ret)
{
	struct nvmm_cpu *vcpu;

	if (cpuid >= NVMM_MAX_VCPUS)
		return EINVAL;
	vcpu = &mach->cpus[cpuid];

	os_mtx_lock(&vcpu->lock);
	if (vcpu->present) {
		os_mtx_unlock(&vcpu->lock);
		return EBUSY;
	}

	vcpu->present = true;
	vcpu->comm = NULL;
	vcpu->vmm_vcpu = NULL;
	*ret = vcpu;
	return 0;
}

void
nvmm_vcpu_free(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu)
{
	OS_ASSERT(os_mtx_owned(&vcpu->lock));
	vcpu->present = false;
	if (vcpu->comm != NULL) {
		os_vmobj_unmap(os_kernel_map, (vaddr_t)vcpu->comm,
		    (vaddr_t)vcpu->comm + NVMM_COMM_PAGE_SIZE, true);
		/* Userland still owns its pageable comm-page mapping. */
		vcpu->comm = NULL;
	}
}

int
nvmm_vcpu_get(struct nvmm_machine *mach, nvmm_cpuid_t cpuid,
	struct nvmm_cpu **ret)
{
	struct nvmm_cpu *vcpu;

	if (__predict_false(cpuid >= NVMM_MAX_VCPUS))
		return EINVAL;
	vcpu = &mach->cpus[cpuid];

	os_mtx_lock(&vcpu->lock);
	if (__predict_false(!vcpu->present)) {
		os_mtx_unlock(&vcpu->lock);
		return ENOENT;
	}
	*ret = vcpu;
	return 0;
}

void
nvmm_vcpu_put(struct nvmm_cpu *vcpu)
{
	os_mtx_unlock(&vcpu->lock);
}

void
nvmm_kill_machines(struct nvmm_owner *owner)
{
	struct nvmm_machine *mach;
	size_t i;
	int error;

	for (i = 0; i < NVMM_MAX_MACHINES; i++) {
		mach = &machines[i];

		os_rwl_wlock(&mach->lock);
		if (!mach->present || mach->owner != owner) {
			os_rwl_unlock(&mach->lock);
			continue;
		}

		error = nvmm_syscall_machine_destroy_locked(mach);
		if (error != 0) {
			kprintf("nvmm: unable to destroy machine %u on close (%d)\n",
			    mach->machid, error);
		}
		os_rwl_unlock(&mach->lock);
	}
}

int
nvmm_init(void)
{
	size_t i, n;

	for (i = 0; i < NVMM_MAX_MACHINES; i++) {
		machines[i].machid = i;
		os_rwl_init(&machines[i].lock);
		for (n = 0; n < NVMM_MAX_VCPUS; n++) {
			machines[i].cpus[n].present = false;
			machines[i].cpus[n].cpuid = n;
			os_mtx_init(&machines[i].cpus[n].lock);
		}
	}
	return 0;
}

void
nvmm_fini(void)
{
	size_t i, n;

	for (i = 0; i < NVMM_MAX_MACHINES; i++) {
		os_rwl_destroy(&machines[i].lock);
		for (n = 0; n < NVMM_MAX_VCPUS; n++)
			os_mtx_destroy(&machines[i].cpus[n].lock);
	}
}

int
nvmm_ioctl(struct nvmm_owner *owner, unsigned long cmd, void *data)
{
	switch (cmd) {
	case NVMM_IOC_CAPABILITY:
		return nvmm_syscall_capability(owner, data);
	case NVMM_IOC_MACHINE_CREATE:
		return nvmm_syscall_machine_create(owner, data);
	case NVMM_IOC_MACHINE_DESTROY:
		return nvmm_syscall_machine_destroy(owner, data);
	case NVMM_IOC_MACHINE_CONFIGURE:
		return nvmm_syscall_machine_configure(owner, data);
	case NVMM_IOC_VCPU_CREATE:
		return nvmm_syscall_vcpu_create(owner, data);
	case NVMM_IOC_VCPU_DESTROY:
		return nvmm_syscall_vcpu_destroy(owner, data);
	case NVMM_IOC_VCPU_CONFIGURE:
		return nvmm_syscall_vcpu_configure(owner, data);
	case NVMM_IOC_VCPU_SETSTATE:
		return nvmm_syscall_vcpu_setstate(owner, data);
	case NVMM_IOC_VCPU_GETSTATE:
		return nvmm_syscall_vcpu_getstate(owner, data);
	case NVMM_IOC_VCPU_INJECT:
		return nvmm_syscall_vcpu_inject(owner, data);
	case NVMM_IOC_VCPU_RUN:
		return nvmm_syscall_vcpu_run(owner, data);
	case NVMM_IOC_GPA_MAP:
		return nvmm_syscall_gpa_map(owner, data);
	case NVMM_IOC_GPA_UNMAP:
		return nvmm_syscall_gpa_unmap(owner, data);
	case NVMM_IOC_HVA_MAP:
		return nvmm_syscall_hva_map(owner, data);
	case NVMM_IOC_HVA_UNMAP:
		return nvmm_syscall_hva_unmap(owner, data);
	case NVMM_IOC_CTL:
		return nvmm_syscall_ctl(owner, data);
	default:
		return EINVAL;
	}
}
