/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <sys/endian.h>

#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "virtiod.h"

struct test_desc {
	uint64_t le_addr;
	uint32_t le_len;
	uint16_t le_flags;
	uint16_t le_next;
} __attribute__((__packed__));

static void test_valid_chain(void);
static void test_chain_loop(void);

int
main(void)
{

	test_valid_chain();
	test_chain_loop();
	puts("PASS: virtiod virtqueue");
	return 0;
}

static void
test_valid_chain(void)
{
	struct virtiod_chain chain;
	struct virtiod_dma_segment segment;
	struct virtiod_vring ring;
	struct test_desc *descriptors;
	uint16_t *available;
	uint16_t *used;
	uint8_t *memory;
	int error;

	memory = calloc(1, 16384);
	if (memory == NULL)
		err(1, "calloc");
	segment.imm_gpa = 0;
	segment.imm_size = 16384;
	segment.own_mut_bytes = memory;
	descriptors = (struct test_desc *)(void *)memory;
	available = (uint16_t *)(void *)(memory + 4096);
	used = (uint16_t *)(void *)(memory + 8192);
	descriptors[0].le_addr = htole64(12288);
	descriptors[0].le_len = htole32(16);
	descriptors[0].le_flags = htole16(VIRTIOD_DESC_F_NEXT);
	descriptors[0].le_next = htole16(1);
	descriptors[1].le_addr = htole64(12304);
	descriptors[1].le_len = htole32(32);
	descriptors[1].le_flags = htole16(VIRTIOD_DESC_F_NEXT |
	    VIRTIOD_DESC_F_WRITE);
	descriptors[1].le_next = htole16(2);
	descriptors[2].le_addr = htole64(12336);
	descriptors[2].le_len = htole32(1);
	descriptors[2].le_flags = htole16(VIRTIOD_DESC_F_WRITE);
	available[2] = htole16(0);
	available[1] = htole16(1);
	error = virtiod_vring_configure(&ring, &segment, 1, 128, 0, 4096, 8192);
	if (error != 0)
		errno = error, err(1, "configure");
	if (virtiod_vring_has_available(&ring) != 1)
		errno = EINVAL, err(1, "available before pop");
	error = virtiod_vring_pop(&ring, &chain);
	if (error != 0 || chain.imm_head != 0 || chain.mut_iov_count != 3 ||
	    chain.mut_readable_count != 1 || chain.mut_writable_count != 2)
		errno = error == 0 ? EINVAL : error, err(1, "valid chain");
	if (virtiod_vring_has_available(&ring) != 0)
		errno = EINVAL, err(1, "available after pop");
	error = virtiod_vring_complete(&ring, &chain, 33);
	if (error != 0 || le16toh(used[1]) != 1)
		errno = error == 0 ? EINVAL : error, err(1, "complete");
	free(memory);
}

static void
test_chain_loop(void)
{
	struct virtiod_chain chain;
	struct virtiod_dma_segment segment;
	struct virtiod_vring ring;
	struct test_desc *descriptors;
	uint16_t *available;
	uint8_t *memory;
	int error;

	memory = calloc(1, 16384);
	if (memory == NULL)
		err(1, "calloc");
	segment.imm_gpa = 0;
	segment.imm_size = 16384;
	segment.own_mut_bytes = memory;
	descriptors = (struct test_desc *)(void *)memory;
	available = (uint16_t *)(void *)(memory + 4096);
	descriptors[0].le_addr = htole64(12288);
	descriptors[0].le_len = htole32(1);
	descriptors[0].le_flags = htole16(VIRTIOD_DESC_F_NEXT);
	descriptors[0].le_next = htole16(0);
	available[2] = htole16(0);
	available[1] = htole16(1);
	error = virtiod_vring_configure(&ring, &segment, 1, 128, 0, 4096, 8192);
	if (error != 0)
		errno = error, err(1, "configure loop");
	error = virtiod_vring_pop(&ring, &chain);
	if (error != ELOOP)
		errno = error == 0 ? EINVAL : error, err(1, "chain loop");
	free(memory);
}
