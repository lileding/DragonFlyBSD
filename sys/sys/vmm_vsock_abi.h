/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private runv-to-virtiod virtio-vsock broker ABI.
 *
 * Every multi-byte field is little-endian.  The control socket is an
 * AF_UNIX SOCK_SEQPACKET capability.  CONNECTED and ACCEPTED return one
 * AF_UNIX SOCK_STREAM descriptor through SCM_RIGHTS when le_status is zero.
 */
#ifndef _SYS_VMM_VSOCK_ABI_H_
#define _SYS_VMM_VSOCK_ABI_H_

#include <sys/types.h>

#define VMM_VSOCK_ABI_MAGIC		0x564d5356U
#define VMM_VSOCK_ABI_VERSION		1U
#define VMM_VSOCK_HOST_CID		2ULL

enum vmm_vsock_abi_message_type {
	VMM_VSOCK_ABI_MSG_CONNECT = 1,
	VMM_VSOCK_ABI_MSG_LISTEN,
	VMM_VSOCK_ABI_MSG_ACCEPT,
	VMM_VSOCK_ABI_MSG_UNLISTEN,
	VMM_VSOCK_ABI_MSG_CONNECTED,
	VMM_VSOCK_ABI_MSG_ACCEPTED,
	VMM_VSOCK_ABI_MSG_COMPLETE,
	VMM_VSOCK_ABI_MSG_DEVICE_DOWN,
};

struct vmm_vsock_abi_header {
	uint32_t	le_magic;
	uint16_t	le_version;
	uint16_t	le_type;
	uint32_t	le_size;
	uint32_t	le_flags;
	uint64_t	le_sequence;
} __attribute__((__packed__));

struct vmm_vsock_abi_request {
	struct vmm_vsock_abi_header header;
	uint64_t	le_cid;
	uint32_t	le_port;
	uint32_t	le_reserved;
} __attribute__((__packed__));

struct vmm_vsock_abi_result {
	struct vmm_vsock_abi_header header;
	uint64_t	le_cid;
	uint32_t	le_port;
	uint32_t	le_status;
} __attribute__((__packed__));

_Static_assert(sizeof(struct vmm_vsock_abi_header) == 24,
    "vsock ABI header size");
_Static_assert(sizeof(struct vmm_vsock_abi_request) == 40,
    "vsock ABI request size");
_Static_assert(sizeof(struct vmm_vsock_abi_result) == 40,
    "vsock ABI result size");

#endif /* _SYS_VMM_VSOCK_ABI_H_ */
