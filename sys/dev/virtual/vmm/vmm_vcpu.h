/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core vCPU private declarations.  Public callers include vmm.h.
 */
#ifndef VMM_VCPU_H
#define VMM_VCPU_H

#include "vmm_machine.h"

struct vmm_x64_emul;

struct vmm_vcpu {
	/* token protects running, destroying, event, and event_pending. */
	struct lwkt_token token;
	struct vmm_machine *machine;
	const struct vmm_backend_ops *backend_ops;
	struct vmm_cpustate *state;
	/* One caller-supplied event, committed only immediately before VM entry. */
	struct vmm_cpuevent event;
	/* Assigned once by machine under its token; stable for this vCPU. */
	unsigned int id;
	/* Published by run(); callers may read it only after run returns. */
	struct vmm_cpuexit exit;
	/* x86 memory instruction state retained across an external MMIO exit. */
	struct vmm_x64_emul *emul;
	void *backend;
	/* Selected by the frontend before the vCPU can run. */
	enum vmm_memory_exit_mode memory_exit_mode;
	int running;
	int destroying;
	int event_pending;
	volatile int kick_pending;
};

#endif /* VMM_VCPU_H */
