/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verify native tty non-blocking behavior on a VMMFS serial endpoint.
 */
#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
fail(const char *operation, const char *path)
{
	fprintf(stderr, "%s %s: %s\n", operation, path, strerror(errno));
	return 1;
}

int
main(int argc, char **argv)
{
	char machine[1024];
	char port[1024];
	char byte;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s mountpoint\n", argv[0]);
		return 2;
	}
	if (snprintf(machine, sizeof(machine), "%s/serial-nonblock-%ld",
	    argv[1], (long)getpid()) >= (int)sizeof(machine)) {
		fprintf(stderr, "machine path too long\n");
		return 2;
	}
	if (snprintf(port, sizeof(port), "%s/serial/com1", machine) >=
	    (int)sizeof(port)) {
		fprintf(stderr, "serial path too long\n");
		return 2;
	}
	if (mkdir(machine, 0755) != 0)
		return fail("mkdir", machine);
	fd = open(port, O_CREAT | O_RDWR | O_NONBLOCK, 0600);
	if (fd < 0)
		return fail("open", port);
	if (read(fd, &byte, 1) != -1 ||
	    (errno != EAGAIN && errno != EWOULDBLOCK)) {
		fprintf(stderr, "empty non-blocking read did not return EWOULDBLOCK\n");
		(void)close(fd);
		return 1;
	}
	if (write(fd, "x", 1) != 1)
		return fail("write", port);
	if (close(fd) != 0)
		return fail("close", port);
	if (unlink(port) != 0)
		return fail("unlink", port);
	if (rmdir(machine) != 0)
		return fail("rmdir", machine);
	puts("PASS: serial endpoint supports O_NONBLOCK");
	return 0;
}
