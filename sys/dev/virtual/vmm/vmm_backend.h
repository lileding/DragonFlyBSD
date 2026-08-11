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

struct vmm_backend_ops {
	const char *name;
	int (*probe)(void);
	int (*init)(void);
	void (*fini)(void);
	int (*capability)(struct vmm_x64_capability *);
	int (*get_supported_cpuid)(struct vmm_cpuid_entry *, size_t *);
	int (*machine_create)(struct vmm_machine *);
	bool (*irqchip_available)(void);
	/* Non-blocking capability check for pre-vCPU irqchip creation. */
	int (*machine_create_irqchip)(struct vmm_machine *);
	int (*irq_raise_msi)(struct vmm_machine *, uint64_t, uint32_t);
	int (*machine_raise_irq)(struct vmm_machine *, uint32_t);
	int (*machine_set_irq)(struct vmm_machine *, uint32_t, bool);
	void (*machine_destroy)(struct vmm_machine *);
	int (*vcpu_create)(struct vmm_vcpu *);
	int (*vcpu_set_cpuid)(struct vmm_vcpu *,
	    const struct vmm_cpuid_entry *, size_t);
	void (*vcpu_destroy)(struct vmm_vcpu *);
	int (*vcpu_run)(struct vmm_vcpu *, struct vmm_cpuexit **);
	void (*vcpu_getstate)(struct vmm_vcpu *);
	void (*vcpu_kick)(struct vmm_vcpu *);
};

#define VMM_BACKEND_SET(ops)	DATA_SET(vmm_backend_set, ops)

#endif /* VMM_BACKEND_H */
