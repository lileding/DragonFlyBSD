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
#include <sys/ucontext.h>
#include <machine/cpufunc.h>
#include <machine/cpu.h>
#include <machine/md_var.h>
#include <machine/npx.h>
#include <machine/specialreg.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>

#include "vmm_loader_x86.h"
#include "vmm_machine.h"
#include "vmm_mem.h"
#include "vmm_svm.h"
#include "vmm_vcpu.h"

#define VMM_SVM_CTRL_INTERCEPT_INTR	(1U << 0)
#define VMM_SVM_CTRL_INTERCEPT_NMI	(1U << 1)
#define VMM_SVM_CTRL_INTERCEPT_SMI	(1U << 2)
#define VMM_SVM_CTRL_INTERCEPT_INIT	(1U << 3)
#define VMM_SVM_CTRL_INTERCEPT_VINTR	(1U << 4)
#define VMM_SVM_CTRL_INTERCEPT_CR0_SEL	(1U << 5)
#define VMM_SVM_CTRL_INTERCEPT_RIDTR	(1U << 6)
#define VMM_SVM_CTRL_INTERCEPT_RGDTR	(1U << 7)
#define VMM_SVM_CTRL_INTERCEPT_RLDTR	(1U << 8)
#define VMM_SVM_CTRL_INTERCEPT_RTR	(1U << 9)
#define VMM_SVM_CTRL_INTERCEPT_WIDTR	(1U << 10)
#define VMM_SVM_CTRL_INTERCEPT_WGDTR	(1U << 11)
#define VMM_SVM_CTRL_INTERCEPT_WLDTR	(1U << 12)
#define VMM_SVM_CTRL_INTERCEPT_WTR	(1U << 13)
#define VMM_SVM_CTRL_INTERCEPT_RDTSC	(1U << 14)
#define VMM_SVM_CTRL_INTERCEPT_RDPMC	(1U << 15)
#define VMM_SVM_CTRL_INTERCEPT_PUSHF	(1U << 16)
#define VMM_SVM_CTRL_INTERCEPT_POPF	(1U << 17)
#define VMM_SVM_CTRL_INTERCEPT_CPUID	(1U << 18)
#define VMM_SVM_CTRL_INTERCEPT_RSM	(1U << 19)
#define VMM_SVM_CTRL_INTERCEPT_IRET	(1U << 20)
#define VMM_SVM_CTRL_INTERCEPT_INTN	(1U << 21)
#define VMM_SVM_CTRL_INTERCEPT_INVD	(1U << 22)
#define VMM_SVM_CTRL_INTERCEPT_PAUSE	(1U << 23)
#define VMM_SVM_CTRL_INTERCEPT_HLT	(1U << 24)
#define VMM_SVM_CTRL_INTERCEPT_INVLPG	(1U << 25)
#define VMM_SVM_CTRL_INTERCEPT_INVLPGA	(1U << 26)
#define VMM_SVM_CTRL_INTERCEPT_IOIO	(1U << 27)
#define VMM_SVM_CTRL_INTERCEPT_MSR	(1U << 28)
#define VMM_SVM_CTRL_INTERCEPT_TASKSW	(1U << 29)
#define VMM_SVM_CTRL_INTERCEPT_FERR	(1U << 30)
#define VMM_SVM_CTRL_INTERCEPT_SHUTDOWN	(1U << 31)

#define VMM_SVM_CTRL_INTERCEPT_VMRUN	(1U << 0)
#define VMM_SVM_CTRL_INTERCEPT_VMMCALL	(1U << 1)
#define VMM_SVM_CTRL_INTERCEPT_VMLOAD	(1U << 2)
#define VMM_SVM_CTRL_INTERCEPT_VMSAVE	(1U << 3)
#define VMM_SVM_CTRL_INTERCEPT_STGI	(1U << 4)
#define VMM_SVM_CTRL_INTERCEPT_CLGI	(1U << 5)
#define VMM_SVM_CTRL_INTERCEPT_SKINIT	(1U << 6)
#define VMM_SVM_CTRL_INTERCEPT_RDTSCP	(1U << 7)
#define VMM_SVM_CTRL_INTERCEPT_MONITOR	(1U << 10)
#define VMM_SVM_CTRL_INTERCEPT_MWAIT	(1U << 11)
#define VMM_SVM_CTRL_INTERCEPT_MWAIT_ARMED (1U << 12)
#define VMM_SVM_CTRL_INTERCEPT_XSETBV	(1U << 13)
#define VMM_SVM_CTRL_INTERCEPT_RDPRU	(1U << 14)
#define VMM_SVM_CTRL_INTERCEPT_EFER	(1U << 15)

#define VMM_SVM_CTRL_INTERCEPT_INVLPGB	(1U << 0)
#define VMM_SVM_CTRL_INTERCEPT_INVLPGB_ILL (1U << 1)
#define VMM_SVM_CTRL_INTERCEPT_PCID	(1U << 2)
#define VMM_SVM_CTRL_INTERCEPT_MCOMMIT	(1U << 3)
#define VMM_SVM_CTRL_INTERCEPT_TLBSYNC	(1U << 4)

#define VMM_SVM_CTRL_ENABLE_NP		0x001ULL
#define VMM_SVM_CTRL_TLB_FLUSH_ALL	0x001U
#define VMM_SVM_CTRL_V_INTR_MASKING	(1ULL << 24)

#define VMM_SVM_EXIT_RDTSC		0x06eULL
#define VMM_SVM_EXIT_PAUSE		0x077ULL
#define VMM_SVM_EXIT_INTR		0x060ULL
#define VMM_SVM_EXIT_NMI		0x061ULL
#define VMM_SVM_EXIT_CPUID		0x072ULL
#define VMM_SVM_EXIT_HLT		0x078ULL
#define VMM_SVM_EXIT_IOIO		0x07bULL
#define VMM_SVM_EXIT_MSR		0x07cULL
#define VMM_SVM_EXIT_SHUTDOWN		0x07fULL
#define VMM_SVM_EXIT_VMMCALL		0x081ULL
#define VMM_SVM_EXIT_RDTSCP		0x087ULL
#define VMM_SVM_EXIT_MONITOR		0x08aULL
#define VMM_SVM_EXIT_MWAIT		0x08bULL
#define VMM_SVM_EXIT_MWAIT_COND	0x08cULL
#define VMM_SVM_EXIT_XSETBV		0x08dULL
#define VMM_SVM_EXIT_NPF		0x400ULL

#define VMM_SVM_IOIO_IN		(1ULL << 0)
#define VMM_SVM_IOIO_STR	(1ULL << 2)
#define VMM_SVM_IOIO_REP	(1ULL << 3)
#define VMM_SVM_IOIO_SZ8	(1ULL << 4)
#define VMM_SVM_IOIO_SZ16	(1ULL << 5)
#define VMM_SVM_IOIO_SZ32	(1ULL << 6)
#define VMM_SVM_IOIO_PORT(info)	(((info) >> 16) & 0xffffULL)

#define VMM_COM1_BASE		0x3f8U
#define VMM_COM1_RBR_THR_DLL	0U
#define VMM_COM1_IER_DLM	1U
#define VMM_COM1_IIR_FCR	2U
#define VMM_COM1_LCR		3U
#define VMM_COM1_MCR		4U
#define VMM_COM1_LSR		5U
#define VMM_COM1_MSR		6U
#define VMM_COM1_SCR		7U
#define VMM_COM1_LCR_DLAB	0x80U

#define VMM_SVM_MSRBM_PAGES		2
#define VMM_SVM_IOBM_PAGES		3
#define VMM_SVM_ASID			1
#define VMM_SVM_EFER_VALID		(EFER_SCE | EFER_LME | EFER_LMA | \
					 EFER_NXE | EFER_SVME | EFER_FFXSR | \
					 EFER_TCE)
#define VMM_SVM_MTRR_DEF_VALID		(MTRR_DEF_ENABLE | \
					 MTRR_DEF_FIXED_ENABLE | MTRR_DEF_TYPE)


#define VMM_X64_NDR			6
#define VMM_X64_DR_DR0			0
#define VMM_X64_DR_DR1			1
#define VMM_X64_DR_DR2			2
#define VMM_X64_DR_DR3			3
#define VMM_X64_DR_DR6			4
#define VMM_X64_DR_DR7			5

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

struct vmm_svm_backend {
	struct vmm_machine *borrow_imm_machine;
	struct vmspace *borrow_mut_vmspace;
	struct vmm_svm_vmcb *own_mut_vmcb;
	uint64_t imm_vmcb_pa;
	uint8_t *own_mut_iobm;
	uint64_t imm_iobm_pa;
	uint8_t *own_mut_msrbm;
	uint64_t imm_msrbm_pa;
	void *own_mut_hsave;
	uint64_t imm_hsave_pa;
	uint64_t imm_guest_xcr0;
	union savefpu mut_guest_fpu __aligned(64);
	mcontext_t mut_host_fpu_ctx;
	uint64_t mut_host_drs[VMM_X64_NDR];
	uint64_t mut_guest_drs[VMM_X64_NDR];
	uint64_t mut_host_fsbase;
	uint64_t mut_host_kernelgsbase;
	uint64_t mut_host_star;
	uint64_t mut_host_lstar;
	uint64_t mut_host_cstar;
	uint64_t mut_host_sfmask;
	uint64_t mut_host_sysenter_cs;
	uint64_t mut_host_sysenter_esp;
	uint64_t mut_host_sysenter_eip;
	uint64_t mut_guest_mtrr_def_type;
	uint64_t mut_guest_tsc_aux;
	uint64_t mut_gprs[VMM_X64_NGPR];
	uint8_t mut_com1_ier;
	uint8_t mut_com1_lcr;
	uint8_t mut_com1_mcr;
	uint8_t mut_com1_scr;
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
	svm->imm_guest_xcr0 = v->cr[VMM_X64_CR_XCR0];
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

static void
vmm_svm_fpu_init(struct vmm_svm_backend *svm)
{
	union savefpu *fpu = &svm->mut_guest_fpu;

	bzero(fpu, sizeof(*fpu));
	fpu->sv_xmm64.sv_env.en_cw = __INITIAL_FPUCW__;
	fpu->sv_xmm64.sv_env.en_mxcsr = __INITIAL_MXCSR__;
	fpu->sv_xmm64.sv_env.en_mxcsr_mask = npx_mxcsr_mask;
	fpu->sv_ymm64.sv_xstate.sx_hd.xstate_bv = npx_xcr0_mask;
	fpu->sv_ymm64.sv_xstate.sx_hd.xstate_xcomp_bv = 0;
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
	if (!vmm_loader_x86_xcr0_valid(
	    launch->imm_vcpu0.cr[VMM_X64_CR_XCR0]) ||
	    (launch->imm_vcpu0.cr[VMM_X64_CR_XCR0] & ~npx_xcr0_mask) != 0)
		return EINVAL;
	*backendp = NULL;
	svm = kmalloc(sizeof(*svm), M_TEMP, M_WAITOK | M_ZERO);
	svm->borrow_imm_machine = m;
	svm->borrow_mut_vmspace = vmm_mem_vmspace(&m->own_mut_mem);
	if (svm->borrow_mut_vmspace == NULL) {
		error = EINVAL;
		goto fail;
	}
	pmap_npt_transform(vmspace_pmap(svm->borrow_mut_vmspace), 0);
	svm->mut_guest_mtrr_def_type = MTRR_WRITE_BACK;
	vmm_svm_fpu_init(svm);

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

	vmcb = svm->own_mut_vmcb;
	memset(svm->own_mut_iobm, 0xff, VMM_SVM_IOBM_PAGES * PAGE_SIZE);
	memset(svm->own_mut_msrbm, 0xff, VMM_SVM_MSRBM_PAGES * PAGE_SIZE);
	vmcb->ctrl.intercept_misc1 =
	    VMM_SVM_CTRL_INTERCEPT_INTR |
	    VMM_SVM_CTRL_INTERCEPT_NMI |
	    VMM_SVM_CTRL_INTERCEPT_SMI |
	    VMM_SVM_CTRL_INTERCEPT_INIT |
	    VMM_SVM_CTRL_INTERCEPT_VINTR |
	    VMM_SVM_CTRL_INTERCEPT_RDTSC |
	    VMM_SVM_CTRL_INTERCEPT_RDPMC |
	    VMM_SVM_CTRL_INTERCEPT_PUSHF |
	    VMM_SVM_CTRL_INTERCEPT_POPF |
	    VMM_SVM_CTRL_INTERCEPT_CPUID |
	    VMM_SVM_CTRL_INTERCEPT_RSM |
	    VMM_SVM_CTRL_INTERCEPT_IRET |
	    VMM_SVM_CTRL_INTERCEPT_INTN |
	    VMM_SVM_CTRL_INTERCEPT_INVD |
	    VMM_SVM_CTRL_INTERCEPT_PAUSE |
	    VMM_SVM_CTRL_INTERCEPT_HLT |
	    VMM_SVM_CTRL_INTERCEPT_INVLPG |
	    VMM_SVM_CTRL_INTERCEPT_INVLPGA |
	    VMM_SVM_CTRL_INTERCEPT_IOIO |
	    VMM_SVM_CTRL_INTERCEPT_MSR |
	    VMM_SVM_CTRL_INTERCEPT_TASKSW |
	    VMM_SVM_CTRL_INTERCEPT_FERR |
	    VMM_SVM_CTRL_INTERCEPT_SHUTDOWN;
	vmcb->ctrl.intercept_misc2 =
	    VMM_SVM_CTRL_INTERCEPT_VMRUN |
	    VMM_SVM_CTRL_INTERCEPT_VMMCALL |
	    VMM_SVM_CTRL_INTERCEPT_VMLOAD |
	    VMM_SVM_CTRL_INTERCEPT_VMSAVE |
	    VMM_SVM_CTRL_INTERCEPT_STGI |
	    VMM_SVM_CTRL_INTERCEPT_CLGI |
	    VMM_SVM_CTRL_INTERCEPT_SKINIT |
	    VMM_SVM_CTRL_INTERCEPT_RDTSCP |
	    VMM_SVM_CTRL_INTERCEPT_MONITOR |
	    VMM_SVM_CTRL_INTERCEPT_MWAIT |
	    VMM_SVM_CTRL_INTERCEPT_MWAIT_ARMED |
	    VMM_SVM_CTRL_INTERCEPT_XSETBV |
	    VMM_SVM_CTRL_INTERCEPT_RDPRU |
	    VMM_SVM_CTRL_INTERCEPT_EFER;
	vmcb->ctrl.intercept_misc3 =
	    VMM_SVM_CTRL_INTERCEPT_INVLPGB |
	    VMM_SVM_CTRL_INTERCEPT_INVLPGB_ILL |
	    VMM_SVM_CTRL_INTERCEPT_PCID |
	    VMM_SVM_CTRL_INTERCEPT_MCOMMIT |
	    VMM_SVM_CTRL_INTERCEPT_TLBSYNC;
	vmcb->ctrl.intercept_vec = 0xffffffffU;
	vmcb->ctrl.iopm_base_pa = svm->imm_iobm_pa;
	vmcb->ctrl.msrpm_base_pa = svm->imm_msrbm_pa;
	vmcb->ctrl.guest_asid = VMM_SVM_ASID;
	vmcb->ctrl.tlb_ctrl = VMM_SVM_CTRL_TLB_FLUSH_ALL;
	vmcb->ctrl.v = VMM_SVM_CTRL_V_INTR_MASKING;
	vmcb->ctrl.enable1 = VMM_SVM_CTRL_ENABLE_NP;
	vmcb->ctrl.n_cr3 = vtophys(vmspace_pmap(svm->borrow_mut_vmspace)->pm_pml4);
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

	if (svm == NULL)
		return;
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
vmm_svm_host_tlb_catchup(struct vmm_svm_backend *svm)
{
	if (svm->borrow_mut_vmspace != NULL)
		pmap_add_cpu(svm->borrow_mut_vmspace, mycpu->gd_cpuid);
	clear_xinvltlb();
}

static int
vmm_svm_host_entry_blocked(void)
{
	return hvm_break_wanted();
}

static void
vmm_svm_guest_fpu_enter(struct vmm_svm_backend *svm)
{
	npxpush(&svm->mut_host_fpu_ctx);
	clts();
	fpurstor(&svm->mut_guest_fpu, npx_xcr0_mask);
	if (npx_xcr0_mask != 0)
		load_xcr(0, svm->imm_guest_xcr0);
}

static void
vmm_svm_guest_fpu_leave(struct vmm_svm_backend *svm)
{
	if (npx_xcr0_mask != 0)
		load_xcr(0, npx_xcr0_mask);
	fpusave(&svm->mut_guest_fpu, npx_xcr0_mask);
	load_cr0(rcr0() | CR0_TS);
	npxpop(&svm->mut_host_fpu_ctx);
}

static void
vmm_svm_guest_dbregs_enter(struct vmm_svm_backend *svm)
{
	svm->mut_host_drs[VMM_X64_DR_DR0] = rdr0();
	svm->mut_host_drs[VMM_X64_DR_DR1] = rdr1();
	svm->mut_host_drs[VMM_X64_DR_DR2] = rdr2();
	svm->mut_host_drs[VMM_X64_DR_DR3] = rdr3();
	svm->mut_host_drs[VMM_X64_DR_DR6] = rdr6();
	svm->mut_host_drs[VMM_X64_DR_DR7] = rdr7();

	load_dr7(0);
	load_dr0(svm->mut_guest_drs[VMM_X64_DR_DR0]);
	load_dr1(svm->mut_guest_drs[VMM_X64_DR_DR1]);
	load_dr2(svm->mut_guest_drs[VMM_X64_DR_DR2]);
	load_dr3(svm->mut_guest_drs[VMM_X64_DR_DR3]);
}

static void
vmm_svm_guest_dbregs_leave(struct vmm_svm_backend *svm)
{
	svm->mut_guest_drs[VMM_X64_DR_DR0] = rdr0();
	svm->mut_guest_drs[VMM_X64_DR_DR1] = rdr1();
	svm->mut_guest_drs[VMM_X64_DR_DR2] = rdr2();
	svm->mut_guest_drs[VMM_X64_DR_DR3] = rdr3();

	load_dr7(0);
	load_dr0(svm->mut_host_drs[VMM_X64_DR_DR0]);
	load_dr1(svm->mut_host_drs[VMM_X64_DR_DR1]);
	load_dr2(svm->mut_host_drs[VMM_X64_DR_DR2]);
	load_dr3(svm->mut_host_drs[VMM_X64_DR_DR3]);
	load_dr6(svm->mut_host_drs[VMM_X64_DR_DR6]);
	load_dr7(svm->mut_host_drs[VMM_X64_DR_DR7]);
}

static void
vmm_svm_guest_misc_enter(struct vmm_svm_backend *svm)
{
	svm->mut_host_fsbase = rdmsr(MSR_FSBASE);
	svm->mut_host_kernelgsbase = rdmsr(MSR_KGSBASE);
	svm->mut_host_star = rdmsr(MSR_STAR);
	svm->mut_host_lstar = rdmsr(MSR_LSTAR);
	svm->mut_host_cstar = rdmsr(MSR_CSTAR);
	svm->mut_host_sfmask = rdmsr(MSR_SF_MASK);
	svm->mut_host_sysenter_cs = rdmsr(MSR_SYSENTER_CS);
	svm->mut_host_sysenter_esp = rdmsr(MSR_SYSENTER_ESP);
	svm->mut_host_sysenter_eip = rdmsr(MSR_SYSENTER_EIP);
}

static void
vmm_svm_guest_misc_leave(struct vmm_svm_backend *svm)
{
	wrmsr(MSR_SYSENTER_CS, svm->mut_host_sysenter_cs);
	wrmsr(MSR_SYSENTER_ESP, svm->mut_host_sysenter_esp);
	wrmsr(MSR_SYSENTER_EIP, svm->mut_host_sysenter_eip);
	wrmsr(MSR_STAR, svm->mut_host_star);
	wrmsr(MSR_LSTAR, svm->mut_host_lstar);
	wrmsr(MSR_CSTAR, svm->mut_host_cstar);
	wrmsr(MSR_SF_MASK, svm->mut_host_sfmask);
	wrmsr(MSR_FSBASE, svm->mut_host_fsbase);
	wrmsr(MSR_KGSBASE, svm->mut_host_kernelgsbase);
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

static void
vmm_svm_advance_rip(struct vmm_svm_vmcb *vmcb)
{
	if (vmcb->ctrl.nrip != 0)
		vmcb->state.rip = vmcb->ctrl.nrip;
	else if (vmcb->ctrl.inst_len != 0)
		vmcb->state.rip += vmcb->ctrl.inst_len;
	else
		vmcb->state.rip += 2;
}

static void
vmm_svm_rdmsr_value(struct vmm_svm_backend *svm, uint64_t val)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;

	vmcb->state.rax = val & 0xffffffffULL;
	svm->mut_gprs[VMM_X64_GPR_RDX] = val >> 32;
	vmm_svm_advance_rip(vmcb);
}

static uint64_t
vmm_svm_wrmsr_value(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;

	return (svm->mut_gprs[VMM_X64_GPR_RDX] << 32) |
	    (vmcb->state.rax & 0xffffffffULL);
}

static int
vmm_svm_handle_msr(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	uint32_t msr = (uint32_t)svm->mut_gprs[VMM_X64_GPR_RCX];
	uint64_t val;

	if (vmcb->ctrl.exitinfo1 == 0) {
		switch (msr) {
		case MSR_EFER:
			vmm_svm_rdmsr_value(svm, vmcb->state.efer & ~EFER_SVME);
			return 1;
		case MSR_PAT:
			vmm_svm_rdmsr_value(svm, vmcb->state.g_pat);
			return 1;
		case MSR_TSC:
			vmm_svm_rdmsr_value(svm, rdtsc() + vmcb->ctrl.tsc_offset);
			return 1;
		case MSR_TSC_AUX:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_tsc_aux);
			return 1;
		case MSR_MTRRdefType:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_mtrr_def_type);
			return 1;
		case MSR_STAR:
			vmm_svm_rdmsr_value(svm, vmcb->state.star);
			return 1;
		case MSR_LSTAR:
			vmm_svm_rdmsr_value(svm, vmcb->state.lstar);
			return 1;
		case MSR_CSTAR:
			vmm_svm_rdmsr_value(svm, vmcb->state.cstar);
			return 1;
		case MSR_SF_MASK:
			vmm_svm_rdmsr_value(svm, vmcb->state.sfmask);
			return 1;
		case MSR_FSBASE:
			vmm_svm_rdmsr_value(svm, vmcb->state.fs.base);
			return 1;
		case MSR_GSBASE:
			vmm_svm_rdmsr_value(svm, vmcb->state.gs.base);
			return 1;
		case MSR_KGSBASE:
			vmm_svm_rdmsr_value(svm, vmcb->state.kernelgsbase);
			return 1;
		case MSR_SYSENTER_CS:
			vmm_svm_rdmsr_value(svm, vmcb->state.sysenter_cs);
			return 1;
		case MSR_SYSENTER_ESP:
			vmm_svm_rdmsr_value(svm, vmcb->state.sysenter_esp);
			return 1;
		case MSR_SYSENTER_EIP:
			vmm_svm_rdmsr_value(svm, vmcb->state.sysenter_eip);
			return 1;
		default:
			return 0;
		}
	}

	val = vmm_svm_wrmsr_value(svm);
	switch (msr) {
	case MSR_EFER:
		if ((val & ~VMM_SVM_EFER_VALID) != 0)
			return 0;
		vmcb->state.efer = (val & ~EFER_SVME) | EFER_SVME;
		vmcb->ctrl.tlb_ctrl = VMM_SVM_CTRL_TLB_FLUSH_ALL;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_PAT:
		vmcb->state.g_pat = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_TSC:
		vmcb->ctrl.tsc_offset = val - rdtsc();
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_TSC_AUX:
		svm->mut_guest_tsc_aux = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_MTRRdefType:
		if ((val & ~VMM_SVM_MTRR_DEF_VALID) != 0)
			return 0;
		svm->mut_guest_mtrr_def_type = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_STAR:
		vmcb->state.star = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_LSTAR:
		vmcb->state.lstar = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_CSTAR:
		vmcb->state.cstar = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_SF_MASK:
		vmcb->state.sfmask = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_FSBASE:
		vmcb->state.fs.base = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_GSBASE:
		vmcb->state.gs.base = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_KGSBASE:
		vmcb->state.kernelgsbase = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_SYSENTER_CS:
		vmcb->state.sysenter_cs = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_SYSENTER_ESP:
		vmcb->state.sysenter_esp = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_SYSENTER_EIP:
		vmcb->state.sysenter_eip = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	default:
		return 0;
	}
}

static void
vmm_svm_yield_after_host_interrupt(void)
{
	/*
	 * A host interrupt VMEXIT is the cooperative scheduling point for a
	 * CPU-bound guest.  Host CPU state has already been restored and STGI
	 * has opened GIF, so let DragonFly process pending root work and
	 * decide whether this vCPU should keep the CPU.
	 */
	lwkt_user_yield();
}

static void
vmm_svm_set_rax_rdx(struct vmm_svm_backend *svm, uint64_t val)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;

	vmcb->state.rax = val & 0xffffffffULL;
	svm->mut_gprs[VMM_X64_GPR_RDX] = val >> 32;
}

static void
vmm_svm_advance_ioio(struct vmm_svm_vmcb *vmcb)
{
	if (vmcb->ctrl.exitinfo2 != 0)
		vmcb->state.rip = vmcb->ctrl.exitinfo2;
	else
		vmm_svm_advance_rip(vmcb);
}

static int
vmm_svm_ioio_size(uint64_t info)
{
	if (info & VMM_SVM_IOIO_SZ8)
		return 1;
	if (info & VMM_SVM_IOIO_SZ16)
		return 2;
	if (info & VMM_SVM_IOIO_SZ32)
		return 4;
	return 0;
}

static void
vmm_svm_set_rax_low(struct vmm_svm_vmcb *vmcb, uint32_t val, int size)
{
	uint64_t mask;

	switch (size) {
	case 1:
		mask = 0xffULL;
		break;
	case 2:
		mask = 0xffffULL;
		break;
	case 4:
		mask = 0xffffffffULL;
		break;
	default:
		return;
	}
	vmcb->state.rax = (vmcb->state.rax & ~mask) | (val & mask);
}

static int
vmm_svm_com1_read(struct vmm_svm_backend *svm, unsigned int reg, int size,
    uint32_t *valp)
{
	if (size != 1)
		return 0;
	switch (reg) {
	case VMM_COM1_RBR_THR_DLL:
		*valp = 0;
		return 1;
	case VMM_COM1_IER_DLM:
		*valp = (svm->mut_com1_lcr & VMM_COM1_LCR_DLAB) ?
		    0 : svm->mut_com1_ier;
		return 1;
	case VMM_COM1_IIR_FCR:
		*valp = 0x01;		/* no interrupt pending */
		return 1;
	case VMM_COM1_LCR:
		*valp = svm->mut_com1_lcr;
		return 1;
	case VMM_COM1_MCR:
		*valp = svm->mut_com1_mcr;
		return 1;
	case VMM_COM1_LSR:
		*valp = 0x60;		/* THR empty, transmitter empty */
		return 1;
	case VMM_COM1_MSR:
		*valp = 0;
		return 1;
	case VMM_COM1_SCR:
		*valp = svm->mut_com1_scr;
		return 1;
	default:
		return 0;
	}
}

static int
vmm_svm_com1_write(struct vmm_svm_backend *svm, unsigned int reg, int size,
    uint32_t val)
{
	char ch;

	if (size != 1)
		return 0;
	switch (reg) {
	case VMM_COM1_RBR_THR_DLL:
		if ((svm->mut_com1_lcr & VMM_COM1_LCR_DLAB) == 0) {
			ch = (char)(val & 0xffU);
			vmm_console_guest_write(
			    &svm->borrow_imm_machine->own_mut_console, &ch, 1);
		}
		return 1;
	case VMM_COM1_IER_DLM:
		if ((svm->mut_com1_lcr & VMM_COM1_LCR_DLAB) == 0)
			svm->mut_com1_ier = val & 0x0fU;
		return 1;
	case VMM_COM1_IIR_FCR:
		return 1;
	case VMM_COM1_LCR:
		svm->mut_com1_lcr = val & 0xffU;
		return 1;
	case VMM_COM1_MCR:
		svm->mut_com1_mcr = val & 0xffU;
		return 1;
	case VMM_COM1_SCR:
		svm->mut_com1_scr = val & 0xffU;
		return 1;
	default:
		return 0;
	}
}

static int
vmm_svm_handle_ioio(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	uint64_t info = vmcb->ctrl.exitinfo1;
	unsigned int port = (unsigned int)VMM_SVM_IOIO_PORT(info);
	int size = vmm_svm_ioio_size(info);
	uint32_t val;

	if (size == 0 || (info & (VMM_SVM_IOIO_STR | VMM_SVM_IOIO_REP)) != 0)
		return 0;
	if (port < VMM_COM1_BASE || port > VMM_COM1_BASE + VMM_COM1_SCR)
		return 0;
	if (info & VMM_SVM_IOIO_IN) {
		if (!vmm_svm_com1_read(svm, port - VMM_COM1_BASE, size, &val))
			return 0;
		vmm_svm_set_rax_low(vmcb, val, size);
	} else {
		val = vmcb->state.rax & 0xffffffffU;
		if (!vmm_svm_com1_write(svm, port - VMM_COM1_BASE, size, val))
			return 0;
	}
	vmm_svm_advance_ioio(vmcb);
	return 1;
}

static void
vmm_svm_handle_rdtsc(struct vmm_svm_backend *svm, int with_aux)
{
	uint64_t tsc = rdtsc() + svm->own_mut_vmcb->ctrl.tsc_offset;

	vmm_svm_set_rax_rdx(svm, tsc);
	if (with_aux)
		svm->mut_gprs[VMM_X64_GPR_RCX] = svm->mut_guest_tsc_aux;
	vmm_svm_advance_rip(svm->own_mut_vmcb);
}

static void
vmm_svm_handle_idle_wait(struct vmm_machine *m, struct vmm_vcpu_thread *vc,
    struct vmm_svm_vmcb *vmcb)
{
	vmm_svm_advance_rip(vmcb);
	if (!vmm_machine_vcpu_should_stop(m))
		tsleep(vc, 0, "vmmhlt", hz / 20 + 1);
}

static int
vmm_svm_handle_npf(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	struct vmm_machine *m = svm->borrow_imm_machine;
	uint64_t gpa = vmcb->ctrl.exitinfo2;
	int prot;
	int error;

	if (vmcb->ctrl.exitinfo1 & PGEX_W)
		prot = VM_PROT_WRITE;
	else if (vmcb->ctrl.exitinfo1 & PGEX_I)
		prot = VM_PROT_EXECUTE;
	else
		prot = VM_PROT_READ;
	error = vmm_mem_fault_gpa(&m->own_mut_mem, gpa, prot);
	if (error)
		return 0;
	vmcb->ctrl.tlb_ctrl = VMM_SVM_CTRL_TLB_FLUSH_ALL;
	return 1;
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
		vmm_svm_enable_cpu(svm);
		vmm_svm_clgi();
		vmm_svm_host_tlb_catchup(svm);
		if (__predict_false(vmm_svm_host_entry_blocked())) {
			vmm_svm_stgi();
			lwkt_user_yield();
			continue;
		}
		vmcb->ctrl.tlb_ctrl = VMM_SVM_CTRL_TLB_FLUSH_ALL;
		vmm_svm_guest_dbregs_enter(svm);
		vmm_svm_guest_misc_enter(svm);
		vmm_svm_guest_fpu_enter(svm);
		vmm_svm_vmrun(svm->imm_vmcb_pa, svm->mut_gprs);
		vmm_svm_guest_fpu_leave(svm);
		vmm_svm_guest_misc_leave(svm);
		vmm_svm_guest_dbregs_leave(svm);
		vmm_svm_stgi();
		switch (vmcb->ctrl.exitcode) {
		case VMM_SVM_EXIT_INTR:
		case VMM_SVM_EXIT_NMI:
			vmm_svm_yield_after_host_interrupt();
			break;
		case VMM_SVM_EXIT_RDTSC:
			vmm_svm_handle_rdtsc(svm, 0);
			break;
		case VMM_SVM_EXIT_CPUID:
			vmm_svm_handle_cpuid(svm);
			break;
		case VMM_SVM_EXIT_PAUSE:
			vmm_svm_advance_rip(vmcb);
			break;
		case VMM_SVM_EXIT_HLT:
			vmm_svm_handle_idle_wait(m, vc, vmcb);
			break;
		case VMM_SVM_EXIT_IOIO:
			if (vmm_svm_handle_ioio(svm))
				break;
			goto unhandled;
		case VMM_SVM_EXIT_SHUTDOWN:
			goto unhandled;
		case VMM_SVM_EXIT_RDTSCP:
			vmm_svm_handle_rdtsc(svm, 1);
			break;
		case VMM_SVM_EXIT_MONITOR:
			vmm_svm_advance_rip(vmcb);
			break;
		case VMM_SVM_EXIT_MWAIT:
		case VMM_SVM_EXIT_MWAIT_COND:
			vmm_svm_handle_idle_wait(m, vc, vmcb);
			break;
		case VMM_SVM_EXIT_NPF:
			if (vmm_svm_handle_npf(svm))
				break;
			goto unhandled;
		case VMM_SVM_EXIT_MSR:
			if (vmm_svm_handle_msr(svm))
				break;
			goto unhandled;
		case VMM_SVM_EXIT_VMMCALL:
		case VMM_SVM_EXIT_XSETBV:
		default:
	unhandled:
			kprintf("vmm_svm: vmexit 0x%jx info1=0x%jx info2=0x%jx rip=0x%jx rcx=0x%jx rax=0x%jx rdx=0x%jx\n",
			    (uintmax_t)vmcb->ctrl.exitcode,
			    (uintmax_t)vmcb->ctrl.exitinfo1,
			    (uintmax_t)vmcb->ctrl.exitinfo2,
			    (uintmax_t)vmcb->state.rip,
			    (uintmax_t)svm->mut_gprs[VMM_X64_GPR_RCX],
			    (uintmax_t)vmcb->state.rax,
			    (uintmax_t)svm->mut_gprs[VMM_X64_GPR_RDX]);
			goto out;
		}
		lwkt_user_yield();
	}
out:
	return;
}
