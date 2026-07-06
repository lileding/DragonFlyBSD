/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Driver-local logging for the native NVIDIA GPU driver.
 */

#include "nvgpu_debug.h"
#include "nvgpu_device.h"

#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

int nvgpu_debug = 0;
TUNABLE_INT("hw.nvgpu.debug", &nvgpu_debug);

SYSCTL_NODE(_hw, OID_AUTO, nvgpu, CTLFLAG_RW, NULL, "NVGPU driver");
SYSCTL_INT(_hw_nvgpu, OID_AUTO, debug, CTLFLAG_RW, &nvgpu_debug, 0,
    "Enable nvgpu debug logs");

static const char *
nvgpu_log_level_name(enum nvgpu_log_level level)
{
	switch (level) {
	case NVGPU_LOG_DEBUG:
		return ("debug");
	case NVGPU_LOG_INFO:
		return ("info");
	default:
		return ("log");
	}
}

static int
nvgpu_log_enabled(enum nvgpu_log_level level)
{
	switch (level) {
	case NVGPU_LOG_DEBUG:
		return (nvgpu_debug != 0);
	case NVGPU_LOG_INFO:
		return (1);
	default:
		return (1);
	}
}

/* Format and emit one already-authorized log message. */
static void
nvgpu_vlog(enum nvgpu_log_level level, const char *file, const char *func,
	int line, const char *fmt, __va_list ap)
{
	char buf[512];
	device_t dev;
	const char *name;

	dev = nvgpu_device_dev(NULL);
	name = dev != NULL ? device_get_nameunit(dev) : "nvgpu";
	kvsnprintf(buf, sizeof(buf), fmt, ap);
	kprintf("%s: %s: %s:%s:%d: %s", name, nvgpu_log_level_name(level),
	    file, func, line, buf);
}

/* Emit one driver log message with call-site metadata. */
/* Backend for nvgpu_log(); use the macro so call-site location is preserved. */
void
nvgpu_log_impl(enum nvgpu_log_level level, const char *file, const char *func,
	int line, const char *fmt, ...)
{
	__va_list ap;

	if (!nvgpu_log_enabled(level))
		return;
	__va_start(ap, fmt);
	nvgpu_vlog(level, file, func, line, fmt, ap);
	__va_end(ap);
}
