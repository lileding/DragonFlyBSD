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
#include <machine/psl.h>
#include <machine/smp.h>
#include <machine/specialreg.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>
#include <vm/vm_page2.h>

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
#define VMM_SVM_CTRL_INTERCEPT_WBINVD	(1U << 9)
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
#define VMM_SVM_CTRL_V_IRQ		(1ULL << 8)
#define VMM_SVM_CTRL_V_IGN_TPR		(1ULL << 20)
#define VMM_SVM_CTRL_V_INTR_MASKING	(1ULL << 24)
#define VMM_SVM_CTRL_V_AVIC_EN		(1ULL << 31)
#define VMM_SVM_EVENTINJ_VALID		(1ULL << 31)
#define VMM_SVM_AVIC_PHYS_VALID		(1ULL << 63)
#define VMM_SVM_AVIC_PHYS_RUNNING	(1ULL << 62)
#define VMM_SVM_AVIC_PHYS_HOST_ID_MASK	0xfffULL
#define VMM_SVM_AVIC_PHYS_MAX_INDEX_MASK 0xffULL
#define VMM_SVM_AVIC_MAX_PHYS_ID	0U
#define VMM_SVM_AVIC_APIC_ID		0U
#define VMM_SVM_APIC_REG_ID		0x020U
#define VMM_SVM_APIC_REG_VERSION	0x030U
#define VMM_SVM_APIC_REG_TPR		0x080U
#define VMM_SVM_APIC_REG_SVR		0x0f0U
#define VMM_SVM_APIC_REG_IRR_BASE	0x200U
#define VMM_SVM_APIC_VERSION		0x00140014U
#define VMM_SVM_APIC_SVR_ENABLE		0x100U
#define MSR_AMD64_SVM_AVIC_DOORBELL	0xc001011bU

#define VMM_SVM_EXIT_INTR		0x060ULL
#define VMM_SVM_EXIT_NMI		0x061ULL
#define VMM_SVM_EXIT_SMI		0x062ULL
#define VMM_SVM_EXIT_INIT		0x063ULL
#define VMM_SVM_EXIT_VINTR		0x064ULL
#define VMM_SVM_EXIT_RDTSC		0x06eULL
#define VMM_SVM_EXIT_CPUID		0x072ULL
#define VMM_SVM_EXIT_INVD		0x076ULL
#define VMM_SVM_EXIT_PAUSE		0x077ULL
#define VMM_SVM_EXIT_HLT		0x078ULL
#define VMM_SVM_EXIT_INVLPG		0x079ULL
#define VMM_SVM_EXIT_INVLPGA		0x07aULL
#define VMM_SVM_EXIT_IOIO		0x07bULL
#define VMM_SVM_EXIT_MSR		0x07cULL
#define VMM_SVM_EXIT_SHUTDOWN		0x07fULL
#define VMM_SVM_EXIT_VMMCALL		0x081ULL
#define VMM_SVM_EXIT_RDTSCP		0x087ULL
#define VMM_SVM_EXIT_WBINVD		0x089ULL
#define VMM_SVM_EXIT_MONITOR		0x08aULL
#define VMM_SVM_EXIT_MWAIT		0x08bULL
#define VMM_SVM_EXIT_MWAIT_COND	0x08cULL
#define VMM_SVM_EXIT_XSETBV		0x08dULL
#define VMM_SVM_EXIT_INVLPGB		0x0a0ULL
#define VMM_SVM_EXIT_NPF		0x400ULL
#define VMM_SVM_EXIT_AVIC_INCOMPLETE_IPI 0x401ULL
#define VMM_SVM_EXIT_AVIC_NOACCEL	0x402ULL

#define VMM_SVM_AVIC_UNACCEL_ACCESS_WRITE_MASK	0x1U
#define VMM_SVM_AVIC_UNACCEL_ACCESS_OFFSET_MASK	0xff0U
#define VMM_SVM_AVIC_UNACCEL_ACCESS_VECTOR_MASK	0xffffffffU

#define VMM_SVM_AVIC_IPI_INVALID_INT_TYPE	0U
#define VMM_SVM_AVIC_IPI_TARGET_NOT_RUNNING	1U
#define VMM_SVM_AVIC_IPI_INVALID_TARGET		2U
#define VMM_SVM_AVIC_IPI_INVALID_BACKING_PAGE	3U
#define VMM_SVM_AVIC_IPI_INVALID_IPI_VECTOR	4U

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
#define VMM_COM1_IIR_NOPEND	0x01U
#define VMM_COM1_LCR_DLAB	0x80U
#define VMM_COM1_LSR_DR		0x01U
#define VMM_COM1_LSR_THRE	0x20U
#define VMM_COM1_LSR_TEMT	0x40U
#define VMM_PIC1_CMD		0x20U
#define VMM_PIC1_DATA		0x21U
#define VMM_PIC2_CMD		0xa0U
#define VMM_PIC2_DATA		0xa1U
#define VMM_CPUID_APIC_ID_MASK	0xff000000U
#define VMM_CPUID1_ECX_X2APIC	(1U << 21)
#define VMM_CPUID1_ECX_TSC_DEADLINE (1U << 24)

#define VMM_IOAPIC_BASE		0xfec00000ULL
#define VMM_IOAPIC_SIZE		PAGE_SIZE

#define VMM_SVM_MSRBM_PAGES		2
#define VMM_SVM_IOBM_PAGES		3
#define VMM_SVM_ASID			1
#define VMM_SVM_EFER_VALID		(EFER_SCE | EFER_LME | EFER_LMA | \
					 EFER_NXE | EFER_SVME | EFER_FFXSR | \
					 EFER_TCE)
#define VMM_SVM_MTRR_DEF_VALID		(MTRR_DEF_ENABLE | \
					 MTRR_DEF_FIXED_ENABLE | MTRR_DEF_TYPE)
#define VMM_SVM_APICBASE_ADDR		VMM_X86_LAPIC_MMIO_GPA
#define VMM_SVM_APICBASE_VALID		(APICBASE_BSP | \
					 APICBASE_ENABLED | APICBASE_ADDRESS)
#define VMM_SVM_X2APIC_MSR_BASE	0x800U
#define VMM_SVM_X2APIC_MSR_LAST	0x83fU
#define VMM_SVM_MSR_K7_HWCR		0xc0010015U
#define VMM_SVM_HWCR_IGNORE		(0x8ULL | 0x40ULL | 0x100ULL)
#define VMM_SVM_HWCR_MC_STATUS_WR_EN	(1ULL << 18)
#define VMM_SVM_HWCR_TSC_FREQ_SEL	(1ULL << 24)
#define VMM_SVM_HWCR_VALID		(VMM_SVM_HWCR_MC_STATUS_WR_EN | \
					 VMM_SVM_HWCR_TSC_FREQ_SEL)
#define VMM_SVM_AMD_PATCH_LEVEL	0ULL
#define VMM_SVM_SMOKE_AVIC_MAGIC	0x43495641U
#define VMM_SVM_SMOKE_AVIC_DELIVER	1U
#define VMM_SVM_SMOKE_AVIC_MARKER	2U


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
	void *own_mut_avic_apic_page;
	uint64_t imm_avic_apic_page_pa;
	vm_page_t own_mut_avic_access_page;
	uint64_t imm_avic_access_page_pa;
	uint64_t *own_mut_avic_phys_table;
	uint64_t imm_avic_phys_table_pa;
	uint32_t *own_mut_avic_log_table;
	uint64_t imm_avic_log_table_pa;
	uint32_t imm_avic_apic_id;
	uint32_t mut_avic_host_apic_id;
	uint32_t mut_avic_host_cpuid;
	int mut_avic_bound;
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
	uint64_t mut_guest_syscfg;
	uint64_t mut_guest_hwcr;
	uint64_t mut_guest_tsc_aux;
	uint64_t mut_guest_apicbase;
	uint64_t mut_gprs[VMM_X64_NGPR];
	uint8_t mut_com1_ier;
	uint8_t mut_com1_lcr;
	uint8_t mut_com1_mcr;
	uint8_t mut_com1_scr;
};

struct vmm_svm_msr_policy {
	uint32_t imm_msr;
	const char *imm_name;
	const char *imm_category;
};

CTASSERT(sizeof(struct vmm_svm_ctrl) == 1024);
CTASSERT(__offsetof(struct vmm_svm_ctrl, v) == 0x060);
CTASSERT(__offsetof(struct vmm_svm_ctrl, intr) == 0x068);
CTASSERT(__offsetof(struct vmm_svm_ctrl, exitcode) == 0x070);
CTASSERT(__offsetof(struct vmm_svm_ctrl, exitinfo1) == 0x078);
CTASSERT(__offsetof(struct vmm_svm_ctrl, exitinfo2) == 0x080);
CTASSERT(__offsetof(struct vmm_svm_ctrl, enable1) == 0x090);
CTASSERT(__offsetof(struct vmm_svm_ctrl, avic) == 0x098);
CTASSERT(__offsetof(struct vmm_svm_ctrl, eventinj) == 0x0a8);
CTASSERT(__offsetof(struct vmm_svm_ctrl, n_cr3) == 0x0b0);
CTASSERT(__offsetof(struct vmm_svm_ctrl, nrip) == 0x0c8);
CTASSERT(__offsetof(struct vmm_svm_ctrl, inst_len) == 0x0d0);
CTASSERT(__offsetof(struct vmm_svm_ctrl, avic_abpp) == 0x0e0);
CTASSERT(__offsetof(struct vmm_svm_ctrl, avic_ltp) == 0x0f0);
CTASSERT(__offsetof(struct vmm_svm_ctrl, avic_phys) == 0x0f8);
CTASSERT(sizeof(struct vmm_svm_state) == 0xc00);
CTASSERT(sizeof(struct vmm_svm_vmcb) == PAGE_SIZE);
CTASSERT(__offsetof(struct vmm_svm_vmcb, state) == 0x400);

void	vmm_svm_vmrun(uint64_t vmcb_pa, uint64_t *gprs);
static void vmm_svm_vcpu_destroy(void *backend);
static int vmm_svm_avic_init(struct vmm_svm_backend *svm);
static void vmm_svm_avic_uninit(struct vmm_svm_backend *svm);

static const struct vmm_svm_msr_policy vmm_svm_msr_policies[] = {
	{ MSR_EFER, "efer", "cpu-state" },
	{ MSR_PAT, "pat", "memory-type" },
	{ MSR_TSC, "tsc", "time" },
	{ MSR_TSC_AUX, "tsc_aux", "time" },
	{ MSR_TSC_DEADLINE, "tsc_deadline", "time" },
	{ MSR_APICBASE, "apicbase", "apic" },
	{ MSR_MTRRdefType, "mtrr_def_type", "memory-type" },
	{ MSR_SYSCFG, "amd_syscfg", "platform-config" },
	{ VMM_SVM_MSR_K7_HWCR, "amd_hwcr", "platform-config" },
	{ MSR_AMD_PATCH_LEVEL, "amd_patch_level", "microcode" },
	{ MSR_AMD_PATCH_LOADER, "amd_patch_loader", "microcode" },
	{ MSR_STAR, "star", "syscall" },
	{ MSR_LSTAR, "lstar", "syscall" },
	{ MSR_CSTAR, "cstar", "syscall" },
	{ MSR_SF_MASK, "sfmask", "syscall" },
	{ MSR_FSBASE, "fsbase", "segment-base" },
	{ MSR_GSBASE, "gsbase", "segment-base" },
	{ MSR_KGSBASE, "kernelgsbase", "segment-base" },
	{ MSR_SYSENTER_CS, "sysenter_cs", "sysenter" },
	{ MSR_SYSENTER_ESP, "sysenter_esp", "sysenter" },
	{ MSR_SYSENTER_EIP, "sysenter_eip", "sysenter" },
};

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

static void
vmm_svm_avic_apic_write32(struct vmm_svm_backend *svm, uint32_t reg,
    uint32_t val)
{
	volatile uint32_t *ptr;

	ptr = (volatile uint32_t *)((uint8_t *)svm->own_mut_avic_apic_page + reg);
	*ptr = val;
}

static vm_page_t
vmm_svm_avic_access_page_alloc(uint64_t *pap)
{
	vm_page_t m;

	m = vm_page_alloc(NULL, (vm_pindex_t)ticks,
	    VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_FORCE_ZERO);
	if (m == NULL)
		return NULL;
	m->valid = VM_PAGE_BITS_ALL;
	vm_page_wire(m);
	vm_page_wakeup(m);
	*pap = VM_PAGE_TO_PHYS(m);
	return m;
}

static void
vmm_svm_avic_access_page_free(vm_page_t m)
{
	if (m == NULL)
		return;
	vm_page_busy_wait(m, FALSE, "vmmavp");
	vm_page_unwire(m, 0);
	vm_page_free(m);
}

static int
vmm_svm_avic_map_access_page(struct vmm_svm_backend *svm)
{
	pmap_t pmap;

	if (svm->borrow_mut_vmspace == NULL ||
	    svm->own_mut_avic_access_page == NULL)
		return EINVAL;
	pmap = vmspace_pmap(svm->borrow_mut_vmspace);
	pmap_enter(pmap, VMM_SVM_APICBASE_ADDR, svm->own_mut_avic_access_page,
	    VM_PROT_READ | VM_PROT_WRITE, 0, NULL);
	return 0;
}

static void
vmm_svm_avic_unmap_access_page(struct vmm_svm_backend *svm)
{
	if (svm == NULL || svm->borrow_mut_vmspace == NULL)
		return;
	pmap_remove(vmspace_pmap(svm->borrow_mut_vmspace),
	    VMM_SVM_APICBASE_ADDR, VMM_SVM_APICBASE_ADDR + PAGE_SIZE);
}

static int
vmm_svm_avic_init(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	uint64_t entry;
	int error;

	svm->imm_avic_apic_id = VMM_SVM_AVIC_APIC_ID;
	svm->mut_avic_host_cpuid = (uint32_t)-1;
	svm->mut_avic_host_apic_id = (uint32_t)-1;
	svm->own_mut_avic_apic_page =
	    vmm_svm_contig_alloc(&svm->imm_avic_apic_page_pa, 1);
	svm->own_mut_avic_phys_table =
	    vmm_svm_contig_alloc(&svm->imm_avic_phys_table_pa, 1);
	svm->own_mut_avic_log_table =
	    vmm_svm_contig_alloc(&svm->imm_avic_log_table_pa, 1);
	svm->own_mut_avic_access_page =
	    vmm_svm_avic_access_page_alloc(&svm->imm_avic_access_page_pa);
	if (svm->own_mut_avic_apic_page == NULL ||
	    svm->own_mut_avic_phys_table == NULL ||
	    svm->own_mut_avic_log_table == NULL ||
	    svm->own_mut_avic_access_page == NULL)
		return ENOMEM;

	vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_ID,
	    svm->imm_avic_apic_id << 24);
	vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_VERSION,
	    VMM_SVM_APIC_VERSION);
	vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TPR, 0);
	vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_SVR,
	    VMM_SVM_APIC_SVR_ENABLE | 0xff);

	entry = svm->imm_avic_apic_page_pa | VMM_SVM_AVIC_PHYS_VALID;
	svm->own_mut_avic_phys_table[svm->imm_avic_apic_id] = entry;

	error = vmm_svm_avic_map_access_page(svm);
	if (error)
		return error;

	vmcb->ctrl.v |= VMM_SVM_CTRL_V_INTR_MASKING | VMM_SVM_CTRL_V_AVIC_EN;
	vmcb->ctrl.avic = VMM_SVM_APICBASE_ADDR;
	vmcb->ctrl.avic_abpp = svm->imm_avic_apic_page_pa;
	vmcb->ctrl.avic_ltp = svm->imm_avic_log_table_pa;
	vmcb->ctrl.avic_phys = svm->imm_avic_phys_table_pa |
	    VMM_SVM_AVIC_MAX_PHYS_ID;
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm avic enabled apic_id=%u apic_pa=0x%jx access_pa=0x%jx",
	    svm->imm_avic_apic_id, (uintmax_t)svm->imm_avic_apic_page_pa,
	    (uintmax_t)svm->imm_avic_access_page_pa);
	return 0;
}

static void
vmm_svm_avic_uninit(struct vmm_svm_backend *svm)
{
	if (svm == NULL)
		return;
	vmm_svm_avic_unmap_access_page(svm);
	vmm_svm_avic_access_page_free(svm->own_mut_avic_access_page);
	svm->own_mut_avic_access_page = NULL;
	vmm_svm_contig_free(svm->own_mut_avic_log_table, 1);
	vmm_svm_contig_free(svm->own_mut_avic_phys_table, 1);
	vmm_svm_contig_free(svm->own_mut_avic_apic_page, 1);
	svm->own_mut_avic_log_table = NULL;
	svm->own_mut_avic_phys_table = NULL;
	svm->own_mut_avic_apic_page = NULL;
}


static void
vmm_svm_avic_bind_cpu(struct vmm_svm_backend *svm)
{
	uint64_t entry;
	uint32_t cpuid;
	uint32_t apicid;

	if (svm == NULL || svm->own_mut_avic_phys_table == NULL)
		return;
	cpuid = mycpu->gd_cpuid;
	apicid = (uint32_t)CPUID_TO_APICID(cpuid);
	if (svm->mut_avic_bound && svm->mut_avic_host_cpuid == cpuid)
		return;
	if (apicid & ~VMM_SVM_AVIC_PHYS_HOST_ID_MASK) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm avic host apic id too large cpuid=%u apicid=%u",
		    cpuid, apicid);
		return;
	}
	entry = svm->imm_avic_apic_page_pa | VMM_SVM_AVIC_PHYS_VALID |
	    VMM_SVM_AVIC_PHYS_RUNNING | apicid;
	svm->own_mut_avic_phys_table[svm->imm_avic_apic_id] = entry;
	svm->mut_avic_host_cpuid = cpuid;
	svm->mut_avic_host_apic_id = apicid;
	svm->mut_avic_bound = 1;
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm avic bound apic_id=%u host_cpuid=%u host_apic_id=%u",
	    svm->imm_avic_apic_id, cpuid, apicid);
}

static void
vmm_svm_avic_deliver(struct vmm_svm_backend *svm, struct vmm_vcpu_thread *vc,
    uint8_t vector, const char *source)
{
	volatile uint32_t *irr;
	uint32_t bit;

	if (vector < 32) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u avic reject source=%s vector=0x%x reason=low_vector",
		    vc->imm_id, source, vector);
		return;
	}
	irr = (volatile uint32_t *)((uint8_t *)svm->own_mut_avic_apic_page +
	    VMM_SVM_APIC_REG_IRR_BASE + (vector / 32) * 0x10);
	bit = 1U << (vector & 31);
	atomic_set_int((volatile u_int *)irr, bit);
	cpu_mfence();
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm vcpu%u avic deliver source=%s vector=0x%x irr=0x%x",
	    vc->imm_id, source, vector, *irr);
	if (svm->mut_avic_bound && mycpu->gd_cpuid != svm->mut_avic_host_cpuid) {
		wrmsr(MSR_AMD64_SVM_AVIC_DOORBELL, svm->mut_avic_host_apic_id);
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u avic doorbell host_apic_id=%u",
		    vc->imm_id, svm->mut_avic_host_apic_id);
	}
}

static int
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
	    (descs[3] & CPUID_AMD_SVM_NRIPS) == 0 ||
	    (descs[3] & CPUID_AMD_SVM_DecodeAssist) == 0 ||
	    (descs[3] & CPUID_AMD_SVM_AVIC) == 0)
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

static int
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
	svm->borrow_mut_vmspace = vmm_mem_borrow_vmspace(&m->own_mut_mem);
	if (svm->borrow_mut_vmspace == NULL) {
		error = EINVAL;
		goto fail;
	}
	pmap_npt_transform(vmspace_pmap(svm->borrow_mut_vmspace), 0);
	svm->mut_guest_mtrr_def_type = MTRR_WRITE_BACK;
	svm->mut_guest_apicbase = VMM_SVM_APICBASE_ADDR |
	    APICBASE_BSP | APICBASE_ENABLED;
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
	    VMM_SVM_CTRL_INTERCEPT_RDTSC |
	    VMM_SVM_CTRL_INTERCEPT_RDPMC |
	    VMM_SVM_CTRL_INTERCEPT_CPUID |
	    VMM_SVM_CTRL_INTERCEPT_RSM |
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
	    VMM_SVM_CTRL_INTERCEPT_WBINVD |
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
	/*
	 * CPU exceptions belong to the guest IDT.  Root scheduling and forced
	 * stop still use the separate external INTR/NMI intercepts above.
	 */
	vmcb->ctrl.intercept_vec = 0;
	vmcb->ctrl.iopm_base_pa = svm->imm_iobm_pa;
	vmcb->ctrl.msrpm_base_pa = svm->imm_msrbm_pa;
	vmcb->ctrl.guest_asid = VMM_SVM_ASID;
	vmcb->ctrl.tlb_ctrl = VMM_SVM_CTRL_TLB_FLUSH_ALL;
	vmcb->ctrl.v = VMM_SVM_CTRL_V_INTR_MASKING;
	vmcb->ctrl.enable1 = VMM_SVM_CTRL_ENABLE_NP;
	vmcb->ctrl.n_cr3 = vtophys(vmspace_pmap(svm->borrow_mut_vmspace)->pm_pml4);
	error = vmm_svm_avic_init(svm);
	if (error)
		goto fail;
	vmm_svm_load_state(svm, &launch->imm_vcpu0);

	*backendp = svm;
	return 0;

fail:
	vmm_svm_vcpu_destroy(svm);
	return error;
}

static void
vmm_svm_vcpu_destroy(void *backend)
{
	struct vmm_svm_backend *svm = backend;

	if (svm == NULL)
		return;
	vmm_svm_avic_uninit(svm);
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
	return (mycpu->gd_reqflags & RQF_HVM_MASK) != 0;
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
	uint32_t leaf;

	leaf = (uint32_t)vmcb->state.rax;
	cpuid_count(leaf,
	    (uint32_t)svm->mut_gprs[VMM_X64_GPR_RCX], regs);
	if (leaf == 1) {
		regs[1] &= ~VMM_CPUID_APIC_ID_MASK;
		regs[2] &= ~(VMM_CPUID1_ECX_X2APIC |
		    VMM_CPUID1_ECX_TSC_DEADLINE);
	}
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
vmm_svm_guest_tsc(struct vmm_svm_backend *svm)
{
	return rdtsc() + svm->own_mut_vmcb->ctrl.tsc_offset;
}

static uint64_t
vmm_svm_wrmsr_value(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;

	return (svm->mut_gprs[VMM_X64_GPR_RDX] << 32) |
	    (vmcb->state.rax & 0xffffffffULL);
}

static const struct vmm_svm_msr_policy *
vmm_svm_msr_lookup(uint32_t msr)
{
	size_t i;

	for (i = 0; i < sizeof(vmm_svm_msr_policies) /
	    sizeof(vmm_svm_msr_policies[0]); i++) {
		if (vmm_svm_msr_policies[i].imm_msr == msr)
			return &vmm_svm_msr_policies[i];
	}
	return NULL;
}

static int
vmm_svm_x2apic_msr(uint32_t msr)
{
	return msr >= VMM_SVM_X2APIC_MSR_BASE &&
	    msr <= VMM_SVM_X2APIC_MSR_LAST;
}

static void
vmm_svm_log_unsupported_msr(struct vmm_svm_backend *svm,
    const struct vmm_vcpu_thread *vc, const char *op, uint32_t msr,
    uint64_t val, int has_val, const char *reason)
{
	const struct vmm_svm_msr_policy *policy;
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	const char *category;
	const char *name;

	policy = vmm_svm_msr_lookup(msr);
	if (policy != NULL) {
		name = policy->imm_name;
		category = policy->imm_category;
	} else if (vmm_svm_x2apic_msr(msr)) {
		name = "x2apic";
		category = "apic";
	} else {
		name = "unknown";
		category = "unknown";
	}

	if (has_val) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u unsupported msr op=%s msr=0x%jx name=%s category=%s val=0x%jx reason=%s rip=0x%jx",
		    vc->imm_id, op, (uintmax_t)msr, name, category,
		    (uintmax_t)val, reason, (uintmax_t)vmcb->state.rip);
	} else {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u unsupported msr op=%s msr=0x%jx name=%s category=%s reason=%s rip=0x%jx",
		    vc->imm_id, op, (uintmax_t)msr, name, category,
		    reason, (uintmax_t)vmcb->state.rip);
	}
}

static void
vmm_svm_requeue_exit_event(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;

	if ((vmcb->ctrl.exitintinfo & VMM_SVM_EVENTINJ_VALID) != 0)
		vmcb->ctrl.eventinj = vmcb->ctrl.exitintinfo;
}

static int
vmm_svm_handle_msr(struct vmm_svm_backend *svm, struct vmm_vcpu_thread *vc)
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
			vmm_svm_rdmsr_value(svm, vmm_svm_guest_tsc(svm));
			return 1;
		case MSR_TSC_AUX:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_tsc_aux);
			return 1;
		case MSR_TSC_DEADLINE:
			vmm_svm_log_unsupported_msr(svm, vc, "rd", msr, 0, 0,
			    "not-implemented");
			return 0;
		case MSR_APICBASE:
			vmm_svm_rdmsr_value(svm,
			    svm->mut_guest_apicbase);
			return 1;
		case MSR_MTRRdefType:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_mtrr_def_type);
			return 1;
		case MSR_SYSCFG:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_syscfg);
			return 1;
		case VMM_SVM_MSR_K7_HWCR:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_hwcr);
			return 1;
		case MSR_AMD_PATCH_LEVEL:
			vmm_svm_rdmsr_value(svm, VMM_SVM_AMD_PATCH_LEVEL);
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
			vmm_svm_log_unsupported_msr(svm, vc, "rd", msr, 0, 0,
			    vmm_svm_x2apic_msr(msr) ? "x2apic-hidden" :
			    "not-in-template");
			return 0;
		}
	}

	val = vmm_svm_wrmsr_value(svm);
	switch (msr) {
	case MSR_EFER:
		if ((val & ~VMM_SVM_EFER_VALID) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		vmcb->state.efer = (val & ~EFER_SVME) | EFER_SVME;
		vmcb->ctrl.tlb_ctrl = VMM_SVM_CTRL_TLB_FLUSH_ALL;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_PAT:
		if (!vmm_loader_x86_pat_valid(val)) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
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
	case MSR_TSC_DEADLINE:
		vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
		    "not-implemented");
		return 0;
	case MSR_APICBASE:
		if ((val & APICBASE_X2APIC) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "x2apic-hidden");
			return 0;
		}
		if ((val & ~VMM_SVM_APICBASE_VALID) != 0 ||
		    (val & APICBASE_ADDRESS) != VMM_SVM_APICBASE_ADDR) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		svm->mut_guest_apicbase = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_MTRRdefType:
		if ((val & ~VMM_SVM_MTRR_DEF_VALID) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		svm->mut_guest_mtrr_def_type = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_SYSCFG:
		vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
		    "read-only");
		return 0;
	case VMM_SVM_MSR_K7_HWCR:
		val &= ~VMM_SVM_HWCR_IGNORE;
		if ((val & ~VMM_SVM_HWCR_VALID) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		svm->mut_guest_hwcr = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_AMD_PATCH_LEVEL:
		vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
		    "read-only");
		return 0;
	case MSR_AMD_PATCH_LOADER:
		vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
		    "microcode-update");
		return 0;
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
		vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
		    vmm_svm_x2apic_msr(msr) ? "x2apic-hidden" :
		    "not-in-template");
		return 0;
	}
}

static void
vmm_svm_handle_root_event(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc, uint32_t reqflags)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	const char *name;
	uint32_t hvmflags = reqflags & RQF_HVM_MASK;

	/*
	 * SVM does not report the physical INTR vector in EXITINFO1.  After
	 * STGI, DragonFly's root vector path marks pending work in gd_reqflags;
	 * only log physical INTR when it corresponds to non-timer HVM work.
	 */
	splz_check();
	if (vmcb->ctrl.exitcode == VMM_SVM_EXIT_INTR &&
	    (hvmflags & RQF_TIMER) != 0 &&
	    (hvmflags & ~RQF_TIMER) == 0) {
		lwkt_user_yield();
		return;
	}
	switch (vmcb->ctrl.exitcode) {
	case VMM_SVM_EXIT_INTR:
		name = "intr";
		break;
	case VMM_SVM_EXIT_NMI:
		name = "nmi";
		break;
	case VMM_SVM_EXIT_SMI:
		name = "smi";
		break;
	case VMM_SVM_EXIT_INIT:
		name = "init";
		break;
	case VMM_SVM_EXIT_VINTR:
		name = "vintr";
		break;
	default:
		name = "unknown";
		break;
	}
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm vcpu%u root %s exit info1=0x%jx info2=0x%jx hvmflags=0x%x reqflags=0x%x rip=0x%jx",
	    vc->imm_id, name, (uintmax_t)vmcb->ctrl.exitinfo1,
	    (uintmax_t)vmcb->ctrl.exitinfo2, hvmflags, reqflags,
	    (uintmax_t)vmcb->state.rip);
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
	struct vmm_console *console = &svm->borrow_imm_machine->own_mut_console;
	char ch;
	uint32_t lsr;

	if (size != 1)
		return 0;
	switch (reg) {
	case VMM_COM1_RBR_THR_DLL:
		if ((svm->mut_com1_lcr & VMM_COM1_LCR_DLAB) != 0) {
			*valp = 0;
		} else if (vmm_console_guest_read(console, &ch)) {
			*valp = (uint8_t)ch;
		} else {
			*valp = 0;
		}
		return 1;
	case VMM_COM1_IER_DLM:
		*valp = (svm->mut_com1_lcr & VMM_COM1_LCR_DLAB) ?
		    0 : svm->mut_com1_ier;
		return 1;
	case VMM_COM1_IIR_FCR:
		*valp = VMM_COM1_IIR_NOPEND;
		return 1;
	case VMM_COM1_LCR:
		*valp = svm->mut_com1_lcr;
		return 1;
	case VMM_COM1_MCR:
		*valp = svm->mut_com1_mcr;
		return 1;
	case VMM_COM1_LSR:
		lsr = VMM_COM1_LSR_THRE | VMM_COM1_LSR_TEMT;
		if (vmm_console_guest_pending(console) != 0)
			lsr |= VMM_COM1_LSR_DR;
		*valp = lsr;
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
vmm_svm_handle_ioio(struct vmm_svm_backend *svm, struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	uint64_t info = vmcb->ctrl.exitinfo1;
	unsigned int port = (unsigned int)VMM_SVM_IOIO_PORT(info);
	const char *op = (info & VMM_SVM_IOIO_IN) ? "in" : "out";
	int size = vmm_svm_ioio_size(info);
	uint32_t val;

	if (size == 0 || (info & (VMM_SVM_IOIO_STR | VMM_SVM_IOIO_REP)) != 0) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u unsupported ioio op=%s port=0x%x size=%d info=0x%jx rip=0x%jx",
		    vc->imm_id, op, port, size, (uintmax_t)info,
		    (uintmax_t)vmcb->state.rip);
		return 0;
	}

	switch (port) {
	case VMM_PIC1_CMD:
	case VMM_PIC1_DATA:
	case VMM_PIC2_CMD:
	case VMM_PIC2_DATA:
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u unsupported pic io op=%s port=0x%x size=%d rip=0x%jx",
		    vc->imm_id, op, port, size, (uintmax_t)vmcb->state.rip);
		return 0;
	default:
		break;
	}

	if (port < VMM_COM1_BASE || port > VMM_COM1_BASE + VMM_COM1_SCR) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u unsupported ioio op=%s port=0x%x size=%d rip=0x%jx",
		    vc->imm_id, op, port, size, (uintmax_t)vmcb->state.rip);
		return 0;
	}

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
vmm_svm_handle_guest_cache_op(struct vmm_svm_backend *svm)
{
	/*
	 * The guest has no visible cache model yet.  Do not let cache
	 * maintenance instructions disturb host caches.
	 */
	vmm_svm_advance_rip(svm->own_mut_vmcb);
}

static void
vmm_svm_handle_guest_tlb_op(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;

	vmcb->ctrl.tlb_ctrl = VMM_SVM_CTRL_TLB_FLUSH_ALL;
	vmm_svm_advance_rip(vmcb);
}

static int
vmm_svm_handle_xsetbv(struct vmm_svm_backend *svm)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	uint64_t xcr0;

	if ((uint32_t)svm->mut_gprs[VMM_X64_GPR_RCX] != 0)
		return 0;
	xcr0 = (svm->mut_gprs[VMM_X64_GPR_RDX] << 32) |
	    (vmcb->state.rax & 0xffffffffULL);
	if (!vmm_loader_x86_xcr0_valid(xcr0) ||
	    (xcr0 & ~npx_xcr0_mask) != 0) {
		return 0;
	}
	svm->imm_guest_xcr0 = xcr0;
	vmm_svm_advance_rip(vmcb);
	return 1;
}

static void
vmm_svm_handle_idle_wait(struct vmm_vcpu_thread *vc, struct vmm_svm_vmcb *vmcb)
{
	vmm_svm_advance_rip(vmcb);
	if (!vmm_vcpu_should_stop(vc))
		tsleep(vc, 0, "vmmhlt", hz / 20 + 1);
}


static int
vmm_svm_handle_vmmcall(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	uint32_t magic = (uint32_t)vmcb->state.rax;
	uint32_t op = (uint32_t)svm->mut_gprs[VMM_X64_GPR_RBX];
	uint32_t arg = (uint32_t)svm->mut_gprs[VMM_X64_GPR_RCX];

	if (magic != VMM_SVM_SMOKE_AVIC_MAGIC)
		return 0;
	switch (op) {
	case VMM_SVM_SMOKE_AVIC_DELIVER:
		vmm_machine_logf(svm->borrow_imm_machine,
		    "smoke avic request vector=0x%x", arg & 0xffU);
		vmm_svm_advance_rip(vmcb);
		vmm_svm_avic_deliver(svm, vc, (uint8_t)arg, "smoke");
		return 1;
	case VMM_SVM_SMOKE_AVIC_MARKER:
		vmm_machine_logf(svm->borrow_imm_machine,
		    "smoke avic marker=0x%x", arg);
		vmm_svm_advance_rip(vmcb);
		return 1;
	default:
		vmm_machine_logf(svm->borrow_imm_machine,
		    "smoke avic unknown op=%u arg=0x%x", op, arg);
		return 0;
	}
}

static int
vmm_svm_handle_avic_exit(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	const char *name;
	uint32_t value;
	uint32_t extra;

	if (vmcb->ctrl.exitcode == VMM_SVM_EXIT_AVIC_INCOMPLETE_IPI) {
		value = (uint32_t)(vmcb->ctrl.exitinfo2 >> 32);
		extra = (uint32_t)(vmcb->ctrl.exitinfo2 &
		    VMM_SVM_AVIC_PHYS_MAX_INDEX_MASK);
		switch (value) {
		case VMM_SVM_AVIC_IPI_INVALID_INT_TYPE:
			name = "invalid_int_type";
			break;
		case VMM_SVM_AVIC_IPI_TARGET_NOT_RUNNING:
			name = "target_not_running";
			break;
		case VMM_SVM_AVIC_IPI_INVALID_TARGET:
			name = "invalid_target";
			break;
		case VMM_SVM_AVIC_IPI_INVALID_BACKING_PAGE:
			name = "invalid_backing_page";
			break;
		case VMM_SVM_AVIC_IPI_INVALID_IPI_VECTOR:
			name = "invalid_ipi_vector";
			break;
		default:
			name = "unknown";
			break;
		}
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u avic incomplete_ipi reason=%s id=%u index=%u icrl=0x%08x icrh=0x%08x info1=0x%jx info2=0x%jx rip=0x%jx",
		    vc->imm_id, name, value, extra,
		    (uint32_t)vmcb->ctrl.exitinfo1,
		    (uint32_t)(vmcb->ctrl.exitinfo1 >> 32),
		    (uintmax_t)vmcb->ctrl.exitinfo1,
		    (uintmax_t)vmcb->ctrl.exitinfo2,
		    (uintmax_t)vmcb->state.rip);
		return 0;
	}
	value = (uint32_t)(vmcb->ctrl.exitinfo1 &
	    VMM_SVM_AVIC_UNACCEL_ACCESS_OFFSET_MASK);
	extra = (uint32_t)((vmcb->ctrl.exitinfo1 >> 32) &
	    VMM_SVM_AVIC_UNACCEL_ACCESS_WRITE_MASK);
	switch (value) {
	case VMM_SVM_APIC_REG_ID:
		name = "id";
		break;
	case 0x0b0:
		name = "eoi";
		break;
	case 0x0c0:
		name = "rrr";
		break;
	case 0x0d0:
		name = "ldr";
		break;
	case 0x0e0:
		name = "dfr";
		break;
	case VMM_SVM_APIC_REG_SVR:
		name = "svr";
		break;
	case 0x280:
		name = "esr";
		break;
	case 0x300:
		name = "icr";
		break;
	case 0x320:
		name = "lvtt";
		break;
	case 0x330:
		name = "lvt_thermal";
		break;
	case 0x340:
		name = "lvt_pc";
		break;
	case 0x350:
		name = "lvt0";
		break;
	case 0x360:
		name = "lvt1";
		break;
	case 0x370:
		name = "lvt_error";
		break;
	case 0x380:
		name = "tmict";
		break;
	case 0x3e0:
		name = "tdcr";
		break;
	default:
		name = "unknown";
		break;
	}
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm vcpu%u avic noaccel reg=%s offset=0x%x write=%u vector=0x%x info1=0x%jx info2=0x%jx rip=0x%jx",
	    vc->imm_id, name, value, extra,
	    (uint32_t)(vmcb->ctrl.exitinfo2 &
	    VMM_SVM_AVIC_UNACCEL_ACCESS_VECTOR_MASK),
	    (uintmax_t)vmcb->ctrl.exitinfo1,
	    (uintmax_t)vmcb->ctrl.exitinfo2, (uintmax_t)vmcb->state.rip);
	return 0;
}

static int
vmm_svm_handle_npf(struct vmm_svm_backend *svm, struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	struct vmm_machine *m = svm->borrow_imm_machine;
	uint64_t gpa = vmcb->ctrl.exitinfo2;
	int prot;
	int error;

	if (gpa >= VMM_IOAPIC_BASE && gpa < VMM_IOAPIC_BASE + VMM_IOAPIC_SIZE) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u unsupported ioapic mmio gpa=0x%jx info=0x%jx rip=0x%jx",
		    vc->imm_id, (uintmax_t)gpa,
		    (uintmax_t)vmcb->ctrl.exitinfo1,
		    (uintmax_t)vmcb->state.rip);
		return 0;
	}
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


static void
vmm_svm_vcpu_run(void *backend, struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_backend *svm = backend;
	struct vmm_svm_vmcb *vmcb;
	uint32_t reqflags;

	if (svm == NULL)
		return;
	vmcb = svm->own_mut_vmcb;
	vmm_svm_avic_bind_cpu(svm);
	while (!vmm_vcpu_should_stop(vc)) {
		vmm_svm_enable_cpu(svm);
		vmm_svm_clgi();
		vmm_svm_host_tlb_catchup(svm);
		if (__predict_false(vmm_svm_host_entry_blocked())) {
			vmm_svm_stgi();
			splz_check();
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
		reqflags = mycpu->gd_reqflags;
		vmm_svm_requeue_exit_event(svm);
		switch (vmcb->ctrl.exitcode) {
		case VMM_SVM_EXIT_INTR:
		case VMM_SVM_EXIT_NMI:
		case VMM_SVM_EXIT_SMI:
		case VMM_SVM_EXIT_INIT:
		case VMM_SVM_EXIT_VINTR:
			vmm_svm_handle_root_event(svm, vc, reqflags);
			break;
		case VMM_SVM_EXIT_INVD:
		case VMM_SVM_EXIT_WBINVD:
			vmm_svm_handle_guest_cache_op(svm);
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
			vmm_svm_handle_idle_wait(vc, vmcb);
			break;
		case VMM_SVM_EXIT_INVLPG:
		case VMM_SVM_EXIT_INVLPGA:
		case VMM_SVM_EXIT_INVLPGB:
			vmm_svm_handle_guest_tlb_op(svm);
			break;
		case VMM_SVM_EXIT_IOIO:
			if (vmm_svm_handle_ioio(svm, vc))
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
			vmm_svm_handle_idle_wait(vc, vmcb);
			break;
		case VMM_SVM_EXIT_NPF:
			if (vmm_svm_handle_npf(svm, vc))
				break;
			goto unhandled;
		case VMM_SVM_EXIT_AVIC_INCOMPLETE_IPI:
		case VMM_SVM_EXIT_AVIC_NOACCEL:
			if (vmm_svm_handle_avic_exit(svm, vc))
				break;
			goto unhandled;
		case VMM_SVM_EXIT_MSR:
			if (vmm_svm_handle_msr(svm, vc))
				break;
			goto unhandled;
		case VMM_SVM_EXIT_XSETBV:
			if (vmm_svm_handle_xsetbv(svm))
				break;
			goto unhandled;
		case VMM_SVM_EXIT_VMMCALL:
			if (vmm_svm_handle_vmmcall(svm, vc))
				break;
			goto unhandled;
		default:
	unhandled:
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unhandled exit=0x%jx info1=0x%jx info2=0x%jx rip=0x%jx rcx=0x%jx rax=0x%jx rdx=0x%jx",
			    vc->imm_id,
			    (uintmax_t)vmcb->ctrl.exitcode,
			    (uintmax_t)vmcb->ctrl.exitinfo1,
			    (uintmax_t)vmcb->ctrl.exitinfo2,
			    (uintmax_t)vmcb->state.rip,
			    (uintmax_t)svm->mut_gprs[VMM_X64_GPR_RCX],
			    (uintmax_t)vmcb->state.rax,
			    (uintmax_t)svm->mut_gprs[VMM_X64_GPR_RDX]);
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

const struct vmm_vcpu_backend_ops vmm_svm_backend_ops = {
	.imm_name = "svm",
	.available = vmm_svm_available,
	.create = vmm_svm_vcpu_create,
	.destroy = vmm_svm_vcpu_destroy,
	.run = vmm_svm_vcpu_run,
};

VMM_VCPU_BACKEND_SET(vmm_svm_backend_ops);
