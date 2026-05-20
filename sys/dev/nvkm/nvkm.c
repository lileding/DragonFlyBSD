/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Native DragonFlyBSD driver for NVIDIA GPUs via the GSP firmware path.
 *
 * Phase 0 skeleton: load/unload only. No hardware interaction yet.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>

static int
nvkm_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		kprintf("nvkm: skeleton loaded, target firmware 570.144\n");
		return (0);
	case MOD_UNLOAD:
		kprintf("nvkm: skeleton unloaded\n");
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t nvkm_moddata = {
	"nvkm",
	nvkm_modevent,
	NULL
};

DECLARE_MODULE(nvkm, nvkm_moddata, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(nvkm, 1);
