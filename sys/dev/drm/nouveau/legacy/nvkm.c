/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Native DragonFlyBSD driver for NVIDIA GPUs via the GSP firmware path.
 *
 * Legacy logging helpers. Module entry lives in nvgpu_device.c.
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
