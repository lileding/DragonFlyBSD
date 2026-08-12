/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD Advanced Virtual Interrupt Controller private interface.
 */
#ifndef VMM_SVM_AVIC_H
#define VMM_SVM_AVIC_H

#include <sys/types.h>

#include <vm/vm.h>

struct vmm_machine;
struct vmm_cpuevent;
struct vmm_cpuexit_io;
struct vmm_ioapic_state;
struct vmm_vcpu;
struct vmm_svm_interrupt_machine;
struct vmm_svm_interrupt_vcpu;

/*
 * The SVM entry code owns the VMCB.  An interrupt implementation returns
 * only the hardware values that it needs written into that VMCB.
 */
struct vmm_svm_interrupt_config {
	bool enabled;
	vm_paddr_t apic_base;
	vm_paddr_t apic_backing_page;
	vm_paddr_t logical_table;
	vm_paddr_t physical_table;
	uint8_t physical_max_index;
};

/* Selected once by vmm_svm_probe(), before any machine can exist. */
struct vmm_svm_interrupt_ops {
	const char *name;
	int (*machine_create)(struct vmm_machine *,
	    struct vmm_svm_interrupt_machine **);
	int (*machine_enable)(struct vmm_svm_interrupt_machine *);
	int (*irq_raise_msi)(struct vmm_svm_interrupt_machine *, uint64_t,
	    uint32_t);
	int (*irq_set)(struct vmm_svm_interrupt_machine *, uint32_t, bool);
	int (*irq_raise_legacy)(struct vmm_svm_interrupt_machine *, uint8_t);
	int (*machine_get_ioapic)(struct vmm_svm_interrupt_machine *,
	    struct vmm_ioapic_state *);
	int (*machine_set_ioapic)(struct vmm_svm_interrupt_machine *,
	    const struct vmm_ioapic_state *);
	int (*vcpu_mmio)(struct vmm_svm_interrupt_vcpu *, uint64_t, bool,
	    uint32_t *);
	int (*vcpu_io)(struct vmm_vcpu *, const struct vmm_cpuexit_io *);
	/* Handle an irqchip-owned MSR; ENOENT leaves it to the frontend. */
	int (*vcpu_msr)(struct vmm_svm_interrupt_vcpu *, bool, uint32_t,
	    uint64_t *);
	void (*machine_destroy)(struct vmm_svm_interrupt_machine *);
	int (*vcpu_create)(struct vmm_svm_interrupt_machine *,
	    struct vmm_vcpu *, struct vmm_svm_interrupt_vcpu **,
	    struct vmm_svm_interrupt_config *);
	int (*vcpu_get_lapic)(struct vmm_svm_interrupt_vcpu *, void *, size_t);
	int (*vcpu_set_lapic)(struct vmm_svm_interrupt_vcpu *, const void *,
	    size_t);
	void (*vcpu_destroy)(struct vmm_svm_interrupt_vcpu *);
	void (*vcpu_enter)(struct vmm_svm_interrupt_vcpu *);
	void (*vcpu_leave)(struct vmm_svm_interrupt_vcpu *);
	/* Resolve an interrupt queued by vcpu_enter after this VMRUN exits. */
	void (*vcpu_event_result)(struct vmm_svm_interrupt_vcpu *, bool);
	/* A software irqchip consumes VINTR as an internal retry point. */
	bool vintr_internal;
	int (*vcpu_exit)(struct vmm_svm_interrupt_vcpu *, uint64_t, uint64_t,
	    uint64_t);
};

bool vmm_svm_avic_available(void);
int vmm_svm_avic_read_register(struct vmm_svm_interrupt_vcpu *, uint32_t,
    uint32_t *);

extern const struct vmm_svm_interrupt_ops vmm_svm_avic_interrupt_ops;
extern const struct vmm_svm_interrupt_ops vmm_svm_softirq_interrupt_ops;
extern const struct vmm_svm_interrupt_ops *vmm_svm_interrupt_ops;

#endif /* VMM_SVM_AVIC_H */
