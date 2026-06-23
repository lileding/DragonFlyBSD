/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * PCIe passthrough device core -- see vmm_device.h.  Pure; no kernel/VFS deps.
 */
#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

#include "vmm_device.h"

void
vmm_device_init(struct vmm_device *d, const char *bdf, int is_host)
{
	size_t i = 0;

	while (bdf[i] != '\0' && i < VMM_BDF_MAX) {
		d->bdf[i] = bdf[i];
		i++;
	}
	d->bdf[i] = '\0';
	d->owner = NULL;
	d->is_host = is_host;
}

void
vmm_device_bind(struct vmm_device *d, struct vmm_machine *owner)
{
	d->owner = owner;
}

void
vmm_device_unbind(struct vmm_device *d)
{
	d->owner = NULL;
}

int
vmm_device_owned_by(const struct vmm_device *d, const struct vmm_machine *m)
{
	return d->owner == m;
}

int
vmm_device_bdf_eq(const struct vmm_device *d, const char *name, size_t nlen)
{
	size_t i;

	for (i = 0; i < nlen; i++) {
		if (d->bdf[i] == '\0' || d->bdf[i] != name[i])
			return 0;
	}
	return d->bdf[nlen] == '\0';
}

size_t
vmm_device_format(const struct vmm_device *d, char *out, size_t cap)
{
	size_t i = 0;

	while (d->bdf[i] != '\0') {
		if (i + 1 >= cap)	/* leave room for '\n' */
			return 0;
		out[i] = d->bdf[i];
		i++;
	}
	if (i + 1 > cap)
		return 0;
	out[i++] = '\n';
	return i;
}
