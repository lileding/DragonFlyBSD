/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core vCPU private declarations.  Public callers include vmm.h.
 */
#ifndef VMM_VCPU_H
#define VMM_VCPU_H

#include "vmm_machine.h"

struct vmm_vcpu {
	/* token protects running and kick_pending. */
	struct lwkt_token token;
	struct vmm_machine *machine;
	struct vmm_cpustate *state;
	int running;
	int kick_pending;
};

#endif /* VMM_VCPU_H */
