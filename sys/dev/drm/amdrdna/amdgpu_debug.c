/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Driver-local logging for the RDNA+ AMD GPU driver.
 */

#include "amdgpu_debug.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

int amdgpu_log_level = AMDGPU_LOG_INFO;
TUNABLE_INT("hw.amdrdna.log_level", &amdgpu_log_level);

static struct sysctl_ctx_list amdgpu_sysctl_ctx;
static struct sysctl_oid *amdgpu_sysctl_tree;
static bool amdgpu_sysctl_ready;

int
amdgpu_debug_init(void)
{
	int error;

	error = sysctl_ctx_init(&amdgpu_sysctl_ctx);
	if (error != 0)
		return (error);

	amdgpu_sysctl_tree = SYSCTL_ADD_NODE(&amdgpu_sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO, "amdrdna", CTLFLAG_RW,
	    NULL, "RDNA+ AMD GPU driver");
	if (amdgpu_sysctl_tree == NULL) {
		sysctl_ctx_free(&amdgpu_sysctl_ctx);
		return (ENOMEM);
	}
	if (SYSCTL_ADD_INT(&amdgpu_sysctl_ctx,
	    SYSCTL_CHILDREN(amdgpu_sysctl_tree), OID_AUTO, "log_level",
	    CTLFLAG_RW, &amdgpu_log_level, 0,
	    "0=error 1=info 2=debug") == NULL) {
		sysctl_ctx_free(&amdgpu_sysctl_ctx);
		amdgpu_sysctl_tree = NULL;
		return (ENOMEM);
	}
	amdgpu_sysctl_ready = true;
	return (0);
}

void
amdgpu_debug_fini(void)
{
	if (!amdgpu_sysctl_ready)
		return;
	sysctl_ctx_free(&amdgpu_sysctl_ctx);
	amdgpu_sysctl_tree = NULL;
	amdgpu_sysctl_ready = false;
}

void
amdgpu_log(enum amdgpu_log_level level, const char *fmt, ...)
{
	__va_list ap;
	char buf[256];

	if ((int)level > amdgpu_log_level)
		return;

	__va_start(ap, fmt);
	kvsnprintf(buf, sizeof(buf), fmt, ap);
	__va_end(ap);
	kprintf("amdrdna: %s", buf);
}
