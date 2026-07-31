/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <sys/uio.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "virtiod.h"

int
main(void)
{
	struct virtiod_block block;
	struct iovec iov;
	uint8_t readback[512];
	uint8_t status;
	uint8_t write_data[512];
	char path[] = "/var/tmp/virtiod_block_test.XXXXXX";
	int error;
	int fd;

	fd = mkstemp(path);
	if (fd < 0)
		err(1, "mkstemp");
	memset(write_data, 0xa5, sizeof(write_data));
	if (ftruncate(fd, 4096) != 0)
		err(1, "ftruncate");
	if (close(fd) != 0)
		err(1, "close image");
	error = virtiod_block_open(&block, path);
	if (error != 0)
		errno = error, err(1, "open");
	iov.iov_base = write_data;
	iov.iov_len = sizeof(write_data);
	error = virtiod_block_request(&block, VIRTIOD_BLK_T_OUT, 2, &iov, 1,
	    &status);
	if (error != 0 || status != VIRTIOD_BLK_S_OK)
		errno = error == 0 ? EIO : error, err(1, "write");
	memset(readback, 0, sizeof(readback));
	iov.iov_base = readback;
	error = virtiod_block_request(&block, VIRTIOD_BLK_T_IN, 2, &iov, 1,
	    &status);
	if (error != 0 || status != VIRTIOD_BLK_S_OK ||
	    memcmp(readback, write_data, sizeof(readback)) != 0)
		errno = error == 0 ? EIO : error, err(1, "read");
	error = virtiod_block_request(&block, VIRTIOD_BLK_T_FLUSH, 0, NULL, 0,
	    &status);
	if (error != 0 || status != VIRTIOD_BLK_S_OK)
		errno = error == 0 ? EIO : error, err(1, "flush");
	error = virtiod_block_request(&block, VIRTIOD_BLK_T_IN, 8, &iov, 1,
	    &status);
	if (error != EINVAL)
		errno = error == 0 ? EIO : error, err(1, "range");
	virtiod_block_close(&block);
	if (unlink(path) != 0)
		err(1, "unlink");
	puts("PASS: virtiod block");
	return 0;
}
