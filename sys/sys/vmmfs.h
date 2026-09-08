/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMMFS public ABI.
 *
 * This is a host-local, native-endian ABI between vmmfs(5) and its clients.
 * It is not a guest wire protocol.
 */
#ifndef _SYS_VMMFS_H_
#define _SYS_VMMFS_H_

#include <sys/types.h>

#ifndef _KERNEL
#ifndef CTASSERT
#define CTASSERT(expression) _Static_assert((expression), #expression)
#endif
#endif

/*
 * One descriptor write contains the header, followed in order by doorbell,
 * register, capability and ext_capability arrays, then data_size bytes.
 * Counts select array lengths; offsets are relative to that final byte array.
 * All reserved and inapplicable fields must be zero. No pointers or packing.
 * A zero-byte write removes the descriptor. Reads return canonical text.
 */
#define VMMFS_PCI_DESCRIPTOR_VERSION 1
#define VMMFS_PCI_MAX_BARS 6
#define VMMFS_PCI_MAX_DOORBELLS 16
#define VMMFS_PCI_MAX_CONFIGS 64
#define VMMFS_PCI_MAX_CAPS 32
#define VMMFS_PCI_MAX_ECAPS 32
#define VMMFS_PCI_MAX_DATA 4096
#define VMMFS_PCI_DESCRIPTOR_MAX 7176
#define VMMFS_PCI_BAR_IO 1
#define VMMFS_PCI_BAR_MEM32 2
#define VMMFS_PCI_BAR_MEM64 3
#define VMMFS_PCI_CAP_PCIE 1
#define VMMFS_PCI_CAP_MSI 2
#define VMMFS_PCI_CAP_MSIX 3
#define VMMFS_PCI_CAP_BLOB 4

struct vmmfs_pci_bar {
	uint64_t size;
	uint8_t type;
	uint8_t prefetchable;
	uint8_t reserved[6];
};

struct vmmfs_pci_descriptor {
	uint32_t version;
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t subsystem_vendor_id;
	uint16_t subsystem_device_id;
	uint32_t class_code;
	uint8_t revision;
	uint8_t intx_pin;
	uint16_t doorbell_count;
	uint16_t config_count;
	uint16_t cap_count;
	uint16_t ecap_count;
	uint16_t reserved;
	uint32_t data_size;
	uint64_t rom_size;
	struct vmmfs_pci_bar bars[VMMFS_PCI_MAX_BARS];
};

struct vmmfs_pci_doorbell {
	uint64_t offset;
	uint64_t size;
	uint8_t bar;
	uint8_t width;
	uint8_t space;
	uint8_t reserved[5];
};

struct vmmfs_pci_register {
	uint64_t offset;
	uint8_t bar;
	uint8_t width;
	uint8_t space;
	uint8_t reserved[5];
};

struct vmmfs_pci_capability {
	uint64_t table_offset;
	uint64_t pba_offset;
	uint32_t data_offset;
	uint32_t data_length;
	uint16_t vectors;
	uint8_t kind;
	uint8_t address_width;
	uint8_t maskable;
	uint8_t table_bar;
	uint8_t pba_bar;
	uint8_t id;
};

struct vmmfs_pci_ext_capability {
	uint32_t data_offset;
	uint32_t data_length;
	uint16_t id;
	uint8_t version;
	uint8_t reserved[5];
};

CTASSERT(VMMFS_PCI_DESCRIPTOR_MAX == sizeof(struct vmmfs_pci_descriptor) +
    VMMFS_PCI_MAX_DOORBELLS * sizeof(struct vmmfs_pci_doorbell) +
    VMMFS_PCI_MAX_CONFIGS * sizeof(struct vmmfs_pci_register) +
    VMMFS_PCI_MAX_CAPS * sizeof(struct vmmfs_pci_capability) +
    VMMFS_PCI_MAX_ECAPS * sizeof(struct vmmfs_pci_ext_capability) +
    VMMFS_PCI_MAX_DATA);
CTASSERT(sizeof(struct vmmfs_pci_bar) == 16);
CTASSERT(sizeof(struct vmmfs_pci_descriptor) == 136);
CTASSERT(__offsetof(struct vmmfs_pci_descriptor, data_size) == 28);
CTASSERT(__offsetof(struct vmmfs_pci_descriptor, rom_size) == 32);
CTASSERT(__offsetof(struct vmmfs_pci_descriptor, bars) == 40);
CTASSERT(sizeof(struct vmmfs_pci_doorbell) == 24);
CTASSERT(__offsetof(struct vmmfs_pci_doorbell, bar) == 16);
CTASSERT(sizeof(struct vmmfs_pci_register) == 16);
CTASSERT(__offsetof(struct vmmfs_pci_register, bar) == 8);
CTASSERT(sizeof(struct vmmfs_pci_capability) == 32);
CTASSERT(__offsetof(struct vmmfs_pci_capability, data_offset) == 16);
CTASSERT(__offsetof(struct vmmfs_pci_capability, vectors) == 24);
CTASSERT(__offsetof(struct vmmfs_pci_capability, kind) == 26);
CTASSERT(sizeof(struct vmmfs_pci_ext_capability) == 16);
CTASSERT(__offsetof(struct vmmfs_pci_ext_capability, id) == 8);

/*
 * Machine events written to <machine>/events.  The event name is fixed by
 * this enum; callers may append event-specific key=value arguments.
 */
enum vmmfs_machine_event {
	VMMFS_MACHINE_EVENT_CREATED = 1,
	VMMFS_MACHINE_EVENT_DESTROY_REFUSED,
	VMMFS_MACHINE_EVENT_STOP_REQUESTED,
	VMMFS_MACHINE_EVENT_STOPPED,
	VMMFS_MACHINE_EVENT_RESET_REQUESTED,
	VMMFS_MACHINE_EVENT_RESET_STARTED,
	VMMFS_MACHINE_EVENT_RESET_COMPLETED,
	VMMFS_MACHINE_EVENT_RESET_FAILED,
	VMMFS_MACHINE_EVENT_START_REQUESTED,
	VMMFS_MACHINE_EVENT_START_COMPLETED,
	VMMFS_MACHINE_EVENT_START_FAILED,
	VMMFS_MACHINE_EVENT_BOOT_REQUESTED,
	VMMFS_MACHINE_EVENT_BOOT_READY,
	VMMFS_MACHINE_EVENT_BOOT_COMPLETED,
	VMMFS_MACHINE_EVENT_BOOT_FAILED,
	VMMFS_MACHINE_EVENT_LOADER_SUBMITTED_CPUSTATE,
	VMMFS_MACHINE_EVENT_LOADER_FAILED,
	VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED,
	VMMFS_MACHINE_EVENT_PCI_CREATE_FAILED,
	VMMFS_MACHINE_EVENT_GUEST_STOP_REQUEST_FAILED,
	VMMFS_MACHINE_EVENT_GUEST_RESET_REQUEST_FAILED,
	VMMFS_MACHINE_EVENT_VCPU_INJECT_GP_FAILED,
	VMMFS_MACHINE_EVENT_VCPU_HALTED,
	VMMFS_MACHINE_EVENT_VCPU_SHUTDOWN,
	VMMFS_MACHINE_EVENT_VCPU_UNSUPPORTED_EXIT,
	VMMFS_MACHINE_EVENT_VCPU_FAILED,
};

/*
 * <machine>/pci/<slot>/powered is a read-only "0\n" or "1\n" attribute.
 * Observe EVFILT_VNODE with NOTE_WRITE | NOTE_REVOKE and EV_CLEAR, then
 * pread at offset zero to obtain current state. Notifications may coalesce.
 * Register before reading the initial state. Revocation of powered means
 * slot removal, not a cold power-off. fdrevoke removes knotes, so terminal
 * notification delivery is not guaranteed. Resource files retain their own lifetime.
 */

/*
 * The latest pending guest doorbell write.  The named kickN node determines
 * the declared doorbell range; offset is relative to that range.  width is
 * one of 1, 2, 4, or 8 and value contains its low width bytes.  A kick is an
 * advisory rescan notification and may replace an unread earlier record.
 */
struct vmmfs_pci_kick {
	uint64_t	offset;
	uint64_t	value;
	uint8_t		width;
	uint8_t		reserved[7];
};

/*
 * A backend writes this record to intx to drive its assigned PCI INTx line.
 * asserted is exactly zero or one; reserved must be zero.
 */
struct vmmfs_pci_intx {
	uint8_t		asserted;
	uint8_t		reserved[7];
};

/*
 * A backend writes this all-zero record to one named msiN or msixN node to
 * request delivery on the route selected by guest PCI configuration state.
 */
struct vmmfs_pci_interrupt {
	uint64_t	reserved;
};

/*
 * One synchronous scalar device-register access.  The slot descriptor names
 * the exact BAR or PIO range; bar and offset identify the access within that
 * declaration.  A backend reads requests from <slot>/config and writes the
 * matching response before the owning guest vCPU may resume.
 */
enum vmmfs_pci_config_space {
	VMMFS_PCI_CONFIG_MMIO = 1,
	VMMFS_PCI_CONFIG_PIO = 2,
};

enum vmmfs_pci_config_operation {
	VMMFS_PCI_CONFIG_READ = 1,
	VMMFS_PCI_CONFIG_WRITE = 2,
};

enum vmmfs_pci_config_status {
	VMMFS_PCI_CONFIG_SUCCESS = 0,
	VMMFS_PCI_CONFIG_UNSUPPORTED = 1,
	VMMFS_PCI_CONFIG_FAILURE = 2,
};

struct vmmfs_pci_config_request {
	uint64_t	generation;
	uint64_t	sequence;
	uint64_t	offset;
	uint64_t	value;
	uint16_t	bar;
	uint8_t		space;
	uint8_t		width;
	uint8_t		operation;
	uint8_t		reserved[3];
};

struct vmmfs_pci_config_response {
	uint64_t	generation;
	uint64_t	sequence;
	uint64_t	value;
	uint32_t	status;
	uint32_t	reserved;
};

CTASSERT(sizeof(struct vmmfs_pci_kick) == 24);
CTASSERT(sizeof(struct vmmfs_pci_intx) == 8);
CTASSERT(sizeof(struct vmmfs_pci_interrupt) == 8);
CTASSERT(sizeof(struct vmmfs_pci_config_request) == 40);
CTASSERT(sizeof(struct vmmfs_pci_config_response) == 32);

#endif /* _SYS_VMMFS_H_ */
