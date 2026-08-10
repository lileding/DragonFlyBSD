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

#include "vmm.h"
#include "vmm_internal.h"

SET_DECLARE(vmm_backend_set, const struct vmm_backend_ops);

const struct vmm_backend_ops *vmm_backend;
struct lwkt_token vmm_token;
int vmm_machine_count;
bool vmm_draining;

int
vmm_x64_get_capability(struct vmm_x64_capability *capability)
{
	const struct vmm_backend_ops *backend;

	if (capability == NULL)
		return EINVAL;
	backend = vmm_backend;
	if (backend == NULL)
		return ENXIO;
	return backend->capability(capability);
}

int
vmm_x64_get_supported_cpuid(struct vmm_cpuid_entry *entries,
	size_t *entry_count)
{
	const struct vmm_backend_ops *backend;

	if (entry_count == NULL)
		return EINVAL;
	backend = vmm_backend;
	if (backend == NULL || backend->get_supported_cpuid == NULL)
		return ENXIO;
	return backend->get_supported_cpuid(entries, entry_count);
}

static int
vmm_modevent(module_t module, int event, void *arg)
{
	const struct vmm_backend_ops **ops;
	const struct vmm_backend_ops *backend;
	int error;

	(void)module;
	(void)arg;

	switch (event) {
	case MOD_LOAD:
		lwkt_token_init(&vmm_token, "vmm");
		backend = NULL;
		vmm_machine_count = 0;
		vmm_draining = false;
		error = ENXIO;
		SET_FOREACH(ops, vmm_backend_set) {
			error = (*ops)->probe();
			if (error != 0)
				continue;
			error = (*ops)->init();
			if (error != 0) {
				kprintf("vmm: %s backend init failed (%d)\n",
				    (*ops)->name, error);
				return error;
			}
			backend = *ops;
			break;
		}
		if (backend == NULL) {
			kprintf("vmm: no usable backend (%d)\n", error);
			return error;
		}
		vmm_backend = backend;
		kprintf("vmm: selected %s backend\n", backend->name);
		return 0;
	case MOD_UNLOAD:
		lwkt_gettoken(&vmm_token);
		if (vmm_machine_count != 0) {
			lwkt_reltoken(&vmm_token);
			return EBUSY;
		}
		vmm_draining = true;
		backend = vmm_backend;
		lwkt_reltoken(&vmm_token);
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
