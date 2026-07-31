/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Virtqueue handling derived from the BSD-licensed bhyve virtio design.
 */
#include <sys/endian.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "virtiod.h"

struct virtiod_vring_desc {
	uint64_t le_addr;
	uint32_t le_len;
	uint16_t le_flags;
	uint16_t le_next;
} __attribute__((__packed__));

struct virtiod_vring_used_elem {
	uint32_t le_id;
	uint32_t le_len;
} __attribute__((__packed__));

int
virtiod_dma_translate(const struct virtiod_dma_segment *segments,
    unsigned int count, uint64_t gpa, size_t size, void **result)
{
	uint64_t end;
	unsigned int i;

	if (segments == NULL || result == NULL || size == 0 ||
	    __builtin_add_overflow(gpa, (uint64_t)size, &end))
		return EINVAL;
	for (i = 0; i < count; i++) {
		uint64_t segment_end;

		if (segments[i].own_mut_bytes == NULL ||
		    __builtin_add_overflow(segments[i].imm_gpa,
		    segments[i].imm_size, &segment_end))
			return EINVAL;
		if (gpa >= segments[i].imm_gpa && end <= segment_end) {
			*result = segments[i].own_mut_bytes +
			    (size_t)(gpa - segments[i].imm_gpa);
			return 0;
		}
	}
	return EFAULT;
}

int
virtiod_vring_configure(struct virtiod_vring *ring,
    struct virtiod_dma_segment *segments, unsigned int segment_count,
    uint16_t size, uint64_t desc_gpa, uint64_t avail_gpa, uint64_t used_gpa)
{
	void *unused;
	uint64_t bytes;
	int error;

	if (ring == NULL || segments == NULL || segment_count == 0 ||
	    segment_count > VIRTIOD_MAX_DMA_SEGMENTS || size == 0 ||
	    size > VIRTIOD_QUEUE_SIZE || (size & (size - 1)) != 0)
		return EINVAL;
	bytes = (uint64_t)size * sizeof(struct virtiod_vring_desc);
	error = virtiod_dma_translate(segments, segment_count, desc_gpa,
	    (size_t)bytes, &unused);
	if (error != 0)
		return error;
	bytes = sizeof(uint16_t) * (3U + size);
	error = virtiod_dma_translate(segments, segment_count, avail_gpa,
	    (size_t)bytes, &unused);
	if (error != 0)
		return error;
	bytes = sizeof(uint16_t) * 2U + (uint64_t)size *
	    sizeof(struct virtiod_vring_used_elem);
	error = virtiod_dma_translate(segments, segment_count, used_gpa,
	    (size_t)bytes, &unused);
	if (error != 0)
		return error;
	memset(ring, 0, sizeof(*ring));
	ring->borrow_imm_dma = segments;
	ring->imm_dma_count = segment_count;
	ring->mut_size = size;
	ring->mut_desc_gpa = desc_gpa;
	ring->mut_avail_gpa = avail_gpa;
	ring->mut_used_gpa = used_gpa;
	return 0;
}

int
virtiod_vring_pop(struct virtiod_vring *ring, struct virtiod_chain *chain)
{
	struct virtiod_vring_desc *descriptors;
	uint16_t *available;
	uint16_t available_index;
	uint16_t current;
	unsigned int count;
	int error;

	if (ring == NULL || chain == NULL || ring->mut_size == 0)
		return EINVAL;
	error = virtiod_dma_translate(ring->borrow_imm_dma, ring->imm_dma_count,
	    ring->mut_desc_gpa, (size_t)ring->mut_size * sizeof(*descriptors),
	    (void **)&descriptors);
	if (error != 0)
		return error;
	error = virtiod_dma_translate(ring->borrow_imm_dma, ring->imm_dma_count,
	    ring->mut_avail_gpa, sizeof(uint16_t) * (3U + ring->mut_size),
	    (void **)&available);
	if (error != 0)
		return error;
	available_index = __atomic_load_n(&available[1], __ATOMIC_ACQUIRE);
	if (ring->mut_last_avail == le16toh(available_index))
		return ENOENT;
	if ((uint16_t)(le16toh(available_index) - ring->mut_last_avail) >
	    ring->mut_size)
		return EPROTO;
	current = le16toh(available[2 + (ring->mut_last_avail &
	    (ring->mut_size - 1U))]);
	ring->mut_last_avail++;
	if (current >= ring->mut_size)
		return EPROTO;
	memset(chain, 0, sizeof(*chain));
	chain->imm_head = current;
	for (count = 0; count < ring->mut_size; count++) {
		struct virtiod_vring_desc descriptor;
		uint16_t flags;
		void *address;

		memcpy(&descriptor, &descriptors[current], sizeof(descriptor));
		flags = le16toh(descriptor.le_flags);
		if ((flags & VIRTIOD_DESC_F_INDIRECT) != 0 ||
		    le32toh(descriptor.le_len) == 0 ||
		    chain->mut_iov_count == VIRTIOD_MAX_CHAIN)
			return EPROTO;
		error = virtiod_dma_translate(ring->borrow_imm_dma,
		    ring->imm_dma_count, le64toh(descriptor.le_addr),
		    le32toh(descriptor.le_len), &address);
		if (error != 0)
			return error;
		chain->own_mut_iov[chain->mut_iov_count].iov_base = address;
		chain->own_mut_iov[chain->mut_iov_count].iov_len =
		    le32toh(descriptor.le_len);
		chain->own_mut_flags[chain->mut_iov_count] = flags;
		if ((flags & VIRTIOD_DESC_F_WRITE) != 0)
			chain->mut_writable_count++;
		else
			chain->mut_readable_count++;
		chain->mut_iov_count++;
		if ((flags & VIRTIOD_DESC_F_NEXT) == 0)
			return 0;
		current = le16toh(descriptor.le_next);
		if (current >= ring->mut_size)
			return EPROTO;
	}
	return ELOOP;
}

int
virtiod_vring_complete(struct virtiod_vring *ring,
    const struct virtiod_chain *chain, uint32_t length)
{
	struct virtiod_vring_used_elem *used;
	uint16_t *used_header;
	int error;

	if (ring == NULL || chain == NULL || ring->mut_size == 0 ||
	    chain->imm_head >= ring->mut_size)
		return EINVAL;
	error = virtiod_dma_translate(ring->borrow_imm_dma, ring->imm_dma_count,
	    ring->mut_used_gpa, sizeof(uint16_t) * 2U +
	    (size_t)ring->mut_size * sizeof(*used), (void **)&used_header);
	if (error != 0)
		return error;
	used = (struct virtiod_vring_used_elem *)(void *)(used_header + 2);
	used[ring->mut_next_used & (ring->mut_size - 1U)].le_id =
	    htole32(chain->imm_head);
	used[ring->mut_next_used & (ring->mut_size - 1U)].le_len = htole32(length);
	__atomic_store_n(&used_header[1], htole16(++ring->mut_next_used),
	    __ATOMIC_RELEASE);
	return 0;
}
