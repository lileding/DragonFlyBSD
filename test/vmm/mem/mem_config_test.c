/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Userland boundary tests for the kernel mem config object.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmm_mem.h"

static int failures;

static void
fail(const char *name)
{
	fprintf(stderr, "FAIL: %s\n", name);
	failures++;
}

static void
expect_parse(const char *name, const char *input, uint64_t initial,
    uint64_t want, int ok)
{
	struct vmm_mem mem;
	int got;

	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = initial;
	got = vmm_mem_parse(&mem, input, strlen(input));
	if (got != ok) {
		fail(name);
		return;
	}
	if (ok && mem.mut_bytes != want)
		fail(name);
	if (!ok && mem.mut_bytes != initial)
		fail(name);
}

static void
expect_format(const char *name, uint64_t bytes, const char *want)
{
	struct vmm_mem mem;
	char buf[64];
	size_t n;

	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = bytes;
	memset(buf, 0xa5, sizeof(buf));
	n = vmm_mem_format(&mem, buf, sizeof(buf));
	if (n != strlen(want) || memcmp(buf, want, n) != 0)
		fail(name);
}

static void
expect_format_reject(const char *name, uint64_t bytes, size_t cap)
{
	struct vmm_mem mem;
	char buf[64];

	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = bytes;
	if (vmm_mem_format(&mem, buf, cap) != 0)
		fail(name);
}

static void
expect_backing_reject(void)
{
	struct vmm_mem mem;
	struct vmm_mem_backing *backing;

	memset(&mem, 0, sizeof(mem));
	mem.mut_bytes = VMM_MEM_ALIGN;
	backing = (struct vmm_mem_backing *)(uintptr_t)1;
	mem.own_mut_backing = backing;
	if (vmm_mem_parse(&mem, "4M", 2) != 0)
		fail("backing rejects config change");
	if (mem.mut_bytes != VMM_MEM_ALIGN || mem.own_mut_backing != backing)
		fail("backing preserves config");
}

int
main(void)
{
	char max_bytes[32];
	char over_max_bytes[32];

	snprintf(max_bytes, sizeof(max_bytes), "%ju",
	    (uintmax_t)VMM_MEM_MAX);
	snprintf(over_max_bytes, sizeof(over_max_bytes), "%ju",
	    (uintmax_t)(VMM_MEM_MAX + VMM_MEM_ALIGN));

	expect_parse("2M", "2M", 0, 2ull * 1024 * 1024, 1);
	expect_parse("lowercase suffix and trim", " \t4m\n", 0,
	    4ull * 1024 * 1024, 1);
	expect_parse("bytes", "2097152", 0, 2ull * 1024 * 1024, 1);
	expect_parse("1G", "1G", 0, 1ull << 30, 1);
	expect_parse("max bytes", max_bytes, 0, VMM_MEM_MAX, 1);
	expect_parse("zero rejected", "0", VMM_MEM_ALIGN, VMM_MEM_ALIGN, 0);
	expect_parse("empty rejected", " \n", VMM_MEM_ALIGN, VMM_MEM_ALIGN, 0);
	expect_parse("unaligned 1M rejected", "1M", VMM_MEM_ALIGN,
	    VMM_MEM_ALIGN, 0);
	expect_parse("unaligned 3M rejected", "3M", VMM_MEM_ALIGN,
	    VMM_MEM_ALIGN, 0);
	expect_parse("bad suffix rejected", "2T", VMM_MEM_ALIGN,
	    VMM_MEM_ALIGN, 0);
	expect_parse("overflow decimal rejected",
	    "18446744073709551616", VMM_MEM_ALIGN, VMM_MEM_ALIGN, 0);
	expect_parse("over max rejected", over_max_bytes, VMM_MEM_ALIGN,
	    VMM_MEM_ALIGN, 0);

	expect_format("format 2M", 2ull * 1024 * 1024, "2097152\n");
	expect_format_reject("format unset", 0, sizeof(max_bytes));
	expect_format_reject("format small buffer", 2ull * 1024 * 1024, 4);
	expect_backing_reject();

	if (failures != 0)
		return 1;
	printf("PASS: mem config parser\n");
	return 0;
}
