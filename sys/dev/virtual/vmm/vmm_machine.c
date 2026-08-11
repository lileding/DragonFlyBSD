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
#include "vmm_io.h"
#include "vmm_machine.h"

MALLOC_DEFINE(M_VMM, "vmm", "vmm runtime objects");

int
vmm_machine_create_irqchip(vmm_machine_t machine)
{
	int error;

	if (machine == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || machine->vcpu_count != 0 ||
	    machine->run_count != 0) {
		lwkt_reltoken(&machine->token);
		return EBUSY;
	}
	if (machine->irqchip) {
		lwkt_reltoken(&machine->token);
		return EALREADY;
	}
	if (machine->backend->machine_create_irqchip == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOTSUP;
	}
	error = machine->backend->machine_create_irqchip(machine);
	if (error == 0)
		machine->irqchip = true;
	lwkt_reltoken(&machine->token);
	return error;
}

bool
vmm_irqchip_available(void)
{
	const struct vmm_backend_ops *backend;
	bool available;

	lwkt_gettoken(&vmm_token);
	backend = vmm_backend;
	available = !vmm_draining && backend != NULL &&
	    backend->irqchip_available != NULL && backend->irqchip_available();
	lwkt_reltoken(&vmm_token);
	return available;
}

int
vmm_machine_raise_msi(vmm_machine_t machine, uint64_t address, uint32_t data)
{
	int error;

	if (machine == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || !machine->irqchip ||
	    machine->backend->irq_raise_msi == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOTSUP;
	}
	error = machine->backend->irq_raise_msi(machine, address, data);
	lwkt_reltoken(&machine->token);
	return error;
}

int
vmm_machine_raise_irq(vmm_machine_t machine, uint32_t gsi)
{
	int error;

	if (machine == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || !machine->irqchip ||
	    machine->backend->machine_raise_irq == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOTSUP;
	}
	error = machine->backend->machine_raise_irq(machine, gsi);
	lwkt_reltoken(&machine->token);
	return error;
}

int
vmm_machine_set_irq(vmm_machine_t machine, uint32_t gsi, bool level)
{
	int error;

	if (machine == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || !machine->irqchip ||
	    machine->backend->machine_set_irq == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOTSUP;
	}
	error = machine->backend->machine_set_irq(machine, gsi, level);
	lwkt_reltoken(&machine->token);
	return error;
}

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
	TAILQ_INIT(&m->io_list);
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
	if (machine->destroying || machine->vcpu_count != 0 ||
	    machine->run_count != 0 || !TAILQ_EMPTY(&machine->io_list)) {
		lwkt_reltoken(&machine->token);
		return EBUSY;
	}
	machine->destroying = true;
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
