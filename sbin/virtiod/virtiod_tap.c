/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * TAP open and packet I/O are derived from FreeBSD bhyve's net_backends.c.
 * Copyright (c) 2019 Vincenzo Maffione.
 */
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "virtiod.h"

#define VIRTIOD_TAP_NAME_MAX 16U

int
virtiod_tap_open(struct virtiod_tap *tap, const char *name)
{
	char path[VIRTIOD_TAP_NAME_MAX + 6U];
	int flags;

	if (tap == NULL || name == NULL || name[0] == '\0' ||
	    strchr(name, '/') != NULL || strlen(name) >= VIRTIOD_TAP_NAME_MAX)
		return EINVAL;
	memset(tap, 0, sizeof(*tap));
	tap->own_fd = -1;
	if (snprintf(path, sizeof(path), "/dev/%s", name) >= (int)sizeof(path))
		return ENAMETOOLONG;
	tap->own_fd = open(path, O_RDWR);
	if (tap->own_fd < 0)
		return errno;
	flags = 1;
	if (ioctl(tap->own_fd, FIONBIO, &flags) != 0) {
		int error = errno;

		(void)close(tap->own_fd);
		tap->own_fd = -1;
		return error;
	}
	return 0;
}

void
virtiod_tap_close(struct virtiod_tap *tap)
{

	if (tap != NULL && tap->own_fd >= 0)
		(void)close(tap->own_fd);
	if (tap != NULL)
		tap->own_fd = -1;
}

ssize_t
virtiod_tap_readv(struct virtiod_tap *tap, const struct iovec *iov,
    int iov_count)
{
	size_t copied;
	int i;

	if (tap == NULL || tap->own_fd < 0 || iov == NULL || iov_count <= 0) {
		errno = EINVAL;
		return -1;
	}
	if (tap->mut_rx_length == 0) {
		errno = EWOULDBLOCK;
		return -1;
	}
	copied = 0;
	for (i = 0; i < iov_count && copied < tap->mut_rx_length; i++) {
		size_t length;

		length = iov[i].iov_len;
		if (length > tap->mut_rx_length - copied)
			length = tap->mut_rx_length - copied;
		memcpy(iov[i].iov_base, tap->own_mut_rx_bytes + copied, length);
		copied += length;
	}
	if (copied != tap->mut_rx_length) {
		errno = EMSGSIZE;
		return -1;
	}
	tap->mut_rx_length = 0;
	return (ssize_t)copied;
}

int
virtiod_tap_pending(struct virtiod_tap *tap, size_t *lengthp)
{
	ssize_t length;

	if (tap == NULL || tap->own_fd < 0 || lengthp == NULL)
		return EINVAL;
	if (tap->mut_rx_length == 0) {
		length = read(tap->own_fd, tap->own_mut_rx_bytes,
		    sizeof(tap->own_mut_rx_bytes));
		if (length < 0) {
			if (errno == EWOULDBLOCK)
				return 0;
			return errno;
		}
		if (length == 0)
			return EIO;
		tap->mut_rx_length = (size_t)length;
	}
	*lengthp = tap->mut_rx_length;
	return 0;
}

ssize_t
virtiod_tap_writev(const struct virtiod_tap *tap, const struct iovec *iov,
    int iov_count)
{

	if (tap == NULL || tap->own_fd < 0 || iov == NULL || iov_count <= 0) {
		errno = EINVAL;
		return -1;
	}
	return writev(tap->own_fd, iov, iov_count);
}
