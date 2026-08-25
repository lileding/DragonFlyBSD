/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Exercise descriptor close-commit and the generation-bound auth token.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <unistd.h>

#include <sys/vmmfs.h>

static int write_all(int, const char *, size_t);
static int commit_descriptor(const char *, int);
static int register_config_readiness(int, int);
static int wait_config_readiness(int, int16_t);
static int map_bar(const char *);

int
main(int argc, char **argv)
{
	struct vmmfs_pci_config_request request;
	struct vmmfs_pci_config_response response;
	struct vmmfs_pci_kick kick;
	int config_fd;
	int kick_fd;
	int queue_fd;
	ssize_t result;

	if (argc != 3 && argc != 6) {
		fprintf(stderr, "usage: %s create|replace <descriptor> | hold <descriptor> <kick> <config> <bar>\n",
		    argv[0]);
		return (2);
	}
	if (argc == 3 && strcmp(argv[1], "create") == 0)
		return (commit_descriptor(argv[2], O_CREAT | O_EXCL));
	if (argc == 3 && strcmp(argv[1], "replace") == 0)
		return (commit_descriptor(argv[2], 0));
	if (argc != 6 || strcmp(argv[1], "hold") != 0)
		return (2);
	if (commit_descriptor(argv[2], 0) != 0)
		return (1);
	if (puts("committed") == EOF || fflush(stdout) != 0)
		return (1);
	config_fd = open(argv[4], O_RDWR);
	if (config_fd < 0) {
		perror("open config");
		return (1);
	}
	queue_fd = kqueue();
	if (queue_fd < 0) {
		perror("kqueue");
		close(config_fd);
		return (1);
	}
	if (register_config_readiness(queue_fd, config_fd) != 0) {
		perror("register config readiness");
		close(queue_fd);
		close(config_fd);
		return (1);
	}
	for (;;) {
		kick_fd = open(argv[3], O_RDONLY);
		if (kick_fd >= 0)
			break;
		if (errno != ENOENT && errno != ENXIO) {
			perror("open resource");
			close(queue_fd);
			close(config_fd);
			return (1);
		}
		usleep(10000);
	}
	if (puts("ready") == EOF || fflush(stdout) != 0) {
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	if (map_bar(argv[5]) != 0) {
		perror("map bar");
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	if (puts("mapped") == EOF || fflush(stdout) != 0)
		return (1);
	result = read(kick_fd, &kick, sizeof(kick));
	if (result != sizeof(kick)) {
		fprintf(stderr, "kick read failed: %zd\n", result);
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	if (kick.offset != 0 || kick.width != 4 || kick.value != 0xdeadbeefU) {
		fprintf(stderr, "unexpected kick: offset=%ju width=%u value=%jx\n",
		    (uintmax_t)kick.offset, kick.width, (uintmax_t)kick.value);
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	if (puts("kick") == EOF || fflush(stdout) != 0) {
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	if (wait_config_readiness(queue_fd, EVFILT_READ) != 0) {
		perror("wait config read readiness");
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	result = read(config_fd, &request, sizeof(request));
	if (result != sizeof(request)) {
		fprintf(stderr, "config read failed: %zd\n", result);
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	if (request.generation == 0 || request.sequence == 0 ||
	    request.offset != 0x100 || request.value != 0 || request.bar != 0 ||
	    request.space != VMMFS_PCI_CONFIG_MMIO || request.width != 4 ||
	    request.operation != VMMFS_PCI_CONFIG_READ ||
	    request.reserved[0] != 0 || request.reserved[1] != 0 ||
	    request.reserved[2] != 0) {
		fprintf(stderr, "unexpected config request\n");
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	if (wait_config_readiness(queue_fd, EVFILT_WRITE) != 0) {
		perror("wait config write readiness");
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	memset(&response, 0, sizeof(response));
	response.generation = request.generation;
	response.sequence = request.sequence;
	response.value = 0xc0decafeU;
	response.status = VMMFS_PCI_CONFIG_SUCCESS;
	if (write_all(config_fd, (const char *)&response, sizeof(response)) != 0) {
		perror("config write");
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	if (puts("config") == EOF || fflush(stdout) != 0) {
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	result = read(kick_fd, &kick, sizeof(kick));
	if (result >= 0) {
		fprintf(stderr, "kick revoke read unexpectedly completed: %zd\n", result);
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	if (puts("revoked") == EOF || fflush(stdout) != 0) {
		close(queue_fd);
		close(config_fd);
		close(kick_fd);
		return (1);
	}
	close(queue_fd);
	close(config_fd);
	close(kick_fd);
	return (0);
}

static int
register_config_readiness(int queue_fd, int config_fd)
{
	struct kevent changes[2];
	struct kevent results[2];
	int index;
	int result;

	EV_SET(&changes[0], config_fd, EVFILT_READ,
	    EV_ADD | EV_CLEAR | EV_RECEIPT, 0, 0, NULL);
	EV_SET(&changes[1], config_fd, EVFILT_WRITE,
	    EV_ADD | EV_CLEAR | EV_RECEIPT, 0, 0, NULL);
	result = kevent(queue_fd, changes, 2, results, 2, NULL);
	if (result != 2) {
		if (result >= 0)
			errno = EPROTO;
		return (-1);
	}
	for (index = 0; index < 2; ++index) {
		if ((results[index].flags & EV_ERROR) == 0 ||
		    results[index].data == 0)
			continue;
		errno = (int)results[index].data;
		return (-1);
	}
	return (0);
}

static int
wait_config_readiness(int queue_fd, int16_t filter)
{
	struct kevent result;

	for (;;) {
		if (kevent(queue_fd, NULL, 0, &result, 1, NULL) != 1)
			return (-1);
		if ((result.flags & EV_ERROR) != 0) {
			errno = (int)result.data;
			return (-1);
		}
		if (result.filter == filter)
			return (0);
	}
}

static int
map_bar(const char *path)
{
	volatile uint8_t *mapping;
	long page_size;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0)
		return (-1);
	page_size = getpagesize();
	mapping = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		return (-1);
	}
	(void)mapping[0];
	if (munmap((void *)mapping, (size_t)page_size) != 0) {
		close(fd);
		return (-1);
	}
	return (close(fd));
}

static int
write_all(int fd, const char *buffer, size_t length)
{
	ssize_t written;

	while (length != 0) {
		written = write(fd, buffer, length);
		if (written < 0)
			return (-1);
		buffer += written;
		length -= (size_t)written;
	}
	return (0);
}

static int
commit_descriptor(const char *path, int flags)
{
	char buffer[1024];
	int fd;
	ssize_t length;

	fd = open(path, O_WRONLY | flags, 0600);
	if (fd < 0) {
		perror("open descriptor");
		return (1);
	}
	while ((length = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
		if (write_all(fd, buffer, (size_t)length) != 0) {
			perror("write descriptor");
			close(fd);
			return (1);
		}
	}
	if (length < 0 || close(fd) != 0) {
		perror("commit descriptor");
		return (1);
	}
	return (0);
}
