/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Userland boundary tests for the vPCIe provider/consumer packet ABI.
 */
#include <sys/endian.h>

#include <errno.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmm_pcie_abi.h"

static void
packet_init(struct vmm_pcie_abi_header *header, uint16_t type, uint32_t size,
    uint32_t flags)
{
	memset(header, 0, sizeof(*header));
	header->le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	header->le_version = htole16(VMM_PCIE_ABI_VERSION);
	header->le_type = htole16(type);
	header->le_size = htole32(size);
	header->le_flags = htole32(flags);
	header->le_sequence = htole64(1);
}

static void
build_register(struct vmm_pcie_abi_register *message)
{
	memset(message, 0, sizeof(*message));
	packet_init(&message->header, VMM_PCIE_ABI_MSG_REGISTER,
	    sizeof(*message), VMM_PCIE_ABI_REGISTER_F_MSIX);
	message->le_vendor_id = htole16(0x1af4);
	message->le_device_id = htole16(0x1042);
	message->le_subsystem_vendor_id = htole16(0x1af4);
	message->le_subsystem_device_id = htole16(0x0002);
	message->le_class_code = htole32(0x010000);
	message->revision = 1;
	message->le_msix_vectors = htole16(1);
	message->bar[0].le_size = htole64(VMM_PCIE_ABI_PAGE_SIZE);
	message->bar[0].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY |
	    VMM_PCIE_ABI_BAR_F_64BIT | VMM_PCIE_ABI_BAR_F_DOORBELL_DIRECT);
}

static void
build_start(struct vmm_pcie_abi_start *message)
{
	memset(message, 0, sizeof(*message));
	packet_init(&message->header, VMM_PCIE_ABI_MSG_START, sizeof(*message),
	    VMM_PCIE_ABI_START_F_DMA_CAPABILITY);
	message->le_device_id = htole64(1);
	message->le_memory_generation = htole64(7);
	message->le_dma_segment_count = htole32(2);
	message->dma_segment[0].le_gpa = htole64(0x1000);
	message->dma_segment[0].le_iova = htole64(0x80001000);
	message->dma_segment[0].le_length = htole64(VMM_PCIE_ABI_PAGE_SIZE);
	message->dma_segment[0].le_flags = htole32(
	    VMM_PCIE_ABI_DMA_F_IOVA_VALID);
	message->dma_segment[0].le_permissions = htole32(
	    VMM_PCIE_ABI_DMA_PERM_READ | VMM_PCIE_ABI_DMA_PERM_WRITE);
	message->dma_segment[1].le_gpa = htole64(0x3000);
	message->dma_segment[1].le_iova = htole64(0x80003000);
	message->dma_segment[1].le_length = htole64(VMM_PCIE_ABI_PAGE_SIZE);
	message->dma_segment[1].le_flags = htole32(
	    VMM_PCIE_ABI_DMA_F_IOVA_VALID);
	message->dma_segment[1].le_permissions = htole32(
	    VMM_PCIE_ABI_DMA_PERM_READ | VMM_PCIE_ABI_DMA_PERM_WRITE);
}

static void
expect_result(const char *name, const void *message, size_t size, int want)
{
	int error;

	error = vmm_pcie_abi_validate(message, size);
	if (error != want)
		errx(1, "%s: got %d want %d", name, error, want);
}

int
main(void)
{
	struct vmm_pcie_abi_register register_message;
	struct vmm_pcie_abi_registered registered_message;
	struct vmm_pcie_abi_consumer_ready consumer_ready_message;
	struct vmm_pcie_abi_start start_message;
	struct vmm_pcie_abi_stop stop_message;
	struct vmm_pcie_abi_msix msix_message;
	struct vmm_pcie_abi_failure failure_message;
	struct vmm_pcie_abi_bar_cap bar_cap_message;

	build_register(&register_message);
	expect_result("valid register", &register_message,
	    sizeof(register_message), 0);

	build_register(&register_message);
	register_message.header.le_magic = htole32(0);
	expect_result("bad magic", &register_message, sizeof(register_message),
	    EINVAL);

	build_register(&register_message);
	register_message.header.le_version = htole16(0);
	expect_result("old version", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.header.le_size = htole32(sizeof(register_message) - 1);
	expect_result("bad packet size", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.header.le_type = htole16(0xffff);
	expect_result("unknown packet type", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.bar[0].le_size = htole64(VMM_PCIE_ABI_PAGE_SIZE + 1);
	expect_result("unaligned bar size", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.bar[0].le_size = htole64(3 * VMM_PCIE_ABI_PAGE_SIZE);
	expect_result("non-power-of-two bar size", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.bar[5].le_size = htole64(VMM_PCIE_ABI_PAGE_SIZE);
	register_message.bar[5].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY |
	    VMM_PCIE_ABI_BAR_F_64BIT);
	expect_result("last bar cannot be 64-bit", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.bar[1].le_size = htole64(VMM_PCIE_ABI_PAGE_SIZE);
	register_message.bar[1].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY);
	expect_result("64-bit bar companion occupied", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.header.le_flags = htole32(0);
	expect_result("msix capability required", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.le_msix_vectors = htole16(
	    VMM_PCIE_ABI_MAX_MSIX_VECTORS + 1);
	expect_result("msix vector limit", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.header.le_flags = htole32(
	    VMM_PCIE_ABI_REGISTER_F_MSIX | VMM_PCIE_ABI_REGISTER_F_PARENT);
	register_message.le_parent_consumer_id = htole64(4);
	expect_result("parent generation required", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.le_parent_consumer_id = htole64(4);
	expect_result("parent flag required", &register_message,
	    sizeof(register_message), EINVAL);

	build_register(&register_message);
	register_message.header.le_flags = htole32(
	    VMM_PCIE_ABI_REGISTER_F_MSIX | VMM_PCIE_ABI_REGISTER_F_PARENT);
	register_message.le_parent_consumer_id = htole64(4);
	register_message.le_parent_generation = htole64(9);
	expect_result("valid child register", &register_message,
	    sizeof(register_message), 0);

	memset(&registered_message, 0, sizeof(registered_message));
	packet_init(&registered_message.header, VMM_PCIE_ABI_MSG_REGISTERED,
	    sizeof(registered_message), 0);
	registered_message.le_device_id = htole64(1);
	registered_message.le_consumer_id = htole64(2);
	registered_message.le_attachment_generation = htole64(3);
	registered_message.le_bdf = htole32(8);
	registered_message.le_bar_fd_mask = htole32(1);
	registered_message.le_msix_vectors = htole16(1);
	expect_result("valid registered", &registered_message,
	    sizeof(registered_message), 0);

	registered_message.le_bar_fd_mask = htole32(1U << VMM_PCIE_ABI_MAX_BARS);
	expect_result("registered bar mask", &registered_message,
	    sizeof(registered_message), EINVAL);

	registered_message.le_bar_fd_mask = htole32(1);
	registered_message.le_bdf = htole32(0x00010000);
	expect_result("registered bdf segment", &registered_message,
	    sizeof(registered_message), EINVAL);

	memset(&consumer_ready_message, 0, sizeof(consumer_ready_message));
	packet_init(&consumer_ready_message.header,
	    VMM_PCIE_ABI_MSG_CONSUMER_READY, sizeof(consumer_ready_message), 0);
	consumer_ready_message.le_device_id = htole64(1);
	consumer_ready_message.le_consumer_id = htole64(2);
	consumer_ready_message.le_attachment_generation = htole64(3);
	expect_result("valid consumer ready", &consumer_ready_message,
	    sizeof(consumer_ready_message), 0);

	consumer_ready_message.le_consumer_id = htole64(0);
	expect_result("consumer ready id", &consumer_ready_message,
	    sizeof(consumer_ready_message), EINVAL);

	build_start(&start_message);
	expect_result("valid start", &start_message, sizeof(start_message), 0);

	build_start(&start_message);
	start_message.dma_segment[0].le_gpa = htole64(0x1001);
	expect_result("unaligned dma gpa", &start_message, sizeof(start_message),
	    EINVAL);

	build_start(&start_message);
	start_message.dma_segment[0].le_length = htole64(2 * VMM_PCIE_ABI_PAGE_SIZE);
	start_message.dma_segment[1].le_gpa = htole64(0x2000);
	expect_result("overlapping dma gpa", &start_message,
	    sizeof(start_message), EINVAL);

	build_start(&start_message);
	start_message.dma_segment[1].le_iova = htole64(0x80001000);
	expect_result("overlapping dma iova", &start_message,
	    sizeof(start_message), EINVAL);

	build_start(&start_message);
	start_message.dma_segment[0].le_permissions = htole32(0);
	expect_result("missing dma permissions", &start_message,
	    sizeof(start_message), EINVAL);

	build_start(&start_message);
	start_message.header.le_flags = htole32(0);
	expect_result("dma capability flag required", &start_message,
	    sizeof(start_message), EINVAL);

	build_start(&start_message);
	start_message.dma_segment[0].le_flags = htole32(0);
	expect_result("iova flag required", &start_message, sizeof(start_message),
	    EINVAL);

	build_start(&start_message);
	start_message.dma_segment[2].le_gpa = htole64(0x5000);
	expect_result("unused dma segment", &start_message, sizeof(start_message),
	    EINVAL);

	memset(&stop_message, 0, sizeof(stop_message));
	packet_init(&stop_message.header, VMM_PCIE_ABI_MSG_STOP,
	    sizeof(stop_message), 0);
	stop_message.le_device_id = htole64(1);
	stop_message.le_memory_generation = htole64(7);
	expect_result("valid stop", &stop_message, sizeof(stop_message), 0);

	stop_message.le_memory_generation = htole64(0);
	expect_result("stop generation", &stop_message, sizeof(stop_message),
	    EINVAL);

	memset(&msix_message, 0, sizeof(msix_message));
	packet_init(&msix_message.header, VMM_PCIE_ABI_MSG_MSIX,
	    sizeof(msix_message), 0);
	msix_message.le_device_id = htole64(1);
	msix_message.le_attachment_generation = htole64(7);
	msix_message.le_vector = htole16(VMM_PCIE_ABI_MAX_MSIX_VECTORS - 1);
	expect_result("valid msix", &msix_message, sizeof(msix_message), 0);

	msix_message.le_vector = htole16(VMM_PCIE_ABI_MAX_MSIX_VECTORS);
	expect_result("msix vector out of range", &msix_message,
	    sizeof(msix_message), EINVAL);

	build_register(&register_message);
	register_message.le_msix_vectors = htole16(VMM_PCIE_ABI_MAX_MSIX_VECTORS);
	register_message.bar[0].le_size = htole64(0x8000);
	expect_result("msix table does not fit bar", &register_message,
	    sizeof(register_message), EINVAL);
	register_message.bar[0].le_size = htole64(0x10000);
	expect_result("max msix table fits bar", &register_message,
	    sizeof(register_message), 0);

	memset(&failure_message, 0, sizeof(failure_message));
	packet_init(&failure_message.header, VMM_PCIE_ABI_MSG_FAILURE,
	    sizeof(failure_message), 0);
	failure_message.le_device_id = htole64(1);
	failure_message.le_generation = htole64(7);
	failure_message.le_error = htole32(EIO);
	failure_message.le_stage = htole32(VMM_PCIE_ABI_FAILURE_PROVIDER);
	memcpy(failure_message.message, "provider disconnected",
	    sizeof("provider disconnected"));
	expect_result("valid failure", &failure_message,
	    sizeof(failure_message), 0);

	memset(failure_message.message, 'x', sizeof(failure_message.message));
	expect_result("failure message terminator", &failure_message,
	    sizeof(failure_message), EINVAL);

	memset(&bar_cap_message, 0, sizeof(bar_cap_message));
	packet_init(&bar_cap_message.header, VMM_PCIE_ABI_MSG_BAR_CAP,
	    sizeof(bar_cap_message), 0);
	bar_cap_message.le_device_id = htole64(1);
	bar_cap_message.le_generation = htole64(7);
	bar_cap_message.le_size = htole64(VMM_PCIE_ABI_PAGE_SIZE);
	bar_cap_message.le_bar_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY |
	    VMM_PCIE_ABI_BAR_F_DOORBELL_DIRECT);
	expect_result("valid bar capability", &bar_cap_message,
	    sizeof(bar_cap_message), 0);

	bar_cap_message.le_bar_index = htole32(VMM_PCIE_ABI_MAX_BARS);
	expect_result("bar capability index", &bar_cap_message,
	    sizeof(bar_cap_message), EINVAL);

	return 0;
}
