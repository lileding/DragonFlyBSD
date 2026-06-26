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
	if (vc->borrow_imm_backend_ops != NULL)
		vc->borrow_imm_backend_ops->run(vc->own_mut_backend, vc);

	if (m->own_mut_vcpu.mut_active_count > 0)
		m->own_mut_vcpu.mut_active_count--;
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
	backend_ops = vmm_vcpu_select_backend();
	if (backend_ops == NULL)
		return EOPNOTSUPP;
	threads = kmalloc(sizeof(*threads) * count, M_TEMP, M_WAITOK | M_ZERO);

	if (v->own_mut_threads != NULL || v->mut_active_count != 0) {
		error = EBUSY;
	} else if (m->mut_status != VMM_MACHINE_STARTING) {
		error = ECANCELED;
	} else {
		v->own_mut_threads = threads;
		v->mut_count = count;
		v->mut_stop_requested = 0;
		threads = NULL;
	}
	if (threads != NULL) {
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
		error = backend_ops->create(m, launch, &vc->own_mut_backend);
		if (error)
			break;
		if (m->mut_status != VMM_MACHINE_STARTING ||
		    v->mut_stop_requested) {
			error = ECANCELED;
			backend_ops->destroy(vc->own_mut_backend);
			vc->own_mut_backend = NULL;
			break;
		}
		v->mut_active_count++;

		error = lwkt_create(vmm_vcpu_thread_main, vc,
		    &vc->borrow_mut_thread, NULL, 0, vc->imm_cpu,
		    "vmmvcpu%u", i);
		if (error) {
			if (v->mut_active_count > 0)
				v->mut_active_count--;
			v->mut_stop_requested = 1;
			backend_ops->destroy(vc->own_mut_backend);
			vc->own_mut_backend = NULL;
			break;
		}
	}

	if (error) {
		struct vmm_vcpu_thread *release_threads = NULL;
		uint32_t release_count = count;

		vmm_vcpu_stop(m);
		if (v->mut_active_count == 0) {
			release_threads = v->own_mut_threads;
			v->own_mut_threads = NULL;
			v->mut_stop_requested = 0;
		}
		vmm_vcpu_release_threads(release_threads, release_count);
	}
	return error;
}

void
vmm_vcpu_stop(struct vmm_machine *m)
{
	struct vmm_vcpu *v = &m->own_mut_vcpu;
	uint32_t i;

	v->mut_stop_requested = 1;
	while (v->mut_active_count != 0) {
		if (v->own_mut_threads != NULL) {
			for (i = 0; i < v->mut_count; i++)
				wakeup(&v->own_mut_threads[i]);
		}
		tsleep(m, 0, "vmmstp", hz / 20 + 1);
	}
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
vmm_vcpu_should_stop(const struct vmm_vcpu_thread *vc)
{
	return vc->borrow_imm_machine->own_mut_vcpu.mut_stop_requested;
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
