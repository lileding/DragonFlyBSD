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

#include "vmm_parse.h"
#include "vmm_host.h"
#include "vmm_loader.h"
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
					kprintf("vmm: backend init failed name=%s error=%d\n",
					    (*ops)->imm_name, error);
					return error;
				}
			}
			vmm_backend = *ops;
			kprintf("vmm: backend selected name=%s\n",
			    vmm_backend->imm_name);
			return 0;
		}
		name = (*ops)->imm_name;
	}
	kprintf("vmm: no usable vcpu backend name=%s reason=%s\n", name,
	    reason);
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
	vmm_machine_logf(m, "vcpu%u thread enter cpu=%d", vc->imm_id,
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
	vmm_machine_logf(m, "vcpu%u thread exit active=%u", vc->imm_id,
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
	uint32_t i;
	int error = 0;

	if (count == 0)
		return EINVAL;
	if (count != 1 || launch == NULL)
		return EOPNOTSUPP;
	backend_ops = vmm_backend;
	if (backend_ops == NULL) {
		vmm_machine_logf(m, "vcpu start failed reason=no_backend");
		return ENXIO;
	}
	vmm_machine_logf(m, "vcpu backend name=%s count=%u",
	    backend_ops->imm_name, count);
	threads = kmalloc(sizeof(*threads) * count, M_TEMP, M_WAITOK | M_ZERO);

	if (v->own_mut_threads != NULL ||
	    atomic_load_acq_int(&v->atomic_mut_active_count) != 0) {
		error = EBUSY;
	} else if (m->mut_status != VMM_MACHINE_STARTING) {
		error = ECANCELED;
	} else {
		v->own_mut_threads = threads;
		v->mut_count = count;
		atomic_store_rel_int(&v->atomic_mut_stop_requested, 0);
		atomic_store_rel_int(&v->atomic_mut_exit_reason,
		    VMM_VCPU_EXIT_NONE);
		threads = NULL;
	}
	if (threads != NULL) {
		vmm_machine_logf(m, "vcpu start rejected error=%d", error);
		kfree(threads, M_TEMP);
		return error;
	}

	for (i = 0; i < count; i++) {
		struct vmm_vcpu_thread *vc = &v->own_mut_threads[i];
		int cpu = vmm_host_next_cpu();

		vc->borrow_imm_machine = m;
		vc->borrow_imm_backend_ops = backend_ops;
		vc->imm_id = i;
		vc->imm_cpu = cpu;
		vmm_machine_logf(m, "vcpu%u create backend=%s cpu=%d", i,
		    backend_ops->imm_name, cpu);
		error = backend_ops->create(m, launch, &vc->own_mut_backend);
		if (error) {
			vmm_machine_logf(m, "vcpu%u backend create failed error=%d",
			    i, error);
			break;
		}
		if (m->mut_status != VMM_MACHINE_STARTING ||
		    atomic_load_acq_int(&v->atomic_mut_stop_requested) != 0) {
			error = ECANCELED;
			vmm_machine_logf(m, "vcpu%u create canceled", i);
			backend_ops->destroy(vc->own_mut_backend);
			vc->own_mut_backend = NULL;
			break;
		}
		atomic_add_int(&v->atomic_mut_active_count, 1);

		error = lwkt_create(vmm_vcpu_thread_main, vc,
		    &vc->borrow_mut_thread, NULL, 0, vc->imm_cpu,
		    "vmmvcpu%u", i);
		if (error) {
			vmm_machine_logf(m, "vcpu%u thread create failed error=%d",
			    i, error);
			atomic_add_int(&v->atomic_mut_active_count, -1);
			atomic_store_rel_int(&v->atomic_mut_stop_requested, 1);
			backend_ops->destroy(vc->own_mut_backend);
			vc->own_mut_backend = NULL;
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
		vmm_vcpu_release_threads(release_threads, release_count);
		vmm_machine_logf(m, "vcpu start cleanup done error=%d", error);
	}
	return error;
}

void
vmm_vcpu_stop(struct vmm_machine *m)
{
	struct vmm_vcpu *v = &m->own_mut_vcpu;
	uint32_t i;

	atomic_store_rel_int(&v->atomic_mut_stop_requested, 1);
	vmm_machine_logf(m, "vcpu stop requested active=%u",
	    atomic_load_acq_int(&v->atomic_mut_active_count));
	while (atomic_load_acq_int(&v->atomic_mut_active_count) != 0) {
		if (v->own_mut_threads != NULL) {
			for (i = 0; i < v->mut_count; i++)
				wakeup(&v->own_mut_threads[i]);
		}
		tsleep(m, 0, "vmmstp", hz / 20 + 1);
	}
	vmm_machine_logf(m, "vcpu stop complete");
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
vmm_vcpu_release_threads(struct vmm_vcpu_thread *threads, uint32_t count)
{
	uint32_t i;

	if (threads == NULL)
		return;
	for (i = 0; i < count; i++) {
		if (threads[i].borrow_imm_backend_ops != NULL) {
			threads[i].borrow_imm_backend_ops->destroy(
			    threads[i].own_mut_backend);
		}
		threads[i].own_mut_backend = NULL;
		threads[i].borrow_imm_backend_ops = NULL;
	}
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
