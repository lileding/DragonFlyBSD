/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The loader config object -- see vmm_loader.h.  Pure; no kernel/VFS deps.
 */
#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

#include "vmm_parse.h"
#include "vmm_loader.h"

int
vmm_loader_parse(struct vmm_loader *l, const char *buf, size_t len)
{
	size_t pl;
	const char *p = vmm_trim(buf, len, &pl);

	if (pl == 0 || pl > VMM_LOADER_MAX)
		return 0;
	memcpy(l->path, p, pl);
	l->len = pl;
	return 1;
}

size_t
vmm_loader_format(const struct vmm_loader *l, char *out, size_t cap)
{
	size_t need = l->len + 1;

	if (l->len == 0 || need > cap)
		return 0;
	memcpy(out, l->path, l->len);
	out[l->len] = '\n';
	return need;
}

size_t
vmm_loader_path(const struct vmm_loader *l, char *out, size_t cap)
{
	if (l->len == 0 || l->len > cap)
		return 0;
	memcpy(out, l->path, l->len);
	return l->len;
}

int
vmm_loader_is_set(const struct vmm_loader *l)
{
	return l->len != 0;
}
