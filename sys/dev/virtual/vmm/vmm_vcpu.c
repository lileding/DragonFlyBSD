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
#include "vmm_internal.h"
#include "vmm_io.h"
#include "vmm_vcpu.h"
#include "x64/vmm_x64.h"
#include "x64/vmm_x64_emul.h"

int
vmm_vcpu_create(vmm_machine_t machine, struct vmm_cpustate *state,
	vmm_vcpu_t *vcpu)
{
	struct vmm_vcpu *vc;
	int error;

	if (machine == NULL || vcpu == NULL)
		return EINVAL;

	*vcpu = NULL;
	vc = kmalloc(sizeof(*vc), M_VMM, M_WAITOK | M_ZERO);
	if (vc == NULL)
		return ENOMEM;

	vc->machine = machine;
	vc->backend_ops = machine->backend;
	if (state != NULL) {
		vc->state = state;
	} else {
		vc->state = kmalloc(sizeof(*vc->state), M_VMM,
		    M_WAITOK | M_ZERO);
		if (vc->state == NULL) {
			kfree(vc, M_VMM);
			return ENOMEM;
		}
		vc->state_allocated = true;
	}
	vc->memory_exit_mode = VMM_MEMORY_EXIT_RAW;
	lwkt_token_init(&vc->token, "vmmvcpu");
	error = vmm_x64_emul_init(vc);
	if (error != 0) {
		if (vc->state_allocated)
			kfree(vc->state, M_VMM);
		kfree(vc, M_VMM);
		return error;
	}
	lwkt_gettoken(&machine->token);
	if (machine->destroying ||
	    machine->next_vcpu_id == (unsigned int)-1) {
		error = EOVERFLOW;
	} else if (state == NULL && !machine->irqchip) {
		error = EINVAL;
	} else {
		vc->id = machine->next_vcpu_id++;
		++machine->vcpu_count;
		error = 0;
	}
	lwkt_reltoken(&machine->token);
	if (error != 0) {
		vmm_x64_emul_uninit(vc);
		if (vc->state_allocated)
			kfree(vc->state, M_VMM);
		kfree(vc, M_VMM);
		return error;
	}
	error = vc->backend_ops->vcpu_create(vc);
	if (error != 0) {
		lwkt_gettoken(&machine->token);
		KKASSERT(machine->vcpu_count > 0);
		--machine->vcpu_count;
		lwkt_reltoken(&machine->token);
		vmm_x64_emul_uninit(vc);
		if (vc->state_allocated)
			kfree(vc->state, M_VMM);
		kfree(vc, M_VMM);
		return error;
	}
	*vcpu = vc;
	return 0;
}

int
vmm_vcpu_set_memory_exit_mode(vmm_vcpu_t vcpu,
	enum vmm_memory_exit_mode mode)
{

	if (vcpu == NULL || (mode != VMM_MEMORY_EXIT_RAW &&
	    mode != VMM_MEMORY_EXIT_EMULATE))
		return EINVAL;

	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	vcpu->memory_exit_mode = mode;
	lwkt_reltoken(&vcpu->token);
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
vmm_vcpu_translate(vmm_vcpu_t vcpu, uint64_t gva, uint64_t *gpa)
{
	int error;

	if (vcpu == NULL || gpa == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying)
		error = EBUSY;
	else
		error = vmm_x64_translate(vcpu, gva, gpa);
	lwkt_reltoken(&vcpu->token);
	return error;
}

int
vmm_vcpu_get_lapic(vmm_vcpu_t vcpu, void *registers, size_t size)
{
	int error;

	if (vcpu == NULL || registers == NULL ||
	    size != VMM_X64_LAPIC_STATE_SIZE)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	if (vcpu->backend_ops->vcpu_get_lapic == NULL) {
		lwkt_reltoken(&vcpu->token);
		return ENOTSUP;
	}
	error = vcpu->backend_ops->vcpu_get_lapic(vcpu, registers, size);
	lwkt_reltoken(&vcpu->token);
	return error;
}

int
vmm_vcpu_set_lapic(vmm_vcpu_t vcpu, const void *registers, size_t size)
{
	int error;

	if (vcpu == NULL || registers == NULL ||
	    size != VMM_X64_LAPIC_STATE_SIZE)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying) {
		lwkt_reltoken(&vcpu->token);
		return EBUSY;
	}
	if (vcpu->backend_ops->vcpu_set_lapic == NULL) {
		lwkt_reltoken(&vcpu->token);
		return ENOTSUP;
	}
	error = vcpu->backend_ops->vcpu_set_lapic(vcpu, registers, size);
	lwkt_reltoken(&vcpu->token);
	return error;
}

int
vmm_vcpu_run(vmm_vcpu_t vcpu, struct vmm_cpuexit **reason)
{
	struct vmm_machine *machine;
	struct vmm_cpuexit *exit;
	bool ran_backend;
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

	ran_backend = false;
	error = vmm_x64_emul_resume(vcpu);
	if (error == EINPROGRESS) {
		*reason = &vcpu->exit;
		error = 0;
		goto out;
	}
	if (error != 0 && error != ENOENT)
		goto out;
	vcpu->backend_ops->vcpu_setstate(vcpu);
	for (;;) {
		ran_backend = true;
		error = vcpu->backend_ops->vcpu_run(vcpu, reason);
		if (error != 0 || *reason == NULL) {
			break;
		}
		exit = *reason;
		if (exit->reason == VMM_CPUEXIT_IO) {
			vcpu->backend_ops->vcpu_getstate(vcpu);
			error = ENOENT;
			if (vcpu->backend_ops->vcpu_io != NULL)
				error = vcpu->backend_ops->vcpu_io(vcpu, &exit->u.io);
			if (error == 0) {
				vcpu->backend_ops->vcpu_setstate(vcpu);
				continue;
			}
			if (error != ENOENT)
				break;
			error = vmm_io_handle_pio(vcpu, exit);
			if (error == 0) {
				vcpu->backend_ops->vcpu_setstate(vcpu);
				continue;
			}
			if (error == ENOENT) {
				error = 0;
				break;
			}
			break;
		}
		if (exit->reason != VMM_CPUEXIT_MEMORY)
			break;
		fault_error = vm_fault(&machine->vmspace->vm_map,
		    trunc_page(exit->u.mem.gpa), exit->u.mem.prot,
		    (exit->u.mem.prot & VM_PROT_WRITE) ?
		    VM_FAULT_DIRTY : VM_FAULT_NORMAL);
		if (fault_error == KERN_SUCCESS) {
			vcpu->backend_ops->vcpu_memory_mapping_changed(vcpu);
			continue;
		}
		if (vcpu->memory_exit_mode == VMM_MEMORY_EXIT_RAW) {
			error = 0;
			break;
		}
		/* Only an unbacked GPA needs state transfer and instruction fetch. */
		vcpu->backend_ops->vcpu_getstate(vcpu);
		if (exit->u.mem.inst_len == 0) {
			exit->u.mem.inst_len = (uint8_t)vmm_x64_fetch_instruction(vcpu,
			    exit->u.mem.inst_bytes,
			    sizeof(exit->u.mem.inst_bytes));
		}
		error = vmm_x64_emul_memory(vcpu, exit);
		if (error == 0) {
			vcpu->backend_ops->vcpu_setstate(vcpu);
			continue;
		}
		if (error == EINPROGRESS) {
			*reason = &vcpu->exit;
			error = 0;
		}
		break;
	}
out:
	if (ran_backend)
		vcpu->backend_ops->vcpu_getstate(vcpu);

	lwkt_gettoken(&machine->token);
	lwkt_gettoken(&vcpu->token);
	vcpu->running = 0;
	--machine->run_count;
	/*
	 * A kick only interrupts a run that has no architecturally observable
	 * exit.  A concurrent kick must not discard a terminal VMEXIT such as
	 * HLT: the caller would retry at the next instruction and lose the
	 * guest's stopped state.
	 */
	if (error == 0 && atomic_swap_int(&vcpu->kick_pending, 0) != 0 &&
	    (*reason == NULL || (*reason)->reason == VMM_CPUEXIT_NONE))
		error = EINTR;
	lwkt_reltoken(&vcpu->token);
	lwkt_reltoken(&machine->token);
	vmm_stat_vcpu_run_return();
	return error;
}

int
vmm_vcpu_complete_mmio_write(vmm_vcpu_t vcpu)
{
	int error;

	if (vcpu == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying)
		error = EBUSY;
	else
		error = vmm_x64_emul_complete_write(vcpu);
	lwkt_reltoken(&vcpu->token);
	return error;
}

int
vmm_vcpu_complete_mmio_read(vmm_vcpu_t vcpu, const void *data, size_t size)
{
	int error;

	if (vcpu == NULL || data == NULL)
		return EINVAL;
	lwkt_gettoken(&vcpu->token);
	if (vcpu->running || vcpu->destroying)
		error = EBUSY;
	else
		error = vmm_x64_emul_complete_read(vcpu, data, size);
	lwkt_reltoken(&vcpu->token);
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
	atomic_store_rel_int(&vcpu->kick_pending, 1);
	lwkt_reltoken(&vcpu->token);
	wakeup(vcpu);
	return 0;
}

int
vmm_vcpu_kick(vmm_vcpu_t vcpu)
{
	int running;

	if (vcpu == NULL)
		return EINVAL;

	lwkt_gettoken(&vcpu->token);
	if (vcpu->destroying) {
		lwkt_reltoken(&vcpu->token);
		return EALREADY;
	}
	atomic_store_rel_int(&vcpu->kick_pending, 1);
	running = vcpu->running;
	lwkt_reltoken(&vcpu->token);
	if (running)
		vcpu->backend_ops->vcpu_kick(vcpu);
	/*
	 * A backend kick interrupts VMRUN, while this wakeup covers a vCPU
	 * parked in a backend-owned pre-entry wait such as AP SIPI.
	 */
	wakeup(vcpu);
	return 0;
}

int
vmm_vcpu_wait(vmm_vcpu_t vcpu)
{
	int error;

	if (vcpu == NULL)
		return EINVAL;

	lwkt_gettoken(&vcpu->token);
	if (vcpu->destroying) {
		error = EALREADY;
	} else if (vcpu->running) {
		error = EBUSY;
	} else if (atomic_swap_int(&vcpu->kick_pending, 0) != 0) {
		error = 0;
	} else {
		/* vcpu->token serializes the condition with kick's wakeup. */
		tsleep_interlock(vcpu, 0);
		lwkt_reltoken(&vcpu->token);
		return tsleep(vcpu, PINTERLOCKED, "vmmhlt", 0);
	}
	lwkt_reltoken(&vcpu->token);
	return error;
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
	if (machine->destroying || vcpu->running || vcpu->destroying) {
		lwkt_reltoken(&vcpu->token);
		lwkt_reltoken(&machine->token);
		return EBUSY;
	}
	vcpu->destroying = 1;
	lwkt_reltoken(&vcpu->token);
	lwkt_reltoken(&machine->token);
	vcpu->backend_ops->vcpu_destroy(vcpu);
	vmm_x64_emul_uninit(vcpu);
	lwkt_gettoken(&machine->token);
	KKASSERT(machine->vcpu_count > 0);
	--machine->vcpu_count;
	lwkt_reltoken(&machine->token);
	if (vcpu->state_allocated)
		kfree(vcpu->state, M_VMM);
	kfree(vcpu, M_VMM);
	return 0;
}
