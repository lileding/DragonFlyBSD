/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Provider for the P7 revocable guest-DMA capability harness.
 */
#include <sys/endian.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vmm_pcie_abi.h"

static sigjmp_buf dma_fault_jmp;
static volatile sig_atomic_t dma_fault_signal;

static void	build_register(struct vmm_pcie_abi_register *, uint64_t);
static void	receive_registered(int, const struct vmm_pcie_abi_start *,
		    int *, int *);
static void	receive_start(int, struct vmm_pcie_abi_start *);
static void	receive_stop(int, const struct vmm_pcie_abi_start *);
static void	send_stopped(int, const struct vmm_pcie_abi_start *);
static void	dma_fault_handler(int);
static void	expect_revoked(volatile uint8_t *);

int
main(int argc, char **argv)
{
	struct vmm_pcie_abi_register register_message;
	struct vmm_pcie_abi_start start_message;
	volatile uint8_t *bar;
	volatile uint8_t *dma;
	char path[1024];
	uint64_t gpa;
	uint64_t size;
	ssize_t n;
	int dma_fd;
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
	receive_start(fd, &start_message);
	build_register(&register_message,
	    le64toh(start_message.header.le_sequence));
	n = send(fd, &register_message, sizeof(register_message), 0);
	if (n != sizeof(register_message))
		errno = n < 0 ? errno : EPROTO, err(1, "send REGISTER");
	receive_registered(fd, &start_message, &bar_fd, &dma_fd);
	bar = mmap(NULL, VMM_PCIE_ABI_PAGE_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, bar_fd, 0);
	if (bar == MAP_FAILED)
		err(1, "mmap BAR");
	bar[0] = 0x5a;
	if (bar[0] != 0x5a)
		errno = EIO, err(1, "BAR shared mapping");
	gpa = le64toh(start_message.dma_segment[0].le_gpa);
	size = le64toh(start_message.dma_segment[0].le_length);
	dma = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fd,
	    (off_t)gpa);
	if (dma == MAP_FAILED)
		err(1, "mmap DMA");
	printf("DFVMM_PCIE_DMA_PROVIDER_READY generation=%ju gpa=0x%jx size=0x%jx\n",
	    (uintmax_t)le64toh(start_message.le_memory_generation),
	    (uintmax_t)gpa, (uintmax_t)size);
	if (fflush(stdout) != 0)
		err(1, "flush ready");
	receive_stop(fd, &start_message);
	send_stopped(fd, &start_message);
	expect_revoked(dma);
	expect_revoked(bar);
	printf("DFVMM_PCIE_DMA_PROVIDER_REVOKED\n");
	if (fflush(stdout) != 0)
		err(1, "flush revoke");
	return 0;
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
	message->le_device_id = htole16(0xdf07);
	message->le_subsystem_vendor_id = htole16(0x1b36);
	message->le_subsystem_device_id = htole16(0xdf07);
	message->le_class_code = htole32(0xff0000);
	message->revision = 1;
	message->le_msix_vectors = htole16(1);
	message->bar[0].le_size = htole64(VMM_PCIE_ABI_PAGE_SIZE);
	message->bar[0].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY |
	    VMM_PCIE_ABI_BAR_F_DOORBELL_DIRECT);
}

static void
receive_registered(int fd, const struct vmm_pcie_abi_start *start,
    int *bar_fdp, int *dma_fdp)
{
	struct vmm_pcie_abi_registered message;
	char control[CMSG_SPACE(sizeof(int) * 2)];
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr socket_message;
	int fds[2];
	ssize_t n;

	memset(control, 0, sizeof(control));
	memset(&socket_message, 0, sizeof(socket_message));
	iov.iov_base = &message;
	iov.iov_len = sizeof(message);
	socket_message.msg_iov = &iov;
	socket_message.msg_iovlen = 1;
	socket_message.msg_control = control;
	socket_message.msg_controllen = sizeof(control);
	n = recvmsg(fd, &socket_message, 0);
	if (n != sizeof(message) ||
	    (socket_message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0)
		errno = n < 0 ? errno : EPROTO, err(1, "recv REGISTERED");
	if (vmm_pcie_abi_validate(&message, sizeof(message)) != 0 ||
	    le16toh(message.header.le_type) != VMM_PCIE_ABI_MSG_REGISTERED ||
	    message.header.le_sequence != start->header.le_sequence ||
	    (le32toh(message.header.le_flags) &
	    VMM_PCIE_ABI_REGISTERED_F_DMA_CAPABILITY) == 0)
		errno = EPROTO, err(1, "invalid REGISTERED");
	cmsg = CMSG_FIRSTHDR(&socket_message);
	if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS ||
	    cmsg->cmsg_len != CMSG_LEN(sizeof(fds)) ||
	    CMSG_NXTHDR(&socket_message, cmsg) != NULL)
		errno = EPROTO, err(1, "REGISTERED rights");
	memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));
	if (bar_fdp == NULL || dma_fdp == NULL ||
	    le32toh(message.le_bar_fd_mask) != 1 || fds[0] < 0 || fds[1] < 0)
		errno = EPROTO, err(1, "REGISTERED capability set");
	*bar_fdp = fds[0];
	*dma_fdp = fds[1];
}

static void
receive_start(int fd, struct vmm_pcie_abi_start *message)
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
receive_stop(int fd, const struct vmm_pcie_abi_start *start)
{
	struct vmm_pcie_abi_stop message;
	ssize_t n;

	n = recv(fd, &message, sizeof(message), 0);
	if (n != sizeof(message))
		errno = n < 0 ? errno : EPROTO, err(1, "recv STOP");
	if (vmm_pcie_abi_validate(&message, sizeof(message)) != 0 ||
	    le16toh(message.header.le_type) != VMM_PCIE_ABI_MSG_STOP ||
	    message.le_device_id != start->le_device_id ||
	    message.le_memory_generation != start->le_memory_generation)
		errno = EPROTO, err(1, "invalid STOP");
}

static void
send_stopped(int fd, const struct vmm_pcie_abi_start *start)
{
	struct vmm_pcie_abi_stopped message;
	ssize_t n;

	memset(&message, 0, sizeof(message));
	message.header.le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	message.header.le_version = htole16(VMM_PCIE_ABI_VERSION);
	message.header.le_type = htole16(VMM_PCIE_ABI_MSG_STOPPED);
	message.header.le_size = htole32(sizeof(message));
	message.header.le_sequence = start->header.le_sequence;
	message.le_device_id = start->le_device_id;
	message.le_memory_generation = start->le_memory_generation;
	n = send(fd, &message, sizeof(message), 0);
	if (n != sizeof(message))
		errno = n < 0 ? errno : EPROTO, err(1, "send STOPPED");
}

static void
dma_fault_handler(int signal_number)
{

	dma_fault_signal = signal_number;
	siglongjmp(dma_fault_jmp, 1);
}

static void
expect_revoked(volatile uint8_t *dma)
{
	struct sigaction action;
	unsigned int i;

	memset(&action, 0, sizeof(action));
	action.sa_handler = dma_fault_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGBUS, &action, NULL) != 0 ||
	    sigaction(SIGSEGV, &action, NULL) != 0)
		err(1, "sigaction");
	for (i = 0; i < 100; i++) {
		dma_fault_signal = 0;
		if (sigsetjmp(dma_fault_jmp, 1) == 0) {
			dma[0] ^= 1;
			usleep(10000);
			continue;
		}
		if (dma_fault_signal == SIGBUS || dma_fault_signal == SIGSEGV)
			return;
	}
	errno = EBUSY;
	err(1, "DMA mapping was not revoked");
}
