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

	if (v->threads != NULL || v->active_count != 0)
		return 0;
	if (!vmm_parse_decimal(t, tl, &n) || n < 1 || n > VMM_VCPU_MAX)
		return 0;
	v->count = (uint32_t)n;
	return 1;
}

size_t
vmm_vcpu_format(const struct vmm_vcpu *v, char *out, size_t cap)
{
	return v->count == 0 ? 0 : vmm_write_decimal(v->count, out, cap);
}

int
vmm_vcpu_is_set(const struct vmm_vcpu *v)
{
	return v->count != 0;
}

static void
vmm_vcpu_thread_main(void *arg)
{
	struct vmm_vcpu_thread *vc = arg;
	struct vmm_machine *m = vc->machine;

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
	struct vmm_vcpu *v = &m->vcpu;
	struct vmm_vcpu_thread *threads;
	uint32_t i;
	int error = 0;

	if (!vmm_vcpu_is_set(v) || host == NULL)
		return EINVAL;
	threads = kmalloc(sizeof(*threads) * v->count, M_TEMP,
	    M_WAITOK | M_ZERO);

	lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
	if (v->threads != NULL || v->active_count != 0) {
		error = EBUSY;
	} else if (vmm_machine_start_cancelled(m)) {
		error = ECANCELED;
	} else {
		v->threads = threads;
		v->stop_requested = 0;
		threads = NULL;
	}
	lockmgr(&m->lifecycle_lock, LK_RELEASE);
	if (threads != NULL) {
		kfree(threads, M_TEMP);
		return error;
	}

	for (i = 0; i < v->count; i++) {
		struct vmm_vcpu_thread *vc = &v->threads[i];

		lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
		if (vmm_machine_start_cancelled(m) || v->stop_requested) {
			error = ECANCELED;
			lockmgr(&m->lifecycle_lock, LK_RELEASE);
			break;
		}
		vc->machine = m;
		vc->id = i;
		vc->cpu = vmm_host_next_cpu(host);
		v->active_count++;
		lockmgr(&m->lifecycle_lock, LK_RELEASE);

		error = lwkt_create(vmm_vcpu_thread_main, vc, &vc->thread, NULL,
		    0, vc->cpu, "vmmvcpu%u", i);
		if (error) {
			lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
			if (v->active_count > 0)
				v->active_count--;
			v->stop_requested = 1;
			lockmgr(&m->lifecycle_lock, LK_RELEASE);
			break;
		}
	}

	if (error) {
		lockmgr(&m->lifecycle_lock, LK_EXCLUSIVE);
		vmm_vcpu_request_stop(v);
		if (v->active_count == 0) {
			kfree(v->threads, M_TEMP);
			v->threads = NULL;
			v->stop_requested = 0;
		}
		lockmgr(&m->lifecycle_lock, LK_RELEASE);
	}
	return error;
}

void
vmm_vcpu_request_stop(struct vmm_vcpu *v)
{
	uint32_t i;

	v->stop_requested = 1;
	if (v->threads == NULL)
		return;
	for (i = 0; i < v->count; i++)
		wakeup(&v->threads[i]);
}

void
vmm_vcpu_request_run(struct vmm_vcpu *v)
{
	v->stop_requested = 0;
}

int
vmm_vcpu_has_active(const struct vmm_vcpu *v)
{
	return v->active_count != 0;
}

int
vmm_vcpu_note_exit(struct vmm_vcpu *v)
{
	if (v->active_count > 0)
		v->active_count--;
	if (v->active_count != 0)
		return 0;
	if (v->threads != NULL) {
		kfree(v->threads, M_TEMP);
		v->threads = NULL;
	}
	v->stop_requested = 0;
	return 1;
}

void
vmm_vcpu_uninit(struct vmm_vcpu *v)
{
	if (v->threads != NULL && v->active_count == 0) {
		kfree(v->threads, M_TEMP);
		v->threads = NULL;
	}
	v->stop_requested = 0;
}
