/*
 * prime_no_share - validate nouveau NO_SHARE PRIME export semantics.
 *
 * Build:
 *   cc -Wall -Wextra -I/usr/local/include/libdrm -o prime_no_share prime_no_share.c
 *
 * Run:
 *   ./prime_no_share [/dev/dri/renderD128|/dev/dri/card0]
 */

#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <drm.h>

#ifndef DRM_RDWR
#define DRM_RDWR O_RDWR
#endif

#define DRM_NOUVEAU_GEM_NEW		0x40
#define NOUVEAU_GEM_DOMAIN_CPU		(1U << 0)
#define NOUVEAU_GEM_DOMAIN_VRAM	(1U << 1)
#define NOUVEAU_GEM_DOMAIN_GART	(1U << 2)
#define NOUVEAU_GEM_DOMAIN_MAPPABLE	(1U << 3)
#define NOUVEAU_GEM_DOMAIN_COHERENT	(1U << 4)
#define NOUVEAU_GEM_DOMAIN_NO_SHARE	(1U << 5)

struct drm_nouveau_gem_info {
	uint32_t handle;
	uint32_t domain;
	uint64_t size;
	uint64_t offset;
	uint64_t map_handle;
	uint32_t tile_mode;
	uint32_t tile_flags;
};

struct drm_nouveau_gem_new {
	struct drm_nouveau_gem_info info;
	uint32_t channel_hint;
	uint32_t align;
};

#define DRM_IOCTL_NOUVEAU_GEM_NEW \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_NEW, \
	    struct drm_nouveau_gem_new)

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
open_default_drm_node(void)
{
	int fd;

	fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (fd >= 0)
		return (fd);

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd >= 0)
		return (fd);

	fprintf(stderr, "open default DRM node failed: %s\n", strerror(errno));
	return (-1);
}

static int
gem_new(int fd, uint32_t domain, uint32_t *handle)
{
	struct drm_nouveau_gem_new req;

	memset(&req, 0, sizeof(req));
	req.info.domain = domain;
	req.info.size = 64 * 1024;

	if (ioctl(fd, DRM_IOCTL_NOUVEAU_GEM_NEW, &req) != 0) {
		fprintf(stderr, "GEM_NEW domain=0x%x failed: %s\n",
		    domain, strerror(errno));
		return (-1);
	}

	*handle = req.info.handle;
	printf("GEM_NEW domain=0x%x -> handle=%u returned_domain=0x%x "
	    "map_handle=0x%llx\n",
	    domain, req.info.handle, req.info.domain,
	    (unsigned long long)req.info.map_handle);
	return (0);
}

static int
prime_handle_to_fd(int fd, uint32_t handle, int *prime_fd)
{
	struct drm_prime_handle req;

	memset(&req, 0, sizeof(req));
	req.handle = handle;
	req.flags = DRM_CLOEXEC | DRM_RDWR;

	if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &req) != 0)
		return (-1);

	*prime_fd = req.fd;
	return (0);
}

static int
prime_fd_to_handle(int fd, int prime_fd, uint32_t *handle)
{
	struct drm_prime_handle req;

	memset(&req, 0, sizeof(req));
	req.fd = prime_fd;

	if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &req) != 0)
		return (-1);

	*handle = req.handle;
	return (0);
}

static void
gem_close_handle(int fd, uint32_t handle)
{
	struct drm_gem_close req;

	if (handle == 0)
		return;

	memset(&req, 0, sizeof(req));
	req.handle = handle;
	if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &req) != 0)
		fprintf(stderr, "GEM_CLOSE handle=%u failed: %s\n",
		    handle, strerror(errno));
}

static int
test_no_share_export_rejected(int fd)
{
	uint32_t handle = 0;
	int prime_fd = -1;
	int saved_errno;

	if (gem_new(fd, NOUVEAU_GEM_DOMAIN_GART |
	    NOUVEAU_GEM_DOMAIN_MAPPABLE |
	    NOUVEAU_GEM_DOMAIN_NO_SHARE, &handle) != 0)
		return (-1);

	if (prime_handle_to_fd(fd, handle, &prime_fd) == 0) {
		fprintf(stderr,
		    "FAIL: NO_SHARE handle exported as prime fd %d\n",
		    prime_fd);
		close(prime_fd);
		gem_close_handle(fd, handle);
		return (-1);
	}

	saved_errno = errno;
	gem_close_handle(fd, handle);
	if (saved_errno != EPERM) {
		fprintf(stderr,
		    "FAIL: NO_SHARE export errno=%d (%s), expected EPERM\n",
		    saved_errno, strerror(saved_errno));
		return (-1);
	}

	printf("PASS: NO_SHARE PRIME export rejected with EPERM\n");
	return (0);
}

static int
test_shared_roundtrip(int fd)
{
	uint32_t handle = 0;
	uint32_t imported = 0;
	int prime_fd = -1;
	int ret = -1;

	if (gem_new(fd, NOUVEAU_GEM_DOMAIN_GART |
	    NOUVEAU_GEM_DOMAIN_MAPPABLE, &handle) != 0)
		return (-1);

	if (prime_handle_to_fd(fd, handle, &prime_fd) != 0) {
		fprintf(stderr, "FAIL: shared export failed: %s\n",
		    strerror(errno));
		goto out;
	}

	if (prime_fd_to_handle(fd, prime_fd, &imported) != 0) {
		fprintf(stderr, "FAIL: same-device import failed: %s\n",
		    strerror(errno));
		goto out;
	}

	printf("PASS: shared PRIME roundtrip handle=%u imported=%u fd=%d\n",
	    handle, imported, prime_fd);
	ret = 0;

out:
	if (prime_fd >= 0)
		close(prime_fd);
	if (imported != 0 && imported != handle)
		gem_close_handle(fd, imported);
	gem_close_handle(fd, handle);
	return (ret);
}

int
main(int argc, char **argv)
{
	int fd;
	int ret = 1;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [drm-node]\n", argv[0]);
		return (2);
	}

	if (argc == 2)
		fd = open_drm_node(argv[1]);
	else
		fd = open_default_drm_node();
	if (fd < 0)
		return (1);

	if (test_no_share_export_rejected(fd) != 0)
		goto out;
	if (test_shared_roundtrip(fd) != 0)
		goto out;

	ret = 0;

out:
	close(fd);
	return (ret);
}
