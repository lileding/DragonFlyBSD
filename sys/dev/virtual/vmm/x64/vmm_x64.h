/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm x86-64 public CPU API.
 */
#ifndef VMM_X64_H
#define VMM_X64_H

#include <sys/types.h>
#include <sys/vmm.h>

/* x86 architectural exception vectors. */
#define VMM_X64_EXCEPTION_GP	13U

/* KVM-compatible LAPIC register image size. */
#define VMM_X64_LAPIC_STATE_SIZE	0x400U

/* x86 IOAPIC state exposed by the architecture-specific public ABI. */
#define VMM_IOAPIC_PIN_COUNT	24U
#define VMM_IOAPIC_BASE		0xfec00000ULL

struct vmm_ioapic_state {
	uint64_t base;
	uint32_t select;
	uint32_t id;
	uint32_t irr;
	uint32_t reserved;
	uint64_t redir[VMM_IOAPIC_PIN_COUNT];
};

/* One x86 8259-compatible interrupt controller state image. */
struct vmm_pic_chip_state {
	uint8_t last_irr;
	uint8_t irr;
	uint8_t imr;
	uint8_t isr;
	uint8_t priority_add;
	uint8_t irq_base;
	uint8_t read_reg_select;
	uint8_t poll;
	uint8_t special_mask;
	uint8_t init_state;
	uint8_t auto_eoi;
	uint8_t rotate_on_auto_eoi;
	uint8_t special_fully_nested_mode;
	uint8_t init4;
	uint8_t elcr;
	uint8_t elcr_mask;
};

/* Complete x86 master/slave 8259 state bound to one machine irqchip. */
struct vmm_pic_state {
	struct vmm_pic_chip_state master;
	struct vmm_pic_chip_state slave;
};

CTASSERT(sizeof(struct vmm_pic_chip_state) == 16);

/*
 * Copies or restores the complete master/slave 8259 state of an existing
 * irqchip.  These calls are safe while vCPUs run; restoring pending,
 * unmasked interrupts may immediately inject the resulting legacy vector.
 * A machine without an irqchip returns ENXIO.
 */
int vmm_machine_get_pic(vmm_machine_t machine, struct vmm_pic_state *state);
int vmm_machine_set_pic(vmm_machine_t machine,
	const struct vmm_pic_state *state);

/* 8254 channel state used for save, restore, and KVM PIT2 compatibility. */
struct vmm_pit_channel_state {
	uint32_t count;
	uint16_t latched_count;
	uint8_t count_latched;
	uint8_t status_latched;
	uint8_t status;
	uint8_t read_state;
	uint8_t write_state;
	uint8_t write_latch;
	uint8_t rw_mode;
	uint8_t mode;
	uint8_t bcd;
	uint8_t gate;
	int64_t count_load_time;
};

/* Complete x86 8254 PIT state bound to one machine irqchip. */
struct vmm_pit_state {
	struct vmm_pit_channel_state channels[3];
	uint32_t flags;
};

CTASSERT(sizeof(struct vmm_pit_channel_state) == 24);

/*
 * Creates the machine's single in-kernel 8254 PIT.  The machine must already
 * have an irqchip and no vCPU may be running.  A second call returns EEXIST.
 * The PIT is destroyed automatically with its machine.
 */
int vmm_machine_create_pit(vmm_machine_t machine);

/*
 * Copies or restores the complete PIT state.  Both operations require a PIT
 * created by vmm_machine_create_pit(); restoring state may immediately arm
 * the channel 0 timer and deliver guest IRQ0.
 */
int vmm_machine_get_pit(vmm_machine_t machine, struct vmm_pit_state *state);
int vmm_machine_set_pit(vmm_machine_t machine,
	const struct vmm_pit_state *state);

/* CPUID entry whose ECX input is significant. */
#define VMM_CPUID_FLAG_SIGNIFICANT_SUBLEAF	0x00000001U

/* One exact CPUID result selected by EAX leaf and, optionally, ECX subleaf. */
struct vmm_cpuid_entry {
	uint32_t leaf;
	uint32_t subleaf;
	uint32_t flags;
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
};

/* Capabilities of the selected x86-64 backend. */
struct vmm_x64_capability {
	uint64_t xcr0_mask;
	uint32_t mxcsr_mask;
};

/* Reports scalar capabilities of the backend selected when the module loaded. */
int vmm_x64_get_capability(struct vmm_x64_capability *capability);

/*
 * Returns a backend-safe CPUID baseline.  Callers choose every static per-vCPU
 * policy value before installing entries with vmm_vcpu_set_cpuid().  The backend
 * recomputes APIC topology and current XSAVE size while executing CPUID.  Set
 * entries to NULL to obtain the required count.  A too-small buffer returns
 * E2BIG and updates entry_count with the required count.
 */
int vmm_x64_get_supported_cpuid(struct vmm_cpuid_entry *entries,
	size_t *entry_count);

/*
 * Sets the shared guest TSC timeline for every vCPU in machine.  Running
 * vCPUs observe the new base before their next VM entry.  This does not
 * change a vCPU's guest-managed IA32_TSC_ADJUST value.
 */
int vmm_machine_set_tsc(vmm_machine_t machine, uint64_t tsc);

/*
 * Returns the current virtual TSC for a stopped vCPU.  It includes the
 * machine-wide TSC base and that vCPU's guest-managed IA32_TSC_ADJUST value.
 * Returns EBUSY while the vCPU runs.
 */
int vmm_vcpu_get_tsc(vmm_vcpu_t vcpu, uint64_t *tsc);

/*
 * Translates one current guest virtual address through the vCPU page tables.
 * The vCPU must not be running.  It only reports the corresponding GPA and
 * does not modify guest CPU state or data.  Resolving guest page-table pages
 * may fault their backing into the machine vmspace.
 */
int vmm_vcpu_translate(vmm_vcpu_t vcpu, uint64_t gva, uint64_t *gpa);

/*
 * Copies the x86 LAPIC register image to or from a stopped vCPU.  The image
 * is fixed at VMM_X64_LAPIC_STATE_SIZE bytes and has no host pointer fields.
 * Restoring it also rebuilds backend-derived LAPIC timer and routing state.
 */
int vmm_vcpu_get_lapic(vmm_vcpu_t vcpu, void *registers, size_t size);
int vmm_vcpu_set_lapic(vmm_vcpu_t vcpu, const void *registers, size_t size);

/*
 * Copies the x86 IOAPIC state to or from a machine with an in-kernel irqchip.
 * The caller supplies only architecture state; routing and delivery remain the
 * backend's responsibility.  Restoring asserted pins may immediately deliver
 * their configured interrupts.
 */
int vmm_machine_get_ioapic(vmm_machine_t machine,
	struct vmm_ioapic_state *state);
int vmm_machine_set_ioapic(vmm_machine_t machine,
	const struct vmm_ioapic_state *state);

#define VMM_X64_STATE_SEGS	0x01
#define VMM_X64_STATE_GPRS	0x02
#define VMM_X64_STATE_CRS	0x04
#define VMM_X64_STATE_DRS	0x08
#define VMM_X64_STATE_MSRS	0x10
#define VMM_X64_STATE_INTR	0x20
#define VMM_X64_STATE_FPU	0x40
#define VMM_X64_STATE_ALL	\
	(VMM_X64_STATE_SEGS | VMM_X64_STATE_GPRS | \
	 VMM_X64_STATE_CRS | VMM_X64_STATE_DRS | \
	 VMM_X64_STATE_MSRS | VMM_X64_STATE_INTR | \
	 VMM_X64_STATE_FPU)

struct vmm_cpuexit_memory {
	int prot;
	uint64_t gpa;
	uint8_t inst_len;
	uint8_t inst_bytes[15];
	/* Valid after VMM decodes an external MMIO fragment. */
	enum vmm_io_width width;
	/* Valid only when prot includes VM_PROT_WRITE. */
	uint64_t value;
};

struct vmm_cpuexit_io {
	bool in;
	uint16_t port;
	int8_t seg;
	uint8_t address_size;
	uint8_t operand_size;
	bool rep;
	bool str;
	uint64_t npc;
};

struct vmm_cpuexit_rdmsr {
	uint32_t msr;
	uint64_t npc;
};

struct vmm_cpuexit_wrmsr {
	uint32_t msr;
	uint64_t val;
	uint64_t npc;
};

struct vmm_cpuexit_insn {
	uint64_t npc;
};

struct vmm_cpuexit_invalid {
	uint64_t hwcode;
};

#define VMM_CPUEXIT_NONE		0x0000000000000000ULL
#define VMM_CPUEXIT_INVALID		0xFFFFFFFFFFFFFFFFULL
#define VMM_CPUEXIT_MEMORY		0x0000000000000001ULL
#define VMM_CPUEXIT_IO			0x0000000000000002ULL
#define VMM_CPUEXIT_SHUTDOWN		0x0000000000001000ULL
#define VMM_CPUEXIT_INT_READY		0x0000000000001001ULL
#define VMM_CPUEXIT_NMI_READY		0x0000000000001002ULL
#define VMM_CPUEXIT_HALTED		0x0000000000001003ULL
#define VMM_CPUEXIT_TPR_CHANGED		0x0000000000001004ULL
#define VMM_CPUEXIT_RDMSR		0x0000000000002000ULL
#define VMM_CPUEXIT_WRMSR		0x0000000000002001ULL
#define VMM_CPUEXIT_MONITOR		0x0000000000002002ULL
#define VMM_CPUEXIT_MWAIT		0x0000000000002003ULL
#define VMM_CPUEXIT_CPUID		0x0000000000002004ULL

/* x86 architectural event classes accepted by a backend. */
#define VMM_CPUEVENT_EXCP	0
#define VMM_CPUEVENT_INTR	1

/* One architectural event requested by the caller before a vCPU run. */
struct vmm_cpuevent {
	uint8_t type;
	uint8_t vector;
	uint64_t error;
};

/*
 * One x86-64 architectural VM exit.  reason selects the valid union member;
 * exitstate reports the interrupt-delivery state at the return boundary.
 */
struct vmm_cpuexit {
	uint64_t reason;
	union {
		struct vmm_cpuexit_memory mem;
		struct vmm_cpuexit_io io;
		struct vmm_cpuexit_rdmsr rdmsr;
		struct vmm_cpuexit_wrmsr wrmsr;
		struct vmm_cpuexit_insn insn;
		struct vmm_cpuexit_invalid inv;
	} u;
	struct {
		uint64_t rflags;
		uint64_t cr8;
		uint64_t int_shadow:1;
		uint64_t int_window_exiting:1;
		uint64_t nmi_window_exiting:1;
		uint64_t evt_pending:1;
		uint64_t rsvd:60;
	} exitstate;
};

#endif /* VMM_X64_H */
