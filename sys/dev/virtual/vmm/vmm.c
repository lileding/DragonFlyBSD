/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm kernel module entry points.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/thread.h>

#include "vmm_backend.h"
#include "x64/svm/vmm_x64_svm.h"

static const struct vmm_backend_ops *vmm_backend;
static struct lwkt_token vmm_backend_token;
static unsigned int vmm_machine_count;

const struct vmm_backend_ops *
vmm_backend_machine_acquire(void)
{
	const struct vmm_backend_ops *backend;

	lwkt_gettoken(&vmm_backend_token);
	backend = vmm_backend;
	if (backend != NULL)
		++vmm_machine_count;
	lwkt_reltoken(&vmm_backend_token);
	return backend;
}

void
vmm_backend_machine_release(const struct vmm_backend_ops *backend)
{
	lwkt_gettoken(&vmm_backend_token);
	KKASSERT(vmm_backend == backend);
	KKASSERT(vmm_machine_count != 0);
	--vmm_machine_count;
	lwkt_reltoken(&vmm_backend_token);
}

static int
vmm_modevent(module_t module, int event, void *arg)
{
	const struct vmm_backend_ops *backend;
	int error;

	(void)module;
	(void)arg;

	switch (event) {
	case MOD_LOAD:
		lwkt_token_init(&vmm_backend_token, "vmmbackend");
		backend = &vmm_x64_svm_backend;
		error = backend->probe();
		if (error != 0) {
			kprintf("vmm: %s backend unavailable (%d)\n", backend->name,
			    error);
			return error;
		}
		error = backend->init();
		if (error != 0)
			return error;
		vmm_backend = backend;
		kprintf("vmm: selected %s backend\n", backend->name);
		return 0;
	case MOD_UNLOAD:
		lwkt_gettoken(&vmm_backend_token);
		if (vmm_machine_count != 0) {
			lwkt_reltoken(&vmm_backend_token);
			return EBUSY;
		}
		backend = vmm_backend;
		vmm_backend = NULL;
		lwkt_reltoken(&vmm_backend_token);
		if (backend != NULL)
			backend->fini();
		return 0;
	case MOD_SHUTDOWN:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static moduledata_t vmm_mod = {
	.name = "vmm",
	.evhand = vmm_modevent,
	.priv = NULL,
};

DECLARE_MODULE(vmm, vmm_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(vmm, 1);
