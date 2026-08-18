/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verify that an open serial vnode prevents its destruction.
 */
#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s mountpoint\n", argv[0]);
		return 2;
	}
	if (snprintf(machine, sizeof(machine), "%s/serial-lifetime-%ld",
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
	fd = open(port, O_CREAT | O_RDWR, 0600);
	if (fd < 0)
		return fail("open", port);
	if (close(fd) != 0)
		return fail("close", port);
	fd = open(port, O_RDWR);
	if (fd < 0)
		return fail("open", port);
	if (unlink(port) == 0 || errno != EBUSY) {
		if (errno == 0)
			fprintf(stderr, "unlink %s unexpectedly succeeded\n", port);
		else
			fprintf(stderr, "unlink %s returned %s, expected EBUSY\n",
			    port, strerror(errno));
		(void)close(fd);
		return 1;
	}
	if (close(fd) != 0)
		return fail("close", port);
	if (unlink(port) != 0)
		return fail("unlink", port);
	if (rmdir(machine) != 0)
		return fail("rmdir", machine);
	printf("PASS: open serial port blocks destruction\n");
	return 0;
}
