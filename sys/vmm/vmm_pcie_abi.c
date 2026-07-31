/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Fixed vPCIe provider/consumer packet ABI validation.
 */
#include <sys/types.h>
#include <sys/endian.h>
#include <sys/errno.h>

#include "vmm_pcie_abi.h"

static int	vmm_pcie_abi_bar_valid(const struct vmm_pcie_abi_bar *bar,
		    unsigned int index, int companion);
static int	vmm_pcie_abi_bar_cap_valid(
		    const struct vmm_pcie_abi_bar_cap *message);
static int	vmm_pcie_abi_bar_range_valid(
		    const struct vmm_pcie_abi_bar_range *range,
		    const struct vmm_pcie_abi_register *message);
static int	vmm_pcie_abi_consumer_ready_valid(
		    const struct vmm_pcie_abi_consumer_ready *message);
static int	vmm_pcie_abi_failure_valid(
		    const struct vmm_pcie_abi_failure *message);
static int	vmm_pcie_abi_header_valid(
		    const struct vmm_pcie_abi_header *header, uint16_t type,
		    size_t size, uint32_t allowed_flags);
static int	vmm_pcie_abi_msix_valid(
		    const struct vmm_pcie_abi_msix *message);
static int	vmm_pcie_abi_mmio_valid(
		    const struct vmm_pcie_abi_mmio *message, uint16_t type);
static int	vmm_pcie_abi_register_valid(
		    const struct vmm_pcie_abi_register *message);
static int	vmm_pcie_abi_registered_valid(
		    const struct vmm_pcie_abi_registered *message);
static int	vmm_pcie_abi_dma_segment_valid(
		    const struct vmm_pcie_abi_dma_segment *segment);
static int	vmm_pcie_abi_range_overlaps(uint64_t a_start,
		    uint64_t a_length, uint64_t b_start, uint64_t b_length);
static int	vmm_pcie_abi_start_valid(
		    const struct vmm_pcie_abi_start *message);
static int	vmm_pcie_abi_stop_valid(
		    const struct vmm_pcie_abi_stop *message);
static int	vmm_pcie_abi_stopped_valid(
		    const struct vmm_pcie_abi_stopped *message);

int
vmm_pcie_abi_validate(const void *message, size_t size)
{
	const struct vmm_pcie_abi_header *header;
	uint16_t type;

	if (message == 0 || size < sizeof(*header))
		return EINVAL;
	header = message;
	type = le16toh(header->le_type);
	switch (type) {
	case VMM_PCIE_ABI_MSG_REGISTER:
		if (size != sizeof(struct vmm_pcie_abi_register))
			return EINVAL;
		return vmm_pcie_abi_register_valid(message);
	case VMM_PCIE_ABI_MSG_REGISTERED:
		if (size != sizeof(struct vmm_pcie_abi_registered))
			return EINVAL;
		return vmm_pcie_abi_registered_valid(message);
	case VMM_PCIE_ABI_MSG_CONSUMER_READY:
		if (size != sizeof(struct vmm_pcie_abi_consumer_ready))
			return EINVAL;
		return vmm_pcie_abi_consumer_ready_valid(message);
	case VMM_PCIE_ABI_MSG_START:
		if (size != sizeof(struct vmm_pcie_abi_start))
			return EINVAL;
		return vmm_pcie_abi_start_valid(message);
	case VMM_PCIE_ABI_MSG_STOP:
		if (size != sizeof(struct vmm_pcie_abi_stop))
			return EINVAL;
		return vmm_pcie_abi_stop_valid(message);
	case VMM_PCIE_ABI_MSG_STOPPED:
		if (size != sizeof(struct vmm_pcie_abi_stopped))
			return EINVAL;
		return vmm_pcie_abi_stopped_valid(message);
	case VMM_PCIE_ABI_MSG_MSIX:
		if (size != sizeof(struct vmm_pcie_abi_msix))
			return EINVAL;
		return vmm_pcie_abi_msix_valid(message);
	case VMM_PCIE_ABI_MSG_FAILURE:
		if (size != sizeof(struct vmm_pcie_abi_failure))
			return EINVAL;
		return vmm_pcie_abi_failure_valid(message);
	case VMM_PCIE_ABI_MSG_BAR_CAP:
		if (size != sizeof(struct vmm_pcie_abi_bar_cap))
			return EINVAL;
		return vmm_pcie_abi_bar_cap_valid(message);
	case VMM_PCIE_ABI_MSG_MMIO_REQUEST:
		if (size != sizeof(struct vmm_pcie_abi_mmio))
			return EINVAL;
		return vmm_pcie_abi_mmio_valid(message, type);
	case VMM_PCIE_ABI_MSG_MMIO_RESPONSE:
		if (size != sizeof(struct vmm_pcie_abi_mmio))
			return EINVAL;
		return vmm_pcie_abi_mmio_valid(message, type);
	default:
		return EINVAL;
	}
}

static int
vmm_pcie_abi_bar_range_valid(const struct vmm_pcie_abi_bar_range *range,
    const struct vmm_pcie_abi_register *message)
{
	uint64_t offset;
	uint64_t size;
	uint64_t bar_size;
	uint32_t bar_index;
	uint32_t flags;

	offset = le64toh(range->le_offset);
	size = le64toh(range->le_size);
	bar_index = le32toh(range->le_bar_index);
	flags = le32toh(range->le_flags);
	if (bar_index >= VMM_PCIE_ABI_MAX_BARS || size == 0 ||
	    (offset & (VMM_PCIE_ABI_PAGE_SIZE - 1)) != 0 ||
	    (size & (VMM_PCIE_ABI_PAGE_SIZE - 1)) != 0 ||
	    (flags != VMM_PCIE_ABI_BAR_RANGE_F_DIRECT &&
	    flags != VMM_PCIE_ABI_BAR_RANGE_F_TRAPPED))
		return 0;
	bar_size = le64toh(message->bar[bar_index].le_size);
	return bar_size != 0 && offset < bar_size && size <= bar_size - offset;
}

static int
vmm_pcie_abi_bar_valid(const struct vmm_pcie_abi_bar *bar,
    unsigned int index, int companion)
{
	uint64_t size;
	uint32_t flags;
	uint32_t known;

	size = le64toh(bar->le_size);
	flags = le32toh(bar->le_flags);
	known = VMM_PCIE_ABI_BAR_F_MEMORY | VMM_PCIE_ABI_BAR_F_64BIT |
	    VMM_PCIE_ABI_BAR_F_PREFETCHABLE |
	    VMM_PCIE_ABI_BAR_F_DOORBELL_DIRECT |
	    VMM_PCIE_ABI_BAR_F_DOORBELL_TRAPPED;
	if (companion)
		return size == 0 && flags == 0 && bar->le_reserved == 0;
	if (size == 0)
		return flags == 0 && bar->le_reserved == 0;
	if (bar->le_reserved != 0 || (flags & ~known) != 0 ||
	    (flags & VMM_PCIE_ABI_BAR_F_MEMORY) == 0 ||
	    size < VMM_PCIE_ABI_PAGE_SIZE ||
	    (size & (VMM_PCIE_ABI_PAGE_SIZE - 1)) != 0 ||
	    (size & (size - 1)) != 0)
		return 0;
	if ((flags & VMM_PCIE_ABI_BAR_F_DOORBELL_DIRECT) != 0 &&
	    (flags & VMM_PCIE_ABI_BAR_F_DOORBELL_TRAPPED) != 0)
		return 0;
	if ((flags & VMM_PCIE_ABI_BAR_F_64BIT) != 0 &&
	    index + 1 >= VMM_PCIE_ABI_MAX_BARS)
		return 0;
	return 1;
}

static int
vmm_pcie_abi_bar_cap_valid(const struct vmm_pcie_abi_bar_cap *message)
{
	struct vmm_pcie_abi_bar bar;
	uint32_t index;

	if (!vmm_pcie_abi_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_BAR_CAP, sizeof(*message), 0) ||
	    le64toh(message->le_device_id) == 0 ||
	    le64toh(message->le_generation) == 0)
		return EINVAL;
	index = le32toh(message->le_bar_index);
	if (index >= VMM_PCIE_ABI_MAX_BARS)
		return EINVAL;
	bar.le_size = message->le_size;
	bar.le_flags = message->le_bar_flags;
	bar.le_reserved = 0;
	return vmm_pcie_abi_bar_valid(&bar, index, 0) ? 0 : EINVAL;
}

static int
vmm_pcie_abi_consumer_ready_valid(
    const struct vmm_pcie_abi_consumer_ready *message)
{
	if (!vmm_pcie_abi_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_CONSUMER_READY, sizeof(*message), 0) ||
	    le64toh(message->le_device_id) == 0 ||
	    le64toh(message->le_consumer_id) == 0 ||
	    le64toh(message->le_attachment_generation) == 0)
		return EINVAL;
	return 0;
}

static int
vmm_pcie_abi_failure_valid(const struct vmm_pcie_abi_failure *message)
{
	unsigned int i;
	uint32_t stage;

	if (!vmm_pcie_abi_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_FAILURE, sizeof(*message), 0) ||
	    le64toh(message->le_device_id) == 0 ||
	    le64toh(message->le_generation) == 0 ||
	    le32toh(message->le_error) == 0)
		return EINVAL;
	stage = le32toh(message->le_stage);
	if (stage < VMM_PCIE_ABI_FAILURE_PROVIDER ||
	    stage > VMM_PCIE_ABI_FAILURE_LIFECYCLE)
		return EINVAL;
	for (i = 0; i < sizeof(message->message); i++) {
		if (message->message[i] == '\0')
			return 0;
	}
	return EINVAL;
}

static int
vmm_pcie_abi_header_valid(const struct vmm_pcie_abi_header *header,
    uint16_t type, size_t size, uint32_t allowed_flags)
{
	if (le32toh(header->le_magic) != VMM_PCIE_ABI_MAGIC ||
	    le16toh(header->le_version) != VMM_PCIE_ABI_VERSION ||
	    le16toh(header->le_type) != type ||
	    le32toh(header->le_size) != size ||
	    (le32toh(header->le_flags) & ~allowed_flags) != 0)
		return 0;
	return 1;
}

static int
vmm_pcie_abi_msix_valid(const struct vmm_pcie_abi_msix *message)
{
	if (!vmm_pcie_abi_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_MSIX, sizeof(*message), 0) ||
	    le64toh(message->le_device_id) == 0 ||
	    le64toh(message->le_attachment_generation) == 0 ||
	    le16toh(message->le_vector) >= VMM_PCIE_ABI_MAX_MSIX_VECTORS ||
	    message->le_reserved0 != 0 || message->le_reserved1 != 0)
		return EINVAL;
	return 0;
}

static int
vmm_pcie_abi_mmio_valid(const struct vmm_pcie_abi_mmio *message,
    uint16_t type)
{
	uint32_t flags;
	uint32_t size;

	flags = le32toh(message->header.le_flags);
	size = le32toh(message->le_size);
	if (!vmm_pcie_abi_header_valid(&message->header, type, sizeof(*message),
	    VMM_PCIE_ABI_MMIO_F_WRITE) ||
	    le64toh(message->le_device_id) == 0 ||
	    le64toh(message->le_attachment_generation) == 0 ||
	    le64toh(message->le_request_id) == 0 ||
	    le32toh(message->le_bar_index) >= VMM_PCIE_ABI_MAX_BARS ||
	    (size != 1 && size != 2 && size != 4) ||
	    message->le_reserved != 0)
		return EINVAL;
	if (type == VMM_PCIE_ABI_MSG_MMIO_REQUEST &&
	    le32toh(message->le_error) != 0)
		return EINVAL;
	if (type == VMM_PCIE_ABI_MSG_MMIO_RESPONSE && flags != 0 &&
	    flags != VMM_PCIE_ABI_MMIO_F_WRITE)
		return EINVAL;
	return 0;
}

static int
vmm_pcie_abi_register_valid(const struct vmm_pcie_abi_register *message)
{
	uint64_t bar0_size;
	const struct vmm_pcie_abi_vendor_cap *vendor_cap;
	uint32_t flags;
	uint32_t class_code;
	unsigned int i;
	unsigned int j;
	unsigned int vendor_cap_count;
	unsigned int bar_range_count;
	int companion;

	if (!vmm_pcie_abi_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_REGISTER, sizeof(*message),
	    VMM_PCIE_ABI_REGISTER_F_MSIX | VMM_PCIE_ABI_REGISTER_F_PARENT) ||
	    le64toh(message->header.le_sequence) == 0 ||
	    le16toh(message->le_vendor_id) == 0 ||
	    le16toh(message->le_vendor_id) == 0xffff ||
	    le16toh(message->le_device_id) == 0 ||
	    le16toh(message->le_device_id) == 0xffff || message->reserved0 != 0)
		return EINVAL;
	class_code = le32toh(message->le_class_code);
	if ((class_code & 0xff000000U) != 0)
		return EINVAL;
	flags = le32toh(message->header.le_flags);
	if ((flags & VMM_PCIE_ABI_REGISTER_F_MSIX) == 0 ||
	    le16toh(message->le_msix_vectors) == 0 ||
	    le16toh(message->le_msix_vectors) > VMM_PCIE_ABI_MAX_MSIX_VECTORS)
		return EINVAL;
	bar0_size = le64toh(message->bar[VMM_PCIE_ABI_MSIX_BAR_INDEX].le_size);
	if (bar0_size < VMM_PCIE_ABI_MSIX_MIN_BAR_SIZE(
	    le16toh(message->le_msix_vectors)))
		return EINVAL;
	if ((flags & VMM_PCIE_ABI_REGISTER_F_PARENT) != 0) {
		if (le64toh(message->le_parent_consumer_id) == 0 ||
		    le64toh(message->le_parent_generation) == 0)
			return EINVAL;
	} else if (message->le_parent_consumer_id != 0 ||
	    message->le_parent_generation != 0) {
		return EINVAL;
	}
	if (message->reserved1[0] != 0 || message->reserved1[1] != 0)
		return EINVAL;
	vendor_cap_count = message->vendor_cap_count;
	if (vendor_cap_count > VMM_PCIE_ABI_MAX_VENDOR_CAPS)
		return EINVAL;
	for (i = 0; i < VMM_PCIE_ABI_MAX_VENDOR_CAPS; i++) {
		vendor_cap = &message->vendor_cap[i];
		if (i >= vendor_cap_count) {
			if (vendor_cap->length != 0)
				return EINVAL;
			for (j = 0; j < sizeof(vendor_cap->bytes); j++) {
				if (vendor_cap->bytes[j] != 0)
					return EINVAL;
			}
			continue;
		}
		if (vendor_cap->length < VMM_PCIE_ABI_VENDOR_CAP_MIN_SIZE ||
		    vendor_cap->length > VMM_PCIE_ABI_VENDOR_CAP_MAX_SIZE)
			return EINVAL;
		for (j = vendor_cap->length -
		    VMM_PCIE_ABI_VENDOR_CAP_MIN_SIZE;
		    j < sizeof(vendor_cap->bytes); j++) {
			if (vendor_cap->bytes[j] != 0)
				return EINVAL;
		}
	}
	bar_range_count = message->bar_range_count;
	if (bar_range_count == 0 || bar_range_count > VMM_PCIE_ABI_MAX_BAR_RANGES)
		return EINVAL;
	for (i = 0; i < VMM_PCIE_ABI_MAX_BAR_RANGES; i++) {
		const struct vmm_pcie_abi_bar_range *range;

		range = &message->bar_range[i];
		if (i >= bar_range_count) {
			if (range->le_offset != 0 || range->le_size != 0 ||
			    range->le_bar_index != 0 || range->le_flags != 0)
				return EINVAL;
			continue;
		}
		if (!vmm_pcie_abi_bar_range_valid(range, message))
			return EINVAL;
		for (j = 0; j < i; j++) {
			const struct vmm_pcie_abi_bar_range *prior;

			prior = &message->bar_range[j];
			if (le32toh(prior->le_bar_index) ==
			    le32toh(range->le_bar_index) &&
			    vmm_pcie_abi_range_overlaps(le64toh(prior->le_offset),
			    le64toh(prior->le_size), le64toh(range->le_offset),
			    le64toh(range->le_size)))
				return EINVAL;
		}
	}
	companion = 0;
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		if (!vmm_pcie_abi_bar_valid(&message->bar[i], i, companion))
			return EINVAL;
		companion = !companion &&
		    (le32toh(message->bar[i].le_flags) &
		    VMM_PCIE_ABI_BAR_F_64BIT) != 0;
	}
	for (i = 0; i < VMM_PCIE_ABI_MAX_BARS; i++) {
		uint64_t covered;

		bar0_size = le64toh(message->bar[i].le_size);
		if (bar0_size == 0)
			continue;
		covered = 0;
		for (j = 0; j < bar_range_count; j++) {
			const struct vmm_pcie_abi_bar_range *range;

			range = &message->bar_range[j];
			if (le32toh(range->le_bar_index) != i ||
			    le64toh(range->le_offset) != covered)
				continue;
			covered += le64toh(range->le_size);
		}
		if (covered != bar0_size)
			return EINVAL;
	}
	return 0;
}

static int
vmm_pcie_abi_registered_valid(const struct vmm_pcie_abi_registered *message)
{
	uint32_t bar_fd_mask;
	uint32_t known_bar_fd_mask;
	uint16_t msix_vectors;

	if (!vmm_pcie_abi_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_REGISTERED, sizeof(*message),
	    VMM_PCIE_ABI_REGISTERED_F_DMA_CAPABILITY) ||
	    le64toh(message->header.le_sequence) == 0 ||
	    le64toh(message->le_device_id) == 0 ||
	    le64toh(message->le_consumer_id) == 0 ||
	    le64toh(message->le_attachment_generation) == 0 ||
	    (le32toh(message->le_bdf) & ~VMM_PCIE_ABI_BDF_MASK) != 0 ||
	    message->le_reserved != 0)
		return EINVAL;
	bar_fd_mask = le32toh(message->le_bar_fd_mask);
	known_bar_fd_mask = (1U << VMM_PCIE_ABI_MAX_BARS) - 1;
	msix_vectors = le16toh(message->le_msix_vectors);
	if ((le32toh(message->header.le_flags) &
	    VMM_PCIE_ABI_REGISTERED_F_DMA_CAPABILITY) == 0 ||
	    (bar_fd_mask & ~known_bar_fd_mask) != 0 || msix_vectors == 0 ||
	    msix_vectors > VMM_PCIE_ABI_MAX_MSIX_VECTORS)
		return EINVAL;
	return 0;
}

static int
vmm_pcie_abi_start_valid(const struct vmm_pcie_abi_start *message)
{
	uint32_t count;
	unsigned int i;
	unsigned int j;

	if (!vmm_pcie_abi_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_START, sizeof(*message), 0) ||
	    le64toh(message->le_device_id) == 0 ||
	    le64toh(message->le_memory_generation) == 0 ||
	    le64toh(message->header.le_sequence) !=
	    le64toh(message->le_memory_generation) ||
	    message->le_reserved != 0)
		return EINVAL;
	count = le32toh(message->le_dma_segment_count);
	if (count == 0 || count > VMM_PCIE_ABI_MAX_DMA_SEGMENTS)
		return EINVAL;
	for (i = 0; i < count; i++) {
		if (!vmm_pcie_abi_dma_segment_valid(&message->dma_segment[i]))
			return EINVAL;
		for (j = 0; j < i; j++) {
			if (vmm_pcie_abi_range_overlaps(
			    le64toh(message->dma_segment[i].le_gpa),
			    le64toh(message->dma_segment[i].le_length),
			    le64toh(message->dma_segment[j].le_gpa),
			    le64toh(message->dma_segment[j].le_length)))
				return EINVAL;
		}
	}
	for (; i < VMM_PCIE_ABI_MAX_DMA_SEGMENTS; i++) {
		if (message->dma_segment[i].le_gpa != 0 ||
		    message->dma_segment[i].le_iova != 0 ||
		    message->dma_segment[i].le_length != 0 ||
		    message->dma_segment[i].le_flags != 0 ||
		    message->dma_segment[i].le_permissions != 0)
			return EINVAL;
	}
	return 0;
}

static int
vmm_pcie_abi_dma_segment_valid(const struct vmm_pcie_abi_dma_segment *segment)
{
	uint64_t gpa;
	uint64_t iova;
	uint64_t length;
	uint32_t flags;
	uint32_t permissions;

	gpa = le64toh(segment->le_gpa);
	iova = le64toh(segment->le_iova);
	length = le64toh(segment->le_length);
	flags = le32toh(segment->le_flags);
	permissions = le32toh(segment->le_permissions);
	if ((gpa & (VMM_PCIE_ABI_PAGE_SIZE - 1)) != 0 || length == 0 ||
	    (length & (VMM_PCIE_ABI_PAGE_SIZE - 1)) != 0 ||
	    length > UINT64_MAX - gpa ||
	    (flags & ~VMM_PCIE_ABI_DMA_F_IOVA_VALID) != 0 ||
	    (permissions & ~(VMM_PCIE_ABI_DMA_PERM_READ |
	    VMM_PCIE_ABI_DMA_PERM_WRITE)) != 0 || permissions == 0)
		return 0;
	if ((flags & VMM_PCIE_ABI_DMA_F_IOVA_VALID) != 0) {
		if (iova == 0 || (iova & (VMM_PCIE_ABI_PAGE_SIZE - 1)) != 0 ||
		    length > UINT64_MAX - iova)
			return 0;
	} else if (iova != 0) {
		return 0;
	}
	return 1;
}

static int
vmm_pcie_abi_range_overlaps(uint64_t a_start, uint64_t a_length,
    uint64_t b_start, uint64_t b_length)
{

	if (a_start <= b_start)
		return b_start - a_start < a_length;
	return a_start - b_start < b_length;
}

static int
vmm_pcie_abi_stop_valid(const struct vmm_pcie_abi_stop *message)
{
	if (!vmm_pcie_abi_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_STOP, sizeof(*message), 0) ||
	    le64toh(message->le_device_id) == 0 ||
	    le64toh(message->le_memory_generation) == 0 ||
	    le64toh(message->header.le_sequence) !=
	    le64toh(message->le_memory_generation))
		return EINVAL;
	return 0;
}

static int
vmm_pcie_abi_stopped_valid(const struct vmm_pcie_abi_stopped *message)
{

	if (!vmm_pcie_abi_header_valid(&message->header,
	    VMM_PCIE_ABI_MSG_STOPPED, sizeof(*message), 0) ||
	    le64toh(message->le_device_id) == 0 ||
	    le64toh(message->le_memory_generation) == 0 ||
	    le64toh(message->header.le_sequence) !=
	    le64toh(message->le_memory_generation))
		return EINVAL;
	return 0;
}
