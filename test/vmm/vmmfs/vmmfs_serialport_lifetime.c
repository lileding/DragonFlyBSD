/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Verify that a serial endpoint drops its rings and revokes open files.
 */
#include <sys/types.h>
#include <sys/event.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int
fail(const char *operation, const char *path)
{
	fprintf(stderr, "%s %s: %s\n", operation, path, strerror(errno));
	return 1;
}

static int
test_kqueue(int fd, const char *path)
{
	struct kevent changes[2];
	struct kevent event;
	struct timespec timeout;
	int kq;
	int count;

	kq = kqueue();
	if (kq < 0)
		return fail("kqueue", path);
	EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
	EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
	if (kevent(kq, changes, 2, NULL, 0, NULL) != 0) {
		(void)close(kq);
		return fail("kevent register", path);
	}
	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;
	count = kevent(kq, NULL, 0, &event, 1, &timeout);
	if (count != 1 || event.filter != EVFILT_WRITE ||
	    (event.flags & EV_ERROR) != 0) {
		fprintf(stderr, "write readiness %s was not reported\n", path);
		(void)close(kq);
		return 1;
	}
	if (close(kq) != 0)
		return fail("close kqueue", path);
	return 0;
}

int
main(int argc, char **argv)
{
	char machine[1024];
	char port[1024];
	char buffer[4096];
	char byte;
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
	memset(buffer, 'x', sizeof(buffer));
	fd = open(port, O_CREAT | O_RDWR | O_NONBLOCK, 0600);
	if (fd < 0)
		return fail("open", port);
	if (test_kqueue(fd, port) != 0) {
		(void)close(fd);
		return 1;
	}
	if (write(fd, buffer, sizeof(buffer)) != (ssize_t)sizeof(buffer))
		return fail("write", port);
	if (unlink(port) != 0)
		return fail("unlink", port);
	if (read(fd, &byte, 1) != -1) {
		fprintf(stderr, "read %s unexpectedly succeeded after unlink\n", port);
		(void)close(fd);
		return 1;
	}
	if (write(fd, &byte, 1) != -1) {
		fprintf(stderr, "write %s unexpectedly succeeded after unlink\n", port);
		(void)close(fd);
		return 1;
	}
	if (close(fd) != 0)
		return fail("close", port);
	if (rmdir(machine) != 0)
		return fail("rmdir", machine);
	printf("PASS: serial endpoint overwrites and revokes\n");
	return 0;
}
