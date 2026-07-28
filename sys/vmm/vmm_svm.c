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
#include <sys/libkern.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/systimer.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <machine/cpufunc.h>
#include <machine/cpu.h>
#include <machine/clock.h>
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

static int vmm_svm_trace_enabled;
static int vmm_svm_timing_trace_enabled;
static int vmm_svm_fpu_check_enabled;

SYSCTL_DECL(_debug_vmm);
SYSCTL_INT(_debug_vmm, OID_AUTO, svm_trace, CTLFLAG_RW,
    &vmm_svm_trace_enabled, 0,
    "record SVM trace events in per-machine events logs");
SYSCTL_INT(_debug_vmm, OID_AUTO, svm_timing_trace, CTLFLAG_RW,
    &vmm_svm_timing_trace_enabled, 0,
    "record bounded SVM PM timer and HPET reference events");
SYSCTL_INT(_debug_vmm, OID_AUTO, svm_fpu_check, CTLFLAG_RW,
    &vmm_svm_fpu_check_enabled, 0,
    "verify root FPU state preservation across each VMRUN");

#define VMM_SVM_TRACE(svm, fmt, ...) do {				\
	if (vmm_svm_trace_enabled)					\
		vmm_machine_logf((svm)->borrow_imm_machine, fmt,	\
		    __VA_ARGS__);					\
} while (0)

#define VMM_SVM_TIMING_TRACE_LIMIT	96U

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
#define VMM_SVM_CTRL_INTR_SHADOW	(1ULL << 0)
#define VMM_SVM_CTRL_V_INTR_PRIO_SHIFT	16
#define VMM_SVM_CTRL_V_INTR_PRIO_MASK	(0xfULL << VMM_SVM_CTRL_V_INTR_PRIO_SHIFT)
#define VMM_SVM_CTRL_V_INTR_VECTOR_SHIFT 32
#define VMM_SVM_CTRL_V_INTR_VECTOR_MASK	(0xffULL << VMM_SVM_CTRL_V_INTR_VECTOR_SHIFT)
#define VMM_SVM_CTRL_V_INTR_MASKING	(1ULL << 24)
#define VMM_SVM_CTRL_V_AVIC_EN		(1ULL << 31)
#define VMM_SVM_EVENTINJ_VALID		(1ULL << 31)
#define VMM_SVM_EVENTINJ_ERROR_VALID	(1ULL << 11)
#define VMM_SVM_EVENTINJ_TYPE_EXCEPTION	(3ULL << 8)
#define VMM_X86_EXCEPTION_UD		6U
#define VMM_X86_EXCEPTION_GP		13U
#define VMM_SVM_PAUSE_FILTER_COUNT	4096U
#define VMM_SVM_PAUSE_FILTER_THRESHOLD	128U
#define VMM_SVM_AVIC_PHYS_VALID		(1ULL << 63)
#define VMM_SVM_AVIC_PHYS_RUNNING	(1ULL << 62)
#define VMM_SVM_AVIC_PHYS_HOST_ID_MASK	0xfffULL
#define VMM_SVM_AVIC_PHYS_MAX_INDEX_MASK 0xffULL
#define VMM_SVM_AVIC_MAX_PHYS_ID	0U
#define VMM_SVM_AVIC_APIC_ID		0U
#define VMM_SVM_APIC_REG_ID		0x020U
#define VMM_SVM_APIC_REG_VERSION	0x030U
#define VMM_SVM_APIC_REG_TPR		0x080U
#define VMM_SVM_APIC_REG_EOI		0x0b0U
#define VMM_SVM_APIC_REG_ISR_BASE	0x100U
#define VMM_SVM_APIC_REG_SVR		0x0f0U
#define VMM_SVM_APIC_REG_IRR_BASE	0x200U
#define VMM_SVM_APIC_REG_ESR		0x280U
#define VMM_SVM_APIC_REG_LVTT		0x320U
#define VMM_SVM_APIC_REG_LVT_THERMAL	0x330U
#define VMM_SVM_APIC_REG_LVT_PC		0x340U
#define VMM_SVM_APIC_REG_LVT0		0x350U
#define VMM_SVM_APIC_REG_LVT1		0x360U
#define VMM_SVM_APIC_REG_LVT_ERROR	0x370U
#define VMM_SVM_APIC_REG_TMICT		0x380U
#define VMM_SVM_APIC_REG_TMCCT		0x390U
#define VMM_SVM_APIC_REG_TDCR		0x3e0U
#define VMM_SVM_APIC_VERSION		0x00140014U
#define VMM_SVM_APIC_SVR_VALID		0x000003ffU
#define VMM_SVM_APIC_SVR_ENABLE		0x100U
#define VMM_SVM_APIC_LVT_VECTOR_MASK	0x000000ffU
#define VMM_SVM_APIC_LVT_DELIVERY_MODE_MASK 0x00000700U
#define VMM_SVM_APIC_LVT_SEND_PENDING	0x00001000U
#define VMM_SVM_APIC_LVT_INPUT_POLARITY	0x00002000U
#define VMM_SVM_APIC_LVT_REMOTE_IRR	0x00004000U
#define VMM_SVM_APIC_LVT_LEVEL_TRIGGER	0x00008000U
#define VMM_SVM_APIC_LVT_MASKED		0x00010000U
#define VMM_SVM_APIC_LVT_TIMER_MODE_MASK 0x00060000U
#define VMM_SVM_APIC_LVT_TIMER_PERIODIC	0x00020000U
#define VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE 0x00040000U
#define VMM_SVM_APIC_LVT_COMMON_VALID	\
	(VMM_SVM_APIC_LVT_VECTOR_MASK | VMM_SVM_APIC_LVT_SEND_PENDING | \
	 VMM_SVM_APIC_LVT_MASKED)
#define VMM_SVM_APIC_LVT_ERROR_VALID	\
	VMM_SVM_APIC_LVT_COMMON_VALID
#define VMM_SVM_APIC_LVT_TIMER_VALID	\
	(VMM_SVM_APIC_LVT_COMMON_VALID | VMM_SVM_APIC_LVT_TIMER_MODE_MASK)
#define VMM_SVM_APIC_LVT_DELIVERY_VALID	\
	(VMM_SVM_APIC_LVT_COMMON_VALID | VMM_SVM_APIC_LVT_DELIVERY_MODE_MASK)
#define VMM_SVM_APIC_LVT_LINT_VALID	\
	(VMM_SVM_APIC_LVT_COMMON_VALID | \
	 VMM_SVM_APIC_LVT_DELIVERY_MODE_MASK | \
	 VMM_SVM_APIC_LVT_INPUT_POLARITY | VMM_SVM_APIC_LVT_REMOTE_IRR | \
	 VMM_SVM_APIC_LVT_LEVEL_TRIGGER)
#define VMM_SVM_APIC_TIMER_DIVIDE_VALID	0x0000000bU
#define VMM_SVM_LAPIC_TIMER_HZ		1000000000ULL
#define VMM_SVM_ROOT_TIMER_MAX_US	(60LL * 1000 * 1000)
#define MSR_AMD64_SVM_AVIC_DOORBELL	0xc001011bU
#define VMM_SVM_MSR_AMD64_TSC_RATIO	0xc0000104U
#define VMM_SVM_TSC_RATIO_MAX		0x000000ffffffffffULL

#define VMM_SVM_EXIT_INTR		0x060ULL
#define VMM_SVM_EXIT_NMI		0x061ULL
#define VMM_SVM_EXIT_SMI		0x062ULL
#define VMM_SVM_EXIT_INIT		0x063ULL
#define VMM_SVM_EXIT_VINTR		0x064ULL
#define VMM_SVM_EXIT_RDPMC		0x06fULL
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
#define VMM_COM1_IER_RDI	0x01U
#define VMM_COM1_IER_THRI	0x02U
#define VMM_COM1_IIR_NOPEND	0x01U
#define VMM_COM1_IIR_THRI	0x02U
#define VMM_COM1_IIR_RDI	0x04U
#define VMM_COM1_FCR_ENABLE	0x01U
#define VMM_COM1_FCR_RX_RESET	0x02U
#define VMM_COM1_FCR_TX_RESET	0x04U
#define VMM_COM1_LCR_DLAB	0x80U
#define VMM_COM1_MCR_DTR	0x01U
#define VMM_COM1_MCR_RTS	0x02U
#define VMM_COM1_MCR_OUT1	0x04U
#define VMM_COM1_MCR_OUT2	0x08U
#define VMM_COM1_MCR_LOOP	0x10U
#define VMM_COM1_LSR_DR		0x01U
#define VMM_COM1_LSR_OE		0x02U
#define VMM_COM1_LSR_THRE	0x20U
#define VMM_COM1_LSR_TEMT	0x40U
#define VMM_COM1_MSR_CTS	0x10U
#define VMM_COM1_MSR_DSR	0x20U
#define VMM_COM1_MSR_RI		0x40U
#define VMM_COM1_MSR_DCD	0x80U
#define VMM_COM1_IOAPIC_PIN	4U
#define VMM_COM2_BASE		0x2f8U
#define VMM_COM3_BASE		0x3e8U
#define VMM_COM4_BASE		0x2e8U
#define VMM_PIC1_CMD		0x20U
#define VMM_PIC1_DATA		0x21U
#define VMM_PIC_ELCR1		0x4d0U
#define VMM_PIC_ELCR2		0x4d1U
#define VMM_PIC_ELCR1_MASK	0xf8U
#define VMM_PIC_ELCR2_MASK	0xdeU
#define VMM_PIT_CH0		0x40U
#define VMM_PIT_CH2		0x42U
#define VMM_PIT_CMD		0x43U
#define VMM_PIT_PORTB		0x61U
#define VMM_PIT_FREQ		1193182ULL
#define VMM_PIT_PORTB_GATE2	0x01U
#define VMM_PIT_PORTB_OUT2	0x20U
#define VMM_ACPI_SLEEP_CONTROL_PORT	0x404U
#define VMM_ACPI_SLEEP_STATUS_PORT	0x405U
#define VMM_ACPI_SLEEP_S5_ENABLE	0x34U
#define VMM_ACPI_RESET_PORT		0x40cU
#define VMM_ACPI_RESET_VALUE		0x01U
#define VMM_PM_TIMER_PORT	0x408U
#define VMM_PM_TIMER_LAST	0x40bU
#define VMM_PM_TIMER_FREQ	3579545ULL
#define VMM_PM_TIMER_MASK	0x00ffffffU
#define VMM_ISA_MISC_PORT	0x80U
#define VMM_ISA_MISC_LAST	0x8fU
#define VMM_PIC2_CMD		0xa0U
#define VMM_PIC2_DATA		0xa1U
#define VMM_PCI_CFG_CTRL	0xcfbU
#define VMM_PCI_CFG_ADDR	0xcf8U
#define VMM_PCI_CFG_ADDR_LAST	0xcfbU
#define VMM_PCI_CFG_TYPE2_FORWARD	0xcfaU
#define VMM_PCI_CFG_DATA	0xcfcU
#define VMM_PCI_CFG_DATA_LAST	0xcffU
#define VMM_CMOS_INDEX		0x70U
#define VMM_CMOS_DATA		0x71U
#define VMM_RTC_IRQ		8U
#define VMM_RTC_SECONDS		0x00U
#define VMM_RTC_SECONDS_ALARM	0x01U
#define VMM_RTC_MINUTES		0x02U
#define VMM_RTC_MINUTES_ALARM	0x03U
#define VMM_RTC_HOURS		0x04U
#define VMM_RTC_HOURS_ALARM	0x05U
#define VMM_RTC_DAY_OF_WEEK	0x06U
#define VMM_RTC_DAY_OF_MONTH	0x07U
#define VMM_RTC_MONTH		0x08U
#define VMM_RTC_YEAR		0x09U
#define VMM_RTC_REG_A		0x0aU
#define VMM_RTC_REG_B		0x0bU
#define VMM_RTC_REG_C		0x0cU
#define VMM_RTC_REG_D		0x0dU
#define VMM_RTC_ALARM_DONT_CARE 0xc0U
#define VMM_RTC_REG_A_DEFAULT	0x26U
#define VMM_RTC_REG_A_UIP	0x80U
#define VMM_RTC_REG_A_DIV_MASK	0x70U
#define VMM_RTC_REG_A_DIV_32KHZ 0x20U
#define VMM_RTC_REG_A_RATE_MASK	0x0fU
#define VMM_RTC_REG_B_SET	0x80U
#define VMM_RTC_REG_B_PIE	0x40U
#define VMM_RTC_REG_B_AIE	0x20U
#define VMM_RTC_REG_B_UIE	0x10U
#define VMM_RTC_REG_B_DM_BINARY 0x04U
#define VMM_RTC_REG_B_24H	0x02U
#define VMM_RTC_REG_C_IRQF	0x80U
#define VMM_RTC_REG_C_PF	0x40U
#define VMM_RTC_REG_C_AF	0x20U
#define VMM_RTC_REG_C_UF	0x10U
#define VMM_RTC_REG_C_CAUSE_MASK (VMM_RTC_REG_C_PF | \
					  VMM_RTC_REG_C_AF | VMM_RTC_REG_C_UF)
#define VMM_RTC_REG_D_VALID	0x80U
#define VMM_RTC_LEAP_YEAR(year) \
	((((year) % 4) == 0 && ((year) % 100) != 0) || ((year) % 400) == 0)
#define VMM_CPUID_APIC_ID_MASK	0xff000000U
#define VMM_CPUID_MAX_BASIC	0x16U
#define VMM_CPUID_MAX_EXTENDED	0x8000001dU
#define VMM_CPUID_XSAVE_LEGACY_SIZE 0x240U
#define VMM_CPUID1_ECX_ALLOWED	(CPUID2_SSE3 | CPUID2_PCLMULQDQ | \
					 CPUID2_SSSE3 | CPUID2_FMA | \
					 CPUID2_CX16 | CPUID2_SSE41 | \
					 CPUID2_SSE42 | CPUID2_MOVBE | \
					 CPUID2_POPCNT | CPUID2_TSCDLT | \
					 CPUID2_AESNI | CPUID2_XSAVE | \
					 CPUID2_OSXSAVE | CPUID2_AVX | \
					 CPUID2_F16C | CPUID2_RDRAND)
#define VMM_CPUID1_EDX_ALLOWED	(CPUID_FPU | CPUID_VME | CPUID_DE | \
					 CPUID_PSE | CPUID_TSC | CPUID_MSR | \
					 CPUID_PAE | CPUID_CX8 | CPUID_APIC | \
					 CPUID_SEP | CPUID_MTRR | CPUID_PGE | \
					 CPUID_CMOV | CPUID_PAT | CPUID_PSE36 | \
					 CPUID_CLFSH | CPUID_MMX | CPUID_FXSR | \
					 CPUID_SSE | CPUID_SSE2 | CPUID_SS)
#define VMM_CPUID7_EBX_ALLOWED	(CPUID_STDEXT_FSGSBASE | \
					 CPUID_STDEXT_BMI1 | CPUID_STDEXT_AVX2 | \
					 CPUID_STDEXT_FDP_EXC | CPUID_STDEXT_SMEP | \
					 CPUID_STDEXT_BMI2 | CPUID_STDEXT_ERMS | \
					 CPUID_STDEXT_NFPUSG | CPUID_STDEXT_RDSEED | \
					 CPUID_STDEXT_ADX | CPUID_STDEXT_SMAP | \
					 CPUID_STDEXT_CLFLUSHOPT | CPUID_STDEXT_CLWB | \
					 CPUID_STDEXT_SHA)
#define VMM_CPUID7_ECX_ALLOWED	(CPUID_STDEXT2_UMIP | CPUID_STDEXT2_RDPID)
#define VMM_CPUID80000001_ECX_ALLOWED (CPUID_LAHF | CPUID_ALTMOVCR0 | \
					 CPUID_ABM | CPUID_SSE4A | \
					 CPUID_MISALIGNSSE | CPUID_3DNOWPF | \
					 CPUID_XOP | CPUID_FMA4 | CPUID_TCE | \
					 CPUID_TBM)
#define VMM_CPUID80000001_EDX_ALLOWED (CPUID_FPU | CPUID_VME | \
					 CPUID_DE | CPUID_PSE | CPUID_TSC | \
					 CPUID_MSR | CPUID_PAE | CPUID_CX8 | \
					 CPUID_APIC | CPUID_SYSCALL | CPUID_MTRR | \
					 CPUID_PGE | CPUID_CMOV | CPUID_PAT | \
					 CPUID_PSE36 | CPUID_XD | CPUID_MMXX | \
					 CPUID_MMX | CPUID_FXSR | CPUID_FFXSR | \
					 CPUID_PAGE1GB | CPUID_RDTSCP | CPUID_EM64T)

#define VMM_IOAPIC_BASE		0xfec00000ULL
#define VMM_IOAPIC_SIZE		PAGE_SIZE
#define VMM_IOAPIC_ID			1U
#define VMM_IOAPIC_PINS		24U
#define VMM_IOAPIC_REG_ID		0x00U
#define VMM_IOAPIC_REG_VERSION		0x01U
#define VMM_IOAPIC_REG_ARB		0x02U
#define VMM_IOAPIC_REDIR_BASE		0x10U
#define VMM_IOAPIC_VERSION		(((VMM_IOAPIC_PINS - 1U) << 16) | \
					 0x11U)
#define VMM_IOAPIC_REDIR_LOW_VALID	0x0001afffU
#define VMM_IOAPIC_REDIR_HIGH_VALID	0xff000000U
#define VMM_IOAPIC_REDIR_MASKED	0x00010000U
#define VMM_IOAPIC_REDIR_DELIVERY_FIXED 0x00000000U
#define VMM_IOAPIC_REDIR_DELIVERY_MASK	0x00000700U
#define VMM_IOAPIC_REDIR_DEST_LOGICAL	0x00000800U
#define VMM_IOAPIC_REDIR_POLARITY_LOW	0x00002000U
#define VMM_IOAPIC_REDIR_TRIGGER_LEVEL	0x00008000U
#define VMM_HPET_BASE		0xfed00000ULL
#define VMM_HPET_SIZE		PAGE_SIZE
#define VMM_HPET_FREQ		10000000ULL
#define VMM_HPET_PERIOD_FS	(1000000000000000ULL / VMM_HPET_FREQ)
#define VMM_HPET_TIMER_COUNT	3
#define VMM_HPET_GSI		16U
#define VMM_HPET_REG_CAP	0x000U
#define VMM_HPET_REG_CONFIG	0x010U
#define VMM_HPET_REG_STATUS	0x020U
#define VMM_HPET_REG_COUNTER	0x0f0U
#define VMM_HPET_TIMER_BASE	0x100U
#define VMM_HPET_TIMER_STRIDE	0x020U
#define VMM_HPET_TIMER_CONFIG	0x000U
#define VMM_HPET_TIMER_COMPARATOR 0x008U
#define VMM_HPET_TIMER_FSB	0x010U
#define VMM_HPET_CAP_ID		((VMM_HPET_PERIOD_FS << 32) | 0x80862201ULL)
#define VMM_HPET_CONFIG_ENABLE	0x001ULL
#define VMM_HPET_CONFIG_VALID	VMM_HPET_CONFIG_ENABLE
#define VMM_HPET_TIMER_ENABLE		0x004ULL
#define VMM_HPET_TIMER_PERIODIC	0x008ULL
#define VMM_HPET_TIMER_PERIODIC_CAP (1ULL << 4)
#define VMM_HPET_TIMER_SIZE_CAP	(1ULL << 5)
#define VMM_HPET_TIMER_SETVAL		0x040ULL
#define VMM_HPET_TIMER_32BIT		0x100ULL
#define VMM_HPET_TIMER_ROUTE_MASK	0x3e00ULL
#define VMM_HPET_TIMER_ROUTE_SHIFT	9
#define VMM_HPET_TIMER_ROUTE_CAP (1ULL << (32 + VMM_HPET_GSI))
#define VMM_HPET_TIMER_CAP	(VMM_HPET_TIMER_PERIODIC_CAP | \
				 VMM_HPET_TIMER_SIZE_CAP | \
				 VMM_HPET_TIMER_ROUTE_CAP)
#define VMM_HPET_TIMER_CONFIG_VALID (VMM_HPET_TIMER_ENABLE | \
					 VMM_HPET_TIMER_PERIODIC | \
					 VMM_HPET_TIMER_SETVAL | \
					 VMM_HPET_TIMER_32BIT | \
					 VMM_HPET_TIMER_ROUTE_MASK)
#define VMM_FCH_PM_BASE		0xfed80300ULL
#define VMM_FCH_PM_S5_RESET_STATUS 0x0c0ULL

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
#define VMM_SVM_SPEC_CTRL_VALID		(SPEC_CTRL_IBRS | \
					 SPEC_CTRL_STIBP | SPEC_CTRL_SSBD)
#define VMM_SVM_PRED_CMD_IBPB		0x1ULL
#define VMM_SVM_PRED_CMD_VALID		VMM_SVM_PRED_CMD_IBPB
#define VMM_SVM_ARCH_CAPABILITIES	0ULL
#define VMM_SVM_X2APIC_MSR_BASE	0x800U
#define VMM_SVM_X2APIC_MSR_LAST	0x83fU
#define VMM_SVM_MSR_K7_HWCR		0xc0010015U
#define VMM_SVM_MSR_AMD64_DE_CFG	0xc0011029U
#define VMM_SVM_DE_CFG_LFENCE_SERIALIZE	(1ULL << 1)
#define VMM_SVM_DE_CFG_ZEN2_FP_BACKUP_FIX (1ULL << 9)
#define VMM_SVM_DE_CFG_ERRATUM_665	(1ULL << 31)
#define VMM_SVM_DE_CFG_VALID		(VMM_SVM_DE_CFG_LFENCE_SERIALIZE | \
					 VMM_SVM_DE_CFG_ZEN2_FP_BACKUP_FIX | \
					 VMM_SVM_DE_CFG_ERRATUM_665)
#define VMM_SVM_MSR_ZEN4_BP_CFG	0xc001102eU
#define VMM_SVM_ZEN4_BP_CFG_BP_SPEC_REDUCE (1ULL << 4)
#define VMM_SVM_ZEN4_BP_CFG_SHARED_BTB_FIX (1ULL << 5)
#define VMM_SVM_ZEN2_BP_CFG_BUG_FIX	(1ULL << 33)
#define VMM_SVM_ZEN4_BP_CFG_VALID	(VMM_SVM_ZEN4_BP_CFG_BP_SPEC_REDUCE | \
					 VMM_SVM_ZEN4_BP_CFG_SHARED_BTB_FIX | \
					 VMM_SVM_ZEN2_BP_CFG_BUG_FIX)
#define VMM_SVM_MSR_ZEN2_SPECTRAL_CHICKEN 0xc00110e3U
#define VMM_SVM_ZEN2_SPECTRAL_CHICKEN_VALID (1ULL << 1)
#define VMM_SVM_MSR_IA32_MPERF		0x000000e7U
#define VMM_SVM_MSR_IA32_APERF		0x000000e8U
#define VMM_SVM_MSR_AMD64_IRPERF	0xc00000e9U
#define VMM_SVM_MSR_AMD64_IBSCTL	0xc001103aU
#define VMM_SVM_NB_CFG_ENABLE_CF8_EXT_CFG (1ULL << 46)
#define VMM_SVM_NB_CFG_VALID		(VMM_SVM_NB_CFG_ENABLE_CF8_EXT_CFG | \
					 NB_CFG_INITAPICCPUIDLO)
#define VMM_SVM_MSR_F15H_PERF_CTL	0xc0010200U
#define VMM_SVM_MSR_F15H_PERF_CTR	0xc0010201U
#define VMM_SVM_PMU_COUNTERS		6U
#define VMM_SVM_HWCR_MC_STATUS_WR_EN	(1ULL << 18)
#define VMM_SVM_HWCR_TSC_FREQ_SEL	(1ULL << 24)
#define VMM_SVM_HWCR_IRPERF_EN		(1ULL << 30)
#define VMM_SVM_HWCR_GUEST_FIXED	VMM_SVM_HWCR_TSC_FREQ_SEL
#define VMM_SVM_HWCR_IGNORE		(0x8ULL | 0x40ULL | 0x100ULL)
#define VMM_SVM_HWCR_VALID		(VMM_SVM_HWCR_MC_STATUS_WR_EN | \
					 VMM_SVM_HWCR_TSC_FREQ_SEL | \
					 VMM_SVM_HWCR_IRPERF_EN)
#define VMM_SVM_AMD_PATCH_LEVEL	0ULL
#define VMM_SVM_SMOKE_AVIC_MAGIC	0x43495641U
#define VMM_SVM_SMOKE_AVIC_DELIVER	1U
#define VMM_SVM_SMOKE_AVIC_MARKER	2U
#define VMM_SVM_SMOKE_IOAPIC_RAISE	3U
#define VMM_SVM_SMOKE_PAUSE_FILTER	4U
#define VMM_SVM_SMOKE_CPU_TEMPLATE_MARKER 5U
#define VMM_SVM_SMOKE_FPU_MARKER	6U


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
	int mut_avic_running;
	uint32_t mut_lapic_timer_lvtt;
	uint32_t mut_lapic_timer_tmict;
	uint32_t mut_lapic_timer_tdcr;
	uint32_t mut_lapic_timer_divisor;
	/*
	 * Root timer state is owned by the vCPU LWKT.  The systimer callback only
	 * wakes that thread and never reads or mutates this state.  The deadline is
	 * the earliest active LAPIC, HPET, or RTC deadline in root TSC units.
	 */
	uint64_t mut_lapic_timer_interval_root_tsc;
	uint64_t mut_lapic_timer_root_deadline;
	uint64_t mut_root_timer_deadline;
	struct systimer own_mut_root_timer_systimer;
	uint32_t mut_lapic_timer_fire_count;
	uint32_t mut_lapic_timer_idle_count;
	uint64_t mut_lapic_timer_tsc_deadline;
	uint32_t mut_pause_exit_count;
	int mut_lapic_timer_active;
	int mut_root_timer_systimer_armed;
	/*
	 * IOAPIC and COM1 device state is owned by the vCPU thread.  Host
	 * console writers may append to vmm_console's input FIFO and wake the
	 * vCPU, but they must not update these fields directly.  The vCPU
	 * thread re-evaluates the level-style RX condition before VM entry.
	 */
	uint32_t mut_ioapic_select;
	uint32_t mut_ioapic_id;
	uint64_t mut_ioapic_redir[VMM_IOAPIC_PINS];
	uint64_t imm_guest_xcr0;
	union savefpu mut_guest_fpu __aligned(64);
	mcontext_t mut_host_fpu_ctx;
	/* [0] is expected root state and [1] is the VMRUN return snapshot. */
	union savefpu *own_mut_fpu_sentinel;
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
	uint64_t mut_host_tsc_aux;
	uint64_t mut_guest_mtrr_def_type;
	uint64_t mut_guest_spec_ctrl;
	uint64_t mut_guest_syscfg;
	uint64_t mut_guest_hwcr;
	uint64_t mut_guest_de_cfg;
	uint64_t mut_guest_zen4_bp_cfg;
	uint64_t mut_guest_zen2_spectral_chicken;
	uint64_t mut_guest_mperf_offset;
	uint64_t mut_guest_aperf_offset;
	uint64_t mut_guest_nb_cfg;
	uint64_t mut_guest_pmu_ctl[VMM_SVM_PMU_COUNTERS];
	uint64_t mut_guest_pmu_ctr[VMM_SVM_PMU_COUNTERS];
	uint64_t mut_guest_tsc_aux;
	uint64_t mut_guest_apicbase;
	uint64_t imm_host_tsc_hz;
	uint64_t imm_guest_tsc_hz;
	uint64_t imm_tsc_ratio;
	uint64_t mut_hpet_config;
	uint32_t mut_hpet_status;
	uint64_t mut_hpet_counter_base;
	uint64_t mut_hpet_counter_tsc;
	uint64_t mut_hpet_timer_config[VMM_HPET_TIMER_COUNT];
	uint64_t mut_hpet_timer_comparator[VMM_HPET_TIMER_COUNT];
	uint64_t mut_hpet_timer_deadline[VMM_HPET_TIMER_COUNT];
	uint64_t mut_hpet_timer_period[VMM_HPET_TIMER_COUNT];
	uint64_t mut_hpet_timer_root_deadline[VMM_HPET_TIMER_COUNT];
	int mut_hpet_timer_comparator_set[VMM_HPET_TIMER_COUNT];
	int mut_hpet_timer_active[VMM_HPET_TIMER_COUNT];
	uint64_t mut_pm_timer_tsc;
	uint32_t mut_timing_trace_count;
	uint64_t mut_gprs[VMM_X64_NGPR];
	/* Set by this vCPU thread before returning to the core lifecycle path. */
	enum vmm_vcpu_exit_reason mut_exit_reason;
	uint8_t mut_com1_dll;
	uint8_t mut_com1_dlm;
	uint8_t mut_com1_ier;
	uint8_t mut_com1_fcr;
	uint8_t mut_com1_lcr;
	uint8_t mut_com1_mcr;
	uint8_t mut_com1_scr;
	int mut_com1_rx_irq_pending;
	int mut_com1_thr_irq_pending;
	int mut_com1_lsr_overrun;
	uint32_t mut_pci_cfg_addr;
	/* vCPU-thread-owned channel 2 mode-0 polling calibration state. */
	uint8_t mut_pit_portb;
	uint8_t mut_pit_ch0_read_state;
	uint8_t mut_pit_ch0_write_state;
	uint16_t mut_pit_ch0_reload;
	uint16_t mut_pit_ch0_count;
	uint64_t mut_pit_ch2_start_tsc;
	uint16_t mut_pit_ch2_reload;
	uint8_t mut_pit_ch2_read_state;
	uint8_t mut_pit_ch2_write_state;
	uint8_t mut_pit_ch2_armed;
	uint8_t mut_pic1_mask;
	uint8_t mut_pic2_mask;
	uint8_t mut_pic_elcr1;
	uint8_t mut_pic_elcr2;
	uint8_t mut_cmos_index;
	uint8_t mut_cmos_nmi_disabled;
	uint8_t mut_cmos_reg_a;
	uint8_t mut_cmos_reg_b;
	uint8_t mut_cmos_reg_c;
	uint8_t mut_cmos_ram[128];
	uint8_t mut_cmos_time[10];
	int64_t mut_cmos_time_offset;
	uint64_t mut_cmos_periodic_root_deadline;
	uint64_t mut_cmos_update_root_deadline;
	int mut_cmos_time_expires;
};

/*
 * Lock map:
 * own_mut_hsave and raw_imm_host_* are initialized and released only by the
 * selected backend's module-lifetime init/uninit callbacks.  Those callbacks
 * run after all mounts have gone away, so no vCPU can enter SVM concurrently.
 * mut_tsc_ratio is written only by the fixed-pCPU vCPU LWKT and is reset by
 * module teardown after every vCPU has stopped.
 */
struct vmm_svm_cpu_state {
	void		*own_mut_hsave;
	uint64_t	 imm_hsave_pa;
	uint64_t	 raw_imm_host_vm_cr;
	uint64_t	 raw_imm_host_efer;
	uint64_t	 raw_imm_host_hsave_pa;
	uint64_t	 raw_imm_host_tsc_ratio;
	uint64_t mut_tsc_ratio;
	int		 mut_enabled;
	int		 mut_restored;
};

static struct vmm_svm_cpu_state vmm_svm_cpu_state[MAXCPU];
static int vmm_svm_initialized;

static void	vmm_svm_cpu_capture(void *arg);
static void	vmm_svm_cpu_enable(void *arg);
static void	vmm_svm_cpu_restore(void *arg);

struct vmm_svm_msr_policy {
	uint32_t imm_msr;
	const char *imm_name;
	const char *imm_category;
};

CTASSERT(sizeof(struct vmm_svm_ctrl) == 1024);
CTASSERT(__offsetof(struct vmm_svm_ctrl, pause_filt_thresh) == 0x03c);
CTASSERT(__offsetof(struct vmm_svm_ctrl, pause_filt_cnt) == 0x03e);
CTASSERT(__offsetof(struct vmm_svm_ctrl, iopm_base_pa) == 0x040);
CTASSERT(__offsetof(struct vmm_svm_ctrl, msrpm_base_pa) == 0x048);
CTASSERT(__offsetof(struct vmm_svm_ctrl, tsc_offset) == 0x050);
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
CTASSERT(__offsetof(struct vmm_svm_state, rsp) == 0x1d8);
CTASSERT(__offsetof(struct vmm_svm_state, rax) == 0x1f8);
CTASSERT(__offsetof(struct vmm_svm_state, star) == 0x200);
CTASSERT(sizeof(struct vmm_svm_vmcb) == PAGE_SIZE);
CTASSERT(__offsetof(struct vmm_svm_vmcb, state) == 0x400);

void	vmm_svm_vmrun(uint64_t vmcb_pa, uint64_t *gprs);
static void vmm_svm_vcpu_destroy(void *backend);
static int vmm_svm_avic_init(struct vmm_svm_backend *svm);
static void vmm_svm_avic_uninit(struct vmm_svm_backend *svm);
static void vmm_svm_advance_rip(struct vmm_svm_vmcb *vmcb);
static uint64_t vmm_svm_guest_tsc(struct vmm_svm_backend *svm);
static void vmm_svm_com1_rx_notify(struct vmm_svm_backend *svm,
		    struct vmm_vcpu_thread *vc, const char *source);
static void vmm_svm_cmos_refresh_time(struct vmm_svm_backend *svm);

static const struct vmm_svm_msr_policy vmm_svm_msr_policies[] = {
	{ MSR_EFER, "efer", "cpu-state" },
	{ MSR_PAT, "pat", "memory-type" },
	{ MSR_TSC, "tsc", "time" },
	{ MSR_TSC_AUX, "tsc_aux", "time" },
	{ MSR_TSC_DEADLINE, "tsc_deadline", "time" },
	{ MSR_APICBASE, "apicbase", "apic" },
	{ MSR_SPEC_CTRL, "spec_ctrl", "cpu-mitigation" },
	{ MSR_PRED_CMD, "pred_cmd", "cpu-mitigation" },
	{ MSR_IA32_ARCH_CAPABILITIES, "arch_capabilities",
	    "cpu-mitigation" },
	{ MSR_AMD_VM_CR, "amd_vm_cr", "nested-svm" },
	{ MSR_MTRRcap, "mtrr_cap", "memory-type" },
	{ MSR_MTRRdefType, "mtrr_def_type", "memory-type" },
	{ MSR_SYSCFG, "amd_syscfg", "platform-config" },
	{ VMM_SVM_MSR_K7_HWCR, "amd_hwcr", "platform-config" },
	{ VMM_SVM_MSR_AMD64_DE_CFG, "amd_de_cfg", "platform-config" },
	{ VMM_SVM_MSR_ZEN4_BP_CFG, "zen4_bp_cfg", "platform-config" },
	{ VMM_SVM_MSR_ZEN2_SPECTRAL_CHICKEN, "zen2_spectral_chicken",
	    "platform-config" },
	{ VMM_SVM_MSR_AMD64_IRPERF, "irperf", "cpu-frequency" },
	{ VMM_SVM_MSR_IA32_MPERF, "mperf", "cpu-frequency" },
	{ VMM_SVM_MSR_IA32_APERF, "aperf", "cpu-frequency" },
	{ VMM_SVM_MSR_AMD64_IBSCTL, "amd_ibsctl", "perf" },
	{ MSR_AMD_NB_CFG, "amd_nb_cfg", "platform-config" },
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

static uint32_t
vmm_svm_avic_apic_read32(struct vmm_svm_backend *svm, uint32_t reg)
{
	volatile uint32_t *ptr;

	ptr = (volatile uint32_t *)((uint8_t *)svm->own_mut_avic_apic_page + reg);
	return *ptr;
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
	if (svm->own_mut_avic_phys_table != NULL) {
		svm->own_mut_avic_phys_table[svm->imm_avic_apic_id] = 0;
		cpu_mfence();
	}
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
	KKASSERT((apicid & ~VMM_SVM_AVIC_PHYS_HOST_ID_MASK) == 0);
	if (!svm->mut_avic_bound || svm->mut_avic_host_cpuid != cpuid) {
		svm->mut_avic_host_cpuid = cpuid;
		svm->mut_avic_host_apic_id = apicid;
		svm->mut_avic_bound = 1;
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm avic bound apic_id=%u host_cpuid=%u host_apic_id=%u",
		    svm->imm_avic_apic_id, cpuid, apicid);
	}
	entry = svm->imm_avic_apic_page_pa | VMM_SVM_AVIC_PHYS_VALID |
	    VMM_SVM_AVIC_PHYS_RUNNING | svm->mut_avic_host_apic_id;
	svm->own_mut_avic_phys_table[svm->imm_avic_apic_id] = entry;
	cpu_mfence();
	svm->mut_avic_running = 1;
}

static void
vmm_svm_avic_unbind_cpu(struct vmm_svm_backend *svm)
{
	uint64_t entry;

	if (svm == NULL || svm->own_mut_avic_phys_table == NULL ||
	    !svm->mut_avic_bound || !svm->mut_avic_running)
		return;
	entry = svm->imm_avic_apic_page_pa | VMM_SVM_AVIC_PHYS_VALID |
	    svm->mut_avic_host_apic_id;
	svm->own_mut_avic_phys_table[svm->imm_avic_apic_id] = entry;
	cpu_mfence();
	svm->mut_avic_running = 0;
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
	VMM_SVM_TRACE(svm,
	    "svm vcpu%u avic deliver source=%s vector=0x%x irr=0x%x",
	    vc->imm_id, source, vector, *irr);
	/* A local producer returns to VMRUN and consumes IRR without a doorbell. */
	if (svm->mut_avic_running &&
	    svm->mut_avic_host_cpuid != mycpu->gd_cpuid) {
		wrmsr(MSR_AMD64_SVM_AVIC_DOORBELL, svm->mut_avic_host_apic_id);
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic doorbell host_apic_id=%u",
		    vc->imm_id, svm->mut_avic_host_apic_id);
	}
}

static void
vmm_svm_root_timer_systimer(struct systimer *timer, int in_ipi,
    struct intrframe *frame)
{
	struct vmm_vcpu_thread *vc = timer->data;

	(void)in_ipi;
	(void)frame;
	wakeup(vc);
}

static void
vmm_svm_lapic_timer_arm(struct vmm_svm_backend *svm, uint32_t count)
{
	_uint128_t delta;
	uint64_t now;

	svm->mut_lapic_timer_tmict = count;
	vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMICT, count);
	vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMCCT, count);
	if (count == 0 ||
	    (svm->mut_lapic_timer_lvtt & VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
	    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE) {
		svm->mut_lapic_timer_active = 0;
		return;
	}
	delta = (_uint128_t)count * svm->mut_lapic_timer_divisor *
	    svm->imm_host_tsc_hz + VMM_SVM_LAPIC_TIMER_HZ - 1;
	delta /= VMM_SVM_LAPIC_TIMER_HZ;
	if (delta == 0)
		delta = 1;
	if (delta > UINT64_MAX)
		delta = UINT64_MAX;
	now = rdtsc();
	svm->mut_lapic_timer_interval_root_tsc = (uint64_t)delta;
	if ((uint64_t)delta > UINT64_MAX - now)
		svm->mut_lapic_timer_root_deadline = UINT64_MAX;
	else
		svm->mut_lapic_timer_root_deadline = now + (uint64_t)delta;
	svm->mut_lapic_timer_active = 1;
	VMM_SVM_TRACE(svm,
	    "svm lapic timer armed count=%u root_tsc=%ju lvtt=0x%x",
	    count, (uintmax_t)svm->mut_lapic_timer_interval_root_tsc,
	    svm->mut_lapic_timer_lvtt);
}

static void
vmm_svm_lapic_timer_sync(struct vmm_svm_backend *svm)
{
	uint32_t lvtt;
	uint32_t tdcr;
	uint32_t tmict;
	uint32_t tmp1;
	uint32_t tmp2;
	uint64_t deadline;
	uint64_t guest_now;
	uint64_t now;
	_uint128_t root_delta;
	int rearm;

	rearm = 0;
	tdcr = vmm_svm_avic_apic_read32(svm, VMM_SVM_APIC_REG_TDCR) &
	    VMM_SVM_APIC_TIMER_DIVIDE_VALID;
	if (tdcr != svm->mut_lapic_timer_tdcr) {
		svm->mut_lapic_timer_tdcr = tdcr;
		tmp1 = tdcr & 0xfU;
		tmp2 = ((tmp1 & 0x3U) | ((tmp1 & 0x8U) >> 1)) + 1;
		svm->mut_lapic_timer_divisor = 1U << (tmp2 & 0x7U);
		VMM_SVM_TRACE(svm,
		    "svm avic tdcr synced value=0x%x divisor=%u",
		    tdcr, svm->mut_lapic_timer_divisor);
		rearm = 1;
	}
	lvtt = vmm_svm_avic_apic_read32(svm, VMM_SVM_APIC_REG_LVTT) &
	    VMM_SVM_APIC_LVT_TIMER_VALID;
	if (lvtt != svm->mut_lapic_timer_lvtt) {
		if ((svm->mut_lapic_timer_lvtt &
		    VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE &&
		    (lvtt & VMM_SVM_APIC_LVT_TIMER_MODE_MASK) !=
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE)
			svm->mut_lapic_timer_active = 0;
		svm->mut_lapic_timer_lvtt = lvtt;
		VMM_SVM_TRACE(svm, "svm avic lvtt synced value=0x%x", lvtt);
		rearm = 1;
		if ((lvtt & VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE)
			svm->mut_lapic_timer_active =
			    svm->mut_lapic_timer_tsc_deadline != 0;
	}
	tmict = vmm_svm_avic_apic_read32(svm, VMM_SVM_APIC_REG_TMICT);
	if ((svm->mut_lapic_timer_lvtt & VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
	    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE) {
		svm->mut_lapic_timer_tmict = 0;
		svm->mut_lapic_timer_interval_root_tsc = 0;
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMICT, 0);
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMCCT, 0);
		deadline = svm->mut_lapic_timer_tsc_deadline;
		if (!svm->mut_lapic_timer_active || deadline == 0) {
			svm->mut_lapic_timer_root_deadline = 0;
			return;
		}
		now = rdtsc();
		guest_now = (uint64_t)(((_uint128_t)now *
		    svm->imm_tsc_ratio) >> 32) +
		    svm->own_mut_vmcb->ctrl.tsc_offset;
		if (guest_now >= deadline) {
			svm->mut_lapic_timer_root_deadline = now;
			return;
		}
		root_delta = ((_uint128_t)(deadline - guest_now) << 32) +
		    svm->imm_tsc_ratio - 1;
		root_delta /= svm->imm_tsc_ratio;
		if (root_delta > UINT64_MAX - now)
			svm->mut_lapic_timer_root_deadline = UINT64_MAX;
		else
			svm->mut_lapic_timer_root_deadline = now +
			    (uint64_t)root_delta;
		return;
	}
	if (tmict != svm->mut_lapic_timer_tmict) {
		VMM_SVM_TRACE(svm, "svm avic tmict synced value=0x%x", tmict);
		vmm_svm_lapic_timer_arm(svm, tmict);
		return;
	}
	if (rearm && tmict != 0 &&
	    (svm->mut_lapic_timer_lvtt & VMM_SVM_APIC_LVT_TIMER_MODE_MASK) !=
	    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE)
		vmm_svm_lapic_timer_arm(svm, tmict);
}

static void
vmm_svm_lapic_timer_check(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc)
{
	uint32_t current;
	uint8_t vector;
	uint64_t deadline;
	uint64_t now;
	uint64_t interval;
	uint64_t remaining;
	uint64_t periods;

	if (!svm->mut_lapic_timer_active)
		return;
	deadline = svm->mut_lapic_timer_root_deadline;
	if (deadline == 0) {
		svm->mut_lapic_timer_active = 0;
		return;
	}
	now = rdtsc();
	if (now < deadline) {
		remaining = deadline - now;
		if ((svm->mut_lapic_timer_lvtt &
		    VMM_SVM_APIC_LVT_TIMER_MODE_MASK) !=
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE) {
			interval = svm->mut_lapic_timer_interval_root_tsc;
			if (interval == 0) {
				vmm_svm_avic_apic_write32(svm,
				    VMM_SVM_APIC_REG_TMCCT,
				    svm->mut_lapic_timer_tmict);
				return;
			}
			current = (uint32_t)(((_uint128_t)
			    svm->mut_lapic_timer_tmict * remaining) / interval);
			vmm_svm_avic_apic_write32(svm,
			    VMM_SVM_APIC_REG_TMCCT, current);
		}
		return;
	}
	vector = svm->mut_lapic_timer_lvtt & VMM_SVM_APIC_LVT_VECTOR_MASK;
	if ((svm->mut_lapic_timer_lvtt & VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
	    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE) {
		vector = svm->mut_lapic_timer_lvtt &
		    VMM_SVM_APIC_LVT_VECTOR_MASK;
		svm->mut_lapic_timer_tsc_deadline = 0;
		svm->mut_lapic_timer_root_deadline = 0;
		svm->mut_lapic_timer_active = 0;
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMCCT, 0);
		if ((svm->mut_lapic_timer_lvtt & VMM_SVM_APIC_LVT_MASKED) == 0) {
			svm->mut_lapic_timer_fire_count++;
			vmm_svm_avic_deliver(svm, vc, vector, "tsc_deadline");
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u tsc deadline fire vector=0x%x count=%u",
			    vc->imm_id, vector, svm->mut_lapic_timer_fire_count);
		} else {
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u tsc deadline masked vector=0x%x",
			    vc->imm_id, vector);
		}
		return;
	}
	vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMCCT, 0);
	if ((svm->mut_lapic_timer_lvtt & VMM_SVM_APIC_LVT_MASKED) == 0) {
		svm->mut_lapic_timer_fire_count++;
		vmm_svm_avic_deliver(svm, vc, vector, "lapic_timer");
		if (svm->mut_lapic_timer_fire_count <= 8 ||
		    (svm->mut_lapic_timer_fire_count & 1023U) == 0) {
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u lapic timer fire vector=0x%x count=%u",
			    vc->imm_id, vector, svm->mut_lapic_timer_fire_count);
		}
	} else if ((svm->mut_lapic_timer_lvtt &
	    VMM_SVM_APIC_LVT_TIMER_MODE_MASK) !=
	    VMM_SVM_APIC_LVT_TIMER_PERIODIC) {
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u lapic timer masked vector=0x%x",
		    vc->imm_id, vector);
	}
	if ((svm->mut_lapic_timer_lvtt & VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
	    VMM_SVM_APIC_LVT_TIMER_PERIODIC &&
	    svm->mut_lapic_timer_tmict != 0) {
		interval = svm->mut_lapic_timer_interval_root_tsc;
		if (interval == 0) {
			svm->mut_lapic_timer_active = 0;
			return;
		}
		periods = (now - deadline) / interval + 1;
		if (periods > (UINT64_MAX - deadline) / interval)
			svm->mut_lapic_timer_root_deadline = UINT64_MAX;
		else
			svm->mut_lapic_timer_root_deadline = deadline +
			    periods * interval;
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMCCT,
		    svm->mut_lapic_timer_tmict);
	} else {
		svm->mut_lapic_timer_active = 0;
	}
}

static void
vmm_svm_lapic_eoi(struct vmm_svm_backend *svm)
{
	volatile u_int *isr;
	int i;
	int bit;

	for (i = 7; i >= 0; --i) {
		isr = (volatile u_int *)((uint8_t *)svm->own_mut_avic_apic_page +
		    VMM_SVM_APIC_REG_ISR_BASE + i * 0x10);
		if (*isr == 0)
			continue;
		for (bit = 31; bit >= 0; --bit) {
			if ((*isr & (1U << bit)) != 0) {
				atomic_clear_int(isr, 1U << bit);
				break;
			}
		}
		return;
	}
}

static const char *
vmm_svm_probe(void)
{
	uint32_t descs[4];
	uint32_t apicid;
	uint32_t i;
	uint64_t msr;

	do_cpuid(0x80000000, descs);
	if (descs[0] < 0x8000000a)
		return "missing_cpuid_8000000a";
	do_cpuid(0x80000001, descs);
	if ((descs[2] & CPUID_SVM) == 0)
		return "missing_svm";
	do_cpuid(0x8000000a, descs);
	if ((descs[3] & CPUID_AMD_SVM_NP) == 0 ||
	    (descs[3] & CPUID_AMD_SVM_NRIPS) == 0 ||
	    (descs[3] & CPUID_AMD_SVM_DecodeAssist) == 0 ||
	    (descs[3] & CPUID_AMD_SVM_PauseFilter) == 0 ||
	    (descs[3] & CPUID_AMD_SVM_PFThreshold) == 0 ||
	    (descs[3] & CPUID_AMD_SVM_AVIC) == 0 ||
	    (descs[3] & CPUID_AMD_SVM_TSCRateCtrl) == 0)
		return "missing_required_feature";
	if (descs[1] < VMM_SVM_ASID)
		return "missing_asid";
	msr = rdmsr(MSR_AMD_VM_CR);
	if ((msr & VM_CR_SVMDIS) && (msr & VM_CR_LOCK))
		return "svm_disabled_locked";
	if (tsc_frequency == 0 || tsc_invariant == 0)
		return "host_tsc_not_invariant";
	if ((npx_xcr0_mask & (CPU_XFEATURE_X87 | CPU_XFEATURE_SSE)) !=
	    (CPU_XFEATURE_X87 | CPU_XFEATURE_SSE))
		return "missing_x87_sse_xstate";
	for (i = 0; i < ncpus; ++i) {
		apicid = (uint32_t)CPUID_TO_APICID(i);
		if (apicid & ~VMM_SVM_AVIC_PHYS_HOST_ID_MASK)
			return "avic_host_apic_id";
	}
	return NULL;
}

static int
vmm_svm_init(void)
{
	struct vmm_svm_cpu_state *cpu_state;
	uint32_t i;
	int error = 0;

	if (vmm_svm_initialized)
		return EALREADY;
	if (ncpus == 0 || ncpus > MAXCPU)
		return E2BIG;
	for (i = 0; i < ncpus; ++i) {
		cpu_state = &vmm_svm_cpu_state[i];
		cpu_state->own_mut_hsave = vmm_svm_contig_alloc(
		    &cpu_state->imm_hsave_pa, 1);
		if (cpu_state->own_mut_hsave == NULL) {
			error = ENOMEM;
			goto fail;
		}
	}
	lwkt_cpusync_simple(smp_active_mask, vmm_svm_cpu_capture, &error);
	if (error != 0)
		goto fail;
	lwkt_cpusync_simple(smp_active_mask, vmm_svm_cpu_enable, NULL);
	for (i = 0; i < ncpus; ++i) {
		cpu_state = &vmm_svm_cpu_state[i];
		if (cpu_state->mut_enabled) {
			kprintf("vmm: svm cpu%d enabled hsave=0x%jx\n", i,
			    (uintmax_t)cpu_state->imm_hsave_pa);
		}
	}
	vmm_svm_initialized = 1;
	kprintf("vmm: svm initialized cpus=%d\n", ncpus);
	return 0;

fail:
	for (i = 0; i < ncpus; ++i) {
		cpu_state = &vmm_svm_cpu_state[i];
		vmm_svm_contig_free(cpu_state->own_mut_hsave, 1);
		bzero(cpu_state, sizeof(*cpu_state));
	}
	return error;
}

static void
vmm_svm_uninit(void)
{
	struct vmm_svm_cpu_state *cpu_state;
	uint32_t i;

	if (!vmm_svm_initialized)
		return;
	lwkt_cpusync_simple(smp_active_mask, vmm_svm_cpu_restore, NULL);
	for (i = 0; i < ncpus; ++i) {
		cpu_state = &vmm_svm_cpu_state[i];
		if (cpu_state->mut_restored)
			kprintf("vmm: svm cpu%d restored\n", i);
	}
	for (i = 0; i < ncpus; ++i) {
		cpu_state = &vmm_svm_cpu_state[i];
		vmm_svm_contig_free(cpu_state->own_mut_hsave, 1);
		bzero(cpu_state, sizeof(*cpu_state));
	}
	vmm_svm_initialized = 0;
	kprintf("vmm: svm uninitialized cpus=%d\n", ncpus);
}

static void
vmm_svm_cpu_capture(void *arg)
{
	struct vmm_svm_cpu_state *cpu_state;
	int *error = arg;
	uint64_t vm_cr;
	uint64_t efer;
	uint64_t hsave_pa;

	if (mycpu->gd_cpuid >= ncpus || mycpu->gd_cpuid >= MAXCPU) {
		atomic_cmpset_int(error, 0, EINVAL);
		return;
	}
	cpu_state = &vmm_svm_cpu_state[mycpu->gd_cpuid];
	if (cpu_state->own_mut_hsave == NULL) {
		atomic_cmpset_int(error, 0, ENOMEM);
		return;
	}
	vm_cr = rdmsr(MSR_AMD_VM_CR);
	efer = rdmsr(MSR_EFER);
	hsave_pa = rdmsr(MSR_AMD_VM_HSAVE_PA);
	cpu_state->raw_imm_host_vm_cr = vm_cr;
	cpu_state->raw_imm_host_efer = efer;
	cpu_state->raw_imm_host_hsave_pa = hsave_pa;
	cpu_state->raw_imm_host_tsc_ratio =
	    rdmsr(VMM_SVM_MSR_AMD64_TSC_RATIO);
	if ((vm_cr & VM_CR_SVMDIS) != 0 && (vm_cr & VM_CR_LOCK) != 0) {
		atomic_cmpset_int(error, 0, ENXIO);
		return;
	}
	if ((efer & EFER_SVME) != 0 || hsave_pa != 0)
		atomic_cmpset_int(error, 0, EBUSY);
}

static void
vmm_svm_cpu_enable(void *arg)
{
	struct vmm_svm_cpu_state *cpu_state;
	uint64_t vm_cr;

	(void)arg;
	KKASSERT(mycpu->gd_cpuid < ncpus);
	cpu_state = &vmm_svm_cpu_state[mycpu->gd_cpuid];
	KKASSERT(cpu_state->own_mut_hsave != NULL);
	vm_cr = cpu_state->raw_imm_host_vm_cr;
	if ((vm_cr & VM_CR_SVMDIS) != 0)
		wrmsr(MSR_AMD_VM_CR, vm_cr & ~VM_CR_SVMDIS);
	wrmsr(MSR_EFER, cpu_state->raw_imm_host_efer | EFER_SVME);
	wrmsr(MSR_AMD_VM_HSAVE_PA, cpu_state->imm_hsave_pa);
	cpu_state->mut_enabled = 1;
}

static void
vmm_svm_cpu_restore(void *arg)
{
	struct vmm_svm_cpu_state *cpu_state;

	(void)arg;
	KKASSERT(mycpu->gd_cpuid < ncpus);
	cpu_state = &vmm_svm_cpu_state[mycpu->gd_cpuid];
	if (cpu_state->own_mut_hsave == NULL)
		return;
	if (cpu_state->mut_tsc_ratio != 0) {
		wrmsr(VMM_SVM_MSR_AMD64_TSC_RATIO,
		    cpu_state->raw_imm_host_tsc_ratio);
		cpu_state->mut_tsc_ratio = 0;
	}
	wrmsr(MSR_AMD_VM_HSAVE_PA, cpu_state->raw_imm_host_hsave_pa);
	wrmsr(MSR_EFER, cpu_state->raw_imm_host_efer);
	wrmsr(MSR_AMD_VM_CR, cpu_state->raw_imm_host_vm_cr);
	cpu_state->mut_restored = 1;
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
	vmcb->ctrl.tsc_offset = v->msr[VMM_X64_MSR_TSC] -
	    (uint64_t)(((_uint128_t)rdtsc() * svm->imm_tsc_ratio) >> 32);

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
	uint64_t ratio;
	uint64_t rate;
	uint64_t remainder;
	uint32_t i;
	int error;

	if (backendp == NULL || launch == NULL || launch->imm_vcpu0.vcpu_id != 0)
		return EINVAL;
	if (!vmm_svm_initialized)
		return ENXIO;
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
	svm->imm_host_tsc_hz = tsc_frequency;
	svm->imm_guest_tsc_hz = launch->imm_guest_tsc_hz;
	if (svm->imm_guest_tsc_hz == 0)
		svm->imm_guest_tsc_hz = svm->imm_host_tsc_hz;
	rate = svm->imm_guest_tsc_hz / svm->imm_host_tsc_hz;
	remainder = svm->imm_guest_tsc_hz % svm->imm_host_tsc_hz;
	if (rate > (VMM_SVM_TSC_RATIO_MAX >> 32)) {
		vmm_machine_logf(m,
		    "svm unavailable reason=guest_tsc_rate host_hz=%ju guest_hz=%ju",
		    (uintmax_t)svm->imm_host_tsc_hz,
		    (uintmax_t)svm->imm_guest_tsc_hz);
		error = EINVAL;
		goto fail;
	}
	/* Build floor(guest_hz * 2^32 / host_hz) without libgcc TI division. */
	ratio = rate << 32;
	for (i = 0; i < 32; ++i) {
		if (remainder >= svm->imm_host_tsc_hz - remainder) {
			ratio |= 1ULL << (31 - i);
			remainder -= svm->imm_host_tsc_hz - remainder;
		} else {
			remainder += remainder;
		}
	}
	if (ratio == 0) {
		vmm_machine_logf(m,
		    "svm unavailable reason=guest_tsc_rate host_hz=%ju guest_hz=%ju",
		    (uintmax_t)svm->imm_host_tsc_hz,
		    (uintmax_t)svm->imm_guest_tsc_hz);
		error = EINVAL;
		goto fail;
	}
	svm->imm_tsc_ratio = ratio;
	vmm_machine_logf(m,
	    "svm tsc scale host_hz=%ju guest_hz=%ju ratio=0x%jx",
	    (uintmax_t)svm->imm_host_tsc_hz,
	    (uintmax_t)svm->imm_guest_tsc_hz,
	    (uintmax_t)svm->imm_tsc_ratio);
	pmap_npt_transform(vmspace_pmap(svm->borrow_mut_vmspace), 0);
	svm->mut_hpet_counter_tsc = rdtsc();
	svm->mut_pm_timer_tsc = svm->mut_hpet_counter_tsc;
	svm->mut_guest_mtrr_def_type = MTRR_WRITE_BACK;
	svm->mut_guest_nb_cfg = NB_CFG_INITAPICCPUIDLO;
	/* Guest TSC and CPUID.15 publish one fixed P0-equivalent frequency. */
	svm->mut_guest_hwcr = VMM_SVM_HWCR_GUEST_FIXED;
	svm->mut_guest_apicbase = VMM_SVM_APICBASE_ADDR |
	    APICBASE_BSP | APICBASE_ENABLED;
	svm->mut_lapic_timer_divisor = 2;
	svm->mut_ioapic_id = VMM_IOAPIC_ID;
	for (i = 0; i < VMM_IOAPIC_PINS; i++)
		svm->mut_ioapic_redir[i] = VMM_IOAPIC_REDIR_MASKED;
	svm->mut_pic1_mask = 0xffU;
	svm->mut_pic2_mask = 0xffU;
	svm->mut_cmos_reg_a = VMM_RTC_REG_A_DEFAULT;
	svm->mut_cmos_reg_b = VMM_RTC_REG_B_24H;
	svm->mut_cmos_ram[VMM_RTC_SECONDS_ALARM] = VMM_RTC_ALARM_DONT_CARE;
	svm->mut_cmos_ram[VMM_RTC_MINUTES_ALARM] = VMM_RTC_ALARM_DONT_CARE;
	svm->mut_cmos_ram[VMM_RTC_HOURS_ALARM] = VMM_RTC_ALARM_DONT_CARE;
	vmm_svm_fpu_init(svm);
	if (vmm_svm_fpu_check_enabled) {
		if ((npx_xcr0_mask & (CPU_XFEATURE_X87 | CPU_XFEATURE_SSE |
		    CPU_XFEATURE_YMM)) != (CPU_XFEATURE_X87 |
		    CPU_XFEATURE_SSE | CPU_XFEATURE_YMM)) {
			vmm_machine_logf(m,
			    "svm unavailable reason=fpu_check_requires_avx xcr0=0x%jx",
			    (uintmax_t)npx_xcr0_mask);
			error = ENXIO;
			goto fail;
		}
		svm->own_mut_fpu_sentinel = kmalloc(
		    2 * sizeof(*svm->own_mut_fpu_sentinel), M_TEMP,
		    M_WAITOK | M_ZERO | M_POWEROF2);
		svm->own_mut_fpu_sentinel[0] = svm->mut_guest_fpu;
		svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_env.en_tw = 1;
		svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_fp[0].
		    fp_acc.fp_bytes[7] = 0x80U;
		svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_fp[0].
		    fp_acc.fp_bytes[8] = 0xffU;
		svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_fp[0].
		    fp_acc.fp_bytes[9] = 0x3fU;
		for (i = 0; i < sizeof(svm->own_mut_fpu_sentinel[0].
		    sv_ymm64.sv_xmm[0].xmm_bytes); ++i) {
			svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_xmm[0].
			    xmm_bytes[i] = 0x10U + i;
			svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_xstate.sx_ymm[0].
			    ymm_bytes[i] = 0x80U + i;
		}
	}

	svm->own_mut_vmcb = vmm_svm_contig_alloc(&svm->imm_vmcb_pa, 1);
	svm->own_mut_iobm = vmm_svm_contig_alloc(&svm->imm_iobm_pa,
	    VMM_SVM_IOBM_PAGES);
	svm->own_mut_msrbm = vmm_svm_contig_alloc(&svm->imm_msrbm_pa,
	    VMM_SVM_MSRBM_PAGES);
	if (svm->own_mut_vmcb == NULL || svm->own_mut_iobm == NULL ||
	    svm->own_mut_msrbm == NULL) {
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
	vmcb->ctrl.pause_filt_thresh = VMM_SVM_PAUSE_FILTER_THRESHOLD;
	vmcb->ctrl.pause_filt_cnt = VMM_SVM_PAUSE_FILTER_COUNT;
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
	if (svm->own_mut_fpu_sentinel != NULL)
		kfree(svm->own_mut_fpu_sentinel, M_TEMP);
	vmm_svm_contig_free(svm->own_mut_msrbm, VMM_SVM_MSRBM_PAGES);
	vmm_svm_contig_free(svm->own_mut_iobm, VMM_SVM_IOBM_PAGES);
	vmm_svm_contig_free(svm->own_mut_vmcb, 1);
	kfree(svm, M_TEMP);
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
	svm->mut_host_tsc_aux = rdmsr(MSR_TSC_AUX);
	wrmsr(MSR_TSC_AUX, svm->mut_guest_tsc_aux);
}

static void
vmm_svm_guest_misc_leave(struct vmm_svm_backend *svm)
{
	wrmsr(MSR_TSC_AUX, svm->mut_host_tsc_aux);
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
	uint32_t xstate[4];
	uint32_t leaf;
	uint32_t subleaf;
	uint32_t xsave_size;
	uint32_t xsave_max_size;

	leaf = (uint32_t)vmcb->state.rax;
	subleaf = (uint32_t)svm->mut_gprs[VMM_X64_GPR_RCX];
	bzero(regs, sizeof(regs));

	/*
	 * This is the dfvmm single-vCPU CPU template.  Do not let an unlisted
	 * CPUID leaf fall through to host CPUID: every advertised feature must
	 * have matching guest state, MSR, interrupt, and VMCB semantics.
	 */
	switch (leaf) {
	case 0:
		cpuid_count(0, 0, regs);
		regs[0] = VMM_CPUID_MAX_BASIC;
		break;
	case 1:
		cpuid_count(1, 0, regs);
		regs[1] &= CPUID_CLFUSH_SIZE;
		regs[1] |= 1U << CPUID_HTT_CORE_SHIFT;
		regs[1] &= ~VMM_CPUID_APIC_ID_MASK;
		regs[2] &= VMM_CPUID1_ECX_ALLOWED;
		regs[3] &= VMM_CPUID1_EDX_ALLOWED;
		if (npx_xcr0_mask == 0) {
			regs[2] &= ~(CPUID2_XSAVE | CPUID2_OSXSAVE |
			    CPUID2_AVX | CPUID2_FMA | CPUID2_F16C);
		} else if ((npx_xcr0_mask & CPU_XFEATURE_YMM) == 0) {
			regs[2] &= ~(CPUID2_AVX | CPUID2_FMA | CPUID2_F16C);
		}
		if ((vmcb->state.cr4 & CR4_OSXSAVE) == 0)
			regs[2] &= ~CPUID2_OSXSAVE;
		break;
	case 6:
		cpuid_count(6, 0, regs);
		regs[0] &= CPUID_THERMAL_ARAT;
		regs[1] = 0;
		regs[2] = 0;
		regs[3] = 0;
		break;
	case 7:
		if (subleaf != 0)
			break;
		cpuid_count(7, 0, regs);
		regs[0] = 0;
		regs[1] &= VMM_CPUID7_EBX_ALLOWED;
		regs[2] &= VMM_CPUID7_ECX_ALLOWED;
		regs[3] = 0;
		if ((npx_xcr0_mask & CPU_XFEATURE_YMM) == 0)
			regs[1] &= ~CPUID_STDEXT_AVX2;
		break;
	case 0x0d:
		if (npx_xcr0_mask == 0)
			break;
		switch (subleaf) {
		case 0:
			xsave_size = VMM_CPUID_XSAVE_LEGACY_SIZE;
			xsave_max_size = xsave_size;
			if ((npx_xcr0_mask & CPU_XFEATURE_YMM) != 0) {
				cpuid_count(0x0d, 2, xstate);
				xsave_max_size = xstate[1] + xstate[0];
				if ((svm->imm_guest_xcr0 & CPU_XFEATURE_YMM) != 0)
					xsave_size = xsave_max_size;
			}
			regs[0] = (uint32_t)npx_xcr0_mask;
			regs[1] = xsave_size;
			regs[2] = xsave_max_size;
			regs[3] = (uint32_t)(npx_xcr0_mask >> 32);
			break;
		case 1:
			cpuid_count(0x0d, 1, regs);
			regs[0] &= CPUID_PES1_XSAVEOPT | CPUID_PES1_XSAVEC |
			    CPUID_PES1_XGETBV;
			/*
			 * XSAVEC uses the compacted format.  With no supervisor
			 * components, its enabled-state size is the same as XSAVE's
			 * current XCR0 size from subleaf 0.
			 */
			regs[1] = VMM_CPUID_XSAVE_LEGACY_SIZE;
			if ((npx_xcr0_mask & CPU_XFEATURE_YMM) != 0 &&
			    (svm->imm_guest_xcr0 & CPU_XFEATURE_YMM) != 0) {
				cpuid_count(0x0d, 2, xstate);
				regs[1] = xstate[1] + xstate[0];
			}
			regs[2] = 0;
			regs[3] = 0;
			break;
		case 2:
			if ((npx_xcr0_mask & CPU_XFEATURE_YMM) != 0)
				cpuid_count(0x0d, 2, regs);
			break;
		default:
			break;
		}
		break;
	case 0x15:
		/*
		 * Publish the exact virtual TSC frequency.  Do not round this leaf:
		 * Linux uses it to decide whether TSC is a trustworthy clocksource.
		 * A 32-bit CPUID ratio cannot encode every possible 64-bit rate, so
		 * leave the leaf unavailable rather than report a different rate.
		 */
		if (svm->imm_guest_tsc_hz <= UINT32_MAX) {
			regs[0] = 1;
			regs[1] = (uint32_t)svm->imm_guest_tsc_hz;
			regs[2] = 1;
		} else {
			regs[0] = 0;
			regs[1] = 0;
			regs[2] = 0;
		}
		regs[3] = 0;
		break;
	case 0x16: {
		uint32_t tsc_mhz;

		tsc_mhz = (uint32_t)((svm->imm_guest_tsc_hz + 500000) / 1000000);
		regs[0] = tsc_mhz;
		regs[1] = tsc_mhz;
		regs[2] = 100;
		regs[3] = 0;
		break;
	}
	case 0x80000000U:
		cpuid_count(0x80000000U, 0, regs);
		regs[0] = VMM_CPUID_MAX_EXTENDED;
		break;
	case 0x80000001U:
		cpuid_count(0x80000001U, 0, regs);
		regs[2] &= VMM_CPUID80000001_ECX_ALLOWED;
		regs[3] &= VMM_CPUID80000001_EDX_ALLOWED;
		if ((npx_xcr0_mask & CPU_XFEATURE_YMM) == 0)
			regs[2] &= ~(CPUID_XOP | CPUID_FMA4);
		break;
	case 0x80000002U:
	case 0x80000003U:
	case 0x80000004U:
	case 0x80000005U:
	case 0x80000006U:
		cpuid_count(leaf, 0, regs);
		break;
	case 0x80000007U:
		regs[3] = CPUID_APM_ITSC;
		break;
	case 0x80000008U:
		cpuid_count(0x80000008U, 0, regs);
		regs[1] = 0;
		regs[2] = 0;
		regs[3] = 0;
		break;
	case 0x8000001dU:
		cpuid_count(0x8000001dU, subleaf, regs);
		if ((regs[0] & 0x1fU) == 0) {
			bzero(regs, sizeof(regs));
		} else {
			regs[0] &= ~0xffffc000U;
		}
		break;
	default:
		break;
	}
	vmcb->state.rax = regs[0];
	svm->mut_gprs[VMM_X64_GPR_RBX] = regs[1];
	svm->mut_gprs[VMM_X64_GPR_RCX] = regs[2];
	svm->mut_gprs[VMM_X64_GPR_RDX] = regs[3];
	vmm_svm_advance_rip(vmcb);
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
	vmcb->ctrl.intr &= ~VMM_SVM_CTRL_INTR_SHADOW;
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
	/* VMCB TSC offset is expressed in the guest's scaled TSC domain. */
	return (uint64_t)(((_uint128_t)rdtsc() * svm->imm_tsc_ratio) >> 32) +
	    svm->own_mut_vmcb->ctrl.tsc_offset;
}

static uint64_t
vmm_svm_gpr_read(struct vmm_svm_backend *svm, unsigned int reg)
{
	if (reg == VMM_X64_GPR_RAX)
		return svm->own_mut_vmcb->state.rax;
	if (reg < VMM_X64_GPR_RIP)
		return svm->mut_gprs[reg];
	return 0;
}

static void
vmm_svm_gpr_write(struct vmm_svm_backend *svm, unsigned int reg, uint64_t val,
    int size)
{
	uint64_t mask;
	uint64_t old;

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
	case 8:
		mask = 0xffffffffffffffffULL;
		break;
	default:
		return;
	}
	old = vmm_svm_gpr_read(svm, reg);
	val = (old & ~mask) | (val & mask);
	if (reg == VMM_X64_GPR_RAX)
		svm->own_mut_vmcb->state.rax = val;
	else if (reg < VMM_X64_GPR_RIP)
		svm->mut_gprs[reg] = val;
}

static int
vmm_svm_modrm_size(const uint8_t *bytes, int len, int off)
{
	uint8_t modrm;
	uint8_t mod;
	uint8_t rm;
	uint8_t sib;
	int size;

	if (off >= len)
		return 0;
	modrm = bytes[off];
	mod = modrm >> 6;
	rm = modrm & 7;
	if (mod == 3)
		return 0;
	size = 1;
	if (rm == 4) {
		if (off + size >= len)
			return 0;
		sib = bytes[off + size];
		size++;
		if (mod == 0 && (sib & 7) == 5)
			size += 4;
	}
	if (mod == 0 && rm == 5)
		size += 4;
	else if (mod == 1)
		size += 1;
	else if (mod == 2)
		size += 4;
	if (off + size > len)
		return 0;
	return size;
}

static int
vmm_svm_guest_va_canonical(uint64_t va)
{
	uint64_t high = va >> 48;

	return high == 0 || high == 0xffffULL;
}

static int
vmm_svm_guest_read_gpa(struct vmm_svm_backend *svm, uint64_t gpa, void *buf,
    size_t len)
{
	struct vmm_machine *m = svm->borrow_imm_machine;

	return vmm_mem_read_gpa(&m->own_mut_mem, gpa, buf, len);
}

static int
vmm_svm_guest_translate(struct vmm_svm_backend *svm, uint64_t va,
    uint64_t *gpap)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	uint64_t entry;
	uint64_t table;
	int error;

	if (gpap == NULL)
		return EINVAL;
	if (!vmm_svm_guest_va_canonical(va))
		return EFAULT;
	if ((vmcb->state.cr4 & CR4_LA57) != 0)
		return EOPNOTSUPP;
	table = vmcb->state.cr3 & PG_FRAME;

	error = vmm_svm_guest_read_gpa(svm,
	    table + (((va >> 39) & 0x1ffULL) * sizeof(entry)),
	    &entry, sizeof(entry));
	if (error || (entry & X86_PG_V) == 0)
		return error ? error : EFAULT;
	table = entry & PG_FRAME;

	error = vmm_svm_guest_read_gpa(svm,
	    table + (((va >> 30) & 0x1ffULL) * sizeof(entry)),
	    &entry, sizeof(entry));
	if (error || (entry & X86_PG_V) == 0)
		return error ? error : EFAULT;
	if ((entry & X86_PG_PS) != 0) {
		*gpap = (entry & 0x000fffffc0000000ULL) |
		    (va & ((1ULL << 30) - 1));
		return 0;
	}
	table = entry & PG_FRAME;

	error = vmm_svm_guest_read_gpa(svm,
	    table + (((va >> 21) & 0x1ffULL) * sizeof(entry)),
	    &entry, sizeof(entry));
	if (error || (entry & X86_PG_V) == 0)
		return error ? error : EFAULT;
	if ((entry & X86_PG_PS) != 0) {
		*gpap = (entry & PG_PS_FRAME) | (va & ((1ULL << 21) - 1));
		return 0;
	}
	table = entry & PG_FRAME;

	error = vmm_svm_guest_read_gpa(svm,
	    table + (((va >> 12) & 0x1ffULL) * sizeof(entry)),
	    &entry, sizeof(entry));
	if (error || (entry & X86_PG_V) == 0)
		return error ? error : EFAULT;
	*gpap = (entry & PG_FRAME) | (va & PAGE_MASK);
	return 0;
}

static int
vmm_svm_guest_read_va(struct vmm_svm_backend *svm, uint64_t va, void *buf,
    size_t len)
{
	uint8_t *dst = buf;
	uint64_t gpa;
	size_t chunk;
	int error;

	while (len != 0) {
		error = vmm_svm_guest_translate(svm, va, &gpa);
		if (error)
			return error;
		chunk = PAGE_SIZE - (size_t)(gpa & PAGE_MASK);
		if (chunk > len)
			chunk = len;
		error = vmm_svm_guest_read_gpa(svm, gpa, dst, chunk);
		if (error)
			return error;
		va += chunk;
		dst += chunk;
		len -= chunk;
	}
	return 0;
}

static uint64_t
vmm_svm_hpet_counter(struct vmm_svm_backend *svm)
{
	uint64_t delta;

	if ((svm->mut_hpet_config & VMM_HPET_CONFIG_ENABLE) == 0)
		return svm->mut_hpet_counter_base;
	delta = rdtsc() - svm->mut_hpet_counter_tsc;
	return svm->mut_hpet_counter_base +
	    (delta / svm->imm_host_tsc_hz) * VMM_HPET_FREQ +
	    ((delta % svm->imm_host_tsc_hz) * VMM_HPET_FREQ) /
	    svm->imm_host_tsc_hz;
}

static uint32_t
vmm_svm_pm_timer_counter(struct vmm_svm_backend *svm)
{
	uint64_t now;
	uint64_t delta;
	uint64_t ticks;
	uint32_t counter;
	uint32_t sample;

	now = rdtsc();
	delta = now - svm->mut_pm_timer_tsc;

	ticks = (delta / svm->imm_host_tsc_hz) * VMM_PM_TIMER_FREQ +
	    ((delta % svm->imm_host_tsc_hz) * VMM_PM_TIMER_FREQ) /
	    svm->imm_host_tsc_hz;
	counter = (uint32_t)ticks & VMM_PM_TIMER_MASK;
	if (vmm_svm_timing_trace_enabled &&
	    svm->mut_timing_trace_count < VMM_SVM_TIMING_TRACE_LIMIT) {
		sample = svm->mut_timing_trace_count++;
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm timing pmtimer sample=%u cpu=%d root_tsc=0x%jx pm=0x%x base_tsc=0x%jx delta=0x%jx offset=0x%jx ratio=0x%jx",
		    sample, mycpu->gd_cpuid, (uintmax_t)now, counter,
		    (uintmax_t)svm->mut_pm_timer_tsc, (uintmax_t)delta,
		    (uintmax_t)svm->own_mut_vmcb->ctrl.tsc_offset,
		    (uintmax_t)rdmsr(VMM_SVM_MSR_AMD64_TSC_RATIO));
	}
	return counter;
}

static uint64_t
vmm_svm_hpet_read64(struct vmm_svm_backend *svm, uint64_t off)
{
	uint64_t idx;
	uint64_t reg;

	switch (off) {
	case VMM_HPET_REG_CAP:
		return VMM_HPET_CAP_ID;
	case VMM_HPET_REG_CONFIG:
		return svm->mut_hpet_config;
	case VMM_HPET_REG_STATUS:
		return svm->mut_hpet_status;
	case VMM_HPET_REG_COUNTER:
		return vmm_svm_hpet_counter(svm);
	default:
		break;
	}
	if (off >= VMM_HPET_TIMER_BASE) {
		idx = (off - VMM_HPET_TIMER_BASE) / VMM_HPET_TIMER_STRIDE;
		reg = (off - VMM_HPET_TIMER_BASE) % VMM_HPET_TIMER_STRIDE;
		if (idx >= VMM_HPET_TIMER_COUNT)
			return 0;
		switch (reg) {
		case VMM_HPET_TIMER_CONFIG:
			return svm->mut_hpet_timer_config[idx] |
			    VMM_HPET_TIMER_CAP;
		case VMM_HPET_TIMER_COMPARATOR:
			return svm->mut_hpet_timer_comparator[idx];
		case VMM_HPET_TIMER_FSB:
			return 0;
		default:
			break;
		}
	}
	return 0;
}

static void
vmm_svm_hpet_write64(struct vmm_svm_backend *svm, uint64_t off, uint64_t val)
{
	uint64_t idx;
	uint64_t reg;
	uint64_t old;

	switch (off) {
	case VMM_HPET_REG_CONFIG:
		old = svm->mut_hpet_config;
		val = val & VMM_HPET_CONFIG_VALID;
		if (((old ^ val) & VMM_HPET_CONFIG_ENABLE) != 0)
			svm->mut_hpet_counter_base = vmm_svm_hpet_counter(svm);
		svm->mut_hpet_config = val;
		if ((old ^ svm->mut_hpet_config) != 0) {
			if (((old ^ svm->mut_hpet_config) &
			    VMM_HPET_CONFIG_ENABLE) != 0)
				svm->mut_hpet_counter_tsc = rdtsc();
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm hpet config=0x%jx tsc_hz=%ju hpet_hz=%ju",
			    (uintmax_t)svm->mut_hpet_config,
			    (uintmax_t)svm->imm_host_tsc_hz,
			    (uintmax_t)VMM_HPET_FREQ);
			if (vmm_svm_timing_trace_enabled &&
			    svm->mut_timing_trace_count < VMM_SVM_TIMING_TRACE_LIMIT) {
				uint32_t sample = svm->mut_timing_trace_count++;

				vmm_machine_logf(svm->borrow_imm_machine,
				    "svm timing hpet config sample=%u cpu=%d root_tsc=0x%jx config=0x%jx base=0x%jx base_tsc=0x%jx offset=0x%jx ratio=0x%jx",
				    sample, mycpu->gd_cpuid, (uintmax_t)rdtsc(),
				    (uintmax_t)svm->mut_hpet_config,
				    (uintmax_t)svm->mut_hpet_counter_base,
				    (uintmax_t)svm->mut_hpet_counter_tsc,
				    (uintmax_t)svm->own_mut_vmcb->ctrl.tsc_offset,
				    (uintmax_t)rdmsr(VMM_SVM_MSR_AMD64_TSC_RATIO));
			}
		}
		return;
	case VMM_HPET_REG_STATUS:
		svm->mut_hpet_status &= ~(uint32_t)val;
		return;
	case VMM_HPET_REG_COUNTER:
		svm->mut_hpet_counter_base = val;
		svm->mut_hpet_counter_tsc = rdtsc();
		return;
	default:
		break;
	}
	if (off >= VMM_HPET_TIMER_BASE) {
		idx = (off - VMM_HPET_TIMER_BASE) / VMM_HPET_TIMER_STRIDE;
		reg = (off - VMM_HPET_TIMER_BASE) % VMM_HPET_TIMER_STRIDE;
		if (idx >= VMM_HPET_TIMER_COUNT)
			return;
		switch (reg) {
		case VMM_HPET_TIMER_CONFIG:
			svm->mut_hpet_timer_config[idx] = val &
			    VMM_HPET_TIMER_CONFIG_VALID;
			if ((svm->mut_hpet_timer_config[idx] &
			    VMM_HPET_TIMER_ENABLE) == 0) {
				svm->mut_hpet_timer_active[idx] = 0;
				return;
			}
			if ((svm->mut_hpet_timer_config[idx] &
			    VMM_HPET_TIMER_PERIODIC) != 0) {
				if ((svm->mut_hpet_timer_config[idx] &
				    VMM_HPET_TIMER_SETVAL) != 0) {
					svm->mut_hpet_timer_period[idx] = 0;
					svm->mut_hpet_timer_active[idx] = 0;
				} else if (svm->mut_hpet_timer_comparator_set[idx] &&
				    svm->mut_hpet_timer_period[idx] != 0) {
					svm->mut_hpet_timer_active[idx] = 1;
				}
			} else if (svm->mut_hpet_timer_comparator_set[idx]) {
				svm->mut_hpet_timer_deadline[idx] =
				    svm->mut_hpet_timer_comparator[idx];
				if ((svm->mut_hpet_timer_config[idx] &
				    VMM_HPET_TIMER_32BIT) != 0)
					svm->mut_hpet_timer_deadline[idx] &= 0xffffffffULL;
				svm->mut_hpet_timer_active[idx] = 1;
			}
			return;
		case VMM_HPET_TIMER_COMPARATOR:
			if ((svm->mut_hpet_timer_config[idx] &
			    VMM_HPET_TIMER_32BIT) != 0)
				val &= 0xffffffffULL;
			svm->mut_hpet_timer_comparator[idx] = val;
			svm->mut_hpet_timer_comparator_set[idx] = 1;
			if ((svm->mut_hpet_timer_config[idx] &
			    VMM_HPET_TIMER_PERIODIC) != 0) {
				if ((svm->mut_hpet_timer_config[idx] &
				    VMM_HPET_TIMER_SETVAL) != 0) {
					svm->mut_hpet_timer_deadline[idx] = val;
					svm->mut_hpet_timer_period[idx] = 0;
					svm->mut_hpet_timer_config[idx] &=
					    ~VMM_HPET_TIMER_SETVAL;
					svm->mut_hpet_timer_active[idx] = 0;
				} else {
					svm->mut_hpet_timer_period[idx] = val;
					if ((svm->mut_hpet_timer_config[idx] &
					    VMM_HPET_TIMER_ENABLE) != 0 && val != 0)
						svm->mut_hpet_timer_active[idx] = 1;
				}
			} else if ((svm->mut_hpet_timer_config[idx] &
			    VMM_HPET_TIMER_ENABLE) != 0) {
				svm->mut_hpet_timer_deadline[idx] = val;
				svm->mut_hpet_timer_active[idx] = 1;
			}
			return;
		case VMM_HPET_TIMER_FSB:
			return;
		default:
			break;
		}
	}
}

static uint64_t
vmm_svm_hpet_read(struct vmm_svm_backend *svm, uint64_t off, int size)
{
	uint64_t val;
	unsigned int shift;

	val = vmm_svm_hpet_read64(svm, off & ~7ULL);
	shift = (unsigned int)((off & 7ULL) * 8);
	val >>= shift;
	if (size == 8)
		return val;
	if (size == 4)
		return val & 0xffffffffULL;
	if (size == 2)
		return val & 0xffffULL;
	return val & 0xffULL;
}

static void
vmm_svm_hpet_write(struct vmm_svm_backend *svm, uint64_t off, int size,
    uint64_t val)
{
	uint64_t reg;
	uint64_t old;
	uint64_t mask;
	unsigned int shift;

	reg = off & ~7ULL;
	if (size == 8) {
		vmm_svm_hpet_write64(svm, reg, val);
		return;
	}
	if (reg == VMM_HPET_REG_STATUS) {
		shift = (unsigned int)((off & 7ULL) * 8);
		mask = ((1ULL << (size * 8)) - 1) << shift;
		vmm_svm_hpet_write64(svm, reg, (val << shift) & mask);
		return;
	}
	old = vmm_svm_hpet_read64(svm, reg);
	shift = (unsigned int)((off & 7ULL) * 8);
	mask = ((1ULL << (size * 8)) - 1) << shift;
	val = (old & ~mask) | ((val << shift) & mask);
	vmm_svm_hpet_write64(svm, reg, val);
}

static int
vmm_svm_handle_hpet_mmio(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc, uint64_t gpa)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	const uint8_t *bytes = vmcb->ctrl.inst_bytes;
	uint64_t val;
	uint8_t modrm;
	uint8_t opcode;
	unsigned int reg;
	int data16 = 0;
	int off = 0;
	int rex = 0;
	int modsz;
	int size;

	if (vmcb->ctrl.inst_len == 0 ||
	    vmcb->ctrl.inst_len > sizeof(vmcb->ctrl.inst_bytes))
		goto fail;
	while (off < vmcb->ctrl.inst_len) {
		if (bytes[off] == 0x66) {
			data16 = 1;
			off++;
			continue;
		}
		if (bytes[off] >= 0x40 && bytes[off] <= 0x4f) {
			rex = bytes[off++];
			continue;
		}
		break;
	}
	if (off >= vmcb->ctrl.inst_len)
		goto fail;
	opcode = bytes[off++];
	size = (rex & 0x08) ? 8 : (data16 ? 2 : 4);
	switch (opcode) {
	case 0x8b:
		if (off >= vmcb->ctrl.inst_len)
			goto fail;
		modrm = bytes[off];
		modsz = vmm_svm_modrm_size(bytes, vmcb->ctrl.inst_len, off);
		if (modsz == 0)
			goto fail;
		reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
		val = vmm_svm_hpet_read(svm, gpa - VMM_HPET_BASE, size);
		vmm_svm_gpr_write(svm, reg, val, size);
		if (gpa - VMM_HPET_BASE == VMM_HPET_REG_COUNTER &&
		    vmm_svm_timing_trace_enabled &&
		    svm->mut_timing_trace_count < VMM_SVM_TIMING_TRACE_LIMIT) {
			uint32_t sample = svm->mut_timing_trace_count++;

			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm timing hpet counter sample=%u cpu=%d root_tsc=0x%jx hpet=0x%jx config=0x%jx base=0x%jx base_tsc=0x%jx offset=0x%jx ratio=0x%jx",
			    sample, mycpu->gd_cpuid, (uintmax_t)rdtsc(),
			    (uintmax_t)val, (uintmax_t)svm->mut_hpet_config,
			    (uintmax_t)svm->mut_hpet_counter_base,
			    (uintmax_t)svm->mut_hpet_counter_tsc,
			    (uintmax_t)vmcb->ctrl.tsc_offset,
			    (uintmax_t)rdmsr(VMM_SVM_MSR_AMD64_TSC_RATIO));
		}
		if (gpa - VMM_HPET_BASE < VMM_HPET_REG_COUNTER)
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u hpet read off=0x%jx size=%d val=0x%jx rip=0x%jx inst_len=%u nrip=0x%jx",
			    vc->imm_id, (uintmax_t)(gpa - VMM_HPET_BASE),
			    size, (uintmax_t)val, (uintmax_t)vmcb->state.rip,
			    vmcb->ctrl.inst_len, (uintmax_t)vmcb->ctrl.nrip);
		vmcb->state.rip += off + modsz;
		return 1;
	case 0x89:
		if (off >= vmcb->ctrl.inst_len)
			goto fail;
		modrm = bytes[off];
		modsz = vmm_svm_modrm_size(bytes, vmcb->ctrl.inst_len, off);
		if (modsz == 0)
			goto fail;
		reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
		val = vmm_svm_gpr_read(svm, reg);
		vmm_svm_hpet_write(svm, gpa - VMM_HPET_BASE, size, val);
		if (gpa - VMM_HPET_BASE < VMM_HPET_REG_COUNTER)
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u hpet write off=0x%jx size=%d val=0x%jx rip=0x%jx inst_len=%u nrip=0x%jx",
			    vc->imm_id, (uintmax_t)(gpa - VMM_HPET_BASE),
			    size, (uintmax_t)val, (uintmax_t)vmcb->state.rip,
			    vmcb->ctrl.inst_len, (uintmax_t)vmcb->ctrl.nrip);
		vmcb->state.rip += off + modsz;
		return 1;
	case 0xc7:
		if (off >= vmcb->ctrl.inst_len)
			goto fail;
		modrm = bytes[off];
		if (((modrm >> 3) & 7) != 0)
			goto fail;
		modsz = vmm_svm_modrm_size(bytes, vmcb->ctrl.inst_len, off);
		if (modsz == 0 || off + modsz + 4 > vmcb->ctrl.inst_len)
			goto fail;
		off += modsz;
		val = bytes[off] | ((uint64_t)bytes[off + 1] << 8) |
		    ((uint64_t)bytes[off + 2] << 16) |
		    ((uint64_t)bytes[off + 3] << 24);
		if (size == 8)
			val = (uint64_t)(int64_t)(int32_t)val;
		vmm_svm_hpet_write(svm, gpa - VMM_HPET_BASE, size, val);
		if (gpa - VMM_HPET_BASE < VMM_HPET_REG_COUNTER)
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u hpet immwrite off=0x%jx size=%d val=0x%jx rip=0x%jx inst_len=%u nrip=0x%jx",
			    vc->imm_id, (uintmax_t)(gpa - VMM_HPET_BASE),
			    size, (uintmax_t)val, (uintmax_t)vmcb->state.rip,
			    vmcb->ctrl.inst_len, (uintmax_t)vmcb->ctrl.nrip);
		vmcb->state.rip += off + 4;
		return 1;
	default:
		break;
	}
fail:
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm vcpu%u unsupported hpet mmio gpa=0x%jx info=0x%jx rip=0x%jx inst_len=%u inst0=0x%x",
	    vc->imm_id, (uintmax_t)gpa, (uintmax_t)vmcb->ctrl.exitinfo1,
	    (uintmax_t)vmcb->state.rip, vmcb->ctrl.inst_len, bytes[0]);
	return 0;
}

static int
vmm_svm_handle_fch_pm_mmio(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc, uint64_t gpa)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	const uint8_t *bytes = vmcb->ctrl.inst_bytes;
	uint8_t fetched[15];
	uint8_t modrm;
	uint8_t opcode;
	unsigned int reg;
	int data16 = 0;
	int fetched_inst = 0;
	int inst_len;
	int off = 0;
	int rex = 0;
	int modsz;
	int size;

	if (gpa != VMM_FCH_PM_BASE + VMM_FCH_PM_S5_RESET_STATUS)
		return 0;

	inst_len = vmcb->ctrl.inst_len;
	if (inst_len == 0 || inst_len > (int)sizeof(vmcb->ctrl.inst_bytes)) {
		if (vmm_svm_guest_read_va(svm, vmcb->state.rip, fetched,
		    sizeof(fetched)) != 0)
			goto fail;
		bytes = fetched;
		inst_len = (int)sizeof(fetched);
		fetched_inst = 1;
	}
	while (off < inst_len) {
		if (bytes[off] == 0x66) {
			data16 = 1;
			off++;
			continue;
		}
		if (bytes[off] >= 0x40 && bytes[off] <= 0x4f) {
			rex = bytes[off++];
			continue;
		}
		break;
	}
	if (off >= inst_len)
		goto fail;
	opcode = bytes[off++];
	size = (rex & 0x08) ? 8 : (data16 ? 2 : 4);
	if (size != 4)
		goto fail;
	switch (opcode) {
	case 0x8b:
		if (off >= inst_len)
			goto fail;
		modrm = bytes[off];
		modsz = vmm_svm_modrm_size(bytes, inst_len, off);
		if (modsz == 0)
			goto fail;
		reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
		vmm_svm_gpr_write(svm, reg, 0xffffffffU, size);
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u fch pm s5 reset status read val=0xffffffff rip=0x%jx",
		    vc->imm_id, (uintmax_t)vmcb->state.rip);
		vmcb->state.rip += off + modsz;
		return 1;
	case 0x89:
		if (off >= inst_len)
			goto fail;
		modrm = bytes[off];
		modsz = vmm_svm_modrm_size(bytes, inst_len, off);
		if (modsz == 0)
			goto fail;
		vmcb->state.rip += off + modsz;
		return 1;
	case 0xc7:
		if (off >= inst_len)
			goto fail;
		modrm = bytes[off];
		if (((modrm >> 3) & 7) != 0)
			goto fail;
		modsz = vmm_svm_modrm_size(bytes, inst_len, off);
		if (modsz == 0 || off + modsz + 4 > inst_len)
			goto fail;
		vmcb->state.rip += off + modsz + 4;
		return 1;
	default:
		break;
	}
fail:
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm vcpu%u unsupported fch pm mmio gpa=0x%jx info=0x%jx rip=0x%jx inst_len=%u inst0=0x%x fetched=%d",
	    vc->imm_id, (uintmax_t)gpa, (uintmax_t)vmcb->ctrl.exitinfo1,
	    (uintmax_t)vmcb->state.rip, vmcb->ctrl.inst_len, bytes[0],
	    fetched_inst);
	return 0;
}

static uint32_t
vmm_svm_ioapic_read(struct vmm_svm_backend *svm)
{
	uint32_t reg = svm->mut_ioapic_select;
	uint32_t pin;

	switch (reg) {
	case VMM_IOAPIC_REG_ID:
		return svm->mut_ioapic_id << 24;
	case VMM_IOAPIC_REG_VERSION:
		return VMM_IOAPIC_VERSION;
	case VMM_IOAPIC_REG_ARB:
		return svm->mut_ioapic_id << 24;
	default:
		break;
	}
	if (reg < VMM_IOAPIC_REDIR_BASE ||
	    reg >= VMM_IOAPIC_REDIR_BASE + VMM_IOAPIC_PINS * 2)
		return 0;
	pin = (reg - VMM_IOAPIC_REDIR_BASE) / 2;
	if ((reg & 1) == 0)
		return (uint32_t)svm->mut_ioapic_redir[pin];
	return (uint32_t)(svm->mut_ioapic_redir[pin] >> 32);
}

static void
vmm_svm_ioapic_write(struct vmm_svm_backend *svm, struct vmm_vcpu_thread *vc,
    uint32_t val)
{
	uint32_t reg = svm->mut_ioapic_select;
	uint32_t pin;
	uint64_t old;

	if (reg == VMM_IOAPIC_REG_ID) {
		svm->mut_ioapic_id = (val >> 24) & 0x0fU;
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u ioapic id=0x%x", vc->imm_id,
		    svm->mut_ioapic_id);
		return;
	}
	if (reg == VMM_IOAPIC_REG_VERSION || reg == VMM_IOAPIC_REG_ARB) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u ignored ioapic readonly reg=0x%x val=0x%x",
		    vc->imm_id, reg, val);
		return;
	}
	if (reg < VMM_IOAPIC_REDIR_BASE ||
	    reg >= VMM_IOAPIC_REDIR_BASE + VMM_IOAPIC_PINS * 2) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u ignored ioapic write reg=0x%x val=0x%x",
		    vc->imm_id, reg, val);
		return;
	}
	pin = (reg - VMM_IOAPIC_REDIR_BASE) / 2;
	old = svm->mut_ioapic_redir[pin];
	if ((reg & 1) == 0) {
		svm->mut_ioapic_redir[pin] =
		    (old & 0xffffffff00000000ULL) |
		    (val & VMM_IOAPIC_REDIR_LOW_VALID);
	} else {
		svm->mut_ioapic_redir[pin] =
		    (old & 0x00000000ffffffffULL) |
		    ((uint64_t)(val & VMM_IOAPIC_REDIR_HIGH_VALID) << 32);
	}
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm vcpu%u ioapic redir pin=%u value=0x%jx",
	    vc->imm_id, pin, (uintmax_t)svm->mut_ioapic_redir[pin]);
}

static void
vmm_svm_ioapic_raise(struct vmm_svm_backend *svm, struct vmm_vcpu_thread *vc,
    uint32_t pin, const char *source)
{
	uint64_t entry;
	uint32_t low;
	uint32_t high;
	uint32_t vector;
	uint32_t dest;

	if (pin >= VMM_IOAPIC_PINS) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u ioapic reject source=%s pin=%u reason=bad_pin",
		    vc->imm_id, source, pin);
		return;
	}
	entry = svm->mut_ioapic_redir[pin];
	low = (uint32_t)entry;
	high = (uint32_t)(entry >> 32);
	if ((low & VMM_IOAPIC_REDIR_MASKED) != 0) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u ioapic drop source=%s pin=%u reason=masked",
		    vc->imm_id, source, pin);
		return;
	}
	vector = low & 0xffU;
	dest = high >> 24;
	/*
	 * Initial IOAPIC delivery is intentionally narrow: fixed delivery,
	 * physical destination, active-high, edge-triggered.  Other modes need
	 * level/remote-IRR/polarity state before they can be delivered safely.
	 */
	if ((low & VMM_IOAPIC_REDIR_DELIVERY_MASK) !=
	    VMM_IOAPIC_REDIR_DELIVERY_FIXED ||
	    (low & VMM_IOAPIC_REDIR_DEST_LOGICAL) != 0 ||
	    (low & VMM_IOAPIC_REDIR_POLARITY_LOW) != 0 ||
	    (low & VMM_IOAPIC_REDIR_TRIGGER_LEVEL) != 0 ||
	    dest != VMM_SVM_AVIC_APIC_ID) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u ioapic reject source=%s pin=%u vector=0x%x low=0x%x high=0x%x",
		    vc->imm_id, source, pin, vector, low, high);
		return;
	}
	VMM_SVM_TRACE(svm,
	    "svm vcpu%u ioapic raise source=%s pin=%u vector=0x%x",
	    vc->imm_id, source, pin, vector);
	vmm_svm_avic_deliver(svm, vc, (uint8_t)vector, source);
}

static void
vmm_svm_timer_check(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc)
{
	uint64_t deadline;
	uint64_t hpet_now;
	uint64_t now;
	uint64_t period;
	uint64_t periods;
	uint64_t remaining;
	_uint128_t root_delta;
	uint32_t config;
	uint8_t rtc_causes;
	uint8_t rtc_old_reg_c;
	uint8_t rtc_rate;
	unsigned int i;

	vmm_svm_lapic_timer_check(svm, vc);
	now = rdtsc();
	rtc_causes = 0;
	for (i = 0; i < VMM_HPET_TIMER_COUNT; ++i)
		svm->mut_hpet_timer_root_deadline[i] = 0;
	if ((svm->mut_hpet_config & VMM_HPET_CONFIG_ENABLE) != 0) {
		hpet_now = vmm_svm_hpet_counter(svm);
		for (i = 0; i < VMM_HPET_TIMER_COUNT; ++i) {
			config = svm->mut_hpet_timer_config[i];
			if (!svm->mut_hpet_timer_active[i])
				continue;
			if ((config & VMM_HPET_TIMER_ENABLE) == 0) {
				svm->mut_hpet_timer_active[i] = 0;
				continue;
			}
			deadline = svm->mut_hpet_timer_deadline[i];
			if ((config & VMM_HPET_TIMER_32BIT) != 0) {
				remaining = (uint32_t)(deadline - hpet_now);
				if (remaining != 0 && remaining <= 0x7fffffffU)
					continue;
			} else if (hpet_now < deadline) {
				continue;
			}
			svm->mut_hpet_status |= 1U << i;
			if ((config & VMM_HPET_TIMER_ROUTE_MASK) ==
			    VMM_HPET_GSI << VMM_HPET_TIMER_ROUTE_SHIFT) {
				vmm_svm_ioapic_raise(svm, vc, VMM_HPET_GSI, "hpet");
			} else {
				vmm_machine_logf(svm->borrow_imm_machine,
				    "svm vcpu%u hpet drop timer=%u route=%ju reason=unsupported_route",
				    vc->imm_id, i, (uintmax_t)((config &
				    VMM_HPET_TIMER_ROUTE_MASK) >>
				    VMM_HPET_TIMER_ROUTE_SHIFT));
			}
			if ((config & VMM_HPET_TIMER_PERIODIC) == 0) {
				svm->mut_hpet_timer_active[i] = 0;
				continue;
			}
			period = svm->mut_hpet_timer_period[i];
			if (period == 0) {
				svm->mut_hpet_timer_active[i] = 0;
				continue;
			}
			if ((config & VMM_HPET_TIMER_32BIT) != 0) {
				period &= 0xffffffffULL;
				if (period == 0) {
					svm->mut_hpet_timer_active[i] = 0;
					continue;
				}
				periods = (uint32_t)(hpet_now - deadline) / period + 1;
				svm->mut_hpet_timer_deadline[i] = (uint32_t)(deadline +
				    periods * period);
			} else {
				periods = (hpet_now - deadline) / period + 1;
				if (periods > (UINT64_MAX - deadline) / period)
					svm->mut_hpet_timer_deadline[i] = UINT64_MAX;
				else
					svm->mut_hpet_timer_deadline[i] = deadline +
					    periods * period;
			}
		}
	}

	/*
	 * The MC146818 periodic output runs from the standard 32.768 kHz
	 * divider.  A rate of n produces 32768 / 2^(n - 1) Hz.  The timer
	 * remains in root TSC units; only the resulting IRQ travels through
	 * the IOAPIC and AVIC.
	 */
	rtc_rate = svm->mut_cmos_reg_a & VMM_RTC_REG_A_RATE_MASK;
	if ((svm->mut_cmos_reg_b & VMM_RTC_REG_B_SET) == 0 &&
	    (svm->mut_cmos_reg_b & VMM_RTC_REG_B_PIE) != 0 &&
	    (svm->mut_cmos_reg_a & VMM_RTC_REG_A_DIV_MASK) ==
	    VMM_RTC_REG_A_DIV_32KHZ && rtc_rate != 0) {
		period = (svm->imm_host_tsc_hz +
		    ((uint64_t)32768U >> (rtc_rate - 1)) - 1) /
		    ((uint64_t)32768U >> (rtc_rate - 1));
		if (svm->mut_cmos_periodic_root_deadline == 0) {
			svm->mut_cmos_periodic_root_deadline = now + period;
		} else if (now >= svm->mut_cmos_periodic_root_deadline) {
			periods = (now - svm->mut_cmos_periodic_root_deadline) /
			    period + 1;
			if (periods >
			    (UINT64_MAX - svm->mut_cmos_periodic_root_deadline) /
			    period)
				svm->mut_cmos_periodic_root_deadline = UINT64_MAX;
			else
				svm->mut_cmos_periodic_root_deadline += periods * period;
			rtc_causes |= VMM_RTC_REG_C_PF;
		}
	} else {
		svm->mut_cmos_periodic_root_deadline = 0;
	}

	if ((svm->mut_cmos_reg_b & VMM_RTC_REG_B_SET) == 0 &&
	    (svm->mut_cmos_reg_b & (VMM_RTC_REG_B_UIE | VMM_RTC_REG_B_AIE)) !=
	    0) {
		if (svm->mut_cmos_update_root_deadline == 0) {
			svm->mut_cmos_update_root_deadline = now + svm->imm_host_tsc_hz;
		} else if (now >= svm->mut_cmos_update_root_deadline) {
			periods = (now - svm->mut_cmos_update_root_deadline) /
			    svm->imm_host_tsc_hz + 1;
			if (periods >
			    (UINT64_MAX - svm->mut_cmos_update_root_deadline) /
			    svm->imm_host_tsc_hz)
				svm->mut_cmos_update_root_deadline = UINT64_MAX;
			else
				svm->mut_cmos_update_root_deadline += periods *
				    svm->imm_host_tsc_hz;
			svm->mut_cmos_time_expires = 0;
			vmm_svm_cmos_refresh_time(svm);
			if ((svm->mut_cmos_reg_b & VMM_RTC_REG_B_UIE) != 0)
				rtc_causes |= VMM_RTC_REG_C_UF;
			if ((svm->mut_cmos_reg_b & VMM_RTC_REG_B_AIE) != 0 &&
			    ((svm->mut_cmos_ram[VMM_RTC_SECONDS_ALARM] &
			    VMM_RTC_ALARM_DONT_CARE) == VMM_RTC_ALARM_DONT_CARE ||
			    svm->mut_cmos_ram[VMM_RTC_SECONDS_ALARM] ==
			    svm->mut_cmos_time[VMM_RTC_SECONDS]) &&
			    ((svm->mut_cmos_ram[VMM_RTC_MINUTES_ALARM] &
			    VMM_RTC_ALARM_DONT_CARE) == VMM_RTC_ALARM_DONT_CARE ||
			    svm->mut_cmos_ram[VMM_RTC_MINUTES_ALARM] ==
			    svm->mut_cmos_time[VMM_RTC_MINUTES]) &&
			    ((svm->mut_cmos_ram[VMM_RTC_HOURS_ALARM] &
			    VMM_RTC_ALARM_DONT_CARE) == VMM_RTC_ALARM_DONT_CARE ||
			    svm->mut_cmos_ram[VMM_RTC_HOURS_ALARM] ==
			    svm->mut_cmos_time[VMM_RTC_HOURS]))
				rtc_causes |= VMM_RTC_REG_C_AF;
		}
	} else {
		svm->mut_cmos_update_root_deadline = 0;
	}
	if (rtc_causes != 0) {
		rtc_old_reg_c = svm->mut_cmos_reg_c;
		svm->mut_cmos_reg_c |= rtc_causes;
		if ((svm->mut_cmos_reg_c & VMM_RTC_REG_C_CAUSE_MASK &
		    svm->mut_cmos_reg_b) != 0)
			svm->mut_cmos_reg_c |= VMM_RTC_REG_C_IRQF;
		if ((rtc_old_reg_c & VMM_RTC_REG_C_IRQF) == 0 &&
		    (svm->mut_cmos_reg_c & VMM_RTC_REG_C_IRQF) != 0)
			vmm_svm_ioapic_raise(svm, vc, VMM_RTC_IRQ, "rtc");
	}

	svm->mut_root_timer_deadline = 0;
	if (svm->mut_lapic_timer_active &&
	    svm->mut_lapic_timer_root_deadline != 0)
		svm->mut_root_timer_deadline = svm->mut_lapic_timer_root_deadline;
	if ((svm->mut_hpet_config & VMM_HPET_CONFIG_ENABLE) != 0) {
		hpet_now = vmm_svm_hpet_counter(svm);
		now = rdtsc();
		for (i = 0; i < VMM_HPET_TIMER_COUNT; ++i) {
			config = svm->mut_hpet_timer_config[i];
			if (!svm->mut_hpet_timer_active[i] ||
			    (config & VMM_HPET_TIMER_ENABLE) == 0)
				continue;
			deadline = svm->mut_hpet_timer_deadline[i];
			if ((config & VMM_HPET_TIMER_32BIT) != 0)
				remaining = (uint32_t)(deadline - hpet_now);
			else if (hpet_now < deadline)
				remaining = deadline - hpet_now;
			else
				remaining = 0;
			if (remaining == 0 ||
			    ((config & VMM_HPET_TIMER_32BIT) != 0 &&
			    remaining > 0x7fffffffU)) {
				svm->mut_hpet_timer_root_deadline[i] = now;
			} else {
				root_delta = (_uint128_t)remaining * svm->imm_host_tsc_hz +
				    VMM_HPET_FREQ - 1;
				root_delta /= VMM_HPET_FREQ;
				if (root_delta == 0)
					root_delta = 1;
				if (root_delta > UINT64_MAX - now)
					svm->mut_hpet_timer_root_deadline[i] = UINT64_MAX;
				else
					svm->mut_hpet_timer_root_deadline[i] = now +
					    (uint64_t)root_delta;
			}
			if (svm->mut_root_timer_deadline == 0 ||
			    svm->mut_hpet_timer_root_deadline[i] <
			    svm->mut_root_timer_deadline) {
				svm->mut_root_timer_deadline =
				    svm->mut_hpet_timer_root_deadline[i];
			}
		}
	}
	if (svm->mut_cmos_periodic_root_deadline != 0 &&
	    (svm->mut_root_timer_deadline == 0 ||
	    svm->mut_cmos_periodic_root_deadline < svm->mut_root_timer_deadline))
		svm->mut_root_timer_deadline = svm->mut_cmos_periodic_root_deadline;
	if (svm->mut_cmos_update_root_deadline != 0 &&
	    (svm->mut_root_timer_deadline == 0 ||
	    svm->mut_cmos_update_root_deadline < svm->mut_root_timer_deadline))
		svm->mut_root_timer_deadline = svm->mut_cmos_update_root_deadline;
}

static int
vmm_svm_handle_ioapic_mmio(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc, uint64_t gpa)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	const uint8_t *bytes = vmcb->ctrl.inst_bytes;
	uint64_t val;
	uint8_t modrm;
	uint8_t opcode;
	unsigned int reg;
	int data16 = 0;
	int off = 0;
	int rex = 0;
	int modsz;
	int size;

	if (vmcb->ctrl.inst_len == 0 ||
	    vmcb->ctrl.inst_len > sizeof(vmcb->ctrl.inst_bytes))
		goto fail;
	while (off < vmcb->ctrl.inst_len) {
		if (bytes[off] == 0x66) {
			data16 = 1;
			off++;
			continue;
		}
		if (bytes[off] >= 0x40 && bytes[off] <= 0x4f) {
			rex = bytes[off++];
			continue;
		}
		break;
	}
	if (off >= vmcb->ctrl.inst_len)
		goto fail;
	opcode = bytes[off++];
	size = (rex & 0x08) ? 8 : (data16 ? 2 : 4);
	if (size != 4)
		goto fail;
	switch (opcode) {
	case 0x8b:
		if (off >= vmcb->ctrl.inst_len)
			goto fail;
		modrm = bytes[off];
		modsz = vmm_svm_modrm_size(bytes, vmcb->ctrl.inst_len, off);
		if (modsz == 0)
			goto fail;
		reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
		if (gpa == VMM_IOAPIC_BASE) {
			val = svm->mut_ioapic_select;
		} else if (gpa == VMM_IOAPIC_BASE + 0x10) {
			val = vmm_svm_ioapic_read(svm);
		} else {
			goto fail;
		}
		vmm_svm_gpr_write(svm, reg, val, size);
		vmcb->state.rip += off + modsz;
		return 1;
	case 0x89:
		if (off >= vmcb->ctrl.inst_len)
			goto fail;
		modrm = bytes[off];
		modsz = vmm_svm_modrm_size(bytes, vmcb->ctrl.inst_len, off);
		if (modsz == 0)
			goto fail;
		reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
		val = vmm_svm_gpr_read(svm, reg);
		if (gpa == VMM_IOAPIC_BASE) {
			svm->mut_ioapic_select = val & 0xffU;
		} else if (gpa == VMM_IOAPIC_BASE + 0x10) {
			vmm_svm_ioapic_write(svm, vc, (uint32_t)val);
		} else {
			goto fail;
		}
		vmcb->state.rip += off + modsz;
		return 1;
	case 0xc7:
		if (off >= vmcb->ctrl.inst_len)
			goto fail;
		modrm = bytes[off];
		if (((modrm >> 3) & 7) != 0)
			goto fail;
		modsz = vmm_svm_modrm_size(bytes, vmcb->ctrl.inst_len, off);
		if (modsz == 0 || off + modsz + 4 > vmcb->ctrl.inst_len)
			goto fail;
		off += modsz;
		val = bytes[off] | ((uint64_t)bytes[off + 1] << 8) |
		    ((uint64_t)bytes[off + 2] << 16) |
		    ((uint64_t)bytes[off + 3] << 24);
		if (gpa == VMM_IOAPIC_BASE) {
			svm->mut_ioapic_select = val & 0xffU;
		} else if (gpa == VMM_IOAPIC_BASE + 0x10) {
			vmm_svm_ioapic_write(svm, vc, (uint32_t)val);
		} else {
			goto fail;
		}
		vmcb->state.rip += off + 4;
		return 1;
	default:
		break;
	}
fail:
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm vcpu%u unsupported ioapic mmio gpa=0x%jx info=0x%jx rip=0x%jx inst_len=%u inst0=0x%x",
	    vc->imm_id, (uintmax_t)gpa, (uintmax_t)vmcb->ctrl.exitinfo1,
	    (uintmax_t)vmcb->state.rip, vmcb->ctrl.inst_len, bytes[0]);
	return 0;
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

static int
vmm_svm_amd_pmu_msr_index(uint32_t msr, uint32_t base, unsigned int *idxp)
{
	uint32_t off = msr - base;

	if (off >= VMM_SVM_PMU_COUNTERS * 2 || (off & 1U) != 0)
		return 0;
	*idxp = off / 2;
	return 1;
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
	unsigned int idx;
	uint64_t old_tsc;
	uint64_t val;

	if (vmcb->ctrl.exitinfo1 == 0) {
		if (vmm_svm_amd_pmu_msr_index(msr, VMM_SVM_MSR_F15H_PERF_CTL,
		    &idx)) {
			vmm_svm_rdmsr_value(svm, svm->mut_guest_pmu_ctl[idx]);
			return 1;
		}
		if (vmm_svm_amd_pmu_msr_index(msr, VMM_SVM_MSR_F15H_PERF_CTR,
		    &idx)) {
			vmm_svm_rdmsr_value(svm, svm->mut_guest_pmu_ctr[idx]);
			return 1;
		}
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
			vmm_svm_rdmsr_value(svm,
			    svm->mut_lapic_timer_tsc_deadline);
			return 1;
		case MSR_APICBASE:
			vmm_svm_rdmsr_value(svm,
			    svm->mut_guest_apicbase);
			return 1;
		case MSR_SPEC_CTRL:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_spec_ctrl);
			return 1;
		case MSR_PRED_CMD:
			vmm_svm_rdmsr_value(svm, 0);
			return 1;
		case MSR_IA32_ARCH_CAPABILITIES:
			vmm_svm_rdmsr_value(svm, VMM_SVM_ARCH_CAPABILITIES);
			return 1;
		case MSR_AMD_VM_CR:
			vmm_svm_rdmsr_value(svm, VM_CR_SVMDIS | VM_CR_LOCK);
			return 1;
		case MSR_MTRRcap:
			vmm_svm_rdmsr_value(svm, 0);
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
		case VMM_SVM_MSR_AMD64_DE_CFG:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_de_cfg);
			return 1;
		case VMM_SVM_MSR_ZEN4_BP_CFG:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_zen4_bp_cfg);
			return 1;
		case VMM_SVM_MSR_ZEN2_SPECTRAL_CHICKEN:
			vmm_svm_rdmsr_value(svm,
			    svm->mut_guest_zen2_spectral_chicken);
			return 1;
		case VMM_SVM_MSR_AMD64_IRPERF:
			vmm_svm_rdmsr_value(svm, vmm_svm_guest_tsc(svm));
			return 1;
		case VMM_SVM_MSR_IA32_MPERF:
			vmm_svm_rdmsr_value(svm, vmm_svm_guest_tsc(svm) +
			    svm->mut_guest_mperf_offset);
			return 1;
		case VMM_SVM_MSR_IA32_APERF:
			vmm_svm_rdmsr_value(svm, vmm_svm_guest_tsc(svm) +
			    svm->mut_guest_aperf_offset);
			return 1;
		case VMM_SVM_MSR_AMD64_IBSCTL:
			vmm_svm_rdmsr_value(svm, 0);
			return 1;
		case MSR_AMD_NB_CFG:
			vmm_svm_rdmsr_value(svm, svm->mut_guest_nb_cfg);
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
			vmcb->ctrl.eventinj = VMM_SVM_EVENTINJ_VALID |
			    VMM_SVM_EVENTINJ_ERROR_VALID |
			    VMM_SVM_EVENTINJ_TYPE_EXCEPTION |
			    VMM_X86_EXCEPTION_GP;
			return 1;
		}
	}

	val = vmm_svm_wrmsr_value(svm);
	if (vmm_svm_amd_pmu_msr_index(msr, VMM_SVM_MSR_F15H_PERF_CTL,
	    &idx)) {
		svm->mut_guest_pmu_ctl[idx] = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	}
	if (vmm_svm_amd_pmu_msr_index(msr, VMM_SVM_MSR_F15H_PERF_CTR,
	    &idx)) {
		svm->mut_guest_pmu_ctr[idx] = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	}
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
		old_tsc = vmm_svm_guest_tsc(svm);
		vmcb->ctrl.tsc_offset = val -
		    (uint64_t)(((_uint128_t)rdtsc() * svm->imm_tsc_ratio) >> 32);
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u tsc write old=0x%jx new=0x%jx offset=0x%jx cpu=%d",
		    vc->imm_id, (uintmax_t)old_tsc, (uintmax_t)val,
		    (uintmax_t)vmcb->ctrl.tsc_offset, mycpu->gd_cpuid);
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_TSC_AUX:
		if (val > UINT32_MAX) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			vmcb->ctrl.eventinj = VMM_SVM_EVENTINJ_VALID |
			    VMM_SVM_EVENTINJ_ERROR_VALID |
			    VMM_SVM_EVENTINJ_TYPE_EXCEPTION |
			    VMM_X86_EXCEPTION_GP;
			return 1;
		}
		svm->mut_guest_tsc_aux = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_TSC_DEADLINE:
		svm->mut_lapic_timer_tsc_deadline = val;
		if ((svm->mut_lapic_timer_lvtt &
		    VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE) {
			svm->mut_lapic_timer_tmict = 0;
			svm->mut_lapic_timer_interval_root_tsc = 0;
			svm->mut_lapic_timer_root_deadline = 0;
			svm->mut_lapic_timer_active = val != 0;
			vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMICT, 0);
			vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMCCT, 0);
		}
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u tsc deadline value=0x%jx active=%d",
		    vc->imm_id, (uintmax_t)val, svm->mut_lapic_timer_active);
		vmm_svm_advance_rip(vmcb);
		return 1;
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
	case MSR_SPEC_CTRL:
		if ((val & ~VMM_SVM_SPEC_CTRL_VALID) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		svm->mut_guest_spec_ctrl = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_PRED_CMD:
		if ((val & ~VMM_SVM_PRED_CMD_VALID) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_IA32_ARCH_CAPABILITIES:
		vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
		    "read-only");
		return 0;
	case MSR_AMD_VM_CR:
		vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
		    "read-only");
		return 0;
	case MSR_MTRRcap:
		vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
		    "read-only");
		return 0;
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
		svm->mut_guest_hwcr =
		    (val & ~VMM_SVM_HWCR_GUEST_FIXED) |
		    VMM_SVM_HWCR_GUEST_FIXED;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case VMM_SVM_MSR_AMD64_DE_CFG:
		if ((val & ~VMM_SVM_DE_CFG_VALID) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		svm->mut_guest_de_cfg = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case VMM_SVM_MSR_ZEN4_BP_CFG:
		if ((val & ~VMM_SVM_ZEN4_BP_CFG_VALID) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		svm->mut_guest_zen4_bp_cfg = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case VMM_SVM_MSR_ZEN2_SPECTRAL_CHICKEN:
		if ((val & ~VMM_SVM_ZEN2_SPECTRAL_CHICKEN_VALID) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		svm->mut_guest_zen2_spectral_chicken = val;
		vmm_svm_advance_rip(vmcb);
		return 1;
	case VMM_SVM_MSR_AMD64_IRPERF:
		vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
		    "read-only");
		return 0;
	case VMM_SVM_MSR_IA32_MPERF:
		svm->mut_guest_mperf_offset = val - vmm_svm_guest_tsc(svm);
		vmm_svm_advance_rip(vmcb);
		return 1;
	case VMM_SVM_MSR_IA32_APERF:
		svm->mut_guest_aperf_offset = val - vmm_svm_guest_tsc(svm);
		vmm_svm_advance_rip(vmcb);
		return 1;
	case VMM_SVM_MSR_AMD64_IBSCTL:
		if (val != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "ibs-hidden");
			return 0;
		}
		vmm_svm_advance_rip(vmcb);
		return 1;
	case MSR_AMD_NB_CFG:
		if ((val & ~VMM_SVM_NB_CFG_VALID) != 0) {
			vmm_svm_log_unsupported_msr(svm, vc, "wr", msr, val, 1,
			    "invalid-value");
			return 0;
		}
		svm->mut_guest_nb_cfg = val;
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
		vmcb->ctrl.eventinj = VMM_SVM_EVENTINJ_VALID |
		    VMM_SVM_EVENTINJ_ERROR_VALID |
		    VMM_SVM_EVENTINJ_TYPE_EXCEPTION | VMM_X86_EXCEPTION_GP;
		return 1;
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
	    (hvmflags & ~RQF_TIMER) == 0)
		return;
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
	VMM_SVM_TRACE(svm,
	    "svm vcpu%u root %s exit info1=0x%jx info2=0x%jx "
	    "hvmflags=0x%x reqflags=0x%x rip=0x%jx",
	    vc->imm_id, name, (uintmax_t)vmcb->ctrl.exitinfo1,
	    (uintmax_t)vmcb->ctrl.exitinfo2, hvmflags, reqflags,
	    (uintmax_t)vmcb->state.rip);
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

static void
vmm_svm_com1_rx_notify(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc, const char *source)
{
	struct vmm_console *console = &svm->borrow_imm_machine->own_mut_console;

	if ((svm->mut_com1_ier & VMM_COM1_IER_RDI) == 0 ||
	    vmm_console_guest_pending(console) == 0)
		return;
	if ((svm->mut_com1_mcr & VMM_COM1_MCR_OUT2) == 0) {
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u com1 rx irq held source=%s reason=out2_disabled",
		    vc->imm_id, source);
		return;
	}
	if (svm->mut_com1_rx_irq_pending == 0) {
		svm->mut_com1_rx_irq_pending = 1;
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u com1 rx irq source=%s", vc->imm_id, source);
	}
	vmm_svm_ioapic_raise(svm, vc, VMM_COM1_IOAPIC_PIN, source);
}

static void
vmm_svm_com1_tx_notify(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc, const char *source)
{
	if ((svm->mut_com1_ier & VMM_COM1_IER_THRI) == 0 ||
	    svm->mut_com1_thr_irq_pending == 0)
		return;
	if ((svm->mut_com1_mcr & VMM_COM1_MCR_OUT2) == 0) {
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u com1 tx irq held source=%s reason=out2_disabled",
		    vc->imm_id, source);
		return;
	}
	VMM_SVM_TRACE(svm,
	    "svm vcpu%u com1 tx irq source=%s", vc->imm_id, source);
	vmm_svm_ioapic_raise(svm, vc, VMM_COM1_IOAPIC_PIN, source);
}

static int
vmm_svm_com1_read(struct vmm_svm_backend *svm, struct vmm_vcpu_thread *vc,
    unsigned int reg, int size, uint32_t *valp)
{
	struct vmm_console *console = &svm->borrow_imm_machine->own_mut_console;
	char ch;
	uint32_t lsr;

	if (size != 1)
		return 0;
	switch (reg) {
	case VMM_COM1_RBR_THR_DLL:
		if ((svm->mut_com1_lcr & VMM_COM1_LCR_DLAB) != 0) {
			*valp = svm->mut_com1_dll;
		} else if (vmm_console_guest_read(console, &ch)) {
			volatile u_int *irr;
			uint8_t vector;

			*valp = (uint8_t)ch;
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u com1 read rbr val=0x%x pending=%zu",
			    vc->imm_id, *valp, vmm_console_guest_pending(console));
			vector = (uint8_t)svm->mut_ioapic_redir[VMM_COM1_IOAPIC_PIN];
			irr = (volatile u_int *)
			    ((uint8_t *)svm->own_mut_avic_apic_page +
			    VMM_SVM_APIC_REG_IRR_BASE + (vector / 32) * 0x10);
			atomic_clear_int(irr, 1U << (vector & 31));
			cpu_mfence();
			svm->mut_com1_rx_irq_pending = 0;
			vmm_svm_com1_rx_notify(svm, vc, "com1_rbr");
		} else {
			*valp = 0;
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u com1 read rbr empty", vc->imm_id);
		}
		return 1;
	case VMM_COM1_IER_DLM:
		*valp = (svm->mut_com1_lcr & VMM_COM1_LCR_DLAB) ?
		    svm->mut_com1_dlm : svm->mut_com1_ier;
		return 1;
	case VMM_COM1_IIR_FCR:
		if ((svm->mut_com1_ier & VMM_COM1_IER_RDI) != 0 &&
		    vmm_console_guest_pending(console) != 0) {
			*valp = VMM_COM1_IIR_RDI;
		} else if ((svm->mut_com1_ier & VMM_COM1_IER_THRI) != 0 &&
		    svm->mut_com1_thr_irq_pending != 0) {
			volatile u_int *irr;
			uint8_t vector;

			*valp = VMM_COM1_IIR_THRI;
			svm->mut_com1_thr_irq_pending = 0;
			vector = (uint8_t)svm->mut_ioapic_redir[VMM_COM1_IOAPIC_PIN];
			irr = (volatile u_int *)
			    ((uint8_t *)svm->own_mut_avic_apic_page +
			    VMM_SVM_APIC_REG_IRR_BASE + (vector / 32) * 0x10);
			atomic_clear_int(irr, 1U << (vector & 31));
			cpu_mfence();
		} else {
			*valp = VMM_COM1_IIR_NOPEND;
		}
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u com1 read iir val=0x%x pending=%zu ier=0x%x rx_pending=%d thr_pending=%d",
		    vc->imm_id, *valp, vmm_console_guest_pending(console),
		    svm->mut_com1_ier, svm->mut_com1_rx_irq_pending,
		    svm->mut_com1_thr_irq_pending);
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
		if (svm->mut_com1_lsr_overrun) {
			lsr |= VMM_COM1_LSR_OE;
			svm->mut_com1_lsr_overrun = 0;
		}
		*valp = lsr;
		return 1;
	case VMM_COM1_MSR:
		if ((svm->mut_com1_mcr & VMM_COM1_MCR_LOOP) != 0) {
			*valp = 0;
			if ((svm->mut_com1_mcr & VMM_COM1_MCR_RTS) != 0)
				*valp |= VMM_COM1_MSR_CTS;
			if ((svm->mut_com1_mcr & VMM_COM1_MCR_DTR) != 0)
				*valp |= VMM_COM1_MSR_DSR;
			if ((svm->mut_com1_mcr & VMM_COM1_MCR_OUT1) != 0)
				*valp |= VMM_COM1_MSR_RI;
			if ((svm->mut_com1_mcr & VMM_COM1_MCR_OUT2) != 0)
				*valp |= VMM_COM1_MSR_DCD;
		} else {
			*valp = VMM_COM1_MSR_CTS | VMM_COM1_MSR_DSR |
			    VMM_COM1_MSR_DCD;
		}
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u com1 read msr val=0x%x mcr=0x%x",
		    vc->imm_id, *valp, svm->mut_com1_mcr);
		return 1;
	case VMM_COM1_SCR:
		*valp = svm->mut_com1_scr;
		return 1;
	default:
		return 0;
	}
}

static int
vmm_svm_com1_write(struct vmm_svm_backend *svm, struct vmm_vcpu_thread *vc,
    unsigned int reg, int size, uint32_t val)
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
			svm->mut_com1_thr_irq_pending = 1;
			vmm_svm_com1_tx_notify(svm, vc, "com1_thr");
		} else {
			svm->mut_com1_dll = val & 0xffU;
		}
		return 1;
	case VMM_COM1_IER_DLM:
		if ((svm->mut_com1_lcr & VMM_COM1_LCR_DLAB) == 0) {
			svm->mut_com1_ier = val & 0x0fU;
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u com1 write ier val=0x%x pending=%zu",
			    vc->imm_id, svm->mut_com1_ier,
			    vmm_console_guest_pending(
			    &svm->borrow_imm_machine->own_mut_console));
			if ((svm->mut_com1_ier & VMM_COM1_IER_RDI) == 0)
				svm->mut_com1_rx_irq_pending = 0;
			if ((svm->mut_com1_ier & VMM_COM1_IER_THRI) != 0)
				svm->mut_com1_thr_irq_pending = 1;
			vmm_svm_com1_rx_notify(svm, vc, "com1_ier");
			vmm_svm_com1_tx_notify(svm, vc, "com1_ier");
		} else {
			svm->mut_com1_dlm = val & 0xffU;
		}
		return 1;
	case VMM_COM1_IIR_FCR:
		svm->mut_com1_fcr = val & VMM_COM1_FCR_ENABLE;
		if ((val & VMM_COM1_FCR_RX_RESET) != 0) {
			volatile u_int *irr;
			uint8_t vector;

			vmm_console_guest_reset_input(
			    &svm->borrow_imm_machine->own_mut_console);
			vector = (uint8_t)svm->mut_ioapic_redir[VMM_COM1_IOAPIC_PIN];
			irr = (volatile u_int *)
			    ((uint8_t *)svm->own_mut_avic_apic_page +
			    VMM_SVM_APIC_REG_IRR_BASE + (vector / 32) * 0x10);
			atomic_clear_int(irr, 1U << (vector & 31));
			cpu_mfence();
			svm->mut_com1_rx_irq_pending = 0;
			svm->mut_com1_lsr_overrun = 0;
		}
		if ((val & VMM_COM1_FCR_TX_RESET) != 0)
			svm->mut_com1_thr_irq_pending = 0;
		vmm_svm_com1_rx_notify(svm, vc, "com1_fcr");
		return 1;
	case VMM_COM1_LCR:
		svm->mut_com1_lcr = val & 0xffU;
		return 1;
	case VMM_COM1_MCR:
		svm->mut_com1_mcr = val & 0xffU;
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u com1 write mcr val=0x%x pending=%zu",
		    vc->imm_id, svm->mut_com1_mcr,
		    vmm_console_guest_pending(
		    &svm->borrow_imm_machine->own_mut_console));
		if ((svm->mut_com1_mcr & VMM_COM1_MCR_OUT2) == 0)
			svm->mut_com1_rx_irq_pending = 0;
		vmm_svm_com1_rx_notify(svm, vc, "com1_mcr");
		vmm_svm_com1_tx_notify(svm, vc, "com1_mcr");
		return 1;
	case VMM_COM1_SCR:
		svm->mut_com1_scr = val & 0xffU;
		return 1;
	default:
		return 0;
	}
}

static void
vmm_svm_cmos_refresh_time(struct vmm_svm_backend *svm)
{
	static const int days_in_month[12] =
	    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	time_t now;
	long days;
	long epoch_days;
	int second;
	int minute;
	int hours;
	int year;
	int month;
	int dim;
	int binary;

	if ((svm->mut_cmos_reg_b & VMM_RTC_REG_B_SET) != 0)
		return;
	if (svm->mut_cmos_time_expires != 0 &&
	    (int)(ticks - svm->mut_cmos_time_expires) < 0)
		return;
	now = time_second + svm->mut_cmos_time_offset;
	if (now < 0)
		now = 0;
	days = now / 86400;
	epoch_days = days;
	second = now % 60;
	minute = (now / 60) % 60;
	hours = (now / 3600) % 24;
	year = 1970;
	while (days >= 365 + VMM_RTC_LEAP_YEAR(year)) {
		days -= 365 + VMM_RTC_LEAP_YEAR(year);
		year++;
	}
	for (month = 0; month < 12; month++) {
		dim = days_in_month[month];
		if (month == 1 && VMM_RTC_LEAP_YEAR(year))
			dim++;
		if (days < dim)
			break;
		days -= dim;
	}
	binary = (svm->mut_cmos_reg_b & VMM_RTC_REG_B_DM_BINARY) != 0;
	if ((svm->mut_cmos_reg_b & VMM_RTC_REG_B_24H) == 0) {
		int pm = hours >= 12;

		hours %= 12;
		if (hours == 0)
			hours = 12;
		if (pm)
			hours |= 0x80;
	}
	svm->mut_cmos_time[VMM_RTC_SECONDS] = binary ? second : bin2bcd(second);
	svm->mut_cmos_time[VMM_RTC_MINUTES] = binary ? minute : bin2bcd(minute);
	svm->mut_cmos_time[VMM_RTC_HOURS] = binary ? hours :
	    ((hours & 0x80) | bin2bcd(hours & 0x7f));
	svm->mut_cmos_time[VMM_RTC_DAY_OF_WEEK] = binary ?
	    ((epoch_days + 4) % 7) + 1 : bin2bcd(((epoch_days + 4) % 7) + 1);
	svm->mut_cmos_time[VMM_RTC_DAY_OF_MONTH] = binary ? days + 1 :
	    bin2bcd(days + 1);
	svm->mut_cmos_time[VMM_RTC_MONTH] = binary ? month + 1 :
	    bin2bcd(month + 1);
	svm->mut_cmos_time[VMM_RTC_YEAR] = binary ? year % 100 :
	    bin2bcd(year % 100);
	svm->mut_cmos_time_expires = ticks + hz;
}

static int
vmm_svm_cmos_value_decode(uint8_t value, int maximum, int binary,
    int *valuep)
{
	int decoded;

	if (binary) {
		decoded = value;
	} else {
		if ((value & 0x0fU) > 9 || ((value >> 4) & 0x0fU) > 9)
			return EINVAL;
		decoded = bcd2bin(value);
	}
	if (decoded > maximum)
		return EINVAL;
	*valuep = decoded;
	return 0;
}

static uint8_t
vmm_svm_cmos_read(struct vmm_svm_backend *svm)
{
	uint8_t reg = svm->mut_cmos_index & 0x7fU;
	uint8_t value;

	switch (reg) {
	case VMM_RTC_SECONDS:
	case VMM_RTC_MINUTES:
	case VMM_RTC_HOURS:
	case VMM_RTC_DAY_OF_WEEK:
	case VMM_RTC_DAY_OF_MONTH:
	case VMM_RTC_MONTH:
	case VMM_RTC_YEAR:
		vmm_svm_cmos_refresh_time(svm);
		value = svm->mut_cmos_time[reg];
		break;
	case VMM_RTC_REG_A:
		value = svm->mut_cmos_reg_a & ~VMM_RTC_REG_A_UIP;
		break;
	case VMM_RTC_REG_B:
		value = svm->mut_cmos_reg_b;
		break;
	case VMM_RTC_REG_C:
		value = svm->mut_cmos_reg_c;
		svm->mut_cmos_reg_c = 0;
		break;
	case VMM_RTC_REG_D:
		value = VMM_RTC_REG_D_VALID;
		break;
	default:
		value = svm->mut_cmos_ram[reg];
		break;
	}
	return value;
}

static void
vmm_svm_cmos_write(struct vmm_svm_backend *svm, uint8_t value)
{
	static const int days_in_month[12] =
	    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	uint8_t reg = svm->mut_cmos_index & 0x7fU;
	uint8_t old_reg_b;
	time_t now;
	long days;
	int second;
	int minute;
	int hour;
	int day;
	int month;
	int year;
	int current_year;
	int current_month;
	int dim;
	int binary;
	int pm;

	switch (reg) {
	case VMM_RTC_REG_A:
		svm->mut_cmos_reg_a = value & ~VMM_RTC_REG_A_UIP;
		svm->mut_cmos_periodic_root_deadline = 0;
		break;
	case VMM_RTC_REG_B:
		old_reg_b = svm->mut_cmos_reg_b;
		if ((old_reg_b & VMM_RTC_REG_B_SET) == 0 &&
		    (value & VMM_RTC_REG_B_SET) != 0)
			vmm_svm_cmos_refresh_time(svm);
		svm->mut_cmos_reg_b = value;
		if ((old_reg_b & VMM_RTC_REG_B_SET) != 0 &&
		    (value & VMM_RTC_REG_B_SET) == 0) {
			binary = (value & VMM_RTC_REG_B_DM_BINARY) != 0;
			if (vmm_svm_cmos_value_decode(
			    svm->mut_cmos_time[VMM_RTC_SECONDS], 59, binary,
			    &second) == 0 &&
			    vmm_svm_cmos_value_decode(
			    svm->mut_cmos_time[VMM_RTC_MINUTES], 59, binary,
			    &minute) == 0 &&
			    vmm_svm_cmos_value_decode(
			    svm->mut_cmos_time[VMM_RTC_DAY_OF_MONTH], 31, binary,
			    &day) == 0 &&
			    vmm_svm_cmos_value_decode(
			    svm->mut_cmos_time[VMM_RTC_MONTH], 12, binary,
			    &month) == 0 && month != 0 &&
			    vmm_svm_cmos_value_decode(
			    svm->mut_cmos_time[VMM_RTC_YEAR], 99, binary,
			    &year) == 0) {
				pm = svm->mut_cmos_time[VMM_RTC_HOURS] & 0x80U;
				if ((value & VMM_RTC_REG_B_24H) == 0)
					pm = pm != 0;
				else
					pm = 0;
				if (vmm_svm_cmos_value_decode(
				    svm->mut_cmos_time[VMM_RTC_HOURS] & 0x7fU,
				    (value & VMM_RTC_REG_B_24H) != 0 ? 23 : 12,
				    binary, &hour) == 0 &&
				    ((value & VMM_RTC_REG_B_24H) != 0 || hour != 0)) {
					if ((value & VMM_RTC_REG_B_24H) == 0) {
						hour %= 12;
						if (pm)
							hour += 12;
					}
					now = time_second + svm->mut_cmos_time_offset;
					if (now < 0)
						now = 0;
					days = now / 86400;
					current_year = 1970;
					while (days >= 365 +
					    VMM_RTC_LEAP_YEAR(current_year)) {
						days -= 365 +
						    VMM_RTC_LEAP_YEAR(current_year);
						current_year++;
					}
					year += (current_year / 100) * 100;
					dim = days_in_month[month - 1];
					if (month == 2 && VMM_RTC_LEAP_YEAR(year))
						dim++;
					if (day != 0 && day <= dim) {
						days = 0;
						for (current_year = 1970;
						    current_year < year; current_year++)
							days += 365 +
							    VMM_RTC_LEAP_YEAR(current_year);
						for (current_month = 1;
						    current_month < month; current_month++) {
							dim = days_in_month[current_month - 1];
							if (current_month == 2 &&
							    VMM_RTC_LEAP_YEAR(year))
								dim++;
							days += dim;
						}
						days += day - 1;
						svm->mut_cmos_time_offset =
						    days * 86400 + hour * 3600 +
						    minute * 60 + second - time_second;
					}
				}
			}
		}
		svm->mut_cmos_time_expires = 0;
		svm->mut_cmos_periodic_root_deadline = 0;
		svm->mut_cmos_update_root_deadline = 0;
		break;
	case VMM_RTC_REG_C:
	case VMM_RTC_REG_D:
		break;
	case VMM_RTC_SECONDS:
	case VMM_RTC_MINUTES:
	case VMM_RTC_HOURS:
	case VMM_RTC_DAY_OF_WEEK:
	case VMM_RTC_DAY_OF_MONTH:
	case VMM_RTC_MONTH:
	case VMM_RTC_YEAR:
		if ((svm->mut_cmos_reg_b & VMM_RTC_REG_B_SET) != 0)
			svm->mut_cmos_time[reg] = value;
		break;
	default:
		svm->mut_cmos_ram[reg] = value;
		break;
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
	uint64_t elapsed;
	uint64_t pit_ticks;
	uint32_t pit_count;
	uint32_t val;

	if (size == 0 || (info & (VMM_SVM_IOIO_STR | VMM_SVM_IOIO_REP)) != 0) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u unsupported ioio op=%s port=0x%x size=%d info=0x%jx rip=0x%jx",
		    vc->imm_id, op, port, size, (uintmax_t)info,
		    (uintmax_t)vmcb->state.rip);
		return 0;
	}

	switch (port) {
	case VMM_ACPI_RESET_PORT:
		if (size != 1 || (info & VMM_SVM_IOIO_IN) != 0) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported acpi reset io op=%s size=%d rip=0x%jx",
			    vc->imm_id, op, size, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		val = vmcb->state.rax & 0xffU;
		if (val != VMM_ACPI_RESET_VALUE) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported acpi reset value=0x%x rip=0x%jx",
			    vc->imm_id, val, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		vmm_svm_advance_ioio(vmcb);
		svm->mut_exit_reason = VMM_VCPU_EXIT_GUEST_RESET;
		vmm_machine_logf(svm->borrow_imm_machine,
		    "guest reset source=acpi_fadt vcpu=%u", vc->imm_id);
		return 1;
	case VMM_ACPI_SLEEP_CONTROL_PORT:
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported acpi sleep-control io op=%s size=%d rip=0x%jx",
			    vc->imm_id, op, size, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN) {
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u acpi sleep-control io op=in size=%d rip=0x%jx",
			    vc->imm_id, size, (uintmax_t)vmcb->state.rip);
			vmm_svm_set_rax_low(vmcb, 0, size);
			vmm_svm_advance_ioio(vmcb);
			return 1;
		}
		val = vmcb->state.rax & 0xffU;
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u acpi sleep-control io op=out size=%d value=0x%x rip=0x%jx",
		    vc->imm_id, size, val, (uintmax_t)vmcb->state.rip);
		if (val != VMM_ACPI_SLEEP_S5_ENABLE) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported acpi sleep-control value=0x%x rip=0x%jx",
			    vc->imm_id, val, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		vmm_svm_advance_ioio(vmcb);
		svm->mut_exit_reason = VMM_VCPU_EXIT_GUEST_SHUTDOWN;
		vmm_machine_logf(svm->borrow_imm_machine,
		    "guest shutdown source=acpi_s5 vcpu=%u", vc->imm_id);
		return 1;
	case VMM_ACPI_SLEEP_STATUS_PORT:
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported acpi sleep-status io op=%s size=%d rip=0x%jx",
			    vc->imm_id, op, size, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN) {
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u acpi sleep-status io op=in size=%d rip=0x%jx",
			    vc->imm_id, size, (uintmax_t)vmcb->state.rip);
			vmm_svm_set_rax_low(vmcb, 0, size);
		} else {
			val = vmcb->state.rax & 0xffU;
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u acpi sleep-status io op=out size=%d value=0x%x rip=0x%jx",
			    vc->imm_id, size, val, (uintmax_t)vmcb->state.rip);
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	case VMM_PIC1_CMD:
	case VMM_PIC2_CMD:
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pic cmd io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size,
			    (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN)
			vmm_svm_set_rax_low(vmcb, 0, size);
		vmm_svm_advance_ioio(vmcb);
		return 1;
	case VMM_PIC1_DATA:
	case VMM_PIC2_DATA:
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pic data io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size,
			    (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN) {
			vmm_svm_set_rax_low(vmcb,
			    port == VMM_PIC1_DATA ? svm->mut_pic1_mask :
			    svm->mut_pic2_mask, size);
		} else if (port == VMM_PIC1_DATA) {
			svm->mut_pic1_mask = vmcb->state.rax & 0xffU;
		} else {
			svm->mut_pic2_mask = vmcb->state.rax & 0xffU;
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	case VMM_PIC_ELCR1:
	case VMM_PIC_ELCR2:
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pic elcr io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size,
			    (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN) {
			vmm_svm_set_rax_low(vmcb,
			    port == VMM_PIC_ELCR1 ? svm->mut_pic_elcr1 :
			    svm->mut_pic_elcr2, size);
		} else if (port == VMM_PIC_ELCR1) {
			svm->mut_pic_elcr1 = vmcb->state.rax &
			    VMM_PIC_ELCR1_MASK;
		} else {
			svm->mut_pic_elcr2 = vmcb->state.rax &
			    VMM_PIC_ELCR2_MASK;
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	default:
		break;
	}

	if (port == VMM_PIT_PORTB) {
		uint8_t old_gate;
		uint8_t new_gate;

		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pit portb io op=%s size=%d rip=0x%jx",
			    vc->imm_id, op, size, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN) {
			val = svm->mut_pit_portb;
			if (svm->mut_pit_ch2_armed == 0 ||
			    (svm->mut_pit_portb & VMM_PIT_PORTB_GATE2) == 0) {
				val |= VMM_PIT_PORTB_OUT2;
			} else {
				elapsed = rdtsc() - svm->mut_pit_ch2_start_tsc;
				pit_ticks = (elapsed / svm->imm_host_tsc_hz) *
				    VMM_PIT_FREQ + ((elapsed % svm->imm_host_tsc_hz) *
				    VMM_PIT_FREQ) / svm->imm_host_tsc_hz;
				pit_count = svm->mut_pit_ch2_reload != 0 ?
				    svm->mut_pit_ch2_reload : 0x10000U;
				if (pit_ticks >= pit_count)
					val |= VMM_PIT_PORTB_OUT2;
			}
			vmm_svm_set_rax_low(vmcb, val, size);
		} else {
			old_gate = svm->mut_pit_portb & VMM_PIT_PORTB_GATE2;
			svm->mut_pit_portb = vmcb->state.rax & 0x03U;
			new_gate = svm->mut_pit_portb & VMM_PIT_PORTB_GATE2;
			if (old_gate == 0 && new_gate != 0 &&
			    svm->mut_pit_ch2_armed != 0)
				svm->mut_pit_ch2_start_tsc = rdtsc();
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}
	if (port == VMM_PIT_CH0) {
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pit ch0 io op=%s size=%d rip=0x%jx",
			    vc->imm_id, op, size, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN) {
			if (svm->mut_pit_ch0_read_state == 0) {
				val = svm->mut_pit_ch0_count & 0xffU;
				svm->mut_pit_ch0_read_state = 1;
			} else {
				val = (svm->mut_pit_ch0_count >> 8) & 0xffU;
				svm->mut_pit_ch0_read_state = 0;
			}
			vmm_svm_set_rax_low(vmcb, val, size);
		} else if (svm->mut_pit_ch0_write_state == 0) {
			svm->mut_pit_ch0_reload &= 0xff00U;
			svm->mut_pit_ch0_reload |= vmcb->state.rax & 0xffU;
			svm->mut_pit_ch0_write_state = 1;
		} else {
			svm->mut_pit_ch0_reload &= 0x00ffU;
			svm->mut_pit_ch0_reload |=
			    (vmcb->state.rax & 0xffU) << 8;
			svm->mut_pit_ch0_count = svm->mut_pit_ch0_reload;
			svm->mut_pit_ch0_write_state = 0;
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}
	if (port == VMM_PIT_CMD) {
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pit io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN)
			vmm_svm_set_rax_low(vmcb, 0xffU, size);
		else {
			val = vmcb->state.rax & 0xffU;
			if ((val & 0xc0U) == 0) {
				if ((val & 0x30U) != 0 && (val & 0x30U) != 0x30U) {
					vmm_machine_logf(svm->borrow_imm_machine,
					    "svm vcpu%u unsupported pit ch0 cmd=0x%x rip=0x%jx",
					    vc->imm_id, val,
					    (uintmax_t)vmcb->state.rip);
					return 0;
				}
				svm->mut_pit_ch0_read_state = 0;
				svm->mut_pit_ch0_write_state = 0;
			} else if ((val & 0xc0U) == 0x80U) {
				if (val != 0xb0U) {
					vmm_machine_logf(svm->borrow_imm_machine,
					    "svm vcpu%u unsupported pit ch2 cmd=0x%x rip=0x%jx",
					    vc->imm_id, val,
					    (uintmax_t)vmcb->state.rip);
					return 0;
				}
				svm->mut_pit_ch2_read_state = 0;
				svm->mut_pit_ch2_write_state = 0;
				svm->mut_pit_ch2_armed = 0;
			} else {
				vmm_machine_logf(svm->borrow_imm_machine,
				    "svm vcpu%u unsupported pit cmd=0x%x rip=0x%jx",
				    vc->imm_id, val,
				    (uintmax_t)vmcb->state.rip);
				return 0;
			}
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}
	if (port == VMM_PIT_CH2) {
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pit io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN) {
			pit_count = svm->mut_pit_ch2_reload != 0 ?
			    svm->mut_pit_ch2_reload : 0x10000U;
			if (svm->mut_pit_ch2_armed != 0 &&
			    (svm->mut_pit_portb & VMM_PIT_PORTB_GATE2) != 0) {
				elapsed = rdtsc() - svm->mut_pit_ch2_start_tsc;
				pit_ticks = (elapsed / svm->imm_host_tsc_hz) *
				    VMM_PIT_FREQ + ((elapsed % svm->imm_host_tsc_hz) *
				    VMM_PIT_FREQ) / svm->imm_host_tsc_hz;
				if (pit_ticks >= pit_count)
					pit_count = 0;
				else
					pit_count -= (uint32_t)pit_ticks;
			}
			if (svm->mut_pit_ch2_read_state == 0) {
				val = pit_count & 0xffU;
				svm->mut_pit_ch2_read_state = 1;
			} else {
				val = (pit_count >> 8) & 0xffU;
				svm->mut_pit_ch2_read_state = 0;
			}
			vmm_svm_set_rax_low(vmcb, val, size);
		} else if (svm->mut_pit_ch2_write_state == 0) {
			svm->mut_pit_ch2_reload &= 0xff00U;
			svm->mut_pit_ch2_reload |= vmcb->state.rax & 0xffU;
			svm->mut_pit_ch2_write_state = 1;
		} else {
			svm->mut_pit_ch2_reload &= 0x00ffU;
			svm->mut_pit_ch2_reload |=
			    (vmcb->state.rax & 0xffU) << 8;
			svm->mut_pit_ch2_write_state = 0;
			svm->mut_pit_ch2_armed = 1;
			if (svm->mut_pit_portb & VMM_PIT_PORTB_GATE2)
				svm->mut_pit_ch2_start_tsc = rdtsc();
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}

	if (port >= VMM_PM_TIMER_PORT && port <= VMM_PM_TIMER_LAST) {
		if (port + (unsigned int)size - 1 > VMM_PM_TIMER_LAST) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pmtimer io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size,
			    (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN) {
			val = vmm_svm_pm_timer_counter(svm) >>
			    ((port - VMM_PM_TIMER_PORT) * 8);
			vmm_svm_set_rax_low(vmcb, val, size);
		} else {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u ignored pmtimer write port=0x%x size=%d val=0x%x rip=0x%jx",
			    vc->imm_id, port, size,
			    (uint32_t)vmcb->state.rax,
			    (uintmax_t)vmcb->state.rip);
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}

	if (port == VMM_CMOS_INDEX || port == VMM_CMOS_DATA) {
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported cmos io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size,
			    (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (port == VMM_CMOS_INDEX) {
			if (info & VMM_SVM_IOIO_IN) {
				val = svm->mut_cmos_index |
				    svm->mut_cmos_nmi_disabled;
				vmm_svm_set_rax_low(vmcb, val, size);
			} else {
				val = vmcb->state.rax & 0xffU;
				svm->mut_cmos_index = val & 0x7fU;
				svm->mut_cmos_nmi_disabled = val & 0x80U;
			}
		} else if (info & VMM_SVM_IOIO_IN) {
			vmm_svm_set_rax_low(vmcb, vmm_svm_cmos_read(svm),
			    size);
		} else {
			vmm_svm_cmos_write(svm, vmcb->state.rax & 0xffU);
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}

	if (port >= VMM_ISA_MISC_PORT && port <= VMM_ISA_MISC_LAST) {
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported isa misc io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size,
			    (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN)
			vmm_svm_set_rax_low(vmcb, 0, size);
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}

	if (port == VMM_PCI_CFG_CTRL) {
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pci cfg ctrl io op=%s size=%d rip=0x%jx",
			    vc->imm_id, op, size, (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN)
			vmm_svm_set_rax_low(vmcb, 0, size);
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}
	if (port >= VMM_PCI_CFG_ADDR && port <= VMM_PCI_CFG_ADDR_LAST) {
		unsigned int shift;
		uint32_t mask;

		if (port + (unsigned int)size - 1 > VMM_PCI_CFG_ADDR_LAST) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pci cfg addr io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size,
			    (uintmax_t)vmcb->state.rip);
			return 0;
		}
		shift = (port - VMM_PCI_CFG_ADDR) * 8U;
		mask = size == 4 ? 0xffffffffU : ((1U << (size * 8)) - 1U);
		if (info & VMM_SVM_IOIO_IN) {
			if (port == VMM_PCI_CFG_TYPE2_FORWARD && size == 1)
				vmm_svm_set_rax_low(vmcb, 0xffU, size);
			else
				vmm_svm_set_rax_low(vmcb,
				    svm->mut_pci_cfg_addr >> shift, size);
		} else {
			svm->mut_pci_cfg_addr &= ~(mask << shift);
			svm->mut_pci_cfg_addr |=
			    ((uint32_t)vmcb->state.rax & mask) << shift;
		}
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}
	if (port >= VMM_PCI_CFG_DATA && port <= VMM_PCI_CFG_DATA_LAST) {
		if (port + (unsigned int)size - 1 > VMM_PCI_CFG_DATA_LAST) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported pci cfg data io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size,
			    (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN)
			vmm_svm_set_rax_low(vmcb, 0xffffffffU, size);
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}

	if ((port >= VMM_COM2_BASE && port <= VMM_COM2_BASE + VMM_COM1_SCR) ||
	    (port >= VMM_COM3_BASE && port <= VMM_COM3_BASE + VMM_COM1_SCR) ||
	    (port >= VMM_COM4_BASE && port <= VMM_COM4_BASE + VMM_COM1_SCR)) {
		if (size != 1) {
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u unsupported absent serial io op=%s port=0x%x size=%d rip=0x%jx",
			    vc->imm_id, op, port, size,
			    (uintmax_t)vmcb->state.rip);
			return 0;
		}
		if (info & VMM_SVM_IOIO_IN)
			vmm_svm_set_rax_low(vmcb, 0xffU, size);
		vmm_svm_advance_ioio(vmcb);
		return 1;
	}

	if (port < VMM_COM1_BASE || port > VMM_COM1_BASE + VMM_COM1_SCR) {
		vmm_machine_logf(svm->borrow_imm_machine,
		    "svm vcpu%u unsupported ioio op=%s port=0x%x size=%d rip=0x%jx",
		    vc->imm_id, op, port, size, (uintmax_t)vmcb->state.rip);
		return 0;
	}

	if (info & VMM_SVM_IOIO_IN) {
		if (!vmm_svm_com1_read(svm, vc, port - VMM_COM1_BASE, size,
		    &val))
			return 0;
		vmm_svm_set_rax_low(vmcb, val, size);
	} else {
		val = vmcb->state.rax & 0xffffffffU;
		if (!vmm_svm_com1_write(svm, vc, port - VMM_COM1_BASE, size,
		    val))
			return 0;
	}
	vmm_svm_advance_ioio(vmcb);
	return 1;
}

static void
vmm_svm_console_input(void *backend, struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_backend *svm = backend;
	struct vmm_console *console;

	if (svm == NULL)
		return;

	/*
	 * This callback runs in the host tty writer's thread.  It is only a
	 * poke path; COM1/IOAPIC state is owned by the vCPU thread and is
	 * converted into AVIC IRR delivery before the next VMRUN.
	 */
	console = &svm->borrow_imm_machine->own_mut_console;
	VMM_SVM_TRACE(svm, "svm vcpu%u console input poke pending=%zu",
	    vc->imm_id, vmm_console_guest_pending(console));
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

	if ((vmcb->state.cr4 & CR4_OSXSAVE) == 0) {
		vmcb->ctrl.eventinj = VMM_SVM_EVENTINJ_VALID |
		    VMM_SVM_EVENTINJ_TYPE_EXCEPTION | VMM_X86_EXCEPTION_UD;
		VMM_SVM_TRACE(svm,
		    "svm vcpu0 inject ud reason=xsetbv-osxsave rip=0x%jx",
		    (uintmax_t)vmcb->state.rip);
		return 1;
	}
	xcr0 = (svm->mut_gprs[VMM_X64_GPR_RDX] << 32) |
	    (vmcb->state.rax & 0xffffffffULL);
	if ((uint32_t)svm->mut_gprs[VMM_X64_GPR_RCX] != 0 ||
	    vmcb->state.cpl != 0 || !vmm_loader_x86_xcr0_valid(xcr0) ||
	    (xcr0 & ~npx_xcr0_mask) != 0) {
		vmcb->ctrl.eventinj = VMM_SVM_EVENTINJ_VALID |
		    VMM_SVM_EVENTINJ_ERROR_VALID |
		    VMM_SVM_EVENTINJ_TYPE_EXCEPTION | VMM_X86_EXCEPTION_GP;
		VMM_SVM_TRACE(svm,
		    "svm vcpu0 inject gp reason=xsetbv-invalid rip=0x%jx",
		    (uintmax_t)vmcb->state.rip);
		return 1;
	}
	svm->imm_guest_xcr0 = xcr0;
	vmm_svm_advance_rip(vmcb);
	return 1;
}

static void
vmm_svm_handle_idle_wait(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	struct vmm_console *console = &svm->borrow_imm_machine->own_mut_console;
	uint64_t deadline;
	uint64_t now;
	unsigned int i;

	vmm_svm_advance_rip(vmcb);
	/* Guest AVIC writes are visible in the access page without a VMEXIT. */
	vmm_svm_lapic_timer_sync(svm);
	vmm_svm_timer_check(svm, vc);
	for (;;) {
		if (vmm_vcpu_should_stop(vc))
			return;

		/*
		 * The FIFO producer writes while holding token_console and wakes vc
		 * after dropping it.  Queue before the final predicate check so an
		 * input byte cannot be lost between the check and tsleep().
		 */
		lwkt_gettoken(&console->token_console);
		tsleep_interlock(vc, 0);
		if (vmm_vcpu_should_stop(vc) ||
		    ((svm->mut_com1_ier & VMM_COM1_IER_RDI) != 0 &&
		    (svm->mut_com1_mcr & VMM_COM1_MCR_OUT2) != 0 &&
		    console->mut_input_len != 0)) {
			lwkt_reltoken(&console->token_console);
			break;
		}
		lwkt_reltoken(&console->token_console);

		if (svm->mut_root_timer_deadline != 0) {
			deadline = svm->mut_root_timer_deadline;
			now = rdtsc();
			if (now >= deadline)
				break;
		}
		svm->mut_lapic_timer_idle_count++;
		if (svm->mut_lapic_timer_idle_count <= 8 ||
		    (svm->mut_lapic_timer_idle_count & 1023U) == 0) {
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u idle wait timer_active=%d",
			    vc->imm_id, svm->mut_root_timer_deadline != 0);
		}
		if (svm->mut_root_timer_deadline != 0) {
			uint64_t delta;
			_uint128_t us;

			KKASSERT(!svm->mut_root_timer_systimer_armed);
			now = rdtsc();
			if (svm->mut_root_timer_deadline <= now)
				delta = 1;
			else
				delta = svm->mut_root_timer_deadline - now;
			us = (_uint128_t)delta * 1000000ULL +
			    svm->imm_host_tsc_hz - 1;
			us /= svm->imm_host_tsc_hz;
			if (us == 0)
				us = 1;
			if (us > VMM_SVM_ROOT_TIMER_MAX_US)
				us = VMM_SVM_ROOT_TIMER_MAX_US;
			systimer_init_oneshot(&svm->own_mut_root_timer_systimer,
			    vmm_svm_root_timer_systimer, vc, (int64_t)us);
			svm->mut_root_timer_systimer_armed = 1;
		}
		vmm_svm_avic_unbind_cpu(svm);
		/*
		 * Pair IsRunning=0 with a final IRR scan.  An interrupt posted
		 * before HLT must cause the next VMRUN, not an indefinite sleep.
		 */
		for (i = 0; i < 8; ++i) {
			if (vmm_svm_avic_apic_read32(svm,
			    VMM_SVM_APIC_REG_IRR_BASE + i * 0x10) != 0) {
				if (svm->mut_root_timer_systimer_armed) {
					systimer_del(&svm->own_mut_root_timer_systimer);
					svm->mut_root_timer_systimer_armed = 0;
				}
				return;
			}
		}
		tsleep(vc, PINTERLOCKED, "vmmhlt", 0);
		if (svm->mut_root_timer_systimer_armed) {
			systimer_del(&svm->own_mut_root_timer_systimer);
			svm->mut_root_timer_systimer_armed = 0;
		}
	}
	vmm_svm_timer_check(svm, vc);
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
	case VMM_SVM_SMOKE_IOAPIC_RAISE:
		vmm_machine_logf(svm->borrow_imm_machine,
		    "smoke ioapic request pin=%u", arg);
		vmm_svm_advance_rip(vmcb);
		vmm_svm_ioapic_raise(svm, vc, arg, "ioapic_smoke");
		return 1;
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
	case VMM_SVM_SMOKE_PAUSE_FILTER:
		vmm_machine_logf(svm->borrow_imm_machine,
		    "smoke pause filter exits=%u", svm->mut_pause_exit_count);
		return 0;
	case VMM_SVM_SMOKE_CPU_TEMPLATE_MARKER:
		vmm_machine_logf(svm->borrow_imm_machine,
		    "smoke cpu template marker=0x%x", arg);
		return 0;
	case VMM_SVM_SMOKE_FPU_MARKER:
		vmm_machine_logf(svm->borrow_imm_machine,
		    "smoke fpu marker=0x%x", arg);
		return 0;
	default:
		vmm_machine_logf(svm->borrow_imm_machine,
		    "smoke avic unknown op=%u arg=0x%x", op, arg);
		return -1;
	}
}

static int
vmm_svm_handle_avic_read(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc, uint32_t apic_reg)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	const uint8_t *bytes = vmcb->ctrl.inst_bytes;
	uint8_t fetched[15];
	uint32_t value;
	uint8_t modrm;
	uint8_t opcode;
	unsigned int reg;
	int data16 = 0;
	int fetched_inst = 0;
	int inst_len;
	int off = 0;
	int rex = 0;
	int insn_len;
	int modsz;
	int size;

	if (apic_reg == VMM_SVM_APIC_REG_TMCCT) {
		uint64_t now;
		uint64_t remaining;

		now = rdtsc();
		if ((svm->mut_lapic_timer_lvtt &
		    VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE ||
		    !svm->mut_lapic_timer_active ||
		    svm->mut_lapic_timer_interval_root_tsc == 0 ||
		    svm->mut_lapic_timer_root_deadline <= now) {
			value = 0;
		} else {
			remaining = svm->mut_lapic_timer_root_deadline - now;
			value = (uint32_t)
			    (((_uint128_t)svm->mut_lapic_timer_tmict * remaining) /
			    svm->mut_lapic_timer_interval_root_tsc);
		}
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TMCCT,
		    value);
	} else {
		value = vmm_svm_avic_apic_read32(svm, apic_reg);
	}
	inst_len = vmcb->ctrl.inst_len;
	if (inst_len == 0 || inst_len > (int)sizeof(vmcb->ctrl.inst_bytes)) {
		if (vmm_svm_guest_read_va(svm, vmcb->state.rip, fetched,
		    sizeof(fetched)) != 0)
			goto fail;
		bytes = fetched;
		inst_len = (int)sizeof(fetched);
		fetched_inst = 1;
	}
	while (off < inst_len) {
		if (bytes[off] == 0x66) {
			data16 = 1;
			off++;
			continue;
		}
		if (bytes[off] >= 0x40 && bytes[off] <= 0x4f) {
			rex = bytes[off++];
			continue;
		}
		break;
	}
	if (off >= inst_len)
		goto fail;
	opcode = bytes[off++];
	size = (rex & 0x08) ? 8 : (data16 ? 2 : 4);
	if (opcode != 0x8b || size != 4 || off >= inst_len)
		goto fail;
	modrm = bytes[off];
	modsz = vmm_svm_modrm_size(bytes, inst_len, off);
	if (modsz == 0)
		goto fail;
	reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
	vmm_svm_gpr_write(svm, reg, value, size);
	insn_len = off + modsz;
	if (fetched_inst && vmcb->ctrl.nrip == 0)
		vmcb->state.rip += insn_len;
	else
		vmm_svm_advance_rip(vmcb);
	return 1;
fail:
	vmm_machine_logf(svm->borrow_imm_machine,
	    "svm vcpu%u unsupported avic read offset=0x%x rip=0x%jx inst_len=%u inst0=0x%x fetched=%d",
	    vc->imm_id, apic_reg, (uintmax_t)vmcb->state.rip,
	    vmcb->ctrl.inst_len, bytes[0], fetched_inst);
	return 0;
}

static int
vmm_svm_handle_avic_exit(struct vmm_svm_backend *svm,
    struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_vmcb *vmcb = svm->own_mut_vmcb;
	const char *name;
	volatile uint32_t *ptr;
	uint32_t value;
	uint32_t extra;
	uint32_t tmp1;
	uint32_t tmp2;

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
	case VMM_SVM_APIC_REG_EOI:
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
	case VMM_SVM_APIC_REG_ESR:
		name = "esr";
		break;
	case 0x300:
		name = "icr";
		break;
	case VMM_SVM_APIC_REG_LVTT:
		name = "lvtt";
		break;
	case VMM_SVM_APIC_REG_LVT_THERMAL:
		name = "lvt_thermal";
		break;
	case VMM_SVM_APIC_REG_LVT_PC:
		name = "lvt_pc";
		break;
	case VMM_SVM_APIC_REG_LVT0:
		name = "lvt0";
		break;
	case VMM_SVM_APIC_REG_LVT1:
		name = "lvt1";
		break;
	case VMM_SVM_APIC_REG_LVT_ERROR:
		name = "lvt_error";
		break;
	case VMM_SVM_APIC_REG_TMICT:
		name = "tmict";
		break;
	case VMM_SVM_APIC_REG_TMCCT:
		name = "tmcct";
		break;
	case VMM_SVM_APIC_REG_TDCR:
		name = "tdcr";
		break;
	default:
		name = "unknown";
		break;
	}
	if (extra != 0 || value != VMM_SVM_APIC_REG_TMCCT) {
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic noaccel reg=%s offset=0x%x write=%u vector=0x%x info1=0x%jx info2=0x%jx rip=0x%jx",
		    vc->imm_id, name, value, extra,
		    (uint32_t)(vmcb->ctrl.exitinfo2 &
		    VMM_SVM_AVIC_UNACCEL_ACCESS_VECTOR_MASK),
		    (uintmax_t)vmcb->ctrl.exitinfo1,
		    (uintmax_t)vmcb->ctrl.exitinfo2,
		    (uintmax_t)vmcb->state.rip);
	}
	ptr = (volatile uint32_t *)
	    ((uint8_t *)svm->own_mut_avic_apic_page + value);
	if (extra == 0) {
		switch (value) {
		case VMM_SVM_APIC_REG_ID:
		case VMM_SVM_APIC_REG_SVR:
		case VMM_SVM_APIC_REG_ESR:
		case VMM_SVM_APIC_REG_LVTT:
		case VMM_SVM_APIC_REG_LVT_THERMAL:
		case VMM_SVM_APIC_REG_LVT_PC:
		case VMM_SVM_APIC_REG_LVT0:
		case VMM_SVM_APIC_REG_LVT1:
		case VMM_SVM_APIC_REG_LVT_ERROR:
		case VMM_SVM_APIC_REG_TMICT:
		case VMM_SVM_APIC_REG_TMCCT:
		case VMM_SVM_APIC_REG_TDCR:
			return vmm_svm_handle_avic_read(svm, vc, value);
		default:
			break;
		}
		return 0;
	}
	switch (value) {
	case VMM_SVM_APIC_REG_LVT_ERROR:
		value = *ptr;
		value &= VMM_SVM_APIC_LVT_ERROR_VALID;
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_LVT_ERROR,
		    value);
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic lvt_error accepted value=0x%x",
		    vc->imm_id, value);
		return 1;
	case VMM_SVM_APIC_REG_LVTT:
		value = *ptr;
		value &= VMM_SVM_APIC_LVT_TIMER_VALID;
		if ((svm->mut_lapic_timer_lvtt &
		    VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE &&
		    (value & VMM_SVM_APIC_LVT_TIMER_MODE_MASK) !=
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE)
			svm->mut_lapic_timer_active = 0;
		svm->mut_lapic_timer_lvtt = value;
		if ((value & VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE) {
			svm->mut_lapic_timer_tmict = 0;
			svm->mut_lapic_timer_interval_root_tsc = 0;
			svm->mut_lapic_timer_root_deadline = 0;
			svm->mut_lapic_timer_active =
			    svm->mut_lapic_timer_tsc_deadline != 0;
			vmm_svm_avic_apic_write32(svm,
			    VMM_SVM_APIC_REG_TMICT, 0);
			vmm_svm_avic_apic_write32(svm,
			    VMM_SVM_APIC_REG_TMCCT, 0);
		}
		if (svm->mut_lapic_timer_tmict != 0)
			vmm_svm_lapic_timer_arm(svm,
			    svm->mut_lapic_timer_tmict);
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_LVTT,
		    value);
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic lvtt accepted value=0x%x",
		    vc->imm_id, value);
		return 1;
	case VMM_SVM_APIC_REG_TMICT:
		value = *ptr;
		if ((svm->mut_lapic_timer_lvtt &
		    VMM_SVM_APIC_LVT_TIMER_MODE_MASK) ==
		    VMM_SVM_APIC_LVT_TIMER_TSCDEADLINE) {
			value = svm->mut_lapic_timer_tmict;
			vmm_svm_avic_apic_write32(svm,
			    VMM_SVM_APIC_REG_TMICT, value);
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u avic tmict ignored tscdeadline value=0x%x",
			    vc->imm_id, value);
			return 1;
		}
		vmm_svm_lapic_timer_arm(svm, value);
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic tmict accepted value=0x%x",
		    vc->imm_id, value);
		return 1;
	case VMM_SVM_APIC_REG_TDCR:
		value = *ptr & VMM_SVM_APIC_TIMER_DIVIDE_VALID;
		svm->mut_lapic_timer_tdcr = value;
		tmp1 = value & 0xfU;
		tmp2 = ((tmp1 & 0x3U) | ((tmp1 & 0x8U) >> 1)) + 1;
		svm->mut_lapic_timer_divisor = 1U << (tmp2 & 0x7U);
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_TDCR,
		    value);
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic tdcr accepted value=0x%x divisor=%u",
		    vc->imm_id, value, svm->mut_lapic_timer_divisor);
		if (svm->mut_lapic_timer_active &&
		    svm->mut_lapic_timer_tmict != 0)
			vmm_svm_lapic_timer_arm(svm,
			    svm->mut_lapic_timer_tmict);
		return 1;
	case VMM_SVM_APIC_REG_EOI:
		vmm_svm_lapic_eoi(svm);
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic eoi accepted", vc->imm_id);
		return 1;
	case VMM_SVM_APIC_REG_SVR:
		value = *ptr & VMM_SVM_APIC_SVR_VALID;
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_SVR, value);
		if ((value & VMM_SVM_APIC_SVR_ENABLE) == 0) {
			svm->mut_lapic_timer_active = 0;
			svm->mut_lapic_timer_lvtt |= VMM_SVM_APIC_LVT_MASKED;
			vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_LVTT,
			    svm->mut_lapic_timer_lvtt);
			vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_LVT0,
			    VMM_SVM_APIC_LVT_MASKED);
			vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_LVT1,
			    VMM_SVM_APIC_LVT_MASKED);
			vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_LVT_ERROR,
			    VMM_SVM_APIC_LVT_MASKED);
			vmm_svm_avic_apic_write32(svm,
			    VMM_SVM_APIC_REG_LVT_THERMAL,
			    VMM_SVM_APIC_LVT_MASKED);
			vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_LVT_PC,
			    VMM_SVM_APIC_LVT_MASKED);
		}
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic svr accepted value=0x%x",
		    vc->imm_id, value);
		return 1;
	case VMM_SVM_APIC_REG_ESR:
		vmm_svm_avic_apic_write32(svm, VMM_SVM_APIC_REG_ESR, 0);
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic esr cleared", vc->imm_id);
		return 1;
	case VMM_SVM_APIC_REG_LVT0:
	case VMM_SVM_APIC_REG_LVT1:
		tmp1 = value;
		value = *ptr & VMM_SVM_APIC_LVT_LINT_VALID;
		vmm_svm_avic_apic_write32(svm, tmp1, value);
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic %s accepted value=0x%x",
		    vc->imm_id, name, value);
		return 1;
	case VMM_SVM_APIC_REG_LVT_THERMAL:
	case VMM_SVM_APIC_REG_LVT_PC:
		tmp1 = value;
		value = *ptr & VMM_SVM_APIC_LVT_DELIVERY_VALID;
		vmm_svm_avic_apic_write32(svm, tmp1, value);
		VMM_SVM_TRACE(svm,
		    "svm vcpu%u avic %s accepted value=0x%x",
		    vc->imm_id, name, value);
		return 1;
	default:
		break;
	}
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

	if (gpa >= VMM_HPET_BASE && gpa < VMM_HPET_BASE + VMM_HPET_SIZE)
		return vmm_svm_handle_hpet_mmio(svm, vc, gpa);
	if (gpa == VMM_FCH_PM_BASE + VMM_FCH_PM_S5_RESET_STATUS)
		return vmm_svm_handle_fch_pm_mmio(svm, vc, gpa);
	if (gpa >= VMM_IOAPIC_BASE && gpa < VMM_IOAPIC_BASE + VMM_IOAPIC_SIZE)
		return vmm_svm_handle_ioapic_mmio(svm, vc, gpa);
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


static enum vmm_vcpu_exit_reason
vmm_svm_vcpu_run(void *backend, struct vmm_vcpu_thread *vc)
{
	struct vmm_svm_backend *svm = backend;
	struct vmm_svm_cpu_state *cpu_state;
	struct vmm_svm_vmcb *vmcb;
	uint64_t observed_ratio;
	uint32_t reqflags;
	int fpu_sentinel_failed;
	int handled;

	if (svm == NULL)
		return VMM_VCPU_EXIT_NONE;
	vmcb = svm->own_mut_vmcb;
	KKASSERT(vmm_svm_initialized);
	KKASSERT(mycpu->gd_cpuid < ncpus);
	cpu_state = &vmm_svm_cpu_state[mycpu->gd_cpuid];
	KKASSERT(cpu_state->own_mut_hsave != NULL);
	while (!vmm_vcpu_should_stop(vc)) {
		vmm_svm_lapic_timer_sync(svm);
		vmm_svm_timer_check(svm, vc);
		vmm_svm_com1_rx_notify(svm, vc, "entry");
		if (cpu_state->mut_tsc_ratio != svm->imm_tsc_ratio) {
			wrmsr(VMM_SVM_MSR_AMD64_TSC_RATIO, svm->imm_tsc_ratio);
			observed_ratio = rdmsr(VMM_SVM_MSR_AMD64_TSC_RATIO);
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm cpu%d tsc ratio request=0x%jx observed=0x%jx match=%d",
			    mycpu->gd_cpuid, (uintmax_t)svm->imm_tsc_ratio,
			    (uintmax_t)observed_ratio,
			    observed_ratio == svm->imm_tsc_ratio);
			cpu_state->mut_tsc_ratio = svm->imm_tsc_ratio;
		}
		fpu_sentinel_failed = 0;
		if (svm->own_mut_fpu_sentinel != NULL) {
			kernel_fpu_begin();
			fpurstor(&svm->own_mut_fpu_sentinel[0], npx_xcr0_mask);
		}
		vmm_svm_clgi();
		vmm_svm_host_tlb_catchup(svm);
		if (__predict_false(vmm_svm_host_entry_blocked())) {
			vmm_svm_stgi();
			if (svm->own_mut_fpu_sentinel != NULL)
				kernel_fpu_end();
			splz_check();
			vmm_svm_avic_unbind_cpu(svm);
			lwkt_user_yield();
			continue;
		}
		vmm_svm_avic_bind_cpu(svm);
		/* Recheck immediately before entry, then let systimer own the wakeup. */
		vmm_svm_timer_check(svm, vc);
		if (svm->mut_root_timer_deadline != 0) {
			uint64_t delta;
			uint64_t now;
			_uint128_t us;

			KKASSERT(!svm->mut_root_timer_systimer_armed);
			now = rdtsc();
			if (svm->mut_root_timer_deadline <= now)
				delta = 1;
			else
				delta = svm->mut_root_timer_deadline - now;
			us = (_uint128_t)delta * 1000000ULL +
			    svm->imm_host_tsc_hz - 1;
			us /= svm->imm_host_tsc_hz;
			if (us == 0)
				us = 1;
			if (us > VMM_SVM_ROOT_TIMER_MAX_US)
				us = VMM_SVM_ROOT_TIMER_MAX_US;
			systimer_init_oneshot(&svm->own_mut_root_timer_systimer,
			    vmm_svm_root_timer_systimer, vc, (int64_t)us);
			svm->mut_root_timer_systimer_armed = 1;
		}
		vmcb->ctrl.tlb_ctrl = VMM_SVM_CTRL_TLB_FLUSH_ALL;
		vmm_svm_guest_dbregs_enter(svm);
		vmm_svm_guest_misc_enter(svm);
		vmm_svm_guest_fpu_enter(svm);
		vmm_svm_vmrun(svm->imm_vmcb_pa, svm->mut_gprs);
		vmm_svm_guest_fpu_leave(svm);
		if (svm->own_mut_fpu_sentinel != NULL) {
			npxdna();
			fpusave(&svm->own_mut_fpu_sentinel[1], npx_xcr0_mask);
			if (bcmp(svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_xmm[0].
			    xmm_bytes, svm->own_mut_fpu_sentinel[1].sv_ymm64.
			    sv_xmm[0].xmm_bytes,
			    sizeof(svm->own_mut_fpu_sentinel[0].sv_ymm64.
			    sv_xmm[0].xmm_bytes)) != 0 ||
			    svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_env.en_tw !=
			    svm->own_mut_fpu_sentinel[1].sv_ymm64.sv_env.en_tw ||
			    bcmp(svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_fp[0].
			    fp_acc.fp_bytes, svm->own_mut_fpu_sentinel[1].sv_ymm64.
			    sv_fp[0].fp_acc.fp_bytes,
			    sizeof(svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_fp[0].
			    fp_acc.fp_bytes)) != 0 ||
			    bcmp(svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_xstate.
			    sx_ymm[0].ymm_bytes, svm->own_mut_fpu_sentinel[1].
			    sv_ymm64.sv_xstate.sx_ymm[0].ymm_bytes,
			    sizeof(svm->own_mut_fpu_sentinel[0].sv_ymm64.sv_xstate.
			    sx_ymm[0].ymm_bytes)) != 0) {
				vmm_machine_logf(svm->borrow_imm_machine,
				    "guest fault source=root_fpu_sentinel vcpu%u",
				    vc->imm_id);
				svm->mut_exit_reason = VMM_VCPU_EXIT_GUEST_FAULT;
				fpu_sentinel_failed = 1;
			}
			kernel_fpu_end();
		}
		vmm_svm_guest_misc_leave(svm);
		vmm_svm_guest_dbregs_leave(svm);
		if (svm->mut_root_timer_systimer_armed) {
			systimer_del(&svm->own_mut_root_timer_systimer);
			svm->mut_root_timer_systimer_armed = 0;
		}
		vmm_svm_stgi();
		reqflags = mycpu->gd_reqflags;
		vmm_svm_requeue_exit_event(svm);
		if (fpu_sentinel_failed)
			goto out;
		switch (vmcb->ctrl.exitcode) {
		case VMM_SVM_EXIT_INTR:
		case VMM_SVM_EXIT_NMI:
		case VMM_SVM_EXIT_SMI:
		case VMM_SVM_EXIT_INIT:
		case VMM_SVM_EXIT_VINTR:
			vmm_svm_handle_root_event(svm, vc, reqflags);
			vmm_svm_avic_unbind_cpu(svm);
			lwkt_user_yield();
			continue;
		case VMM_SVM_EXIT_INVD:
		case VMM_SVM_EXIT_WBINVD:
			vmm_svm_handle_guest_cache_op(svm);
			break;
		case VMM_SVM_EXIT_CPUID:
			vmm_svm_handle_cpuid(svm);
			break;
		case VMM_SVM_EXIT_RDPMC:
			vmcb->ctrl.eventinj = VMM_SVM_EVENTINJ_VALID |
			    VMM_SVM_EVENTINJ_ERROR_VALID |
			    VMM_SVM_EVENTINJ_TYPE_EXCEPTION |
			    VMM_X86_EXCEPTION_GP;
			VMM_SVM_TRACE(svm,
			    "svm vcpu%u inject gp reason=rdpmc-hidden rip=0x%jx",
			    vc->imm_id, (uintmax_t)vmcb->state.rip);
			break;
		case VMM_SVM_EXIT_PAUSE:
			vmm_svm_advance_rip(vmcb);
			svm->mut_pause_exit_count++;
			vmm_svm_avic_unbind_cpu(svm);
			lwkt_user_yield();
			continue;
		case VMM_SVM_EXIT_HLT:
			vmm_svm_handle_idle_wait(svm, vc);
			break;
		case VMM_SVM_EXIT_INVLPG:
		case VMM_SVM_EXIT_INVLPGA:
		case VMM_SVM_EXIT_INVLPGB:
			vmm_svm_handle_guest_tlb_op(svm);
			break;
		case VMM_SVM_EXIT_IOIO:
			if (vmm_svm_handle_ioio(svm, vc)) {
				if (svm->mut_exit_reason != VMM_VCPU_EXIT_NONE)
					goto out;
				break;
			}
			goto unhandled;
		case VMM_SVM_EXIT_SHUTDOWN:
			vmm_machine_logf(svm->borrow_imm_machine,
			    "guest fault source=svm_shutdown vcpu%u rip=0x%jx",
			    vc->imm_id, (uintmax_t)vmcb->state.rip);
			svm->mut_exit_reason = VMM_VCPU_EXIT_GUEST_FAULT;
			goto out;
		case VMM_SVM_EXIT_MONITOR:
			vmm_svm_advance_rip(vmcb);
			break;
		case VMM_SVM_EXIT_MWAIT:
		case VMM_SVM_EXIT_MWAIT_COND:
			vmcb->ctrl.eventinj = VMM_SVM_EVENTINJ_VALID |
			    VMM_SVM_EVENTINJ_TYPE_EXCEPTION | VMM_X86_EXCEPTION_UD;
			vmm_machine_logf(svm->borrow_imm_machine,
			    "svm vcpu%u inject ud reason=mwait exit=0x%jx rip=0x%jx",
			    vc->imm_id, (uintmax_t)vmcb->ctrl.exitcode,
			    (uintmax_t)vmcb->state.rip);
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
			handled = vmm_svm_handle_vmmcall(svm, vc);
			if (handled > 0)
				break;
			if (handled == 0) {
				vmm_machine_logf(svm->borrow_imm_machine,
				    "svm vcpu%u vmmcall exit rip=0x%jx",
				    vc->imm_id, (uintmax_t)vmcb->state.rip);
				goto out;
			}
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
			vmm_machine_logf(svm->borrow_imm_machine,
			    "guest fault source=unhandled_vmexit exit=0x%jx vcpu%u",
			    (uintmax_t)vmcb->ctrl.exitcode, vc->imm_id);
			svm->mut_exit_reason = VMM_VCPU_EXIT_GUEST_FAULT;
			goto out;
		}
		vmm_svm_avic_unbind_cpu(svm);
	}
out:
	if (svm->mut_root_timer_systimer_armed) {
		systimer_del(&svm->own_mut_root_timer_systimer);
		svm->mut_root_timer_systimer_armed = 0;
	}
	vmm_svm_avic_unbind_cpu(svm);
	if (cpu_state->mut_tsc_ratio != 0) {
		wrmsr(VMM_SVM_MSR_AMD64_TSC_RATIO,
		    cpu_state->raw_imm_host_tsc_ratio);
		cpu_state->mut_tsc_ratio = 0;
	}
	return svm->mut_exit_reason;
}

const struct vmm_vcpu_backend_ops vmm_svm_backend_ops = {
	.imm_name = "svm",
	.probe = vmm_svm_probe,
	.init = vmm_svm_init,
	.uninit = vmm_svm_uninit,
	.create = vmm_svm_vcpu_create,
	.destroy = vmm_svm_vcpu_destroy,
	.run = vmm_svm_vcpu_run,
	.console_input = vmm_svm_console_input,
};

VMM_VCPU_BACKEND_SET(vmm_svm_backend_ops);
