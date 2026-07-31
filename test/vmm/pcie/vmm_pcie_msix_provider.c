/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Provider for the Linux direct-doorbell and MSI-X test.
 */
#include <sys/endian.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vmm_pcie_abi.h"

#define DFVMM_PCIE_BAR_SIZE		0x2000U
#define DFVMM_PCIE_KICK_OFFSET		0x1000U
#define DFVMM_PCIE_STATUS_OFFSET	0x1004U
#define DFVMM_PCIE_KICK_VALUE		0x6b69636bU
#define DFVMM_PCIE_STATUS_VALUE		0x646f6e65U

static void build_register(struct vmm_pcie_abi_register *message,
	    uint64_t generation);
static int recv_registered(int fd, const struct vmm_pcie_abi_start *start,
	    struct vmm_pcie_abi_registered *message);
static void recv_start(int fd, struct vmm_pcie_abi_start *message);
static void send_msix(int fd, const struct vmm_pcie_abi_registered *registered);

int
main(int argc, char **argv)
{
	struct vmm_pcie_abi_register register_message;
	struct vmm_pcie_abi_registered registered_message;
	struct vmm_pcie_abi_start start_message;
	volatile uint32_t *bar;
	uint8_t *bar_bytes;
	char path[1024];
	ssize_t n;
	int bar_fd;
	int fd;

	if (argc != 2)
		errno = EINVAL, err(1, "usage: %s DEVICE_DIR", argv[0]);
	if (snprintf(path, sizeof(path), "%s/provider", argv[1]) >=
	    (int)sizeof(path))
		err(1, "provider path");
	fd = open(path, O_RDWR);
	if (fd < 0)
		err(1, "open %s", path);
	recv_start(fd, &start_message);
	build_register(&register_message,
	    le64toh(start_message.header.le_sequence));
	n = send(fd, &register_message, sizeof(register_message), 0);
	if (n != sizeof(register_message))
		errno = n < 0 ? errno : EPROTO, err(1, "send REGISTER");
	bar_fd = recv_registered(fd, &start_message, &registered_message);
	bar = mmap(NULL, DFVMM_PCIE_BAR_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, bar_fd, 0);
	if (bar == MAP_FAILED)
		err(1, "mmap BAR");
	bar_bytes = (uint8_t *)(uintptr_t)bar;
	__atomic_store_n((uint32_t *)(uintptr_t)(bar_bytes +
	    DFVMM_PCIE_KICK_OFFSET), 0, __ATOMIC_RELEASE);
	__atomic_store_n((uint32_t *)(uintptr_t)(bar_bytes +
	    DFVMM_PCIE_STATUS_OFFSET), 0, __ATOMIC_RELEASE);
	printf("DFVMM_PCIE_MSIX_PROVIDER_READY\n");
	if (fflush(stdout) != 0)
		err(1, "flush stdout");
	for (;;) {
		uint32_t kick;

		kick = __atomic_load_n((const uint32_t *)(uintptr_t)(bar_bytes +
		    DFVMM_PCIE_KICK_OFFSET),
		    __ATOMIC_ACQUIRE);
		if (kick == DFVMM_PCIE_KICK_VALUE) {
			__atomic_store_n((uint32_t *)(uintptr_t)(bar_bytes +
			    DFVMM_PCIE_STATUS_OFFSET), DFVMM_PCIE_STATUS_VALUE,
			    __ATOMIC_RELEASE);
			printf("DFVMM_PCIE_MSIX_PROVIDER_KICK addr=%08x:%08x data=%08x control=%08x pba=%08x\n",
			    __atomic_load_n((const uint32_t *)(uintptr_t)(bar_bytes + 4),
			    __ATOMIC_ACQUIRE),
			    __atomic_load_n((const uint32_t *)(uintptr_t)bar_bytes,
			    __ATOMIC_ACQUIRE),
			    __atomic_load_n((const uint32_t *)(uintptr_t)(bar_bytes + 8),
			    __ATOMIC_ACQUIRE),
			    __atomic_load_n((const uint32_t *)(uintptr_t)(bar_bytes + 12),
			    __ATOMIC_ACQUIRE),
			    __atomic_load_n((const uint32_t *)(uintptr_t)(bar_bytes + 16),
			    __ATOMIC_ACQUIRE));
			if (fflush(stdout) != 0)
				err(1, "flush kick");
			send_msix(fd, &registered_message);
			printf("DFVMM_PCIE_MSIX_PROVIDER_SENT\n");
			if (fflush(stdout) != 0)
				err(1, "flush msix");
			for (;;)
				usleep(1000000);
		}
		usleep(1000);
	}
}

static void
build_register(struct vmm_pcie_abi_register *message, uint64_t generation)
{

	memset(message, 0, sizeof(*message));
	message->header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	message->header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	message->header.le_type = htole16(VMM_PCIE_ABI_MSG_REGISTER);
	message->header.le_size = htole32(sizeof(*message));
	message->header.le_flags = htole32(VMM_PCIE_ABI_REGISTER_F_MSIX);
	message->header.le_sequence = htole64(generation);
	message->le_vendor_id = htole16(0x1b36);
	message->le_device_id = htole16(0xdf03);
	message->le_subsystem_vendor_id = htole16(0x1b36);
	message->le_subsystem_device_id = htole16(0xdf03);
	message->le_class_code = htole32(0xff0000);
	message->revision = 1;
	message->le_msix_vectors = htole16(1);
	message->bar[0].le_size = htole64(DFVMM_PCIE_BAR_SIZE);
	message->bar[0].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY |
	    VMM_PCIE_ABI_BAR_F_DOORBELL_DIRECT);
}

static int
recv_registered(int fd, const struct vmm_pcie_abi_start *start,
	struct vmm_pcie_abi_registered *message)
{
	char control[CMSG_SPACE(sizeof(int) * 2)];
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr msg;
	ssize_t n;
	int fds[2];

	memset(control, 0, sizeof(control));
	memset(&msg, 0, sizeof(msg));
	iov.iov_base = message;
	iov.iov_len = sizeof(*message);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	n = recvmsg(fd, &msg, 0);
	if (n != sizeof(*message) ||
	    (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0)
		errno = n < 0 ? errno : EPROTO, err(1, "recv REGISTERED");
	if (vmm_pcie_abi_validate(message, sizeof(*message)) != 0 ||
	    le16toh(message->header.le_type) != VMM_PCIE_ABI_MSG_REGISTERED ||
	    message->header.le_sequence != start->header.le_sequence ||
	    (le32toh(message->header.le_flags) &
	    VMM_PCIE_ABI_REGISTERED_F_DMA_CAPABILITY) == 0 ||
	    le32toh(message->le_bdf) != VMM_PCIE_ABI_BDF(0, 1, 0))
		errno = EPROTO, err(1, "invalid REGISTERED");
	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS ||
	    cmsg->cmsg_len != CMSG_LEN(sizeof(fds)) ||
	    CMSG_NXTHDR(&msg, cmsg) != NULL)
		errno = EPROTO, err(1, "REGISTERED rights");
	memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));
	if (le32toh(message->le_bar_fd_mask) != 1 || fds[0] < 0 || fds[1] < 0)
		errno = EPROTO, err(1, "REGISTERED capability set");
	if (close(fds[1]) != 0)
		err(1, "close DMA fd");
	return fds[0];
}

static void
recv_start(int fd, struct vmm_pcie_abi_start *message)
{
	ssize_t n;

	n = recv(fd, message, sizeof(*message), 0);
	if (n != sizeof(*message))
		errno = n < 0 ? errno : EPROTO, err(1, "recv START");
	if (vmm_pcie_abi_validate(message, sizeof(*message)) != 0 ||
	    le16toh(message->header.le_type) != VMM_PCIE_ABI_MSG_START ||
	    le32toh(message->le_dma_segment_count) == 0)
		errno = EPROTO, err(1, "invalid START");
}

static void
send_msix(int fd, const struct vmm_pcie_abi_registered *registered)
{
	struct vmm_pcie_abi_msix message;
	ssize_t n;

	memset(&message, 0, sizeof(message));
	message.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	message.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	message.header.le_type = htole16(VMM_PCIE_ABI_MSG_MSIX);
	message.header.le_size = htole32(sizeof(message));
	message.header.le_sequence = htole64(2);
	message.le_device_id = registered->le_device_id;
	message.le_attachment_generation = registered->le_attachment_generation;
	message.le_vector = htole16(0);
	n = send(fd, &message, sizeof(message), 0);
	if (n != sizeof(message))
		errno = n < 0 ? errno : EPROTO, err(1, "send MSI-X");
}
