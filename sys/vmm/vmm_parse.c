/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Text scanners shared by the kernel config value objects.
 */
#include <sys/types.h>
#include <sys/systm.h>

#include "vmm_parse.h"

int
vmm_is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

const char *
vmm_trim(const char *b, size_t len, size_t *outlen)
{
	size_t s = 0, e = len;

	while (s < e && vmm_is_ws(b[s]))
		s++;
	while (e > s && vmm_is_ws(b[e - 1]))
		e--;
	*outlen = e - s;
	return b + s;
}

int
vmm_parse_decimal(const char *b, size_t len, uint64_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (len == 0)
		return 0;
	for (i = 0; i < len; i++) {
		char c = b[i];

		if (c < '0' || c > '9')
			return 0;
		if (v > ((uint64_t)-1 - (uint64_t)(c - '0')) / 10)
			return 0;
		v = v * 10 + (uint64_t)(c - '0');
	}
	*out = v;
	return 1;
}

size_t
vmm_write_decimal(uint64_t v, char *out, size_t cap)
{
	char tmp[20];
	size_t i = sizeof(tmp), digits, need;

	do {
		tmp[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v != 0);
	digits = sizeof(tmp) - i;
	need = digits + 1;
	if (need > cap)
		return 0;
	memcpy(out, tmp + i, digits);
	out[need - 1] = '\n';
	return need;
}
