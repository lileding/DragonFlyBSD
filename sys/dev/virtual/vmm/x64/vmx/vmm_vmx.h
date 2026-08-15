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
struct vmm_cpuid_entry;
struct vmm_x64_capability;

bool vmm_vmx_ident(void);
int vmm_vmx_probe(void);
int vmm_vmx_init(void);
void vmm_vmx_fini(void);
int vmm_vmx_capability(struct vmm_x64_capability *);
int vmm_vmx_get_supported_cpuid(struct vmm_cpuid_entry *, size_t *);
int vmm_vmx_machine_create(struct vmm_machine *);
void vmm_vmx_machine_destroy(struct vmm_machine *);
int vmm_vmx_vcpu_create(struct vmm_vcpu *);
int vmm_vmx_vcpu_set_cpuid(struct vmm_vcpu *,
    const struct vmm_cpuid_entry *, size_t);
void vmm_vmx_vcpu_destroy(struct vmm_vcpu *);
void vmm_vmx_vcpu_setstate(struct vmm_vcpu *);
int vmm_vmx_vcpu_run(struct vmm_vcpu *, struct vmm_cpuexit **);
void vmm_vmx_vcpu_getstate(struct vmm_vcpu *);
void vmm_vmx_vcpu_kick(struct vmm_vcpu *);

#endif /* VMM_VMX_H */
