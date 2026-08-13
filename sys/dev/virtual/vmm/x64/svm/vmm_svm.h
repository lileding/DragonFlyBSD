/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * AMD SVM backend private entry points.
 */
#ifndef VMM_SVM_H
#define VMM_SVM_H

#include <sys/types.h>

struct vmm_machine;
struct vmm_vcpu;
struct vmm_cpuexit;
struct vmm_cpuexit_io;
struct vmm_ioapic_state;
struct vmm_svm_interrupt_machine;

/* SVM exit code shared by VMRUN and the software irqchip. */
#define VMCB_EXITCODE_IRET	0x0074

struct vmm_svm_machdata {
	volatile uint64_t mach_htlb_gen;
	struct vmm_svm_interrupt_machine *interrupt;
};

bool vmm_svm_ident(void);
int vmm_svm_probe(void);
int vmm_svm_init(void);
void vmm_svm_fini(void);
int vmm_svm_capability(struct vmm_x64_capability *);
int vmm_svm_get_supported_cpuid(struct vmm_cpuid_entry *, size_t *);
int vmm_svm_machine_create(struct vmm_machine *);
bool vmm_svm_irqchip_available(void);
int vmm_svm_machine_create_irqchip(struct vmm_machine *);
int vmm_svm_irq_raise_msi(struct vmm_machine *, uint64_t, uint32_t);
int vmm_svm_machine_set_irq(struct vmm_machine *, uint32_t, bool);
int vmm_svm_machine_raise_legacy(struct vmm_machine *, uint8_t);
int vmm_svm_machine_get_ioapic(struct vmm_machine *,
	struct vmm_ioapic_state *);
int vmm_svm_machine_set_ioapic(struct vmm_machine *,
	const struct vmm_ioapic_state *);
void vmm_svm_machine_destroy(struct vmm_machine *);
int vmm_svm_vcpu_create(struct vmm_vcpu *);
int vmm_svm_vcpu_set_cpuid(struct vmm_vcpu *,
	const struct vmm_cpuid_entry *, size_t);
int vmm_svm_vcpu_get_lapic(struct vmm_vcpu *, void *, size_t);
int vmm_svm_vcpu_set_lapic(struct vmm_vcpu *, const void *, size_t);
int vmm_svm_vcpu_io(struct vmm_vcpu *, const struct vmm_cpuexit_io *);
int vmm_svm_vcpu_mmio(struct vmm_vcpu *, uint64_t, size_t, bool,
	uint64_t *);
void vmm_svm_vcpu_destroy(struct vmm_vcpu *);
void vmm_svm_vcpu_setstate(struct vmm_vcpu *);
int vmm_svm_vcpu_run(struct vmm_vcpu *, struct vmm_cpuexit **);
void vmm_svm_vcpu_getstate(struct vmm_vcpu *);
void vmm_svm_vcpu_kick(struct vmm_vcpu *);
int vmm_svm_vcpu_inject_interrupt(struct vmm_vcpu *, uint8_t);
bool vmm_svm_vcpu_interrupt_allowed(struct vmm_vcpu *);
void vmm_svm_vcpu_request_interrupt_window(struct vmm_vcpu *);
void vmm_svm_restore_tr(uint16_t);

#endif /* VMM_SVM_H */
