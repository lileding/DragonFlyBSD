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
	void (*machine_destroy)(struct vmm_svm_interrupt_machine *);
	int (*vcpu_create)(struct vmm_svm_interrupt_machine *,
	    struct vmm_vcpu *, struct vmm_svm_interrupt_vcpu **,
	    struct vmm_svm_interrupt_config *);
	void (*vcpu_destroy)(struct vmm_svm_interrupt_vcpu *);
	void (*vcpu_enter)(struct vmm_svm_interrupt_vcpu *);
	void (*vcpu_leave)(struct vmm_svm_interrupt_vcpu *);
	int (*vcpu_exit)(struct vmm_svm_interrupt_vcpu *, uint64_t, uint64_t,
	    uint64_t);
};

bool vmm_svm_avic_available(void);
int vmm_svm_avic_read_register(struct vmm_svm_interrupt_vcpu *, uint32_t,
    uint32_t *);

extern const struct vmm_svm_interrupt_ops vmm_svm_avic_interrupt_ops;
extern const struct vmm_svm_interrupt_ops vmm_svm_soft_interrupt_ops;
extern const struct vmm_svm_interrupt_ops *vmm_svm_interrupt_ops;

#endif /* VMM_SVM_AVIC_H */
