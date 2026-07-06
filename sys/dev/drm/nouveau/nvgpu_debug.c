/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Driver-local logging for the native NVIDIA GPU driver.
 */

#include "nvgpu_debug.h"

int nvgpu_debug = 0;

static void
nvgpu_vprintf(device_t dev, const char *fmt, __va_list ap)
{
	char buf[512];

	kvsnprintf(buf, sizeof(buf), fmt, ap);
	kprintf("%s: %s", device_get_nameunit(dev), buf);
}

void
nvgpu_infof(device_t dev, const char *fmt, ...)
{
	__va_list ap;

	__va_start(ap, fmt);
	nvgpu_vprintf(dev, fmt, ap);
	__va_end(ap);
}

void
nvgpu_debugf(device_t dev, const char *fmt, ...)
{
	__va_list ap;

	if (!nvgpu_debug)
		return;
	__va_start(ap, fmt);
	nvgpu_vprintf(dev, fmt, ap);
	__va_end(ap);
}
