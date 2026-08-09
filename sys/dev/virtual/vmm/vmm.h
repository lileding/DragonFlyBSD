/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm public kernel API.
 */
#ifndef VMM_H
#define VMM_H

#include <sys/types.h>

struct vmspace;
struct vmm_machine;
struct vmm_vcpu;

/* Opaque runtime handles created and destroyed only through this API. */
typedef struct vmm_machine *vmm_machine_t;
typedef struct vmm_vcpu *vmm_vcpu_t;

#if defined(__x86_64__)
#include "x64/vmm_x64.h"
#else
#error "vmm has no public API for this architecture"
#endif

/*
 * Creates a machine using vmspace as its guest GPA address space.
 * vmspace must be initialized and private to the guest.  VMM marks its pmap
 * threadable and converts it for nested paging.  The caller retains ownership,
 * keeps it valid until vmm_machine_destroy() returns, and must not reuse it as
 * a host vmspace.  Returns EBUSY while the module drains.
 */
int vmm_machine_create(struct vmspace *vmspace, vmm_machine_t *machine);

/*
 * Reports the capabilities of the backend selected when the module loaded.
 * The result is architecture-specific and does not expose backend internals.
 */
int vmm_capability(struct vmm_x64_capability *capability);

/*
 * Creates a vCPU using caller-owned architectural state.  state must remain
 * valid until vmm_vcpu_destroy(), and machine must remain valid throughout.
 * cpuid_masks is copied during this call and may be NULL when the count is
 * zero.  Callers must not change state while the vCPU runs.
 */
int vmm_vcpu_create(vmm_machine_t machine, struct vmm_cpustate *state,
	const struct vmm_cpuid_mask *cpuid_masks, size_t cpuid_mask_count,
	vmm_vcpu_t *vcpu);

/*
 * Runs a vCPU until it exits to the caller.  The backend resolves known
 * architectural exits and registered I/O in kernel; VMM resolves nested page
 * faults through the machine vmspace.  On success, *reason is valid until the
 * next vmm_vcpu_run() call or vmm_vcpu_destroy(), and state is current.
 * Returns EBUSY when another caller is running it.
 */
int vmm_vcpu_run(vmm_vcpu_t vcpu, struct vmm_cpuexit **reason);

/*
 * Queues one architectural event for the next vCPU run.  The vCPU must not be
 * running; callers choose the synchronization and delivery policy.
 */
int vmm_vcpu_inject(vmm_vcpu_t vcpu, const struct vmm_cpuevent *event);

/*
 * Requests that a running vCPU return promptly without changing guest state.
 * Returns EALREADY when the vCPU is not running.
 */
int vmm_vcpu_kick(vmm_vcpu_t vcpu);

/*
 * Destroys a non-running vCPU; returns EBUSY while vmm_vcpu_run() is active.
 */
int vmm_vcpu_destroy(vmm_vcpu_t vcpu);

/*
 * Destroys a machine after every vCPU has been destroyed.  This removes VMM's
 * pmap CPU association but does not otherwise manage vmspace.
 * Returns EBUSY while a vCPU exists or is running.
 */
int vmm_machine_destroy(vmm_machine_t machine);

#endif /* VMM_H */
