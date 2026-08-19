/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM runtime backend interface.
 */
#ifndef VMM_BACKEND_H
#define VMM_BACKEND_H

#include <sys/types.h>

struct vmm_machine;
struct vmm_vcpu;
struct vmm_cpuid_entry;
struct vmm_x64_capability;
struct vmm_cpuexit;
struct vmm_cpuexit_io;
struct vmm_ioapic_state;

struct vmm_backend_ops {
	const char *name;
	int (*probe)(void);
	int (*init)(void);
	void (*fini)(void);
	int (*capability)(struct vmm_x64_capability *);
	int (*get_supported_cpuid)(struct vmm_cpuid_entry *, size_t *);
	int (*machine_create)(struct vmm_machine *);
	int (*machine_set_tsc)(struct vmm_machine *, uint64_t);
	bool (*irqchip_available)(void);
	/* Non-blocking capability check for pre-vCPU irqchip creation. */
	int (*machine_create_irqchip)(struct vmm_machine *);
	int (*irq_raise_msi)(struct vmm_machine *, uint64_t, uint32_t);
	int (*machine_set_irq)(struct vmm_machine *, uint32_t, bool);
	int (*machine_raise_legacy)(struct vmm_machine *, uint8_t);
	int (*machine_get_ioapic)(struct vmm_machine *,
	    struct vmm_ioapic_state *);
	int (*machine_set_ioapic)(struct vmm_machine *,
	    const struct vmm_ioapic_state *);
	void (*machine_destroy)(struct vmm_machine *);
	int (*vcpu_create)(struct vmm_vcpu *);
	int (*vcpu_set_cpuid)(struct vmm_vcpu *,
	    const struct vmm_cpuid_entry *, size_t);
	int (*vcpu_get_tsc)(struct vmm_vcpu *, uint64_t *);
	int (*vcpu_get_lapic)(struct vmm_vcpu *, void *, size_t);
	int (*vcpu_set_lapic)(struct vmm_vcpu *, const void *, size_t);
	/* Handle an in-kernel PIO device, or return ENOENT for the frontend. */
	int (*vcpu_io)(struct vmm_vcpu *, const struct vmm_cpuexit_io *);
	/* Handle one scalar access claimed by the backend, or return ENOENT. */
	int (*vcpu_mmio)(struct vmm_vcpu *, uint64_t, size_t, bool, uint64_t *);
	/* Retry after the core populated a guest RAM mapping. */
	void (*vcpu_memory_mapping_changed)(struct vmm_vcpu *);
	void (*vcpu_destroy)(struct vmm_vcpu *);
	/* Load the frontend-owned architectural state before a public run. */
	void (*vcpu_setstate)(struct vmm_vcpu *);
	int (*vcpu_run)(struct vmm_vcpu *, struct vmm_cpuexit **);
	void (*vcpu_getstate)(struct vmm_vcpu *);
	void (*vcpu_kick)(struct vmm_vcpu *);
};

#define VMM_BACKEND_SET(ops)	DATA_SET(vmm_backend_set, ops)

#endif /* VMM_BACKEND_H */
