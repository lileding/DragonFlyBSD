/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Native DragonFlyBSD driver for NVIDIA GPUs via the GSP firmware path.
 *
 * Module entry point. PCI bus attachment lives in nvkm_pci.c.
 */

#include "nvkm_priv.h"

int nvkm_debug = 0;

static void
nvkm_vprintf(device_t dev, const char *fmt, __va_list ap)
{
	char buf[512];

	kvsnprintf(buf, sizeof(buf), fmt, ap);
	kprintf("%s: %s", device_get_nameunit(dev), buf);
}

void
nvkm_infof(device_t dev, const char *fmt, ...)
{
	__va_list ap;

	__va_start(ap, fmt);
	nvkm_vprintf(dev, fmt, ap);
	__va_end(ap);
}

void
nvkm_debugf(device_t dev, const char *fmt, ...)
{
	__va_list ap;

	if (!nvkm_debug)
		return;
	__va_start(ap, fmt);
	nvkm_vprintf(dev, fmt, ap);
	__va_end(ap);
}

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
MODULE_DEPEND(nvkm, nvgsp_570_fw, 1, 1, 1);
