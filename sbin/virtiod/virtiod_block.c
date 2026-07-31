/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Raw block I/O follows the BSD-licensed bhyve block_if design, restricted to
 * regular image files for the initial virtiod provider.
 */
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "virtiod.h"

int
virtiod_block_open(struct virtiod_block *block, const char *path)
{
	struct stat status;
	int fd;

	if (block == NULL || path == NULL || path[0] == '\0')
		return EINVAL;
	memset(block, 0, sizeof(*block));
	block->own_fd = -1;
	fd = open(path, O_RDWR);
	if (fd < 0 && errno == EACCES) {
		fd = open(path, O_RDONLY);
		block->imm_read_only = 1;
	}
	if (fd < 0)
		return errno;
	if (fstat(fd, &status) != 0) {
		int error = errno;

		(void)close(fd);
		return error;
	}
	if (!S_ISREG(status.st_mode) || status.st_size <= 0 ||
	    (uintmax_t)status.st_size > UINT64_MAX ||
	    ((uint64_t)status.st_size & 511U) != 0) {
		(void)close(fd);
		return EINVAL;
	}
	block->own_fd = fd;
	block->imm_size = (uint64_t)status.st_size;
	return 0;
}

void
virtiod_block_close(struct virtiod_block *block)
{

	if (block == NULL)
		return;
	if (block->own_fd >= 0)
		(void)close(block->own_fd);
	memset(block, 0, sizeof(*block));
	block->own_fd = -1;
}

int
virtiod_block_request(struct virtiod_block *block, uint32_t type,
    uint64_t sector, const struct iovec *iov, unsigned int iov_count,
    uint8_t *status)
{
	off_t offset;
	uint64_t offset_bytes;
	uint64_t transfer_bytes;
	unsigned int i;
	ssize_t actual;

	if (block == NULL || status == NULL || block->own_fd < 0 ||
	    (iov == NULL && iov_count != 0))
		return EINVAL;
	*status = VIRTIOD_BLK_S_IOERR;
	if (type == VIRTIOD_BLK_T_FLUSH) {
		if (iov_count != 0)
			return EINVAL;
		if (fsync(block->own_fd) != 0)
			return errno;
		*status = VIRTIOD_BLK_S_OK;
		return 0;
	}
	if (type != VIRTIOD_BLK_T_IN && type != VIRTIOD_BLK_T_OUT) {
		*status = VIRTIOD_BLK_S_UNSUPP;
		return 0;
	}
	if (iov_count == 0 || iov_count > VIRTIOD_MAX_CHAIN ||
	    __builtin_mul_overflow(sector, 512ULL, &offset_bytes))
		return EINVAL;
	transfer_bytes = 0;
	for (i = 0; i < iov_count; i++) {
		if (iov[i].iov_len == 0 ||
		    __builtin_add_overflow(transfer_bytes,
		    (uint64_t)iov[i].iov_len, &transfer_bytes))
			return EINVAL;
	}
	if (offset_bytes > block->imm_size ||
	    transfer_bytes > block->imm_size - offset_bytes)
		return EINVAL;
	offset = (off_t)offset_bytes;
	if ((uint64_t)offset != offset_bytes)
		return EINVAL;
	if (type == VIRTIOD_BLK_T_OUT && block->imm_read_only) {
		*status = VIRTIOD_BLK_S_IOERR;
		return 0;
	}
	if (type == VIRTIOD_BLK_T_IN)
		actual = preadv(block->own_fd, iov, (int)iov_count, offset);
	else
		actual = pwritev(block->own_fd, iov, (int)iov_count, offset);
	if (actual < 0)
		return errno;
	if ((uint64_t)actual != transfer_bytes)
		return EIO;
	*status = VIRTIOD_BLK_S_OK;
	return 0;
}
