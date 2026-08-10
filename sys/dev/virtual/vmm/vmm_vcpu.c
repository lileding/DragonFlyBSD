/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core vCPU model -- see vmm_vcpu.h.
 */
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/mman.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_param.h>

#include "vmm_backend.h"
#include "vmm_vcpu.h"

int
vmm_vcpu_create(vmm_machine_t machine, struct vmm_cpustate *state,
	vmm_vcpu_t *vcpu)
{
	struct vmm_vcpu *vc;
	int error;

	if (machine == NULL || state == NULL || vcpu == NULL)
		return EINVAL;

	*vcpu = NULL;
	vc = kmalloc(sizeof(*vc), M_VMM, M_WAITOK | M_ZERO);
	if (vc == NULL)
		return ENOMEM;

	vc->machine = machine;
	vc->backend_ops = machine->backend;
	vc->state = state;
	lwkt_token_init(&vc->token, "vmmvcpu");
	lwkt_gettoken(&machine->token);
	if (machine->next_vcpu_id == (unsigned int)-1) {
		lwkt_reltoken(&machine->token);
		kfree(vc, M_VMM);
		return EOVERFLOW;
	}
	vc->id = machine->next_vcpu_id++;
	++machine->vcpu_count;
	lwkt_reltoken(&machine->token);
	error = vc->backend_ops->vcpu_create(vc);
	if (error != 0) {
		lwkt_gettoken(&machine->token);
		KKASSERT(machine->vcpu_count > 0);
		--machine->vcpu_count;
		lwkt_reltoken(&machine->token);
		kfree(vc, M_VMM);
		return error;
	}
	*vcpu = vc;
	return 0;
}

int
vmm_vcpu_set_cpuid(vmm_vcpu_t vcpu,
	const struct vmm_cpuid_entry *entries, size_t entry_count)
{
	int error;

	if (vcpu == NULL || (entry_count != 0 && entries == NULL))
		return EINVAL;

	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	if (vcpu->backend_ops->vcpu_set_cpuid == NULL) {
		lwkt_reltoken(&vcpu->token);
		return ENOTSUP;
	}
	error = vcpu->backend_ops->vcpu_set_cpuid(vcpu, entries, entry_count);
	lwkt_reltoken(&vcpu->token);
	return error;
}

int
vmm_vcpu_run(vmm_vcpu_t vcpu, struct vmm_cpuexit **reason)
{
	struct vmm_machine *machine;
	struct vmm_cpuexit *exit;
	int fault_error;
	int error;

	if (vcpu == NULL || reason == NULL)
		return EINVAL;

	*reason = NULL;
	machine = vcpu->machine;
	lwkt_gettoken(&machine->token);
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying) {
		error = EBUSY;
	} else {
		vcpu->running = 1;
		++machine->run_count;
		error = 0;
	}
	lwkt_reltoken(&vcpu->token);
	lwkt_reltoken(&machine->token);
	if (error != 0)
		return error;

	for (;;) {
		error = vcpu->backend_ops->vcpu_run(vcpu, reason);
		if (error != 0 || *reason == NULL ||
		    (*reason)->reason != VMM_CPUEXIT_MEMORY) {
			break;
		}
		exit = *reason;
		fault_error = vm_fault(&machine->vmspace->vm_map,
		    trunc_page(exit->u.mem.gpa), exit->u.mem.prot,
		    (exit->u.mem.prot & VM_PROT_WRITE) ?
		    VM_FAULT_DIRTY : VM_FAULT_NORMAL);
		if (fault_error != KERN_SUCCESS)
			break;
	}
	vcpu->backend_ops->vcpu_getstate(vcpu);

	lwkt_gettoken(&machine->token);
	lwkt_gettoken(&vcpu->token);
	vcpu->running = 0;
	--machine->run_count;
	if (error == 0 && atomic_swap_int(&vcpu->kick_pending, 0) != 0) {
		error = EINTR;
	}
	lwkt_reltoken(&vcpu->token);
	lwkt_reltoken(&machine->token);
	return error;
}

int
vmm_vcpu_inject(vmm_vcpu_t vcpu, const struct vmm_cpuevent *event)
{
	if (vcpu == NULL || event == NULL)
		return EINVAL;
	if (event->type != VMM_CPUEVENT_EXCP &&
	    event->type != VMM_CPUEVENT_INTR)
		return EINVAL;

	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying || vcpu->event_pending) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	vcpu->event = *event;
	vcpu->event_pending = 1;
	lwkt_reltoken(&vcpu->token);
	return 0;
}

int
vmm_vcpu_kick(vmm_vcpu_t vcpu)
{
	if (vcpu == NULL)
		return EINVAL;

	lwkt_gettoken(&vcpu->token);
	if (!vcpu->running || vcpu->destroying) {
		lwkt_reltoken(&vcpu->token);
		return EALREADY;
	}
	atomic_store_rel_int(&vcpu->kick_pending, 1);
	lwkt_reltoken(&vcpu->token);
	vcpu->backend_ops->vcpu_kick(vcpu);
	return 0;
}

int
vmm_vcpu_destroy(vmm_vcpu_t vcpu)
{
	struct vmm_machine *machine;

	if (vcpu == NULL)
		return EINVAL;

	machine = vcpu->machine;
	lwkt_gettoken(&machine->token);
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying) {
		lwkt_reltoken(&vcpu->token);
		lwkt_reltoken(&machine->token);
		return EBUSY;
	}
	vcpu->destroying = 1;
	lwkt_reltoken(&vcpu->token);
	lwkt_reltoken(&machine->token);
	vcpu->backend_ops->vcpu_destroy(vcpu);
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->vcpu_count > 0);
	--machine->vcpu_count;
	lwkt_reltoken(&machine->token);
	kfree(vcpu, M_VMM);
	return 0;
}
