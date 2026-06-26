/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Offline file-backed checker for x86 launch manifests.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "vmm_loader_x86.h"

static void *
map_manifest(const char *path, size_t *sizep)
{
	struct stat st;
	void *addr;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		err(1, "open %s", path);
	if (fstat(fd, &st) != 0)
		err(1, "fstat %s", path);
	if (st.st_size <= 0)
		errx(1, "%s is empty", path);
	addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED)
		err(1, "mmap %s", path);
	close(fd);
	*sizep = (size_t)st.st_size;
	return addr;
}

static uint64_t
file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		err(1, "stat %s", path);
	if (st.st_size <= 0)
		errx(1, "%s is empty", path);
	return (uint64_t)st.st_size;
}

int
main(int argc, char **argv)
{
	struct vmm_launch launch;
	const uint8_t *manifest;
	uint64_t mem_size;
	size_t manifest_size;
	int error;

	if (argc != 3)
		errx(1, "usage: %s mem-file manifest-file", argv[0]);
	mem_size = file_size(argv[1]);
	manifest = map_manifest(argv[2], &manifest_size);
	error = vmm_loader_x86_manifest_load(mem_size, manifest,
	    manifest_size, &launch);
	if (error)
		errx(1, "manifest rejected: %d", error);
	return 0;
}
