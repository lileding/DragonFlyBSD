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
			vmm_debug_trace("backend selected name=%s",
			    vmm_backend->imm_name);
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
vmm_vcpu_parse(struct vmm_vcpu *v, const char *buf, size_t len)
{
	size_t tl;
	const char *t = vmm_trim(buf, len, &tl);
	uint64_t n;

	if (v->own_mut_threads != NULL ||
	    atomic_load_acq_int(&v->atomic_mut_active_count) != 0)
		return 0;
	if (!vmm_parse_decimal(t, tl, &n) || n < 1 || n > VMM_VCPU_MAX)
		return 0;
	v->mut_count = (uint32_t)n;
	return 1;
}

size_t
vmm_vcpu_format(const struct vmm_vcpu *v, char *out, size_t cap)
{
	return v->mut_count == 0 ? 0 : vmm_write_decimal(v->mut_count, out, cap);
}

int
vmm_vcpu_is_set(const struct vmm_vcpu *v)
{
	return v->mut_count != 0;
}

static void
vmm_vcpu_thread_main(void *arg)
{
	struct vmm_vcpu_thread *vc = arg;
	struct vmm_machine *m = vc->borrow_imm_machine;
	enum vmm_vcpu_exit_reason exit_reason = VMM_VCPU_EXIT_NONE;
	u_int active_count;

	lwkt_setpri_self(TDPRI_USER_NORM);
	vmm_debug_trace("vcpu%u thread enter machine=%p cpu=%d", vc->imm_id, m,
	    vc->imm_cpu);
	if (vc->borrow_imm_backend_ops != NULL)
		exit_reason = vc->borrow_imm_backend_ops->run(
		    vc->own_mut_backend, vc);
	if (exit_reason != VMM_VCPU_EXIT_NONE) {
		atomic_cmpset_int(&m->own_mut_vcpu.atomic_mut_exit_reason,
		    VMM_VCPU_EXIT_NONE, exit_reason);
	}

	active_count = atomic_fetchadd_int(
	    &m->own_mut_vcpu.atomic_mut_active_count, -1) - 1;
	vmm_debug_trace("vcpu%u thread exit machine=%p active=%u", vc->imm_id, m,
	    active_count);
	if (active_count == 0 && atomic_load_acq_int(
	    &m->own_mut_vcpu.atomic_mut_exit_reason) != VMM_VCPU_EXIT_NONE)
		vmm_machine_vcpu_exited(m);
	wakeup(m);
}

int
vmm_vcpu_start(struct vmm_machine *m, uint32_t count,
    const struct vmm_launch *launch)
{
	struct vmm_vcpu *v = &m->own_mut_vcpu;
	struct vmm_vcpu_thread *threads;
	const struct vmm_vcpu_backend_ops *backend_ops;
	void *backend_context = NULL;
	uint32_t i;
	int error = 0;

	if (count == 0)
		return EINVAL;
	if (launch == NULL ||
	    launch->imm_cpu_topology.imm_vcpu_count != count)
		return EINVAL;
	backend_ops = vmm_backend;
	if (backend_ops == NULL) {
		vmm_debug_trace("vcpu start failed machine=%p reason=no_backend", m);
		return ENXIO;
	}
	vmm_debug_trace("vcpu backend machine=%p name=%s count=%u", m,
	    backend_ops->imm_name, count);
	if (v->own_mut_threads != NULL ||
	    atomic_load_acq_int(&v->atomic_mut_active_count) != 0)
		return EBUSY;
	if (m->mut_status != VMM_MACHINE_STARTING)
		return ECANCELED;
	threads = kmalloc(sizeof(*threads) * count, M_TEMP, M_WAITOK | M_ZERO);

	error = backend_ops->context_create(m, count, launch, &backend_context);
	if (error != 0) {
		kfree(threads, M_TEMP);
		return error;
	}
	v->own_mut_threads = threads;
	v->borrow_imm_backend_ops = backend_ops;
	v->own_mut_backend_context = backend_context;
	v->mut_count = count;
	atomic_store_rel_int(&v->atomic_mut_stop_requested, 0);
	atomic_store_rel_int(&v->atomic_mut_exit_reason, VMM_VCPU_EXIT_NONE);

	/*
	 * Construct the complete backend topology before any vCPU can run.  A
	 * running BSP may otherwise route an early platform interrupt before its
	 * secondary APIC targets have been registered with the backend context.
	 */
	for (i = 0; i < count; i++) {
		struct vmm_vcpu_thread *vc = &v->own_mut_threads[i];
		int cpu = vmm_host_next_cpu();

		vc->borrow_imm_machine = m;
		vc->borrow_imm_backend_ops = backend_ops;
		vc->imm_id = i;
		vc->imm_cpu = cpu;
		vmm_debug_trace("vcpu%u create machine=%p backend=%s cpu=%d", i,
		    m, backend_ops->imm_name, cpu);
		error = backend_ops->vcpu_create(v->own_mut_backend_context, launch,
		    vc, &vc->own_mut_backend);
		if (error) {
			vmm_debug_trace("vcpu%u backend create failed machine=%p error=%d",
			    i, m, error);
			break;
		}
	}
	if (error == 0 && (m->mut_status != VMM_MACHINE_STARTING ||
	    atomic_load_acq_int(&v->atomic_mut_stop_requested) != 0)) {
		error = ECANCELED;
		vmm_debug_trace("vcpu create canceled machine=%p", m);
	}
	for (i = 0; error == 0 && i < count; i++) {
		struct vmm_vcpu_thread *vc = &v->own_mut_threads[i];

		atomic_add_int(&v->atomic_mut_active_count, 1);
		error = lwkt_create(vmm_vcpu_thread_main, vc,
		    &vc->borrow_mut_thread, NULL, 0, vc->imm_cpu,
		    "vmmvcpu%u", i);
		if (error) {
			vmm_debug_trace("vcpu%u thread create failed machine=%p error=%d",
			    i, m, error);
			atomic_add_int(&v->atomic_mut_active_count, -1);
			atomic_store_rel_int(&v->atomic_mut_stop_requested, 1);
			break;
		}
	}

	if (error) {
		struct vmm_vcpu_thread *release_threads = NULL;
		uint32_t release_count = count;

		vmm_vcpu_stop(m);
		if (atomic_load_acq_int(&v->atomic_mut_active_count) == 0) {
			release_threads = v->own_mut_threads;
			v->own_mut_threads = NULL;
			atomic_store_rel_int(&v->atomic_mut_stop_requested, 0);
		}
		vmm_vcpu_release_threads(v, release_threads, release_count);
		vmm_debug_trace("vcpu start cleanup done machine=%p error=%d", m,
		    error);
	}
	return error;
}

void
vmm_vcpu_stop(struct vmm_machine *m)
{
	struct vmm_vcpu *v = &m->own_mut_vcpu;
	uint32_t i;

	atomic_store_rel_int(&v->atomic_mut_stop_requested, 1);
	vmm_debug_trace("vcpu stop requested machine=%p active=%u", m,
	    atomic_load_acq_int(&v->atomic_mut_active_count));
	while (atomic_load_acq_int(&v->atomic_mut_active_count) != 0) {
		if (v->own_mut_threads != NULL) {
			for (i = 0; i < v->mut_count; i++)
				wakeup(&v->own_mut_threads[i]);
		}
		tsleep(m, 0, "vmmstp", hz / 20 + 1);
	}
	vmm_debug_trace("vcpu stop complete machine=%p", m);
}

int
vmm_vcpu_has_active(struct vmm_vcpu *v)
{
	return atomic_load_acq_int(&v->atomic_mut_active_count) != 0;
}

static struct vmm_vcpu_thread *
vmm_vcpu_detach_threads_locked(struct vmm_vcpu *v)
{
	struct vmm_vcpu_thread *threads;

	threads = v->own_mut_threads;
	v->own_mut_threads = NULL;
	atomic_store_rel_int(&v->atomic_mut_stop_requested, 0);
	return threads;
}

void
vmm_vcpu_release_threads(struct vmm_vcpu *v,
    struct vmm_vcpu_thread *threads, uint32_t count)
{
	const struct vmm_vcpu_backend_ops *backend_ops;
	void *backend_context;
	uint32_t i;

	backend_ops = v->borrow_imm_backend_ops;
	backend_context = v->own_mut_backend_context;
	v->borrow_imm_backend_ops = NULL;
	v->own_mut_backend_context = NULL;
	for (i = 0; i < count; i++) {
		if (threads != NULL && backend_ops != NULL) {
			backend_ops->vcpu_destroy(threads[i].own_mut_backend);
		}
		if (threads != NULL) {
			threads[i].own_mut_backend = NULL;
			threads[i].borrow_imm_backend_ops = NULL;
		}
	}
	if (backend_ops != NULL)
		backend_ops->context_destroy(backend_context);
	if (threads != NULL)
		kfree(threads, M_TEMP);
}

int
vmm_vcpu_should_stop(const struct vmm_vcpu_thread *vc)
{
	return atomic_load_acq_int(
	    &vc->borrow_imm_machine->own_mut_vcpu.atomic_mut_stop_requested) != 0;
}

void
vmm_vcpu_console_input_locked(struct vmm_machine *m)
{
	struct vmm_vcpu *v = &m->own_mut_vcpu;
	uint32_t i;

	if (v->own_mut_threads == NULL ||
	    atomic_load_acq_int(&v->atomic_mut_active_count) == 0)
		return;
	for (i = 0; i < v->mut_count; i++) {
		struct vmm_vcpu_thread *vc = &v->own_mut_threads[i];

		if (vc->borrow_imm_backend_ops != NULL &&
		    vc->borrow_imm_backend_ops->console_input != NULL &&
		    vc->own_mut_backend != NULL) {
			vc->borrow_imm_backend_ops->console_input(
			    vc->own_mut_backend, vc);
			wakeup(vc);
		}
	}
}

void
vmm_vcpu_interrupt_locked(struct vmm_machine *m, uint8_t vector)
{
	struct vmm_vcpu *v = &m->own_mut_vcpu;
	struct vmm_vcpu_thread *vc;

	if (v->own_mut_threads == NULL ||
	    atomic_load_acq_int(&v->atomic_mut_active_count) == 0)
		return;
	/* Single-vCPU MSI-X always targets the sole local APIC, ID zero. */
	vc = &v->own_mut_threads[0];
	if (vc->borrow_imm_backend_ops == NULL ||
	    vc->borrow_imm_backend_ops->interrupt == NULL ||
	    vc->own_mut_backend == NULL)
		return;
	vc->borrow_imm_backend_ops->interrupt(vc->own_mut_backend, vc, vector);
	wakeup(vc);
}

void
vmm_vcpu_uninit(struct vmm_vcpu *v, struct vmm_vcpu_thread **threadsp)
{
	*threadsp = NULL;
	if (v->own_mut_threads != NULL &&
	    atomic_load_acq_int(&v->atomic_mut_active_count) == 0) {
		*threadsp = vmm_vcpu_detach_threads_locked(v);
	}
	atomic_store_rel_int(&v->atomic_mut_stop_requested, 0);
}
