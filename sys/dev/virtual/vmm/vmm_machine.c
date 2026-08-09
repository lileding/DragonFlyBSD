/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine model -- see vmm_machine.h.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>

#include "vmm_internal.h"
#include "vmm_machine.h"

MALLOC_DEFINE(M_VMM, "vmm", "vmm runtime objects");

int
vmm_machine_create(struct vmspace *vmspace, vmm_machine_t *machine)
{
	struct vmm_machine *m;
	const struct vmm_backend_ops *backend;
	int error;

	if (vmspace == NULL || machine == NULL)
		return EINVAL;

	*machine = NULL;
	lwkt_gettoken(&vmm_token);
	if (vmm_draining) {
		lwkt_reltoken(&vmm_token);
		return EBUSY;
	}
	backend = vmm_backend;
	if (backend == NULL) {
		lwkt_reltoken(&vmm_token);
		return ENXIO;
	}
	++vmm_machine_count;
	lwkt_reltoken(&vmm_token);

	m = kmalloc(sizeof(*m), M_VMM, M_WAITOK | M_ZERO);
	if (m == NULL) {
		error = ENOMEM;
		goto fail_count;
	}

	lwkt_token_init(&m->token, "vmmmach");
	m->backend = backend;
	m->vmspace = vmspace;
	pmap_maybethreaded(&vmspace->vm_pmap);
	error = backend->machine_create(m);
	if (error != 0) {
		kfree(m, M_VMM);
		goto fail_count;
	}
	*machine = m;
	return 0;

fail_count:
	lwkt_gettoken(&vmm_token);
	KKASSERT(vmm_machine_count > 0);
	--vmm_machine_count;
	lwkt_reltoken(&vmm_token);
	return error;
}

int
vmm_machine_destroy(vmm_machine_t machine)
{
	if (machine == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->vcpu_count != 0 || machine->run_count != 0) {
		lwkt_reltoken(&machine->token);
		return EBUSY;
	}
	lwkt_reltoken(&machine->token);

	machine->backend->machine_destroy(machine);
	pmap_del_all_cpus(machine->vmspace);
	kfree(machine, M_VMM);

	lwkt_gettoken(&vmm_token);
	KKASSERT(vmm_machine_count > 0);
	--vmm_machine_count;
	lwkt_reltoken(&vmm_token);
	return 0;
}
