/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Public vPCIe provider/consumer packet ABI.
 *
 * Every multi-byte field is little-endian on the wire.  Each SOCK_SEQPACKET
 * message has one of the fixed-size layouts below; SCM_RIGHTS carries the
 * capability file descriptors associated with REGISTERED or BAR_CAP.  START
 * carries only the run's DMA aperture description; REGISTERED carries the
 * corresponding capability fds.  The header sequence is the run generation
 * for START, REGISTER, REGISTERED, STOP, and STOPPED.
 */
#ifndef VMM_PCIE_ABI_H
#define VMM_PCIE_ABI_H

#include <sys/types.h>

#define VMM_PCIE_ABI_MAGIC			0x564d4d50U
#define VMM_PCIE_ABI_VERSION			4U
#define VMM_PCIE_ABI_PAGE_SIZE			4096ULL
#define VMM_PCIE_ABI_MAX_BARS			6U
#define VMM_PCIE_ABI_MAX_MSIX_VECTORS		2048U
#define VMM_PCIE_ABI_MAX_DMA_SEGMENTS		32U
#define VMM_PCIE_ABI_MAX_VENDOR_CAPS		4U
#define VMM_PCIE_ABI_VENDOR_CAP_MIN_SIZE	3U
#define VMM_PCIE_ABI_VENDOR_CAP_MAX_SIZE	128U
#define VMM_PCIE_ABI_VENDOR_CAP_PAYLOAD_SIZE \
	(VMM_PCIE_ABI_VENDOR_CAP_MAX_SIZE - VMM_PCIE_ABI_VENDOR_CAP_MIN_SIZE)
#define VMM_PCIE_ABI_FAILURE_TEXT_SIZE		96U

/*
 * The ABI reserves the beginning of BAR0 for the standard MSI-X table and PBA.
 * A provider's device-specific BAR0 layout starts after
 * VMM_PCIE_ABI_MSIX_MIN_BAR_SIZE(vectors).
 */
#define VMM_PCIE_ABI_MSIX_BAR_INDEX		0U
#define VMM_PCIE_ABI_MSIX_TABLE_OFFSET		0ULL
#define VMM_PCIE_ABI_MSIX_ENTRY_SIZE		16ULL
#define VMM_PCIE_ABI_MSIX_PBA_ALIGN		8ULL
#define VMM_PCIE_ABI_MSIX_TABLE_SIZE(vectors) \
	((uint64_t)(vectors) * VMM_PCIE_ABI_MSIX_ENTRY_SIZE)
#define VMM_PCIE_ABI_MSIX_PBA_OFFSET(vectors) \
	((VMM_PCIE_ABI_MSIX_TABLE_SIZE(vectors) + \
	VMM_PCIE_ABI_MSIX_PBA_ALIGN - 1ULL) & \
	~(VMM_PCIE_ABI_MSIX_PBA_ALIGN - 1ULL))
#define VMM_PCIE_ABI_MSIX_PBA_SIZE(vectors) \
	((((uint64_t)(vectors) + 63ULL) / 64ULL) * sizeof(uint64_t))
#define VMM_PCIE_ABI_MSIX_MIN_BAR_SIZE(vectors) \
	(VMM_PCIE_ABI_MSIX_PBA_OFFSET(vectors) + \
	VMM_PCIE_ABI_MSIX_PBA_SIZE(vectors))

#define VMM_PCIE_ABI_BDF_BUS_SHIFT		8U
#define VMM_PCIE_ABI_BDF_DEVICE_SHIFT		3U
#define VMM_PCIE_ABI_BDF_FUNCTION_MASK		0x00000007U
#define VMM_PCIE_ABI_BDF_DEVICE_MASK		0x0000001fU
#define VMM_PCIE_ABI_BDF_BUS_MASK		0x000000ffU
#define VMM_PCIE_ABI_BDF_MASK			0x0000ffffU
#define VMM_PCIE_ABI_BDF(bus, device, function) \
	((((uint32_t)(bus) & VMM_PCIE_ABI_BDF_BUS_MASK) << \
	VMM_PCIE_ABI_BDF_BUS_SHIFT) | \
	(((uint32_t)(device) & VMM_PCIE_ABI_BDF_DEVICE_MASK) << \
	VMM_PCIE_ABI_BDF_DEVICE_SHIFT) | \
	((uint32_t)(function) & VMM_PCIE_ABI_BDF_FUNCTION_MASK))

enum vmm_pcie_abi_message_type {
	VMM_PCIE_ABI_MSG_REGISTER = 1,
	VMM_PCIE_ABI_MSG_REGISTERED,
	VMM_PCIE_ABI_MSG_CONSUMER_READY,
	VMM_PCIE_ABI_MSG_START,
	VMM_PCIE_ABI_MSG_STOP,
	VMM_PCIE_ABI_MSG_STOPPED,
	VMM_PCIE_ABI_MSG_MSIX,
	VMM_PCIE_ABI_MSG_FAILURE,
	VMM_PCIE_ABI_MSG_BAR_CAP,
};

#define VMM_PCIE_ABI_REGISTER_F_MSIX		0x00000001U
#define VMM_PCIE_ABI_REGISTER_F_PARENT		0x00000002U

#define VMM_PCIE_ABI_BAR_F_MEMORY		0x00000001U
#define VMM_PCIE_ABI_BAR_F_64BIT		0x00000002U
#define VMM_PCIE_ABI_BAR_F_PREFETCHABLE		0x00000004U
#define VMM_PCIE_ABI_BAR_F_DOORBELL_DIRECT	0x00000008U
#define VMM_PCIE_ABI_BAR_F_DOORBELL_TRAPPED	0x00000010U

#define VMM_PCIE_ABI_REGISTERED_F_DMA_CAPABILITY	0x00000001U

#define VMM_PCIE_ABI_DMA_F_IOVA_VALID		0x00000001U

#define VMM_PCIE_ABI_DMA_PERM_READ		0x00000001U
#define VMM_PCIE_ABI_DMA_PERM_WRITE		0x00000002U

enum vmm_pcie_abi_failure_stage {
	VMM_PCIE_ABI_FAILURE_PROVIDER = 1,
	VMM_PCIE_ABI_FAILURE_PROTOCOL,
	VMM_PCIE_ABI_FAILURE_LIFECYCLE,
};

struct vmm_pcie_abi_header {
	uint32_t	le_magic;
	uint16_t	le_version;
	uint16_t	le_type;
	uint32_t	le_size;
	uint32_t	le_flags;
	uint64_t	le_sequence;
} __attribute__((__packed__));

struct vmm_pcie_abi_bar {
	uint64_t	le_size;
	uint32_t	le_flags;
	uint32_t	le_reserved;
} __attribute__((__packed__));

/*
 * A read-only PCI vendor-specific capability.  The core supplies the standard
 * capability ID and next pointer; bytes[] is the provider-defined payload
 * immediately following that three-byte PCI capability header.  This keeps
 * device-specific capability semantics outside vmm_pcie while retaining one
 * core-owned, validated capability chain.
 */
struct vmm_pcie_abi_vendor_cap {
	uint8_t		length;
	uint8_t		bytes[VMM_PCIE_ABI_VENDOR_CAP_PAYLOAD_SIZE];
} __attribute__((__packed__));

struct vmm_pcie_abi_dma_segment {
	uint64_t	le_gpa;
	uint64_t	le_iova;
	uint64_t	le_length;
	uint32_t	le_flags;
	uint32_t	le_permissions;
} __attribute__((__packed__));

struct vmm_pcie_abi_register {
	struct vmm_pcie_abi_header header;
	uint16_t	le_vendor_id;
	uint16_t	le_device_id;
	uint16_t	le_subsystem_vendor_id;
	uint16_t	le_subsystem_device_id;
	uint32_t	le_class_code;
	uint8_t	revision;
	uint8_t	reserved0;
	uint16_t	le_msix_vectors;
	uint64_t	le_parent_consumer_id;
	uint64_t	le_parent_generation;
	struct vmm_pcie_abi_bar bar[VMM_PCIE_ABI_MAX_BARS];
	uint8_t		vendor_cap_count;
	uint8_t		reserved1[3];
	struct vmm_pcie_abi_vendor_cap
			vendor_cap[VMM_PCIE_ABI_MAX_VENDOR_CAPS];
} __attribute__((__packed__));

struct vmm_pcie_abi_registered {
	struct vmm_pcie_abi_header header;
	uint64_t	le_device_id;
	uint64_t	le_consumer_id;
	uint64_t	le_attachment_generation;
	uint32_t	le_bdf;
	uint32_t	le_bar_fd_mask;
	uint16_t	le_msix_vectors;
	uint16_t	le_reserved;
} __attribute__((__packed__));

struct vmm_pcie_abi_consumer_ready {
	struct vmm_pcie_abi_header header;
	uint64_t	le_device_id;
	uint64_t	le_consumer_id;
	uint64_t	le_attachment_generation;
} __attribute__((__packed__));

struct vmm_pcie_abi_start {
	struct vmm_pcie_abi_header header;
	uint64_t	le_device_id;
	uint64_t	le_memory_generation;
	uint32_t	le_dma_segment_count;
	uint32_t	le_reserved;
	struct vmm_pcie_abi_dma_segment
			dma_segment[VMM_PCIE_ABI_MAX_DMA_SEGMENTS];
} __attribute__((__packed__));

struct vmm_pcie_abi_stop {
	struct vmm_pcie_abi_header header;
	uint64_t	le_device_id;
	uint64_t	le_memory_generation;
} __attribute__((__packed__));

struct vmm_pcie_abi_stopped {
	struct vmm_pcie_abi_header header;
	uint64_t	le_device_id;
	uint64_t	le_memory_generation;
} __attribute__((__packed__));

struct vmm_pcie_abi_msix {
	struct vmm_pcie_abi_header header;
	uint64_t	le_device_id;
	uint64_t	le_attachment_generation;
	uint16_t	le_vector;
	uint16_t	le_reserved0;
	uint32_t	le_reserved1;
} __attribute__((__packed__));

struct vmm_pcie_abi_failure {
	struct vmm_pcie_abi_header header;
	uint64_t	le_device_id;
	uint64_t	le_generation;
	uint32_t	le_error;
	uint32_t	le_stage;
	char		message[VMM_PCIE_ABI_FAILURE_TEXT_SIZE];
} __attribute__((__packed__));

struct vmm_pcie_abi_bar_cap {
	struct vmm_pcie_abi_header header;
	uint64_t	le_device_id;
	uint64_t	le_generation;
	uint64_t	le_size;
	uint32_t	le_bar_index;
	uint32_t	le_bar_flags;
} __attribute__((__packed__));

_Static_assert(sizeof(struct vmm_pcie_abi_header) == 24,
    "vPCIe ABI header size");
_Static_assert(sizeof(struct vmm_pcie_abi_bar) == 16,
    "vPCIe ABI BAR size");
_Static_assert(sizeof(struct vmm_pcie_abi_dma_segment) == 32,
    "vPCIe ABI DMA segment size");
_Static_assert(sizeof(struct vmm_pcie_abi_vendor_cap) == 126,
    "vPCIe ABI vendor capability size");
_Static_assert(sizeof(struct vmm_pcie_abi_register) == 660,
    "vPCIe ABI register size");
_Static_assert(sizeof(struct vmm_pcie_abi_registered) == 60,
    "vPCIe ABI registered size");
_Static_assert(sizeof(struct vmm_pcie_abi_consumer_ready) == 48,
    "vPCIe ABI consumer-ready size");
_Static_assert(sizeof(struct vmm_pcie_abi_start) == 1072,
    "vPCIe ABI start size");
_Static_assert(sizeof(struct vmm_pcie_abi_stop) == 40,
    "vPCIe ABI stop size");
_Static_assert(sizeof(struct vmm_pcie_abi_stopped) == 40,
    "vPCIe ABI stopped size");
_Static_assert(sizeof(struct vmm_pcie_abi_msix) == 48,
    "vPCIe ABI MSI-X size");
_Static_assert(sizeof(struct vmm_pcie_abi_failure) == 144,
    "vPCIe ABI failure size");
_Static_assert(sizeof(struct vmm_pcie_abi_bar_cap) == 56,
    "vPCIe ABI BAR capability size");

int	vmm_pcie_abi_validate(const void *message, size_t size);

#endif /* VMM_PCIE_ABI_H */
