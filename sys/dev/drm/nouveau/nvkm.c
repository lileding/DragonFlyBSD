/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Native DragonFlyBSD driver for NVIDIA GPUs via the GSP firmware path.
 *
 * Module entry point. PCI bus attachment lives in nvkm_pci.c.
 */

#include "nvkm_priv.h"

static int
nvkm_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		kprintf("nvkm: loaded (target GSP firmware 570.144)\n");
		return (0);
	case MOD_UNLOAD:
		kprintf("nvkm: unloaded\n");
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
/* Pull in the GSP firmware blobs registered by the fw module so
 * firmware_get() works during device attach, regardless of which
 * .ko the kld scanner picks up first at boot. */
MODULE_DEPEND(nvkm, nvkm_fw_tu102_570_fw, 1, 1, 1);
