/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Intel VMX backend private entry points.
 */
#ifndef VMM_VMX_H
#define VMM_VMX_H

#include <sys/types.h>

struct vmm_machine;
struct vmm_vcpu;
struct vmm_cpuexit;
struct vmm_cpuexit_io;
struct vmm_cpuid_entry;
struct vmm_ioapic_state;
struct vmm_x64_capability;

bool vmm_vmx_ident(void);
int vmm_vmx_probe(void);
int vmm_vmx_init(void);
void vmm_vmx_fini(void);
int vmm_vmx_capability(struct vmm_x64_capability *);
int vmm_vmx_get_supported_cpuid(struct vmm_cpuid_entry *, size_t *);
int vmm_vmx_machine_create(struct vmm_machine *);
bool vmm_vmx_irqchip_available(void);
int vmm_vmx_machine_create_irqchip(struct vmm_machine *);
int vmm_vmx_irq_raise_msi(struct vmm_machine *, uint64_t, uint32_t);
int vmm_vmx_machine_set_irq(struct vmm_machine *, uint32_t, bool);
int vmm_vmx_machine_raise_legacy(struct vmm_machine *, uint8_t);
int vmm_vmx_machine_get_ioapic(struct vmm_machine *,
    struct vmm_ioapic_state *);
int vmm_vmx_machine_set_ioapic(struct vmm_machine *,
    const struct vmm_ioapic_state *);
void vmm_vmx_machine_destroy(struct vmm_machine *);
int vmm_vmx_vcpu_create(struct vmm_vcpu *);
int vmm_vmx_vcpu_set_cpuid(struct vmm_vcpu *,
    const struct vmm_cpuid_entry *, size_t);
int vmm_vmx_vcpu_get_lapic(struct vmm_vcpu *, void *, size_t);
int vmm_vmx_vcpu_set_lapic(struct vmm_vcpu *, const void *, size_t);
int vmm_vmx_vcpu_io(struct vmm_vcpu *, const struct vmm_cpuexit_io *);
int vmm_vmx_vcpu_mmio(struct vmm_vcpu *, uint64_t, size_t, bool,
    uint64_t *);
void vmm_vmx_vcpu_destroy(struct vmm_vcpu *);
void vmm_vmx_vcpu_memory_mapping_changed(struct vmm_vcpu *);
void vmm_vmx_vcpu_setstate(struct vmm_vcpu *);
int vmm_vmx_vcpu_run(struct vmm_vcpu *, struct vmm_cpuexit **);
void vmm_vmx_vcpu_getstate(struct vmm_vcpu *);
void vmm_vmx_vcpu_kick(struct vmm_vcpu *);
int vmm_vmx_vcpu_inject_interrupt(struct vmm_vcpu *, uint8_t);
bool vmm_vmx_vcpu_interrupt_allowed(struct vmm_vcpu *);
void vmm_vmx_vcpu_request_interrupt_window(struct vmm_vcpu *);
void vmm_vmx_vcpu_set_rvi(struct vmm_vcpu *, uint8_t);

/* VMX exit codes consumed internally by the software irqchip. */
#define VMM_VMX_EXIT_INT_WINDOW	7U
#define VMM_VMX_EXIT_NMI_WINDOW	8U
#define VMM_VMX_EXIT_HLT		12U
#define VMM_VMX_EXIT_VEOI		45U
#define VMM_VMX_EXIT_APIC_WRITE	56U

#endif /* VMM_VMX_H */
