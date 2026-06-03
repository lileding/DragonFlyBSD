/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Native DragonFlyBSD driver for NVIDIA GPUs via the GSP firmware path.
 *
 * Module entry point. PCI bus attachment lives in nvkm_pci.c.
 */

#include "nvkm_priv.h"

int nvkm_debug = 0;

/* EXEC completion-based backpressure (mirrors real nouveau, whose EXEC scheduler
 * uses credit_limit = gpfifo.max with credits freed on the job completion fence).
 * Caps the total GPFIFO entries (push_count+1 per submit) of not-yet-completed
 * submits; a submit that would exceed it blocks (FIFO) until the oldest completes.
 * Bounds how far NVK runs ahead so it cannot reuse a command-buffer / root-descriptor
 * chunk the GPU is still reading.
 *
 * Nouveau's limit is gpfifo.max (1023). Empirically NVK/ggml reuse their command
 * buffers after only ~2 submits, so our safe limit is far tighter (~4 entries); the
 * gap is still under investigation (likely a separate completion-fence ordering bug
 * that lets ggml's per-reuse fence fire early, masked here by the tight bound). 0 = off.
 *
 * Default 2 = at most one submit in flight (push_count+1 credits per submit). This is
 * the robust value: ggml reuses its command buffers after ~2 submits, and heavy
 * dispatches keep the prior one executing, so 2 in flight (cr=4) faults intermittently.
 * Throughput at this bound is limited by per-submit overhead + interrupt-completion
 * latency, addressed separately. */
int nvkm_exec_max_credits = 2;

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
