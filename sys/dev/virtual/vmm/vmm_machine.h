/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core: opaque machine allocation and lifetime entry points.  The machine
 * implementation is independent of the VFS frontend.
 */
#ifndef VMM_MACHINE_H
#define VMM_MACHINE_H

struct vmm_machine;

int vmm_machine_create(struct vmm_machine **machine);
int vmm_machine_destroy(struct vmm_machine *machine);

#endif /* VMM_MACHINE_H */
