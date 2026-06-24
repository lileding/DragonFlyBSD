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
#include "vmm_loader.h"
#include "vmm_machine.h"
#include "vmm_svm.h"
#include "vmm_vcpu.h"

#define VMM_VCPU_MAX	256u

struct vmm_vcpu_backend_ops {
	const char *imm_name;
	int (*available)(void);
	int (*create)(struct vmm_machine *m, const struct vmm_launch *launch,
	    void **backendp);
	void (*destroy)(void *backend);
	void (*run)(void *backend, struct vmm_vcpu_thread *vc);
};

static const struct vmm_vcpu_backend_ops vmm_vcpu_svm_ops = {
	.imm_name = "svm",
	.available = vmm_svm_available,
	.create = vmm_svm_vcpu_create,
	.destroy = vmm_svm_vcpu_destroy,
	.run = vmm_svm_vcpu_run,
};

static const struct vmm_vcpu_backend_ops * const vmm_vcpu_backends[] = {
	&vmm_vcpu_svm_ops,
	NULL
};

static const struct vmm_vcpu_backend_ops *
vmm_vcpu_select_backend(void)
{
	uint32_t i;

	for (i = 0; vmm_vcpu_backends[i] != NULL; i++) {
		if (vmm_vcpu_backends[i]->available())
			return vmm_vcpu_backends[i];
	}
	return NULL;
}

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
	if (!vmm_machine_vcpu_wait_start(m, vc))
		goto out;
	if (vc->borrow_imm_backend_ops != NULL)
		vc->borrow_imm_backend_ops->run(vc->own_mut_backend, vc);
out:
	vmm_machine_vcpu_exited(m);
}

int
vmm_vcpu_start_all(struct vmm_machine *m, struct vmm_host *host,
    const struct vmm_launch *launch)
{
	struct vmm_vcpu *v = &m->own_mut_vcpu;
	struct vmm_vcpu_thread *threads;
	const struct vmm_vcpu_backend_ops *backend_ops;
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
	if (count != 1 || launch == NULL)
		return EOPNOTSUPP;
	backend_ops = vmm_vcpu_select_backend();
	if (backend_ops == NULL)
		return EOPNOTSUPP;
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

		vc->borrow_imm_machine = m;
		vc->borrow_imm_backend_ops = backend_ops;
		vc->imm_id = i;
		vc->imm_cpu = cpu;
		error = backend_ops->create(m, launch, &vc->own_mut_backend);
		if (error)
			break;
		vmm_machine_lock(m);
		if (vmm_machine_start_cancelled_locked(m) ||
		    v->mut_stop_requested) {
			error = ECANCELED;
			vmm_machine_unlock(m);
			backend_ops->destroy(vc->own_mut_backend);
			vc->own_mut_backend = NULL;
			break;
		}
		v->mut_active_count++;
		vmm_machine_unlock(m);

		error = lwkt_create(vmm_vcpu_thread_main, vc,
		    &vc->borrow_mut_thread, NULL, 0, vc->imm_cpu,
		    "vmmvcpu%u", i);
		if (error) {
			vmm_machine_lock(m);
			if (v->mut_active_count > 0) {
				v->mut_active_count--;
			}
			v->mut_stop_requested = 1;
			vmm_machine_unlock(m);
			backend_ops->destroy(vc->own_mut_backend);
			vc->own_mut_backend = NULL;
			break;
		}
	}

	if (error) {
		struct vmm_vcpu_thread *release_threads = NULL;
		uint32_t release_count = count;

		vmm_machine_lock(m);
		vmm_vcpu_request_stop(v);
		if (v->mut_active_count == 0) {
			release_threads = v->own_mut_threads;
			v->own_mut_threads = NULL;
			v->mut_stop_requested = 0;
		}
		vmm_machine_unlock(m);
		vmm_vcpu_release_threads(release_threads, release_count);
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

void
vmm_vcpu_wakeup_all(struct vmm_vcpu *v)
{
	uint32_t i;

	if (v->own_mut_threads == NULL)
		return;
	for (i = 0; i < v->mut_count; i++)
		wakeup(&v->own_mut_threads[i]);
}

int
vmm_vcpu_has_active(const struct vmm_vcpu *v)
{
	return v->mut_active_count != 0;
}

static struct vmm_vcpu_thread *
vmm_vcpu_detach_threads_locked(struct vmm_vcpu *v)
{
	struct vmm_vcpu_thread *threads;

	threads = v->own_mut_threads;
	v->own_mut_threads = NULL;
	v->mut_stop_requested = 0;
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
vmm_vcpu_note_exit(struct vmm_vcpu *v, struct vmm_vcpu_thread **threadsp)
{
	*threadsp = NULL;
	if (v->mut_active_count > 0)
		v->mut_active_count--;
	if (v->mut_active_count != 0)
		return 0;
	*threadsp = vmm_vcpu_detach_threads_locked(v);
	return 1;
}

void
vmm_vcpu_uninit(struct vmm_vcpu *v, struct vmm_vcpu_thread **threadsp)
{
	*threadsp = NULL;
	if (v->own_mut_threads != NULL && v->mut_active_count == 0) {
		*threadsp = vmm_vcpu_detach_threads_locked(v);
	}
	v->mut_stop_requested = 0;
}
