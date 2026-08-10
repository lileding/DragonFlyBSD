/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly KVM-compatible control ABI.
 */
#ifndef _SYS_KVM_H_
#define _SYS_KVM_H_

#include <sys/ioccom.h>
#include <sys/types.h>

/*
 * Keep the Linux KVM API version and command numbers.  DragonFly encodes
 * ioctl directions with its native _IO* macros; DragonFly QEMU builds must
 * include this header instead of Linux's ioctl definitions.
 */
#define KVM_API_VERSION	12
#define KVMIO			0xAE

#define KVM_GET_API_VERSION	_IO(KVMIO, 0x00)
#define KVM_CREATE_VM		_IO(KVMIO, 0x01)
#define KVM_CHECK_EXTENSION	_IO(KVMIO, 0x03)
#define KVM_GET_VCPU_MMAP_SIZE	_IO(KVMIO, 0x04)

struct kvm_userspace_memory_region {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};

#define KVM_MEM_LOG_DIRTY_PAGES	(1U << 0)
#define KVM_MEM_READONLY		(1U << 1)
#define KVM_SET_USER_MEMORY_REGION \
	_IOW(KVMIO, 0x46, struct kvm_userspace_memory_region)

/*
 * DragonFly extension used by the QEMU port to create an event counter fd.
 * It will become the backing primitive for KVM_IOEVENTFD and KVM_IRQFD.
 */
#define KVM_DFLY_EVENTFD_NONBLOCK	0x00000001U
#define KVM_DFLY_EVENTFD_CLOEXEC	0x00000002U
#define KVM_DFLY_EVENTFD_VALID_FLAGS	(KVM_DFLY_EVENTFD_NONBLOCK | \
	KVM_DFLY_EVENTFD_CLOEXEC)

struct kvm_dfly_eventfd {
	uint64_t initial;
	uint32_t flags;
	int32_t fd;
};

#define KVM_DFLY_CREATE_EVENTFD \
	_IOWR('K', 0x01, struct kvm_dfly_eventfd)

/* KVM_CHECK_EXTENSION values used by the initial QEMU KVM probe. */
#define KVM_CAP_IRQCHIP		0
#define KVM_CAP_USER_MEMORY	3
#define KVM_CAP_NR_VCPUS		9
#define KVM_CAP_DESTROY_MEMORY_REGION_WORKS	21
#define KVM_CAP_JOIN_MEMORY_REGIONS_WORKS	30
#define KVM_CAP_IRQFD		32
#define KVM_CAP_IOEVENTFD		36
#define KVM_CAP_INTERNAL_ERROR_DATA	40
#define KVM_CAP_MAX_VCPUS		66
#define KVM_CAP_IOEVENTFD_ANY_LENGTH	122

#endif /* _SYS_KVM_H_ */
