/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core vCPU model -- see vmm_vcpu.h.
 */
#include <sys/errno.h>
#include <sys/malloc.h>

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
	error = vc->backend_ops->vcpu_create(vc);
	if (error != 0) {
		kfree(vc, M_VMM);
		return error;
	}
	lwkt_gettoken(&machine->token);
	if (machine->next_vcpu_id == (unsigned int)-1) {
		lwkt_reltoken(&machine->token);
		vc->backend_ops->vcpu_destroy(vc);
		kfree(vc, M_VMM);
		return EOVERFLOW;
	}
	vc->id = machine->next_vcpu_id++;
	++machine->vcpu_count;
	lwkt_reltoken(&machine->token);
	*vcpu = vc;
	return 0;
}

int
vmm_vcpu_run(vmm_vcpu_t vcpu, struct vmm_cpuexit **reason)
{
	struct vmm_machine *machine;
	int error;

	if (vcpu == NULL || reason == NULL)
		return EINVAL;

	*reason = NULL;
	machine = vcpu->machine;
	lwkt_gettoken(&machine->token);
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running) {
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

	error = vcpu->backend_ops->vcpu_run(vcpu, reason);

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
vmm_vcpu_kick(vmm_vcpu_t vcpu)
{
	if (vcpu == NULL)
		return EINVAL;

	lwkt_gettoken(&vcpu->token);
	if (!vcpu->running) {
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
	if (vcpu->running) {
		lwkt_reltoken(&vcpu->token);
		lwkt_reltoken(&machine->token);
		return EBUSY;
	}
	--machine->vcpu_count;
	lwkt_reltoken(&vcpu->token);
	lwkt_reltoken(&machine->token);
	vcpu->backend_ops->vcpu_destroy(vcpu);
	kfree(vcpu, M_VMM);
	return 0;
}
