/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM execution backend for the vcpu object.
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <machine/cpufunc.h>
#include <machine/specialreg.h>
#include <vm/vm.h>
#include <vm/pmap.h>

#include "vmm_loader_x86.h"
#include "vmm_machine.h"
#include "vmm_mem.h"
#include "vmm_svm.h"
#include "vmm_vcpu.h"

#define VMM_SVM_PTE_P		0x001ULL
#define VMM_SVM_PTE_RW		0x002ULL
#define VMM_SVM_PTE_U		0x004ULL
#define VMM_SVM_PTE_FLAGS	(VMM_SVM_PTE_P | VMM_SVM_PTE_RW | VMM_SVM_PTE_U)

#define VMM_SVM_CTRL_INTERCEPT_INTR	(1U << 0)
#define VMM_SVM_CTRL_INTERCEPT_NMI	(1U << 1)
#define VMM_SVM_CTRL_INTERCEPT_INIT	(1U << 3)
#define VMM_SVM_CTRL_INTERCEPT_CPUID	(1U << 18)
#define VMM_SVM_CTRL_INTERCEPT_HLT	(1U << 24)
#define VMM_SVM_CTRL_INTERCEPT_IOIO	(1U << 27)
#define VMM_SVM_CTRL_INTERCEPT_MSR	(1U << 28)
#define VMM_SVM_CTRL_INTERCEPT_SHUTDOWN	(1U << 31)

#define VMM_SVM_CTRL_INTERCEPT_VMMCALL	(1U << 1)
#define VMM_SVM_CTRL_INTERCEPT_XSETBV	(1U << 13)

#define VMM_SVM_CTRL_ENABLE_NP		0x001ULL
#define VMM_SVM_CTRL_TLB_FLUSH_ALL	0x001U
#define VMM_SVM_CTRL_V_INTR_MASKING	(1ULL << 24)

#define VMM_SVM_EXIT_INTR		0x060ULL
#define VMM_SVM_EXIT_NMI		0x061ULL
#define VMM_SVM_EXIT_CPUID		0x072ULL
#define VMM_SVM_EXIT_HLT		0x078ULL
#define VMM_SVM_EXIT_IOIO		0x07bULL
#define VMM_SVM_EXIT_MSR		0x07cULL
#define VMM_SVM_EXIT_SHUTDOWN		0x07fULL
#define VMM_SVM_EXIT_VMMCALL		0x081ULL
#define VMM_SVM_EXIT_XSETBV		0x08dULL
#define VMM_SVM_EXIT_NPF		0x400ULL

#define VMM_SVM_MSRBM_PAGES		2
#define VMM_SVM_IOBM_PAGES		3
#define VMM_SVM_ASID			1

struct vmm_svm_segment {
	uint16_t selector;
	uint16_t attrib;
	uint32_t limit;
	uint64_t base;
} __packed;

struct vmm_svm_ctrl {
	uint32_t intercept_cr;
	uint32_t intercept_dr;
	uint32_t intercept_vec;
	uint32_t intercept_misc1;
	uint32_t intercept_misc2;
	uint32_t intercept_misc3;
	uint8_t  reserved1[36];
	uint16_t pause_filt_thresh;
	uint16_t pause_filt_cnt;
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
	uint32_t vmcb_clean;
	uint32_t reserved2;
	uint64_t nrip;
	uint8_t inst_len;
	uint8_t inst_bytes[15];
	uint64_t avic_abpp;
	uint64_t reserved3;
	uint64_t avic_ltp;
	uint64_t avic_phys;
	uint64_t reserved4;
	uint64_t vmsa_ptr;
	uint8_t pad[752];
} __packed;

struct vmm_svm_state {
	struct vmm_svm_segment es;
	struct vmm_svm_segment cs;
	struct vmm_svm_segment ss;
	struct vmm_svm_segment ds;
	struct vmm_svm_segment fs;
	struct vmm_svm_segment gs;
	struct vmm_svm_segment gdt;
	struct vmm_svm_segment ldt;
	struct vmm_svm_segment idt;
	struct vmm_svm_segment tr;
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
	uint64_t g_pat;
	uint64_t dbgctl;
	uint64_t br_from;
	uint64_t br_to;
	uint64_t int_from;
	uint64_t int_to;
	uint8_t pad[2408];
} __packed;

struct vmm_svm_vmcb {
	struct vmm_svm_ctrl ctrl;
	struct vmm_svm_state state;
} __packed;

struct vmm_svm_page {
	void *va;
	uint64_t pa;
};

struct vmm_svm_backend {
	struct vmm_machine *borrow_imm_machine;
	struct vmm_svm_vmcb *own_mut_vmcb;
	uint64_t imm_vmcb_pa;
	uint8_t *own_mut_iobm;
	uint64_t imm_iobm_pa;
	uint8_t *own_mut_msrbm;
	uint64_t imm_msrbm_pa;
	void *own_mut_hsave;
	uint64_t imm_hsave_pa;
	struct vmm_svm_page *own_mut_npt_pages;
	uint32_t imm_npt_page_count;
	uint64_t imm_npt_root_pa;
	uint64_t mut_gprs[VMM_X64_NGPR];
};

CTASSERT(sizeof(struct vmm_svm_ctrl) == 1024);
CTASSERT(sizeof(struct vmm_svm_state) == 0xc00);
CTASSERT(sizeof(struct vmm_svm_vmcb) == PAGE_SIZE);

void	vmm_svm_vmrun(uint64_t vmcb_pa, uint64_t *gprs);

static void *
vmm_svm_contig_alloc(uint64_t *pa, size_t pages)
{
	void *va;

	va = contigmalloc(pages * PAGE_SIZE, M_TEMP, M_WAITOK | M_ZERO,
	    0, ~0UL, PAGE_SIZE, 0);
	if (va != NULL)
		*pa = vtophys(va);
	return va;
}

static void
vmm_svm_contig_free(void *va, size_t pages)
{
	if (va != NULL)
		contigfree(va, pages * PAGE_SIZE, M_TEMP);
}

int
vmm_svm_available(void)
{
	uint32_t descs[4];
	uint64_t msr;

	do_cpuid(0x80000000, descs);
	if (descs[0] < 0x8000000a)
		return 0;
	do_cpuid(0x80000001, descs);
	if ((descs[2] & CPUID_SVM) == 0)
		return 0;
	do_cpuid(0x8000000a, descs);
	if ((descs[3] & CPUID_AMD_SVM_NP) == 0 ||
	    (descs[3] & CPUID_AMD_SVM_NRIPS) == 0)
		return 0;
	msr = rdmsr(MSR_AMD_VM_CR);
	if ((msr & VM_CR_SVMDIS) && (msr & VM_CR_LOCK))
		return 0;
	return 1;
}

static void
vmm_svm_seg_load(const struct vmm_x64_seg_state *src,
    struct vmm_svm_segment *dst)
{
	dst->selector = src->selector;
	dst->attrib = src->attrib;
	dst->limit = src->limit;
	dst->base = src->base;
}

static int
vmm_svm_npt_alloc_page(struct vmm_svm_backend *svm, uint32_t idx)
{
	svm->own_mut_npt_pages[idx].va =
	    vmm_svm_contig_alloc(&svm->own_mut_npt_pages[idx].pa, 1);
	return svm->own_mut_npt_pages[idx].va == NULL ? ENOMEM : 0;
}

static int
vmm_svm_build_npt(struct vmm_svm_backend *svm, struct vmm_machine *m)
{
	struct vmm_mem *mem = &m->own_mut_mem;
	uint64_t mem_size = vmm_mem_size(mem);
	uint64_t gpa, hpa;
	uint64_t *pml4, *pdpt, *pd, *pt;
	uint32_t pd_pages, pt_pages, page_count;
	uint32_t i, pdp_i, pd_i, pt_i;
	int error;

	if (mem_size == 0 || mem_size > (512ULL << 30))
		return EOPNOTSUPP;
	pd_pages = (uint32_t)((mem_size + ((1ULL << 30) - 1)) >> 30);
	pt_pages = (uint32_t)((mem_size + ((2ULL << 20) - 1)) >> 21);
	page_count = 2 + pd_pages + pt_pages;

	svm->own_mut_npt_pages = kmalloc(sizeof(*svm->own_mut_npt_pages) *
	    page_count, M_TEMP, M_WAITOK | M_ZERO);
	svm->imm_npt_page_count = page_count;
	for (i = 0; i < page_count; i++) {
		error = vmm_svm_npt_alloc_page(svm, i);
		if (error)
			return error;
	}

	pml4 = svm->own_mut_npt_pages[0].va;
	pdpt = svm->own_mut_npt_pages[1].va;
	pml4[0] = svm->own_mut_npt_pages[1].pa | VMM_SVM_PTE_FLAGS;
	for (i = 0; i < pd_pages; i++) {
		pdpt[i] = svm->own_mut_npt_pages[2 + i].pa | VMM_SVM_PTE_FLAGS;
	}
	for (gpa = 0; gpa < mem_size; gpa += PAGE_SIZE) {
		error = vmm_mem_gpa_pa(mem, gpa, &hpa);
		if (error)
			return error;
		pdp_i = (uint32_t)(gpa >> 30);
		pd_i = (uint32_t)((gpa >> 21) & 0x1ff);
		pt_i = (uint32_t)((gpa >> 12) & 0x1ff);
		pd = svm->own_mut_npt_pages[2 + pdp_i].va;
		if (pd[pd_i] == 0) {
			uint32_t pt_page = 2 + pd_pages +
			    (uint32_t)(gpa >> 21);
			pd[pd_i] = svm->own_mut_npt_pages[pt_page].pa |
			    VMM_SVM_PTE_FLAGS;
		}
		pt = (uint64_t *)(uintptr_t)
		    svm->own_mut_npt_pages[2 + pd_pages +
		    (uint32_t)(gpa >> 21)].va;
		pt[pt_i] = (hpa & ~PAGE_MASK) | VMM_SVM_PTE_FLAGS;
	}
	svm->imm_npt_root_pa = svm->own_mut_npt_pages[0].pa;
	return 0;
}

static void
vmm_svm_load_state(struct vmm_svm_backend *svm,
    const struct vmm_x64_vcpu_state *v)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;

	bcopy(v->gpr, svm->mut_gprs, sizeof(svm->mut_gprs));
	vmcb->state.rax = v->gpr[VMM_X64_GPR_RAX];
	vmcb->state.rsp = v->gpr[VMM_X64_GPR_RSP];
	vmcb->state.rip = v->gpr[VMM_X64_GPR_RIP];
	vmcb->state.rflags = v->gpr[VMM_X64_GPR_RFLAGS];
	if (vmcb->state.rflags == 0)
		vmcb->state.rflags = 2;

	vmcb->state.cr0 = v->cr[VMM_X64_CR_CR0] | CR0_ET | CR0_NE;
	vmcb->state.cr2 = v->cr[VMM_X64_CR_CR2];
	vmcb->state.cr3 = v->cr[VMM_X64_CR_CR3];
	vmcb->state.cr4 = v->cr[VMM_X64_CR_CR4];
	vmcb->state.efer = v->msr[VMM_X64_MSR_EFER] | EFER_SVME;
	vmcb->state.g_pat = v->msr[VMM_X64_MSR_PAT];
	vmcb->state.star = v->msr[VMM_X64_MSR_STAR];
	vmcb->state.lstar = v->msr[VMM_X64_MSR_LSTAR];
	vmcb->state.cstar = v->msr[VMM_X64_MSR_CSTAR];
	vmcb->state.sfmask = v->msr[VMM_X64_MSR_SFMASK];
	vmcb->state.kernelgsbase = v->msr[VMM_X64_MSR_KERNELGSBASE];
	vmcb->state.sysenter_cs = v->msr[VMM_X64_MSR_SYSENTER_CS];
	vmcb->state.sysenter_esp = v->msr[VMM_X64_MSR_SYSENTER_ESP];
	vmcb->state.sysenter_eip = v->msr[VMM_X64_MSR_SYSENTER_EIP];

	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_ES], &vmcb->state.es);
	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_CS], &vmcb->state.cs);
	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_SS], &vmcb->state.ss);
	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_DS], &vmcb->state.ds);
	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_FS], &vmcb->state.fs);
	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_GS], &vmcb->state.gs);
	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_GDT], &vmcb->state.gdt);
	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_IDT], &vmcb->state.idt);
	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_LDT], &vmcb->state.ldt);
	vmm_svm_seg_load(&v->seg[VMM_X64_SEG_TR], &vmcb->state.tr);
	vmcb->state.cpl = (vmcb->state.ss.attrib >> 5) & 3;
}

int
vmm_svm_vcpu_create(struct vmm_machine *m, const struct vmm_launch *launch,
    void **backendp)
{
	struct vmm_svm_backend *svm;
	struct vmm_svm_vmcb *vmcb;
	int error;

	if (backendp == NULL || launch == NULL || launch->imm_vcpu0.vcpu_id != 0)
		return EINVAL;
	*backendp = NULL;
	svm = kmalloc(sizeof(*svm), M_TEMP, M_WAITOK | M_ZERO);
	svm->borrow_imm_machine = m;

	svm->own_mut_vmcb = vmm_svm_contig_alloc(&svm->imm_vmcb_pa, 1);
	svm->own_mut_iobm = vmm_svm_contig_alloc(&svm->imm_iobm_pa,
	    VMM_SVM_IOBM_PAGES);
	svm->own_mut_msrbm = vmm_svm_contig_alloc(&svm->imm_msrbm_pa,
	    VMM_SVM_MSRBM_PAGES);
	svm->own_mut_hsave = vmm_svm_contig_alloc(&svm->imm_hsave_pa, 1);
	if (svm->own_mut_vmcb == NULL || svm->own_mut_iobm == NULL ||
	    svm->own_mut_msrbm == NULL || svm->own_mut_hsave == NULL) {
		error = ENOMEM;
		goto fail;
	}
	error = vmm_svm_build_npt(svm, m);
	if (error)
		goto fail;

	vmcb = svm->own_mut_vmcb;
	memset(svm->own_mut_iobm, 0xff, VMM_SVM_IOBM_PAGES * PAGE_SIZE);
	memset(svm->own_mut_msrbm, 0xff, VMM_SVM_MSRBM_PAGES * PAGE_SIZE);
	vmcb->ctrl.intercept_misc1 =
	    VMM_SVM_CTRL_INTERCEPT_INTR |
	    VMM_SVM_CTRL_INTERCEPT_NMI |
	    VMM_SVM_CTRL_INTERCEPT_INIT |
	    VMM_SVM_CTRL_INTERCEPT_CPUID |
	    VMM_SVM_CTRL_INTERCEPT_HLT |
	    VMM_SVM_CTRL_INTERCEPT_IOIO |
	    VMM_SVM_CTRL_INTERCEPT_MSR |
	    VMM_SVM_CTRL_INTERCEPT_SHUTDOWN;
	vmcb->ctrl.intercept_misc2 =
	    VMM_SVM_CTRL_INTERCEPT_VMMCALL |
	    VMM_SVM_CTRL_INTERCEPT_XSETBV;
	vmcb->ctrl.iopm_base_pa = svm->imm_iobm_pa;
	vmcb->ctrl.msrpm_base_pa = svm->imm_msrbm_pa;
	vmcb->ctrl.guest_asid = VMM_SVM_ASID;
	vmcb->ctrl.tlb_ctrl = VMM_SVM_CTRL_TLB_FLUSH_ALL;
	vmcb->ctrl.v = VMM_SVM_CTRL_V_INTR_MASKING;
	vmcb->ctrl.enable1 = VMM_SVM_CTRL_ENABLE_NP;
	vmcb->ctrl.n_cr3 = svm->imm_npt_root_pa;
	vmm_svm_load_state(svm, &launch->imm_vcpu0);

	*backendp = svm;
	return 0;

fail:
	vmm_svm_vcpu_destroy(svm);
	return error;
}

void
vmm_svm_vcpu_destroy(void *backend)
{
	struct vmm_svm_backend *svm = backend;
	uint32_t i;

	if (svm == NULL)
		return;
	if (svm->own_mut_npt_pages != NULL) {
		for (i = 0; i < svm->imm_npt_page_count; i++)
			vmm_svm_contig_free(svm->own_mut_npt_pages[i].va, 1);
		kfree(svm->own_mut_npt_pages, M_TEMP);
	}
	vmm_svm_contig_free(svm->own_mut_hsave, 1);
	vmm_svm_contig_free(svm->own_mut_msrbm, VMM_SVM_MSRBM_PAGES);
	vmm_svm_contig_free(svm->own_mut_iobm, VMM_SVM_IOBM_PAGES);
	vmm_svm_contig_free(svm->own_mut_vmcb, 1);
	kfree(svm, M_TEMP);
}

static void
vmm_svm_enable_cpu(struct vmm_svm_backend *svm)
{
	uint64_t msr;

	msr = rdmsr(MSR_AMD_VM_CR);
	if (msr & VM_CR_SVMDIS)
		wrmsr(MSR_AMD_VM_CR, msr & ~VM_CR_SVMDIS);
	msr = rdmsr(MSR_EFER);
	if ((msr & EFER_SVME) == 0)
		wrmsr(MSR_EFER, msr | EFER_SVME);
	wrmsr(MSR_AMD_VM_HSAVE_PA, svm->imm_hsave_pa);
}

static void
vmm_svm_clgi(void)
{
	__asm volatile("clgi" ::: "memory");
}

static void
vmm_svm_stgi(void)
{
	__asm volatile("stgi" ::: "memory");
}

static void
vmm_svm_handle_cpuid(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	uint32_t regs[4];

	cpuid_count((uint32_t)vmcb->state.rax,
	    (uint32_t)svm->mut_gprs[VMM_X64_GPR_RCX], regs);
	vmcb->state.rax = regs[0];
	svm->mut_gprs[VMM_X64_GPR_RBX] = regs[1];
	svm->mut_gprs[VMM_X64_GPR_RCX] = regs[2];
	svm->mut_gprs[VMM_X64_GPR_RDX] = regs[3];
	vmcb->state.rip = vmcb->ctrl.nrip;
}

void
vmm_svm_vcpu_run(void *backend, struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_backend *svm = backend;
	struct vmm_svm_vmcb *vmcb;
	struct vmm_machine *m = vc->borrow_imm_machine;

	if (svm == NULL)
		return;
	vmcb = svm->own_mut_vmcb;
	while (!vmm_machine_vcpu_should_stop(m)) {
		crit_enter();
		vmm_svm_enable_cpu(svm);
		vmm_svm_clgi();
		vmm_svm_vmrun(svm->imm_vmcb_pa, svm->mut_gprs);
		vmm_svm_stgi();
		crit_exit();
		switch (vmcb->ctrl.exitcode) {
		case VMM_SVM_EXIT_INTR:
		case VMM_SVM_EXIT_NMI:
			break;
		case VMM_SVM_EXIT_CPUID:
			vmm_svm_handle_cpuid(svm);
			break;
		case VMM_SVM_EXIT_HLT:
			return;
		case VMM_SVM_EXIT_SHUTDOWN:
		case VMM_SVM_EXIT_NPF:
		case VMM_SVM_EXIT_IOIO:
		case VMM_SVM_EXIT_MSR:
		case VMM_SVM_EXIT_VMMCALL:
		case VMM_SVM_EXIT_XSETBV:
		default:
			kprintf("vmm_svm: vmexit 0x%jx info1=0x%jx info2=0x%jx rip=0x%jx\n",
			    (uintmax_t)vmcb->ctrl.exitcode,
			    (uintmax_t)vmcb->ctrl.exitinfo1,
			    (uintmax_t)vmcb->ctrl.exitinfo2,
			    (uintmax_t)vmcb->state.rip);
			return;
		}
		lwkt_user_yield();
	}
}
