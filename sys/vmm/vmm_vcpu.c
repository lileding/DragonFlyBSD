/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The vcpu object -- see vmm_vcpu.h.
 */
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>

#include "vmm_parse.h"
#include "vmm_host.h"
#include "vmm_loader.h"
#include "vmm_loader_x86.h"
#include "vmm_machine.h"
#include "vmm_vcpu.h"

#define VMM_VCPU_MAX	256u

SET_DECLARE(vmm_vcpu_backend_set, const struct vmm_vcpu_backend_ops);

static const struct vmm_vcpu_backend_ops *vmm_backend;

static void vmm_vcpu_thread_main(void *arg);
static int vmm_vcpu_backend_ready(struct vmm_vcpu *vc);

int
vmm_backend_probe(void)
{
	const struct vmm_vcpu_backend_ops **ops;
	const char *reason = "no_backend";
	const char *name = "none";

	if (vmm_backend != NULL)
		return 0;
	SET_FOREACH(ops, vmm_vcpu_backend_set) {
		reason = (*ops)->probe();
		if (reason == NULL) {
			if ((*ops)->init != NULL) {
				int error = (*ops)->init();

				if (error != 0) {
					kprintf("vmm tsc=%020ju backend init failed name=%s error=%d\n",
					    (uintmax_t)rdtsc(), (*ops)->imm_name, error);
					return error;
				}
			}
			vmm_backend = *ops;
			vmm_debug_trace("backend selected name=%s", vmm_backend->imm_name);
			return 0;
		}
		name = (*ops)->imm_name;
	}
	kprintf("vmm tsc=%020ju no usable vcpu backend name=%s reason=%s\n",
	    (uintmax_t)rdtsc(), name, reason);
	return ENXIO;
}

void
vmm_backend_uninit(void)
{
	if (vmm_backend == NULL)
		return;
	if (vmm_backend->uninit != NULL)
		vmm_backend->uninit();
	vmm_backend = NULL;
}

int
vmm_vcpus_parse(struct vmm_vcpus *vcpus, const char *buf, size_t len)
{
	size_t tl;
	const char *t = vmm_trim(buf, len, &tl);
	uint64_t n;

	if (vcpus->own_mut_vcpus != NULL ||
	    atomic_load_acq_int(&vcpus->atomic_mut_active_count) != 0)
		return 0;
	if (!vmm_parse_decimal(t, tl, &n) || n < 1 || n > VMM_VCPU_MAX)
		return 0;
	vcpus->mut_count = (uint32_t)n;
	return 1;
}

size_t
vmm_vcpus_format(const struct vmm_vcpus *vcpus, char *out, size_t cap)
{
	return vcpus->mut_count == 0 ? 0 :
	    vmm_write_decimal(vcpus->mut_count, out, cap);
}

int
vmm_vcpus_is_set(const struct vmm_vcpus *vcpus)
{
	return vcpus->mut_count != 0;
}

int
vmm_vcpus_create(struct vmm_machine *m, uint32_t count,
    const struct vmm_launch *launch)
{
	struct vmm_vcpus *vcpus = &m->own_mut_vcpus;
	const struct vmm_vcpu_backend_ops *backend_ops;
	void *backend_context = NULL;
	int error;

	if (count == 0 || launch == NULL ||
	    launch->imm_cpu_topology.imm_vcpu_count != count)
		return EINVAL;
	backend_ops = vmm_backend;
	if (backend_ops == NULL)
		return ENXIO;
	if (vcpus->own_mut_vcpus != NULL ||
	    atomic_load_acq_int(&vcpus->atomic_mut_active_count) != 0)
		return EBUSY;
	error = backend_ops->context_create(m, count, launch, &backend_context);
	if (error != 0)
		return error;
	vcpus->own_mut_vcpus = kmalloc(sizeof(*vcpus->own_mut_vcpus) * count,
	    M_TEMP, M_WAITOK | M_ZERO);
	if (vcpus->own_mut_vcpus == NULL) {
		backend_ops->context_destroy(backend_context);
		return ENOMEM;
	}
	vcpus->borrow_imm_backend_ops = backend_ops;
	vcpus->own_mut_backend_context = backend_context;
	vcpus->borrow_imm_launch = launch;
	vcpus->mut_count = count;
	atomic_store_rel_int(&vcpus->atomic_mut_stop_requested, 0);
	atomic_store_rel_int(&vcpus->atomic_mut_exit_reason, VMM_VCPU_EXIT_NONE);
	atomic_store_rel_int(&vcpus->atomic_mut_backend_ready_count, 0);
	atomic_store_rel_int(&vcpus->atomic_mut_start_failed, 0);
	return 0;
}

int
vmm_vcpu_start(struct vmm_vcpu *vc)
{
	struct vmm_machine *m = vc->borrow_imm_machine;
	struct vmm_vcpus *vcpus = &m->own_mut_vcpus;
	int error;

	atomic_add_int(&vcpus->atomic_mut_active_count, 1);
	error = lwkt_create(vmm_vcpu_thread_main, vc, &vc->borrow_mut_thread,
	    NULL, 0, vc->imm_cpu, "vmmvcpu%u", vc->imm_id);
	if (error != 0)
		atomic_add_int(&vcpus->atomic_mut_active_count, -1);
	return error;
}

void
vmm_vcpus_request_stop(struct vmm_machine *m)
{
	struct vmm_vcpus *vcpus = &m->own_mut_vcpus;
	uint32_t i;

	atomic_store_rel_int(&vcpus->atomic_mut_stop_requested, 1);
	if (vcpus->own_mut_vcpus == NULL)
		return;
	for (i = 0; i < vcpus->mut_count; i++)
		wakeup(&vcpus->own_mut_vcpus[i]);
	wakeup(vcpus);
}

void
vmm_vcpus_stop(struct vmm_machine *m)
{
	struct vmm_vcpus *vcpus = &m->own_mut_vcpus;

	vmm_vcpus_request_stop(m);
	while (atomic_load_acq_int(&vcpus->atomic_mut_active_count) != 0)
		tsleep(m, 0, "vmmstp", hz / 20 + 1);
}

void
vmm_vcpu_report_started(struct vmm_vcpu *vc)
{
	struct vmm_machine *m = vc->borrow_imm_machine;

	if (!atomic_cmpset_int(&vc->atomic_mut_start_reported, 0, 1))
		return;
	KKASSERT(atomic_load_acq_int(&m->atomic_mut_start_wait_count) != 0);
	if (atomic_fetchadd_int(&m->atomic_mut_start_wait_count, -1) == 1)
		wakeup(m);
}

int
vmm_vcpu_should_stop(const struct vmm_vcpu *vc)
{
	return atomic_load_acq_int(
	    &vc->borrow_imm_machine->own_mut_vcpus.atomic_mut_stop_requested) != 0;
}

void
vmm_vcpus_console_input_locked(struct vmm_machine *m)
{
	struct vmm_vcpus *vcpus = &m->own_mut_vcpus;
	uint32_t i;

	if (vcpus->own_mut_vcpus == NULL ||
	    atomic_load_acq_int(&vcpus->atomic_mut_active_count) == 0)
		return;
	for (i = 0; i < vcpus->mut_count; i++) {
		struct vmm_vcpu *vc = &vcpus->own_mut_vcpus[i];

		if (vc->borrow_imm_backend_ops != NULL &&
		    vc->borrow_imm_backend_ops->console_input != NULL &&
		    vc->own_mut_backend != NULL) {
			vc->borrow_imm_backend_ops->console_input(vc->own_mut_backend, vc);
			wakeup(vc);
		}
	}
}

void
vmm_vcpus_interrupt_locked(struct vmm_machine *m, uint8_t vector)
{
	struct vmm_vcpus *vcpus = &m->own_mut_vcpus;
	struct vmm_vcpu *vc;

	if (vcpus->own_mut_vcpus == NULL ||
	    atomic_load_acq_int(&vcpus->atomic_mut_active_count) == 0)
		return;
	vc = &vcpus->own_mut_vcpus[0];
	if (vc->borrow_imm_backend_ops == NULL ||
	    vc->borrow_imm_backend_ops->interrupt == NULL ||
	    vc->own_mut_backend == NULL)
		return;
	vc->borrow_imm_backend_ops->interrupt(vc->own_mut_backend, vc, vector);
	wakeup(vc);
}

void
vmm_vcpus_uninit(struct vmm_vcpus *vcpus, struct vmm_vcpu **vcpusp)
{
	*vcpusp = NULL;
	if (vcpus->own_mut_vcpus != NULL &&
	    atomic_load_acq_int(&vcpus->atomic_mut_active_count) == 0) {
		*vcpusp = vcpus->own_mut_vcpus;
		vcpus->own_mut_vcpus = NULL;
	}
	atomic_store_rel_int(&vcpus->atomic_mut_stop_requested, 0);
}

void
vmm_vcpus_release(struct vmm_vcpus *vcpus, struct vmm_vcpu *vcpus_array,
    uint32_t count)
{
	const struct vmm_vcpu_backend_ops *backend_ops;
	void *backend_context;
	uint32_t i;

	backend_ops = vcpus->borrow_imm_backend_ops;
	backend_context = vcpus->own_mut_backend_context;
	vcpus->borrow_imm_backend_ops = NULL;
	vcpus->own_mut_backend_context = NULL;
	vcpus->borrow_imm_launch = NULL;
	for (i = 0; i < count; i++) {
		if (vcpus_array != NULL && backend_ops != NULL &&
		    vcpus_array[i].own_mut_backend != NULL)
			backend_ops->vcpu_destroy(vcpus_array[i].own_mut_backend);
	}
	if (backend_ops != NULL)
		backend_ops->context_destroy(backend_context);
	if (vcpus_array != NULL)
		kfree(vcpus_array, M_TEMP);
}

static void
vmm_vcpu_thread_main(void *arg)
{
	struct vmm_vcpu *vc = arg;
	struct vmm_machine *m = vc->borrow_imm_machine;
	struct vmm_vcpus *vcpus = &m->own_mut_vcpus;
	const struct vmm_vcpu_backend_ops *backend_ops = vc->borrow_imm_backend_ops;
	enum vmm_vcpu_exit_reason exit_reason = VMM_VCPU_EXIT_NONE;
	u_int active_count;
	int error = ENXIO;

	lwkt_setpri_self(TDPRI_USER_NORM);
	if (backend_ops != NULL) {
		error = backend_ops->vcpu_create(vcpus->own_mut_backend_context,
		    vcpus->borrow_imm_launch, vc, &vc->own_mut_backend);
	}
	if (error != 0) {
		atomic_store_rel_int(&vcpus->atomic_mut_start_failed, 1);
		vmm_machine_vcpu_start_failed(m);
		wakeup(vcpus);
		/* A failed LWKT still completes the machine start barrier. */
		vmm_vcpu_report_started(vc);
	} else {
		/*
		 * All LWKT backends exist before the machine is published RUNNING.
		 * An AP may then remain in WAIT_SIPI while the BSP starts executing;
		 * waiting for an AP's first VMRUN here would deadlock that startup.
		 */
		if (vmm_vcpu_backend_ready(vc) != 0) {
			vmm_vcpu_report_started(vc);
			exit_reason = backend_ops->run(vc->own_mut_backend, vc);
		} else {
			vmm_vcpu_report_started(vc);
		}
	}
	if (exit_reason != VMM_VCPU_EXIT_NONE) {
		if (atomic_load_acq_int(&m->atomic_mut_status) == VMM_MACHINE_STARTING)
			vmm_machine_vcpu_start_failed(m);
		else
			vmm_machine_vcpu_terminal(m, exit_reason);
	}
	if (backend_ops != NULL && vc->own_mut_backend != NULL)
		backend_ops->vcpu_destroy(vc->own_mut_backend);
	vc->own_mut_backend = NULL;
	vc->borrow_imm_backend_ops = NULL;

	active_count = atomic_fetchadd_int(&vcpus->atomic_mut_active_count, -1) - 1;
	if (active_count == 0) {
		wakeup(m);
		vmm_machine_vcpu_drained(m);
	} else {
		wakeup(m);
	}
}

static int
vmm_vcpu_backend_ready(struct vmm_vcpu *vc)
{
	struct vmm_vcpus *vcpus = &vc->borrow_imm_machine->own_mut_vcpus;

	if (atomic_load_acq_int(&vcpus->atomic_mut_start_failed) != 0)
		return 0;
	if (atomic_fetchadd_int(&vcpus->atomic_mut_backend_ready_count, 1) + 1 ==
	    vcpus->mut_count) {
		wakeup(vcpus);
		return 1;
	}
	for (;;) {
		tsleep_interlock(vcpus, 0);
		if (vmm_vcpu_should_stop(vc) ||
		    atomic_load_acq_int(&vcpus->atomic_mut_start_failed) != 0)
			return 0;
		if (atomic_load_acq_int(&vcpus->atomic_mut_backend_ready_count) ==
		    vcpus->mut_count)
			return 1;
		tsleep(vcpus, PINTERLOCKED, "vmmvcpus", 0);
	}
}
