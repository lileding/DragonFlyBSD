/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Nonblocking console capture helper for the pc64 SVM smoke harness.
 */
#include <sys/types.h>
#include <sys/poll.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void
console_capture_stop(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static void
console_capture_write(int fd, const char *buf, size_t len)
{
	ssize_t written;

	while (len != 0) {
		written = write(fd, buf, len);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			err(1, "write");
		}
		buf += written;
		len -= (size_t)written;
	}
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	struct pollfd pfd[2];
	char buf[4096];
	int console_fd;
	int input_fd;
	int output_fd;
	int ready_fd;
	int nready;
	ssize_t nread;

	if (argc != 5)
		errx(1, "usage: console_capture console output ready input-fifo");

	console_fd = open(argv[1], O_RDWR | O_NONBLOCK);
	if (console_fd < 0)
		err(1, "%s", argv[1]);
	output_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (output_fd < 0)
		err(1, "%s", argv[2]);
	ready_fd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (ready_fd < 0)
		err(1, "%s", argv[3]);
	input_fd = open(argv[4], O_RDWR | O_NONBLOCK);
	if (input_fd < 0)
		err(1, "%s", argv[4]);
	console_capture_write(ready_fd, "ready\n", 6);
	close(ready_fd);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = console_capture_stop;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, NULL) != 0)
		err(1, "sigaction SIGTERM");
	if (sigaction(SIGINT, &sa, NULL) != 0)
		err(1, "sigaction SIGINT");

	pfd[0].fd = console_fd;
	pfd[0].events = POLLIN;
	pfd[1].fd = input_fd;
	pfd[1].events = POLLIN;
	while (!stop_requested) {
		pfd[0].revents = 0;
		pfd[1].revents = 0;
		nready = poll(pfd, 2, -1);
		if (nready < 0) {
			if (errno == EINTR)
				continue;
			err(1, "poll");
		}
		if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			errx(1, "console poll revents=0x%x", pfd[0].revents);
		if (pfd[1].revents & (POLLERR | POLLHUP | POLLNVAL))
			errx(1, "input poll revents=0x%x", pfd[1].revents);
		if (pfd[0].revents & POLLIN) {
			nread = read(console_fd, buf, sizeof(buf));
			if (nread < 0) {
				if (errno != EINTR && errno != EWOULDBLOCK)
					err(1, "console read");
			} else if (nread == 0) {
				errx(1, "console eof");
			} else {
				console_capture_write(output_fd, buf, (size_t)nread);
			}
		}
		if (pfd[1].revents & POLLIN) {
			nread = read(input_fd, buf, sizeof(buf));
			if (nread < 0) {
				if (errno != EINTR && errno != EWOULDBLOCK)
					err(1, "input read");
			} else if (nread != 0) {
				console_capture_write(console_fd, buf, (size_t)nread);
			}
		}
	}
	close(input_fd);
	close(output_fd);
	close(console_fd);
	return 0;
}
