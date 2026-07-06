/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * syncobj_chain_release - stress syncobj timeline chain teardown.
 *
 * Build:
 *   cc -Wall -Wextra \
 *      -I/home/lileding/src/nvkm/sys/dev/drm/include \
 *      -I/home/lileding/src/nvkm/sys/dev/drm/include/uapi/drm \
 *      -o /var/tmp/syncobj_chain_release syncobj_chain_release.c
 *
 * Run:
 *   /var/tmp/syncobj_chain_release [/dev/dri/renderD128] [points]
 */

#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm.h>

static int
open_drm_node(const char *path)
{
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd >= 0)
		return (fd);

	fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
	return (-1);
}

static int
syncobj_create(int fd, uint32_t *handle)
{
	struct drm_syncobj_create req;

	memset(&req, 0, sizeof(req));
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &req) != 0) {
		fprintf(stderr, "SYNCOBJ_CREATE failed: %s\n", strerror(errno));
		return (-1);
	}

	*handle = req.handle;
	return (0);
}

static int
syncobj_timeline_signal(int fd, uint32_t handle, uint64_t point)
{
	struct drm_syncobj_timeline_array req;
	uint32_t handles[1];
	uint64_t points[1];

	handles[0] = handle;
	points[0] = point;
	memset(&req, 0, sizeof(req));
	req.handles = (uint64_t)(uintptr_t)handles;
	req.points = (uint64_t)(uintptr_t)points;
	req.count_handles = 1;

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &req) != 0) {
		fprintf(stderr, "TIMELINE_SIGNAL point=%llu failed: %s\n",
		    (unsigned long long)point, strerror(errno));
		return (-1);
	}

	return (0);
}

static void
syncobj_destroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy req;

	if (handle == 0)
		return;

	memset(&req, 0, sizeof(req));
	req.handle = handle;
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &req) != 0)
		fprintf(stderr, "SYNCOBJ_DESTROY handle=%u failed: %s\n",
		    handle, strerror(errno));
}

int
main(int argc, char **argv)
{
	const char *node = "/dev/dri/renderD128";
	unsigned long points = 20000;
	uint32_t handle = 0;
	int fd;
	int ret = 1;

	if (argc > 1)
		node = argv[1];
	if (argc > 2) {
		char *end = NULL;

		errno = 0;
		points = strtoul(argv[2], &end, 0);
		if (errno != 0 || end == argv[2] || *end != '\0' || points == 0) {
			fprintf(stderr, "invalid point count: %s\n", argv[2]);
			return (1);
		}
	}

	fd = open_drm_node(node);
	if (fd < 0)
		return (1);

	if (syncobj_create(fd, &handle) != 0)
		goto out;

	for (unsigned long i = 1; i <= points; i++) {
		if (syncobj_timeline_signal(fd, handle, i) != 0)
			goto out;
	}

	syncobj_destroy(fd, handle);
	handle = 0;
	printf("PASS: created and destroyed %lu syncobj timeline points\n",
	    points);
	ret = 0;
out:
	syncobj_destroy(fd, handle);
	close(fd);
	return (ret);
}
