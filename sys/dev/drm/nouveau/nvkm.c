/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Native DragonFlyBSD driver for NVIDIA GPUs via the GSP firmware path.
 *
 * Module entry point. PCI bus attachment lives in nvkm_pci.c.
 */

#include "nvkm_priv.h"

int nvkm_debug = 0;

/* EXEC GPFIFO-entry flow control, modelled on nouveau's scheduler credit_limit =
 * gpfifo.max. Caps the total GPFIFO entries (push_count+1 per submit) of
 * not-yet-completed submits; a submit that would exceed it blocks (FIFO) until the
 * oldest completes (refunded on the completion interrupt). 0 = off.
 *
 * This was originally introduced as a tight bound (2) to contain the async-EXEC
 * torn-pointer fault, but that fault was actually caused by signal-only EXECs
 * signalling out of channel order (fixed in nvkm_drm.c); with that fix the fault
 * does not occur even with backpressure off (cr=0 validated clean, single and
 * parallel). So this is no longer load-bearing for correctness and now only bounds
 * ring usage like nouveau. Default = ring - 1, matching nouveau's gpfifo.max; the
 * real in-flight limits are the 64 post slots and gpfifo_wait_space, so this is an
 * effectively-inert backstop. cr 2..511 and 0 all validated RC-free. */
int nvkm_exec_max_credits = NVKM_DRM_GPFIFO_ENTRIES - 1;

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
MODULE_DEPEND(nvkm, nvkm_fw_tu102_570_fw, 1, 1, 1);
