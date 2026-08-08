/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm public kernel API.
 */
#ifndef VMM_H
#define VMM_H

#include <sys/types.h>

struct vm_object;
struct vmm_machine;
struct vmm_vcpu;

typedef struct vmm_machine *vmm_machine_t;
typedef struct vmm_vcpu *vmm_vcpu_t;

#if defined(__x86_64__)
#include "vmm_x64.h"
#else
#error "vmm has no public API for this architecture"
#endif

int vmm_machine_create(vmm_machine_t *machine);
int vmm_machine_map(vmm_machine_t machine, uint64_t gpa_base,
	uint64_t gpa_size, struct vm_object *object, uint64_t offset);
int vmm_machine_unmap(vmm_machine_t machine, uint64_t gpa_base,
	uint64_t gpa_size);

int vmm_vcpu_create(vmm_machine_t machine, struct vmm_cpustate *state,
	vmm_vcpu_t *vcpu);
int vmm_vcpu_run(vmm_vcpu_t vcpu, struct vmm_cpuexit **reason);
int vmm_vcpu_kick(vmm_vcpu_t vcpu);
int vmm_vcpu_destroy(vmm_vcpu_t vcpu);

int vmm_machine_destroy(vmm_machine_t machine);

#endif /* VMM_H */
