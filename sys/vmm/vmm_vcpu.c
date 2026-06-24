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

#include "vmm_parse.h"
#include "vmm_host.h"
#include "vmm_machine.h"
#include "vmm_vcpu.h"

#define VMM_VCPU_MAX	256u

int
vmm_vcpu_parse(struct vmm_vcpu *v, const char *buf, size_t len)
{
	size_t tl;
	const char *t = vmm_trim(buf, len, &tl);
	uint64_t n;

	if (v->own_mut_threads != NULL || v->mut_active_count != 0)
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

	lwkt_setpri_self(TDPRI_USER_NORM);
	while (!vmm_machine_vcpu_should_stop(m)) {
		lwkt_user_yield();
		tsleep(vc, 0, "vmmvcpu", 1);
	}
	vmm_machine_vcpu_exited(m);
}

int
vmm_vcpu_start_all(struct vmm_machine *m, struct vmm_host *host)
{
	struct vmm_vcpu *v = &m->own_mut_vcpu;
	struct vmm_vcpu_thread *threads;
	uint32_t count;
	uint32_t i;
	int error = 0;

	if (host == NULL)
		return EINVAL;
	vmm_machine_lock(m);
	count = v->mut_count;
	vmm_machine_unlock(m);
	if (count == 0)
		return EINVAL;
	threads = kmalloc(sizeof(*threads) * count, M_TEMP, M_WAITOK | M_ZERO);

	vmm_machine_lock(m);
	if (v->own_mut_threads != NULL || v->mut_active_count != 0) {
		error = EBUSY;
	} else if (vmm_machine_start_cancelled_locked(m)) {
		error = ECANCELED;
	} else if (v->mut_count != count) {
		error = EBUSY;
	} else {
		v->own_mut_threads = threads;
		v->mut_stop_requested = 0;
		threads = NULL;
	}
	vmm_machine_unlock(m);
	if (threads != NULL) {
		kfree(threads, M_TEMP);
		return error;
	}

	for (i = 0; i < count; i++) {
		struct vmm_vcpu_thread *vc = &v->own_mut_threads[i];
		int cpu = vmm_host_next_cpu(host);

		vmm_machine_lock(m);
		if (vmm_machine_start_cancelled_locked(m) ||
		    v->mut_stop_requested) {
			error = ECANCELED;
			vmm_machine_unlock(m);
			break;
		}
		vc->borrow_imm_machine = m;
		vc->imm_id = i;
		vc->imm_cpu = cpu;
		v->mut_active_count++;
		vmm_machine_unlock(m);

		error = lwkt_create(vmm_vcpu_thread_main, vc,
		    &vc->borrow_mut_thread, NULL, 0, vc->imm_cpu,
		    "vmmvcpu%u", i);
		if (error) {
			vmm_machine_lock(m);
			if (v->mut_active_count > 0)
				v->mut_active_count--;
			v->mut_stop_requested = 1;
			vmm_machine_unlock(m);
			break;
		}
	}

	if (error) {
		vmm_machine_lock(m);
		vmm_vcpu_request_stop(v);
		if (v->mut_active_count == 0) {
			kfree(v->own_mut_threads, M_TEMP);
			v->own_mut_threads = NULL;
			v->mut_stop_requested = 0;
		}
		vmm_machine_unlock(m);
	}
	return error;
}

void
vmm_vcpu_request_stop(struct vmm_vcpu *v)
{
	uint32_t i;

	v->mut_stop_requested = 1;
	if (v->own_mut_threads == NULL)
		return;
	for (i = 0; i < v->mut_count; i++)
		wakeup(&v->own_mut_threads[i]);
}

void
vmm_vcpu_request_run(struct vmm_vcpu *v)
{
	v->mut_stop_requested = 0;
}

int
vmm_vcpu_has_active(const struct vmm_vcpu *v)
{
	return v->mut_active_count != 0;
}

int
vmm_vcpu_note_exit(struct vmm_vcpu *v)
{
	if (v->mut_active_count > 0)
		v->mut_active_count--;
	if (v->mut_active_count != 0)
		return 0;
	if (v->own_mut_threads != NULL) {
		kfree(v->own_mut_threads, M_TEMP);
		v->own_mut_threads = NULL;
	}
	v->mut_stop_requested = 0;
	return 1;
}

void
vmm_vcpu_uninit(struct vmm_vcpu *v)
{
	if (v->own_mut_threads != NULL && v->mut_active_count == 0) {
		kfree(v->own_mut_threads, M_TEMP);
		v->own_mut_threads = NULL;
	}
	v->mut_stop_requested = 0;
}
