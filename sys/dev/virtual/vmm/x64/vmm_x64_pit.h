/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reusable x86-64 8254 PIT state machine.
 */
#ifndef VMM_X64_PIT_H
#define VMM_X64_PIT_H

#include <sys/types.h>

struct vmm_machine;
struct vmm_cpustate;
struct vmm_cpuexit_io;
struct vmm_x64_pit;

#define VMM_X64_PIT_CHANNELS		3U
#define VMM_X64_PIT_FREQUENCY		1193182U
#define VMM_PIT_FLAG_HPET_LEGACY	0x00000001U
#define VMM_PIT_FLAG_SPEAKER_DATA_ON	0x00000002U

/* Internal machine-owned PIT lifecycle and PIO dispatch. */
int vmm_x64_pit_create(struct vmm_machine *);
void vmm_x64_pit_destroy(struct vmm_machine *);
int vmm_x64_pit_get_state(struct vmm_machine *, struct vmm_pit_state *);
int vmm_x64_pit_set_state(struct vmm_machine *,
	const struct vmm_pit_state *);
int vmm_x64_pit_io(struct vmm_machine *, struct vmm_cpustate *,
	const struct vmm_cpuexit_io *);

#endif /* VMM_X64_PIT_H */
