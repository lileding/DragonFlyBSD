/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Runtime test for the public DragonFly KVM eventfd() interface.
 */
#include <sys/event.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <linux/kvm.h>

int
main(void)
{
	struct kevent change;
	struct kevent result;
	struct timespec timeout = { 0, 0 };
	uint64_t value;
	int event_fd;
	int queue_fd;

	event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (event_fd < 0)
		err(1, "eventfd");
	if ((fcntl(event_fd, F_GETFD) & FD_CLOEXEC) == 0)
		err(1, "eventfd lacks FD_CLOEXEC");
	queue_fd = kqueue();
	if (queue_fd < 0)
		err(1, "kqueue");
	EV_SET(&change, event_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	if (kevent(queue_fd, &change, 1, NULL, 0, NULL) != 0)
		err(1, "register EVFILT_READ");
	value = 5;
	if (write(event_fd, &value, sizeof(value)) != sizeof(value))
		err(1, "eventfd write");
	if (kevent(queue_fd, NULL, 0, &result, 1, &timeout) != 1)
		err(1, "eventfd readiness");
	if (result.filter != EVFILT_READ)
		errx(1, "unexpected kevent filter");
	value = 0;
	if (read(event_fd, &value, sizeof(value)) != sizeof(value))
		err(1, "eventfd read");
	if (value != 5)
		errx(1, "eventfd returned %ju instead of 5", (uintmax_t)value);
	if (read(event_fd, &value, sizeof(value)) != -1 || errno != EWOULDBLOCK)
		err(1, "empty nonblocking eventfd read");
	if (close(queue_fd) != 0 || close(event_fd) != 0)
		err(1, "close");
	puts("kvm eventfd: PASS");
	return 0;
}
