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
#include "x64/vmm_x64_pic.h"
#include "x64/vmm_x64_pit.h"

MALLOC_DEFINE(M_VMM, "vmm", "vmm runtime objects");

int
vmm_machine_create_irqchip(vmm_machine_t machine)
{
	struct vmm_x64_pic *pic;
	int error;

	if (machine == NULL)
		return EINVAL;
	pic = vmm_x64_pic_alloc();
	if (pic == NULL)
		return ENOMEM;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || machine->vcpu_count != 0 ||
	    machine->run_count != 0) {
		lwkt_reltoken(&machine->token);
		vmm_x64_pic_free(pic);
		return EBUSY;
	}
	if (machine->irqchip) {
		lwkt_reltoken(&machine->token);
		vmm_x64_pic_free(pic);
		return EALREADY;
	}
	if (machine->backend->machine_create_irqchip == NULL) {
		lwkt_reltoken(&machine->token);
		vmm_x64_pic_free(pic);
		return ENOTSUP;
	}
	error = machine->backend->machine_create_irqchip(machine);
	if (error == 0) {
		machine->pic = pic;
		machine->irqchip = true;
		pic = NULL;
	}
	lwkt_reltoken(&machine->token);
	if (pic != NULL)
		vmm_x64_pic_free(pic);
	return error;
}

int
vmm_machine_create_pit(vmm_machine_t machine)
{

	if (machine == NULL)
		return EINVAL;
	return vmm_x64_pit_create(machine);
}

int
vmm_machine_get_pit(vmm_machine_t machine, struct vmm_pit_state *state)
{

	if (machine == NULL || state == NULL)
		return EINVAL;
	return vmm_x64_pit_get_state(machine, state);
}

int
vmm_machine_get_pic(vmm_machine_t machine, struct vmm_pic_state *state)
{

	if (machine == NULL || state == NULL)
		return EINVAL;
	return vmm_x64_pic_get_state(machine, state);
}

int
vmm_machine_set_pic(vmm_machine_t machine,
	const struct vmm_pic_state *state)
{

	if (machine == NULL || state == NULL)
		return EINVAL;
	return vmm_x64_pic_set_state(machine, state);
}

int
vmm_machine_set_pit(vmm_machine_t machine,
	const struct vmm_pit_state *state)
{

	if (machine == NULL || state == NULL)
		return EINVAL;
	return vmm_x64_pit_set_state(machine, state);
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

	error = vmm_machine_set_irq(machine, gsi, true);
	if (error != 0)
		return error;
	return vmm_machine_set_irq(machine, gsi, false);
}

int
vmm_machine_set_irq(vmm_machine_t machine, uint32_t gsi, bool level)
{
	int vector;
	int error;

	if (machine == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || !machine->irqchip ||
	    machine->backend->machine_set_irq == NULL ||
	    machine->backend->machine_raise_legacy == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOTSUP;
	}
	error = machine->backend->machine_set_irq(machine, gsi, level);
	if (error == 0 && gsi < 16) {
		/* ISA lines feed both IOAPIC and 8259 PIC, like a physical PC. */
		error = vmm_x64_pic_set_irq_machine_locked(machine, gsi, level,
		    &vector);
		if (error == 0 && vector >= 0)
			error = machine->backend->machine_raise_legacy(machine,
			    (uint8_t)vector);
	}
	lwkt_reltoken(&machine->token);
	return error;
}

int
vmm_machine_get_ioapic(vmm_machine_t machine, struct vmm_ioapic_state *state)
{
	int error;

	if (machine == NULL || state == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || !machine->irqchip ||
	    machine->backend->machine_get_ioapic == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOTSUP;
	}
	error = machine->backend->machine_get_ioapic(machine, state);
	lwkt_reltoken(&machine->token);
	return error;
}

int
vmm_machine_set_ioapic(vmm_machine_t machine,
	const struct vmm_ioapic_state *state)
{
	int error;

	if (machine == NULL || state == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || !machine->irqchip ||
	    machine->backend->machine_set_ioapic == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOTSUP;
	}
	error = machine->backend->machine_set_ioapic(machine, state);
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
vmm_machine_set_tsc(vmm_machine_t machine, uint64_t tsc)
{
	int error;

	if (machine == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || machine->backend->machine_set_tsc == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOTSUP;
	}
	error = machine->backend->machine_set_tsc(machine, tsc);
	lwkt_reltoken(&machine->token);
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

	vmm_x64_pit_destroy(machine);
	vmm_x64_pic_destroy(machine);
	machine->backend->machine_destroy(machine);
	kfree(machine, M_VMM);

	lwkt_gettoken(&vmm_token);
	KKASSERT(vmm_machine_count > 0);
	--vmm_machine_count;
	lwkt_reltoken(&vmm_token);
	return 0;
}
