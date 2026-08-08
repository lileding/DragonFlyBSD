/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core vCPU private declarations.  Public callers include vmm.h.
 */
#ifndef VMM_VCPU_H
#define VMM_VCPU_H

#include "vmm_machine.h"

struct vmm_vcpu {
	/* token protects running; kick_pending is atomic. */
	struct lwkt_token token;
	struct vmm_machine *machine;
	const struct vmm_backend_ops *backend_ops;
	struct vmm_cpustate *state;
	/* Published by run(); callers may read it only after run returns. */
	struct vmm_cpuexit exit;
	void *backend;
	int running;
	volatile int kick_pending;
};

#endif /* VMM_VCPU_H */
