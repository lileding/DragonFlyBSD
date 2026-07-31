/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Black-box P2 test for a vPCIe provider/consumer session.
 */
#include <sys/endian.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vmm_pcie_abi.h"

static void
packet_init(struct vmm_pcie_abi_header *header, uint16_t type, uint32_t size)
{

	memset(header, 0, sizeof(*header));
	header->le_magic = htole32(VMM_PCIE_ABI_MAGIC);
	header->le_version = htole16(VMM_PCIE_ABI_VERSION);
	header->le_type = htole16(type);
	header->le_size = htole32(size);
	header->le_sequence = htole64(1);
}

static void
build_register(struct vmm_pcie_abi_register *message)
{

	memset(message, 0, sizeof(*message));
	packet_init(&message->header, VMM_PCIE_ABI_MSG_REGISTER,
	    sizeof(*message));
	message->header.le_flags = htole32(VMM_PCIE_ABI_REGISTER_F_MSIX);
	message->le_vendor_id = htole16(0x1af4);
	message->le_device_id = htole16(0x1050);
	message->le_subsystem_vendor_id = htole16(0x1af4);
	message->le_subsystem_device_id = htole16(0x0001);
	message->le_class_code = htole32(0x010000);
	message->revision = 1;
	message->le_msix_vectors = htole16(1);
	message->bar[0].le_size = htole64(VMM_PCIE_ABI_PAGE_SIZE);
	message->bar[0].le_flags = htole32(VMM_PCIE_ABI_BAR_F_MEMORY |
	    VMM_PCIE_ABI_BAR_F_DOORBELL_DIRECT);
}

static void
read_state(const char *device, char *buf, size_t size)
{
	char path[PATH_MAX];
	int fd;
	ssize_t n;

	if (snprintf(path, sizeof(path), "%s/state", device) >= (int)sizeof(path))
		err(1, "state path");
	fd = open(path, O_RDONLY);
	if (fd < 0)
		err(1, "open %s", path);
	n = read(fd, buf, size - 1);
	if (n < 0)
		err(1, "read %s", path);
	if (close(fd) != 0)
		err(1, "close %s", path);
	buf[n] = '\0';
}

static void
expect_state(const char *device, const char *provider, const char *consumer)
{
	char buf[256];
	char want[128];

	read_state(device, buf, sizeof(buf));
	if (snprintf(want, sizeof(want), "provider=%s\nconsumer=%s\n",
	    provider, consumer) >= (int)sizeof(want))
		err(1, "state expectation");
	if (strcmp(buf, want) != 0)
		errno = EPROTO, err(1, "state got %s", buf);
}

static void
expect_state_eventually(const char *device, const char *provider,
    const char *consumer)
{
	char buf[256];
	char want[128];
	unsigned int i;

	if (snprintf(want, sizeof(want), "provider=%s\nconsumer=%s\n",
	    provider, consumer) >= (int)sizeof(want))
		err(1, "state expectation");
	for (i = 0; i < 100; ++i) {
		read_state(device, buf, sizeof(buf));
		if (strcmp(buf, want) == 0)
			return;
		usleep(10000);
	}
	errno = EPROTO;
	err(1, "state did not become %s", want);
}

static int
recv_registered(int fd, struct vmm_pcie_abi_registered *message)
{
	char control[CMSG_SPACE(sizeof(int) * VMM_PCIE_ABI_MAX_BARS)];
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr msg;
	int bar_fd = -1;
	ssize_t n;

	memset(control, 0, sizeof(control));
	memset(&msg, 0, sizeof(msg));
	iov.iov_base = message;
	iov.iov_len = sizeof(*message);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	n = recvmsg(fd, &msg, 0);
	if (n != sizeof(*message))
		errno = n < 0 ? errno : EPROTO, err(1, "recv REGISTERED");
	if (vmm_pcie_abi_validate(message, sizeof(*message)) != 0 ||
	    le16toh(message->header.le_type) != VMM_PCIE_ABI_MSG_REGISTERED)
		errno = EPROTO, err(1, "invalid REGISTERED");
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
	    cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET ||
		    cmsg->cmsg_type != SCM_RIGHTS ||
		    cmsg->cmsg_len != CMSG_LEN(sizeof(int)) || bar_fd != -1)
			errno = EPROTO, err(1, "REGISTERED rights");
		memcpy(&bar_fd, CMSG_DATA(cmsg), sizeof(bar_fd));
	}
	if (bar_fd < 0 || le32toh(message->le_bar_fd_mask) != 1)
		errno = EPROTO, err(1, "REGISTERED BAR set");
	return bar_fd;
}

static void
expect_consumer_ready(int fd)
{
	struct vmm_pcie_abi_consumer_ready message;
	ssize_t n;

	n = recv(fd, &message, sizeof(message), 0);
	if (n != sizeof(message))
		errno = n < 0 ? errno : EPROTO, err(1, "recv CONSUMER_READY");
	if (vmm_pcie_abi_validate(&message, sizeof(message)) != 0 ||
	    le16toh(message.header.le_type) != VMM_PCIE_ABI_MSG_CONSUMER_READY)
		errno = EPROTO, err(1, "invalid CONSUMER_READY");
}

static void
send_fd(int fd, int sent_fd)
{
	char control[CMSG_SPACE(sizeof(sent_fd))];
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr message;
	char byte;

	memset(&message, 0, sizeof(message));
	memset(control, 0, sizeof(control));
	byte = 'F';
	iov.iov_base = &byte;
	iov.iov_len = sizeof(byte);
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	cmsg = CMSG_FIRSTHDR(&message);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(sent_fd));
	memcpy(CMSG_DATA(cmsg), &sent_fd, sizeof(sent_fd));
	if (sendmsg(fd, &message, 0) != 1)
		err(1, "send provider fd");
}

static int
receive_fd(int fd)
{
	char control[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct msghdr message;
	char byte;
	int received_fd;

	memset(&message, 0, sizeof(message));
	memset(control, 0, sizeof(control));
	byte = 0;
	received_fd = -1;
	iov.iov_base = &byte;
	iov.iov_len = sizeof(byte);
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	if (recvmsg(fd, &message, 0) != 1)
		err(1, "receive provider fd");
	if ((message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0)
		errno = EPROTO, err(1, "truncated provider fd");
	for (cmsg = CMSG_FIRSTHDR(&message); cmsg != NULL;
	    cmsg = CMSG_NXTHDR(&message, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET ||
		    cmsg->cmsg_type != SCM_RIGHTS ||
		    cmsg->cmsg_len != CMSG_LEN(sizeof(received_fd)) ||
		    received_fd != -1)
			errno = EPROTO, err(1, "provider fd rights");
		memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
	}
	if (received_fd < 0)
		errno = EPROTO, err(1, "missing provider fd");
	return received_fd;
}

int
main(int argc, char **argv)
{
	struct vmm_pcie_abi_register register_message;
	struct vmm_pcie_abi_registered registered_message;
	struct stat st;
	char path[PATH_MAX];
	volatile uint8_t *bar;
	int provider;
	int consumer;
	int bar_fd;
	int handoff[2];
	int status;
	pid_t child;
	ssize_t n;

	if (argc != 2)
		errno = EINVAL, err(1, "usage: %s DEVICE_DIR", argv[0]);
	if (snprintf(path, sizeof(path), "%s/provider", argv[1]) >=
	    (int)sizeof(path))
		err(1, "provider path");
	provider = open(path, O_RDWR);
	if (provider < 0)
		err(1, "open %s", path);
	expect_state(argv[1], "attached", "root");

	build_register(&register_message);
	n = send(provider, &register_message, sizeof(register_message), 0);
	if (n != sizeof(register_message))
		errno = n < 0 ? errno : EPROTO, err(1, "send REGISTER");
	bar_fd = recv_registered(provider, &registered_message);
	if (fstat(bar_fd, &st) != 0)
		err(1, "fstat BAR");
	if (st.st_size != VMM_PCIE_ABI_PAGE_SIZE)
		errno = EPROTO, err(1, "BAR size");
	bar = mmap(NULL, VMM_PCIE_ABI_PAGE_SIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, bar_fd, 0);
	if (bar == MAP_FAILED)
		err(1, "mmap BAR");
	bar[0] = 0x5a;
	if (bar[0] != 0x5a)
		errno = EIO, err(1, "BAR shared mapping");
	if (munmap(__DECONST(void *, bar), VMM_PCIE_ABI_PAGE_SIZE) != 0)
		err(1, "munmap BAR");
	if (close(bar_fd) != 0)
		err(1, "close BAR");
	expect_state(argv[1], "registered", "root");
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, handoff) != 0)
		err(1, "socketpair provider handoff");
	child = fork();
	if (child < 0)
		err(1, "fork provider handoff");
	if (child == 0) {
		int transferred;
		char command;

		if (close(handoff[0]) != 0)
			err(1, "child close handoff parent");
		if (close(provider) != 0)
			err(1, "child close inherited provider");
		transferred = receive_fd(handoff[1]);
		if (send(handoff[1], "R", 1, 0) != 1)
			err(1, "child provider ready");
		if (recv(handoff[1], &command, sizeof(command), 0) != 1 ||
		    command != 'C')
			errno = EPROTO, err(1, "child provider close command");
		if (close(transferred) != 0)
			err(1, "child close transferred provider");
		if (close(handoff[1]) != 0)
			err(1, "child close handoff");
		return 0;
	}
	if (close(handoff[1]) != 0)
		err(1, "parent close handoff child");
	send_fd(handoff[0], provider);
	if (recv(handoff[0], path, 1, 0) != 1 || path[0] != 'R')
		errno = EPROTO, err(1, "provider transfer ready");
	if (close(provider) != 0)
		err(1, "close original provider");
	expect_state(argv[1], "registered", "root");
	if (send(handoff[0], "C", 1, 0) != 1)
		err(1, "provider transfer close");
	if (close(handoff[0]) != 0)
		err(1, "parent close handoff");
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		errno = EPROTO, err(1, "provider handoff child");
	expect_state_eventually(argv[1], "detached", "root");

	if (snprintf(path, sizeof(path), "%s/consumer", argv[1]) >=
	    (int)sizeof(path))
		err(1, "consumer path");
	consumer = open(path, O_RDWR);
	if (consumer < 0)
		err(1, "open %s", path);
	expect_consumer_ready(consumer);
	expect_state(argv[1], "detached", "offloaded");
	if (close(consumer) != 0)
		err(1, "close consumer");
	expect_state_eventually(argv[1], "detached", "root");
	return 0;
}
