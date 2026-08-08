/*-
 * Copyright (c) 2018-2021 Maxime Villard, m00nbsd.net
 * All rights reserved.
 *
 * Portions derive from DragonFly NVMM's AMD SVM backend.
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
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * AMD SVM runtime backend.  This port deliberately excludes NVMM's owner,
 * ioctl, and userspace-memory machinery.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include <machine/cpufunc.h>
#include <machine/specialreg.h>

#include <vm/pmap.h>

#include "../../vmm_machine.h"
#include "../../vmm_vcpu.h"
#include "vmm_x64_svm.h"

#define VMM_X64_SVM_EXIT_INVALID	(~0ULL)

#define VMM_X64_SVM_INTERCEPT_INTR		(1U << 0)
#define VMM_X64_SVM_INTERCEPT_NMI		(1U << 1)
#define VMM_X64_SVM_INTERCEPT_INIT		(1U << 3)
#define VMM_X64_SVM_INTERCEPT_CR0_SEL		(1U << 5)
#define VMM_X64_SVM_INTERCEPT_RDPMC		(1U << 15)
#define VMM_X64_SVM_INTERCEPT_CPUID		(1U << 18)
#define VMM_X64_SVM_INTERCEPT_RSM		(1U << 19)
#define VMM_X64_SVM_INTERCEPT_INVD		(1U << 22)
#define VMM_X64_SVM_INTERCEPT_HLT		(1U << 24)
#define VMM_X64_SVM_INTERCEPT_INVLPGA		(1U << 26)
#define VMM_X64_SVM_INTERCEPT_IOIO		(1U << 27)
#define VMM_X64_SVM_INTERCEPT_MSR		(1U << 28)
#define VMM_X64_SVM_INTERCEPT_FERR		(1U << 30)
#define VMM_X64_SVM_INTERCEPT_SHUTDOWN		(1U << 31)

#define VMM_X64_SVM_INTERCEPT_VMRUN		(1U << 0)
#define VMM_X64_SVM_INTERCEPT_VMMCALL		(1U << 1)
#define VMM_X64_SVM_INTERCEPT_VMLOAD		(1U << 2)
#define VMM_X64_SVM_INTERCEPT_VMSAVE		(1U << 3)
#define VMM_X64_SVM_INTERCEPT_STGI		(1U << 4)
#define VMM_X64_SVM_INTERCEPT_CLGI		(1U << 5)
#define VMM_X64_SVM_INTERCEPT_SKINIT		(1U << 6)
#define VMM_X64_SVM_INTERCEPT_RDTSCP		(1U << 7)
#define VMM_X64_SVM_INTERCEPT_MONITOR		(1U << 10)
#define VMM_X64_SVM_INTERCEPT_MWAIT		(1U << 11)
#define VMM_X64_SVM_INTERCEPT_XSETBV		(1U << 13)
#define VMM_X64_SVM_INTERCEPT_RDPRU		(1U << 14)

#define VMM_X64_SVM_INTERCEPT_INVLPGB		(1U << 0)
#define VMM_X64_SVM_INTERCEPT_PCID		(1U << 2)
#define VMM_X64_SVM_INTERCEPT_MCOMMIT		(1U << 3)
#define VMM_X64_SVM_INTERCEPT_TLBSYNC		(1U << 4)

#define VMM_X64_SVM_ENABLE_NP			(1ULL << 0)
#define VMM_X64_SVM_TLB_FLUSH_ALL		0x01U
#define VMM_X64_SVM_V_INTR_MASKING		(1ULL << 24)

#define VMM_X64_SVM_MSR_VM_CR			0xc0010114U
#define VMM_X64_SVM_MSR_VM_CR_LOCK		(1ULL << 3)
#define VMM_X64_SVM_MSR_VM_CR_SVME_DISABLE	(1ULL << 4)

#define VMM_X64_SVM_CPUID_SVM			(1U << 2)

#define VMM_X64_SVM_MSRBM_PAGES	2
#define VMM_X64_SVM_MSRBM_SIZE		(VMM_X64_SVM_MSRBM_PAGES * PAGE_SIZE)
#define VMM_X64_SVM_IOBM_PAGES	3
#define VMM_X64_SVM_IOBM_SIZE		(VMM_X64_SVM_IOBM_PAGES * PAGE_SIZE)

struct vmm_x64_svm_ctrl {
	uint32_t intercept_cr;
	uint32_t intercept_dr;
	uint32_t intercept_vec;
	uint32_t intercept_misc1;
	uint32_t intercept_misc2;
	uint32_t intercept_misc3;
	uint8_t reserved1[36];
	uint16_t pause_filter_threshold;
	uint16_t pause_filter_count;
	uint64_t iopm_base_pa;
	uint64_t msrpm_base_pa;
	uint64_t tsc_offset;
	uint32_t guest_asid;
	uint32_t tlb_ctrl;
	uint64_t v;
	uint64_t intr;
	uint64_t exitcode;
	uint64_t exitinfo1;
	uint64_t exitinfo2;
	uint64_t exitintinfo;
	uint64_t enable1;
	uint64_t avic;
	uint64_t ghcb;
	uint64_t eventinj;
	uint64_t n_cr3;
	uint64_t enable2;
	uint32_t clean;
	uint32_t reserved2;
	uint64_t nrip;
	uint8_t inst_len;
	uint8_t inst_bytes[15];
	uint64_t avic_backing_page_pa;
	uint64_t reserved3;
	uint64_t avic_logical_table_pa;
	uint64_t avic_physical_table;
	uint64_t reserved4;
	uint64_t vmsa_pa;
	uint8_t pad[752];
} __packed;

CTASSERT(sizeof(struct vmm_x64_svm_ctrl) == 0x400);
CTASSERT(__offsetof(struct vmm_x64_svm_ctrl, avic) == 0x98);
CTASSERT(__offsetof(struct vmm_x64_svm_ctrl, n_cr3) == 0xb0);
CTASSERT(__offsetof(struct vmm_x64_svm_ctrl, avic_backing_page_pa) == 0x0e0);

struct vmm_x64_svm_segment {
	uint16_t selector;
	uint16_t attrib;
	uint32_t limit;
	uint64_t base;
} __packed;

CTASSERT(sizeof(struct vmm_x64_svm_segment) == 16);

struct vmm_x64_svm_state {
	struct vmm_x64_svm_segment es;
	struct vmm_x64_svm_segment cs;
	struct vmm_x64_svm_segment ss;
	struct vmm_x64_svm_segment ds;
	struct vmm_x64_svm_segment fs;
	struct vmm_x64_svm_segment gs;
	struct vmm_x64_svm_segment gdt;
	struct vmm_x64_svm_segment ldt;
	struct vmm_x64_svm_segment idt;
	struct vmm_x64_svm_segment tr;
	uint8_t reserved1[43];
	uint8_t cpl;
	uint8_t reserved2[4];
	uint64_t efer;
	uint8_t reserved3[112];
	uint64_t cr4;
	uint64_t cr3;
	uint64_t cr0;
	uint64_t dr7;
	uint64_t dr6;
	uint64_t rflags;
	uint64_t rip;
	uint8_t reserved4[88];
	uint64_t rsp;
	uint64_t s_cet;
	uint64_t ssp;
	uint64_t isst_addr;
	uint64_t rax;
	uint64_t star;
	uint64_t lstar;
	uint64_t cstar;
	uint64_t sfmask;
	uint64_t kernelgsbase;
	uint64_t sysenter_cs;
	uint64_t sysenter_esp;
	uint64_t sysenter_eip;
	uint64_t cr2;
	uint8_t reserved5[32];
	uint64_t pat;
	uint64_t dbgctl;
	uint64_t br_from;
	uint64_t br_to;
	uint64_t int_from;
	uint64_t int_to;
	uint8_t pad[2408];
} __packed;

CTASSERT(sizeof(struct vmm_x64_svm_state) == 0xc00);

struct vmm_x64_svm_vmcb {
	struct vmm_x64_svm_ctrl ctrl;
	struct vmm_x64_svm_state state;
} __packed;

CTASSERT(sizeof(struct vmm_x64_svm_vmcb) == PAGE_SIZE);
CTASSERT(__offsetof(struct vmm_x64_svm_vmcb, state) == 0x400);

struct vmm_x64_svm_vcpu {
	struct vmm_x64_svm_vmcb *vmcb;
	uint64_t vmcb_pa;
	uint8_t *iobm;
	uint64_t iobm_pa;
	uint8_t *msrbm;
	uint64_t msrbm_pa;
	uint64_t gpr[VMM_X64_GPR_COUNT];
};

struct vmm_x64_svm_cpuid {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
};

static int vmm_x64_svm_probe(void);
static int vmm_x64_svm_init(void);
static void vmm_x64_svm_fini(void);
static int vmm_x64_svm_machine_create(struct vmm_machine *);
static void vmm_x64_svm_machine_destroy(struct vmm_machine *);
static int vmm_x64_svm_machine_pmap_init(struct vmm_machine *,
	struct pmap *);
static int vmm_x64_svm_vcpu_create(struct vmm_vcpu *);
static void vmm_x64_svm_vcpu_destroy(struct vmm_vcpu *);
static int vmm_x64_svm_vcpu_run(struct vmm_vcpu *, struct vmm_cpuexit **);
static void vmm_x64_svm_vcpu_kick(struct vmm_vcpu *);
static int vmm_x64_svm_alloc(size_t, void **, uint64_t *);
static void vmm_x64_svm_free(void *, size_t);
static void vmm_x64_svm_load_segment(const struct vmm_segment *,
	struct vmm_x64_svm_segment *);
static void vmm_x64_svm_load_state(struct vmm_vcpu *,
	struct vmm_x64_svm_vcpu *);
static void vmm_x64_svm_init_intercepts(struct vmm_x64_svm_vcpu *);

const struct vmm_backend_ops vmm_x64_svm_backend = {
	.name = "x64/svm",
	.probe = vmm_x64_svm_probe,
	.init = vmm_x64_svm_init,
	.fini = vmm_x64_svm_fini,
	.machine_create = vmm_x64_svm_machine_create,
	.machine_destroy = vmm_x64_svm_machine_destroy,
	.machine_pmap_init = vmm_x64_svm_machine_pmap_init,
	.vcpu_create = vmm_x64_svm_vcpu_create,
	.vcpu_destroy = vmm_x64_svm_vcpu_destroy,
	.vcpu_run = vmm_x64_svm_vcpu_run,
	.vcpu_kick = vmm_x64_svm_vcpu_kick,
};

static int
vmm_x64_svm_probe(void)
{
	struct vmm_x64_svm_cpuid desc;
	uint64_t vm_cr;

	do_cpuid(0, (uint32_t *)&desc);
	if (bcmp(&desc.ebx, "Auth", 4) != 0 ||
	    bcmp(&desc.edx, "enti", 4) != 0 ||
	    bcmp(&desc.ecx, "cAMD", 4) != 0)
		return ENXIO;
	do_cpuid(0x80000000, (uint32_t *)&desc);
	if (desc.eax < 0x8000000a)
		return ENXIO;
	do_cpuid(0x80000001, (uint32_t *)&desc);
	if ((desc.ecx & VMM_X64_SVM_CPUID_SVM) == 0)
		return ENXIO;
	do_cpuid(0x8000000a, (uint32_t *)&desc);
	if ((desc.eax & CPUID_AMD_SVM_REV) != 1 ||
	    (desc.edx & CPUID_AMD_SVM_NP) == 0 ||
	    (desc.edx & CPUID_AMD_SVM_NRIPS) == 0)
		return ENXIO;
	vm_cr = rdmsr(VMM_X64_SVM_MSR_VM_CR);
	if ((vm_cr & VMM_X64_SVM_MSR_VM_CR_SVME_DISABLE) != 0 &&
	    (vm_cr & VMM_X64_SVM_MSR_VM_CR_LOCK) != 0)
		return EPERM;
	return 0;
}

static int
vmm_x64_svm_init(void)
{

	return 0;
}

static void
vmm_x64_svm_fini(void)
{
}

static int
vmm_x64_svm_machine_create(struct vmm_machine *machine)
{

	(void)machine;
	return 0;
}

static void
vmm_x64_svm_machine_destroy(struct vmm_machine *machine)
{

	(void)machine;
}

static int
vmm_x64_svm_machine_pmap_init(struct vmm_machine *machine,
	struct pmap *pmap)
{

	(void)machine;
	pmap_npt_transform(pmap, 0);
	return 0;
}

static int
vmm_x64_svm_vcpu_create(struct vmm_vcpu *vcpu)
{
	struct vmm_x64_svm_vcpu *svm;
	int error;

	svm = kmalloc(sizeof(*svm), M_VMM, M_WAITOK | M_ZERO);
	if (svm == NULL)
		return ENOMEM;
	error = vmm_x64_svm_alloc(PAGE_SIZE, (void **)&svm->vmcb,
	    &svm->vmcb_pa);
	if (error != 0)
		goto fail;
	error = vmm_x64_svm_alloc(VMM_X64_SVM_IOBM_SIZE,
	    (void **)&svm->iobm, &svm->iobm_pa);
	if (error != 0)
		goto fail;
	error = vmm_x64_svm_alloc(VMM_X64_SVM_MSRBM_SIZE,
	    (void **)&svm->msrbm, &svm->msrbm_pa);
	if (error != 0)
		goto fail;
	vmm_x64_svm_init_intercepts(svm);
	vmm_x64_svm_load_state(vcpu, svm);
	vcpu->backend = svm;
	return 0;

fail:
	vmm_x64_svm_free(svm->msrbm, VMM_X64_SVM_MSRBM_SIZE);
	vmm_x64_svm_free(svm->iobm, VMM_X64_SVM_IOBM_SIZE);
	vmm_x64_svm_free(svm->vmcb, PAGE_SIZE);
	kfree(svm, M_VMM);
	return error;
}

static void
vmm_x64_svm_vcpu_destroy(struct vmm_vcpu *vcpu)
{
	struct vmm_x64_svm_vcpu *svm;

	svm = vcpu->backend;
	if (svm == NULL)
		return;
	vmm_x64_svm_free(svm->msrbm, VMM_X64_SVM_MSRBM_SIZE);
	vmm_x64_svm_free(svm->iobm, VMM_X64_SVM_IOBM_SIZE);
	vmm_x64_svm_free(svm->vmcb, PAGE_SIZE);
	kfree(svm, M_VMM);
	vcpu->backend = NULL;
}

static int
vmm_x64_svm_vcpu_run(struct vmm_vcpu *vcpu, struct vmm_cpuexit **reason)
{

	(void)vcpu;
	(void)reason;
	/* VMRUN starts once the machine mapper supplies the NPT root. */
	return ENOTSUP;
}

static void
vmm_x64_svm_vcpu_kick(struct vmm_vcpu *vcpu)
{

	(void)vcpu;
}

static int
vmm_x64_svm_alloc(size_t size, void **vap, uint64_t *pap)
{
	void *va;

	va = contigmalloc(size, M_VMM, M_WAITOK | M_ZERO, 0, ~0UL,
	    PAGE_SIZE, 0);
	if (va == NULL)
		return ENOMEM;
	*vap = va;
	*pap = vtophys(va);
	return 0;
}

static void
vmm_x64_svm_free(void *va, size_t size)
{
	if (va != NULL)
		contigfree(va, size, M_VMM);
}

static void
vmm_x64_svm_load_segment(const struct vmm_segment *source,
	struct vmm_x64_svm_segment *target)
{
	target->selector = source->selector;
	target->attrib = source->attrib;
	target->limit = source->limit;
	target->base = source->base;
}

static void
vmm_x64_svm_load_state(struct vmm_vcpu *vcpu,
	struct vmm_x64_svm_vcpu *svm)
{
	struct vmm_cpustate *state;
	struct vmm_x64_svm_vmcb *vmcb;

	state = vcpu->state;
	vmcb = svm->vmcb;
	bcopy(state->gpr, svm->gpr, sizeof(svm->gpr));
	vmcb->state.rax = state->gpr[VMM_X64_GPR_RAX];
	vmcb->state.rsp = state->gpr[VMM_X64_GPR_RSP];
	vmcb->state.rip = state->gpr[VMM_X64_GPR_RIP];
	vmcb->state.rflags = state->gpr[VMM_X64_GPR_RFLAGS];
	vmcb->state.cr0 = state->cr[VMM_X64_CR_CR0];
	vmcb->state.cr2 = state->cr[VMM_X64_CR_CR2];
	vmcb->state.cr3 = state->cr[VMM_X64_CR_CR3];
	vmcb->state.cr4 = state->cr[VMM_X64_CR_CR4];
	vmcb->state.efer = state->msr[VMM_X64_MSR_EFER] | EFER_SVME;
	vmcb->state.pat = state->msr[VMM_X64_MSR_PAT];
	vmcb->state.star = state->msr[VMM_X64_MSR_STAR];
	vmcb->state.lstar = state->msr[VMM_X64_MSR_LSTAR];
	vmcb->state.cstar = state->msr[VMM_X64_MSR_CSTAR];
	vmcb->state.sfmask = state->msr[VMM_X64_MSR_SFMASK];
	vmcb->state.kernelgsbase = state->msr[VMM_X64_MSR_KERNELGSBASE];
	vmcb->state.sysenter_cs = state->msr[VMM_X64_MSR_SYSENTER_CS];
	vmcb->state.sysenter_esp = state->msr[VMM_X64_MSR_SYSENTER_ESP];
	vmcb->state.sysenter_eip = state->msr[VMM_X64_MSR_SYSENTER_EIP];
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_ES], &vmcb->state.es);
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_CS], &vmcb->state.cs);
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_SS], &vmcb->state.ss);
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_DS], &vmcb->state.ds);
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_FS], &vmcb->state.fs);
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_GS], &vmcb->state.gs);
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_GDT], &vmcb->state.gdt);
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_IDT], &vmcb->state.idt);
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_LDT], &vmcb->state.ldt);
	vmm_x64_svm_load_segment(&state->seg[VMM_X64_SEG_TR], &vmcb->state.tr);
	vmcb->state.cpl = (vmcb->state.ss.attrib >> 5) & 3;
	vmcb->ctrl.tsc_offset = state->msr[VMM_X64_MSR_TSC] - rdtsc();
}

static void
vmm_x64_svm_init_intercepts(struct vmm_x64_svm_vcpu *svm)
{
	struct vmm_x64_svm_vmcb *vmcb;

	vmcb = svm->vmcb;
	vmcb->ctrl.intercept_misc1 = VMM_X64_SVM_INTERCEPT_INTR |
	    VMM_X64_SVM_INTERCEPT_NMI | VMM_X64_SVM_INTERCEPT_INIT |
	    VMM_X64_SVM_INTERCEPT_CR0_SEL | VMM_X64_SVM_INTERCEPT_RDPMC |
	    VMM_X64_SVM_INTERCEPT_CPUID | VMM_X64_SVM_INTERCEPT_RSM |
	    VMM_X64_SVM_INTERCEPT_INVD | VMM_X64_SVM_INTERCEPT_HLT |
	    VMM_X64_SVM_INTERCEPT_INVLPGA | VMM_X64_SVM_INTERCEPT_IOIO |
	    VMM_X64_SVM_INTERCEPT_MSR | VMM_X64_SVM_INTERCEPT_FERR |
	    VMM_X64_SVM_INTERCEPT_SHUTDOWN;
	vmcb->ctrl.intercept_misc2 = VMM_X64_SVM_INTERCEPT_VMRUN |
	    VMM_X64_SVM_INTERCEPT_VMMCALL | VMM_X64_SVM_INTERCEPT_VMLOAD |
	    VMM_X64_SVM_INTERCEPT_VMSAVE | VMM_X64_SVM_INTERCEPT_STGI |
	    VMM_X64_SVM_INTERCEPT_CLGI | VMM_X64_SVM_INTERCEPT_SKINIT |
	    VMM_X64_SVM_INTERCEPT_RDTSCP | VMM_X64_SVM_INTERCEPT_MONITOR |
	    VMM_X64_SVM_INTERCEPT_MWAIT | VMM_X64_SVM_INTERCEPT_XSETBV |
	    VMM_X64_SVM_INTERCEPT_RDPRU;
	vmcb->ctrl.intercept_misc3 = VMM_X64_SVM_INTERCEPT_INVLPGB |
	    VMM_X64_SVM_INTERCEPT_PCID | VMM_X64_SVM_INTERCEPT_MCOMMIT |
	    VMM_X64_SVM_INTERCEPT_TLBSYNC;
	memset(svm->iobm, 0xff, VMM_X64_SVM_IOBM_SIZE);
	vmcb->ctrl.iopm_base_pa = svm->iobm_pa;
	memset(svm->msrbm, 0xff, VMM_X64_SVM_MSRBM_SIZE);
	vmcb->ctrl.msrpm_base_pa = svm->msrbm_pa;
	vmcb->ctrl.guest_asid = 1;
	vmcb->ctrl.tlb_ctrl = VMM_X64_SVM_TLB_FLUSH_ALL;
	vmcb->ctrl.v = VMM_X64_SVM_V_INTR_MASKING;
	vmcb->ctrl.enable1 = VMM_X64_SVM_ENABLE_NP;
	vmcb->ctrl.exitcode = VMM_X64_SVM_EXIT_INVALID;
}
