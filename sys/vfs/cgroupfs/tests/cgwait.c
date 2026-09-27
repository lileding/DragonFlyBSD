/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * cgwait: waits for one change notification on a cgroupfs file, for the
 * cgroupfs regression test.
 *
 * usage: cgwait kqueue|poll file seconds
 *
 * Once the wait is registered, "ready" is printed on stderr (for poll,
 * immediately before poll(2), which registers inside the call).  The
 * outcome is printed on stdout: "write" or "delete" for kqueue
 * EVFILT_VNODE, "pri" for poll POLLPRI, or "timeout".
 */
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include <err.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr, "usage: cgwait kqueue|poll file seconds\n");
	exit(2);
}

static void
wait_kqueue(int fd, int seconds)
{
	struct kevent change;
	struct kevent event;
	struct timespec timeout;
	int kq;
	int n;

	kq = kqueue();
	if (kq < 0)
		err(1, "kqueue");
	EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
	    NOTE_WRITE | NOTE_DELETE, 0, NULL);
	if (kevent(kq, &change, 1, NULL, 0, NULL) < 0)
		err(1, "kevent register");
	fprintf(stderr, "ready\n");
	timeout.tv_sec = seconds;
	timeout.tv_nsec = 0;
	n = kevent(kq, NULL, 0, &event, 1, &timeout);
	if (n < 0)
		err(1, "kevent wait");
	if (n == 0)
		printf("timeout\n");
	else if (event.fflags & NOTE_DELETE)
		printf("delete\n");
	else if (event.fflags & NOTE_WRITE)
		printf("write\n");
	else
		printf("fflags %#x\n", event.fflags);
}

static void
wait_poll(int fd, int seconds)
{
	struct pollfd pfd;
	int n;

	pfd.fd = fd;
	pfd.events = POLLPRI;
	pfd.revents = 0;
	fprintf(stderr, "ready\n");
	n = poll(&pfd, 1, seconds * 1000);
	if (n < 0)
		err(1, "poll");
	if (n == 0)
		printf("timeout\n");
	else if (pfd.revents & POLLPRI)
		printf("pri\n");
	else
		printf("revents %#x\n", pfd.revents);
}

int
main(int argc, char **argv)
{
	char *end;
	long seconds;
	int fd;

	if (argc != 4)
		usage();
	seconds = strtol(argv[3], &end, 10);
	if (*argv[3] == '\0' || *end != '\0' || seconds < 0 || seconds > 3600)
		usage();
	fd = open(argv[2], O_RDONLY);
	if (fd < 0)
		err(1, "%s", argv[2]);
	if (strcmp(argv[1], "kqueue") == 0)
		wait_kqueue(fd, (int)seconds);
	else if (strcmp(argv[1], "poll") == 0)
		wait_poll(fd, (int)seconds);
	else
		usage();
	return (0);
}
