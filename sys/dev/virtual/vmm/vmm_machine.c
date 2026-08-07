/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine model -- see vmm_machine.h.
 */
#include <sys/kernel.h>
#include <sys/errno.h>
#include <sys/malloc.h>

#include "vmm_machine.h"

struct vmm_machine {
};

MALLOC_DEFINE(M_VMM, "vmm", "vmm runtime objects");

int
vmm_machine_create(struct vmm_machine **machine)
{
	struct vmm_machine *m;

	if (machine == NULL)
		return EINVAL;

	*machine = NULL;
	m = kmalloc(sizeof(*m), M_VMM, M_WAITOK | M_ZERO);
	if (m == NULL)
		return ENOMEM;

	*machine = m;
	return 0;
}

int
vmm_machine_destroy(struct vmm_machine *machine)
{
	if (machine == NULL)
		return EINVAL;

	kfree(machine, M_VMM);
	return 0;
}
