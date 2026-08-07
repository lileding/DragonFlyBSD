/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmm kernel module entry points.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>

static int
vmm_modevent(module_t module, int event, void *arg)
{

	(void)module;
	(void)arg;

	switch (event) {
	case MOD_LOAD:
	case MOD_UNLOAD:
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
