/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private x86 PIC support for the VMM core.
 */
#ifndef VMM_X64_PIC_H
#define VMM_X64_PIC_H

#include <sys/types.h>

struct vmm_cpustate;
struct vmm_cpuexit_io;
struct vmm_machine;
struct vmm_pic_state;
struct vmm_x64_pic;

struct vmm_x64_pic *vmm_x64_pic_alloc(void);
void vmm_x64_pic_free(struct vmm_x64_pic *);
void vmm_x64_pic_destroy(struct vmm_machine *);
int vmm_x64_pic_get_state(struct vmm_machine *, struct vmm_pic_state *);
int vmm_x64_pic_set_state(struct vmm_machine *,
	const struct vmm_pic_state *);
int vmm_x64_pic_set_irq_locked(struct vmm_machine *, uint32_t, bool, int *);
/* Caller holds machine->token; inspect the current PIC output. */
int vmm_x64_pic_peek_locked(struct vmm_machine *, uint8_t *);
/* Caller holds machine->token; acknowledge the current PIC output. */
int vmm_x64_pic_accept_locked(struct vmm_machine *, uint8_t *);
int vmm_x64_pic_io(struct vmm_machine *, struct vmm_cpustate *,
	const struct vmm_cpuexit_io *);

#endif /* VMM_X64_PIC_H */
