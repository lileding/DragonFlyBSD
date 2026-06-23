/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The mem config object -- see vmm_mem.h.  Pure; no kernel/VFS deps.
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
#include "vmm_mem.h"

#define VMM_MEM_ALIGN	(2ull * 1024 * 1024)	/* large-page granularity */

int
vmm_mem_parse(struct vmm_mem *m, const char *buf, size_t len)
{
	size_t tl;
	const char *t = vmm_trim(buf, len, &tl);
	uint64_t mult, v;
	size_t dlen;
	char last;

	if (tl == 0)
		return 0;
	last = t[tl - 1];
	if (last == 'K' || last == 'k') {
		mult = 1024;
		dlen = tl - 1;
	} else if (last == 'M' || last == 'm') {
		mult = 1024 * 1024;
		dlen = tl - 1;
	} else if (last == 'G' || last == 'g') {
		mult = 1024 * 1024 * 1024;
		dlen = tl - 1;
	} else if (last >= '0' && last <= '9') {
		mult = 1;
		dlen = tl;
	} else {
		return 0;
	}
	if (!vmm_parse_decimal(t, dlen, &v))
		return 0;
	if (v > ((uint64_t)-1) / mult)
		return 0;
	v *= mult;
	if (v == 0 || (v % VMM_MEM_ALIGN) != 0)
		return 0;
	m->bytes = v;
	return 1;
}

size_t
vmm_mem_format(const struct vmm_mem *m, char *out, size_t cap)
{
	return m->bytes == 0 ? 0 : vmm_write_decimal(m->bytes, out, cap);
}

int
vmm_mem_is_set(const struct vmm_mem *m)
{
	return m->bytes != 0;
}
