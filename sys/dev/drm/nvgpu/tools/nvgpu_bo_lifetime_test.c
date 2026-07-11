/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reusable userspace regression test for BO reservation, PRIME, and mmap.
 */

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <libdrm/drm.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DRM_NOUVEAU_CHANNEL_ALLOC 0x02
#define DRM_NOUVEAU_CHANNEL_FREE 0x03
#define DRM_NOUVEAU_EXEC 0x12
#define DRM_NOUVEAU_GEM_NEW 0x40
#define DRM_NOUVEAU_GEM_CPU_PREP 0x42
#define DRM_NOUVEAU_GEM_CPU_FINI 0x43
#define DRM_NOUVEAU_GEM_INFO 0x44

#define NOUVEAU_FIFO_ENGINE_GR 0x01
#define NOUVEAU_GEM_DOMAIN_GART (1u << 2)
#define NOUVEAU_GEM_DOMAIN_NO_SHARE (1u << 5)
#define NOUVEAU_GEM_CPU_PREP_NOWAIT 0x00000001
#define DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ 0x1
#define NO_SHARE_SUBMITS 16384

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

struct drm_nouveau_gem_cpu_prep {
	uint32_t handle;
	uint32_t flags;
};

struct drm_nouveau_gem_cpu_fini {
	uint32_t handle;
};

struct drm_nouveau_sync {
	uint32_t flags;
	uint32_t handle;
	uint64_t timeline_value;
};

struct drm_nouveau_exec {
	uint32_t channel;
	uint32_t push_count;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t push_ptr;
};

struct drm_nouveau_channel_alloc {
	uint32_t fb_ctxdma_handle;
	uint32_t tt_ctxdma_handle;
	int32_t channel;
	uint32_t pushbuf_domains;
	uint32_t notifier_handle;
	struct {
		uint32_t handle;
		uint32_t grclass;
	} subchan[8];
	uint32_t nr_subchan;
};

struct drm_nouveau_channel_free {
	int32_t channel;
};

#define DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_ALLOC, \
	    struct drm_nouveau_channel_alloc)
#define DRM_IOCTL_NOUVEAU_CHANNEL_FREE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_FREE, \
	    struct drm_nouveau_channel_free)
#define DRM_IOCTL_NOUVEAU_EXEC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_EXEC, struct drm_nouveau_exec)
#define DRM_IOCTL_NOUVEAU_GEM_NEW \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_NEW, \
	    struct drm_nouveau_gem_new)
#define DRM_IOCTL_NOUVEAU_GEM_CPU_PREP \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_CPU_PREP, \
	    struct drm_nouveau_gem_cpu_prep)
#define DRM_IOCTL_NOUVEAU_GEM_CPU_FINI \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_CPU_FINI, \
	    struct drm_nouveau_gem_cpu_fini)
#define DRM_IOCTL_NOUVEAU_GEM_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_INFO, \
	    struct drm_nouveau_gem_info)

static int
xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static int64_t
abs_timeout_nsec(unsigned seconds)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return INT64_MAX;
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec +
	    (int64_t)seconds * 1000000000LL;
}

static uint32_t
syncobj_create(int fd)
{
	struct drm_syncobj_create args;

	memset(&args, 0, sizeof(args));
	if (xioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args) != 0)
		return 0;
	return args.handle;
}

static void
syncobj_destroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy args;

	if (handle == 0)
		return;
	memset(&args, 0, sizeof(args));
	args.handle = handle;
	(void)xioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

static bool
syncobj_wait_timeline(int fd, uint32_t handle, uint64_t point)
{
	struct drm_syncobj_timeline_wait args;

	memset(&args, 0, sizeof(args));
	args.handles = (uint64_t)(uintptr_t)&handle;
	args.points = (uint64_t)(uintptr_t)&point;
	args.timeout_nsec = abs_timeout_nsec(5);
	args.count_handles = 1;
	args.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	return xioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args) == 0;
}

static uint32_t
gem_new(int fd, uint32_t domain, struct drm_nouveau_gem_info *info)
{
	struct drm_nouveau_gem_new args;

	memset(&args, 0, sizeof(args));
	args.info.size = 0x10000;
	args.info.domain = domain;
	args.align = 0x1000;
	if (xioctl(fd, DRM_IOCTL_NOUVEAU_GEM_NEW, &args) != 0)
		return 0;
	*info = args.info;
	return args.info.handle;
}

static void
gem_close(int fd, uint32_t handle)
{
	struct drm_gem_close args;

	if (handle == 0)
		return;
	memset(&args, 0, sizeof(args));
	args.handle = handle;
	(void)xioctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

static bool
test_no_share_reservation(int fd, uint32_t channel)
{
	struct drm_nouveau_gem_info info;
	struct drm_nouveau_gem_cpu_prep prep;
	struct drm_nouveau_gem_cpu_fini fini;
	struct drm_nouveau_sync signal;
	struct drm_nouveau_exec exec;
	uint32_t done = syncobj_create(fd);
	uint32_t handle = gem_new(fd, NOUVEAU_GEM_DOMAIN_GART |
	    NOUVEAU_GEM_DOMAIN_NO_SHARE, &info);
	int ready_pipe[2] = { -1, -1 };
	pid_t child = -1;
	int status = 0;
	bool ok = done != 0 && handle != 0;
	bool busy = false;
	bool prepared = false;

	if (ok)
		ok = pipe(ready_pipe) == 0;
	if (ok)
		child = fork();
	if (child == 0) {
		char ready = 'R';

		close(ready_pipe[0]);
		for (uint64_t point = 1; point <= NO_SHARE_SUBMITS; point++) {
			signal.flags = DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ;
			signal.handle = done;
			signal.timeline_value = point;
			memset(&exec, 0, sizeof(exec));
			exec.channel = channel;
			exec.sig_count = 1;
			exec.sig_ptr = (uint64_t)(uintptr_t)&signal;
			if (xioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec) != 0)
				_exit(1);
			if (point == 1 && write(ready_pipe[1], &ready, 1) != 1)
				_exit(1);
		}
		close(ready_pipe[1]);
		_exit(0);
	}
	if (ok && child < 0)
		ok = false;
	if (child > 0) {
		char ready;

		close(ready_pipe[1]);
		ready_pipe[1] = -1;
		ok = ok && read(ready_pipe[0], &ready, 1) == 1 && ready == 'R';
		for (unsigned attempt = 0; ok && !busy && attempt < 100000;
		    attempt++) {
			memset(&prep, 0, sizeof(prep));
			prep.handle = handle;
			prep.flags = NOUVEAU_GEM_CPU_PREP_NOWAIT;
			errno = 0;
			if (xioctl(fd, DRM_IOCTL_NOUVEAU_GEM_CPU_PREP, &prep) != 0) {
				if (errno == EBUSY)
					busy = true;
				else
					ok = false;
			} else {
				memset(&fini, 0, sizeof(fini));
				fini.handle = handle;
				ok = xioctl(fd, DRM_IOCTL_NOUVEAU_GEM_CPU_FINI,
				    &fini) == 0;
			}
		}
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 0)
			ok = false;
		close(ready_pipe[0]);
		ready_pipe[0] = -1;
	}
	ok = ok && busy;
	if (ok)
		ok = syncobj_wait_timeline(fd, done, NO_SHARE_SUBMITS);
	memset(&prep, 0, sizeof(prep));
	prep.handle = handle;
	prep.flags = NOUVEAU_GEM_CPU_PREP_NOWAIT;
	if (ok) {
		prepared = xioctl(fd, DRM_IOCTL_NOUVEAU_GEM_CPU_PREP, &prep) == 0;
		ok = prepared;
	}
	memset(&fini, 0, sizeof(fini));
	fini.handle = handle;
	if (prepared)
		ok = xioctl(fd, DRM_IOCTL_NOUVEAU_GEM_CPU_FINI, &fini) == 0 && ok;
	if (ready_pipe[0] >= 0)
		close(ready_pipe[0]);
	if (ready_pipe[1] >= 0)
		close(ready_pipe[1]);
	gem_close(fd, handle);
	syncobj_destroy(fd, done);
	return ok;
}

static bool
test_prime_self_only(int fd)
{
	struct drm_nouveau_gem_info shared_info;
	struct drm_nouveau_gem_info private_info;
	struct drm_nouveau_gem_info imported_info;
	struct drm_prime_handle prime;
	uint32_t shared = gem_new(fd, NOUVEAU_GEM_DOMAIN_GART, &shared_info);
	uint32_t private = gem_new(fd, NOUVEAU_GEM_DOMAIN_GART |
	    NOUVEAU_GEM_DOMAIN_NO_SHARE, &private_info);
	uint32_t imported = 0;
	bool ok = shared != 0 && private != 0;

	memset(&prime, 0, sizeof(prime));
	prime.handle = shared;
	prime.flags = DRM_CLOEXEC;
	prime.fd = -1;
	if (ok)
		ok = xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0;
	if (ok) {
		struct drm_prime_handle import;

		memset(&import, 0, sizeof(import));
		import.fd = prime.fd;
		ok = xioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import) == 0;
		imported = import.handle;
	}
	memset(&imported_info, 0, sizeof(imported_info));
	imported_info.handle = imported;
	if (ok)
		ok = xioctl(fd, DRM_IOCTL_NOUVEAU_GEM_INFO, &imported_info) == 0 &&
		    imported_info.size == shared_info.size;
	if (prime.fd >= 0)
		close(prime.fd);
	memset(&prime, 0, sizeof(prime));
	prime.handle = private;
	prime.flags = DRM_CLOEXEC;
	prime.fd = -1;
	errno = 0;
	if (ok)
		ok = xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0 &&
		    errno == EPERM;
	gem_close(fd, imported);
	gem_close(fd, private);
	gem_close(fd, shared);
	return ok;
}

static bool
test_repeated_mmap(int fd, size_t map_count)
{
	struct drm_nouveau_gem_info info;
	void *maps[64];
	uint32_t handle = gem_new(fd, NOUVEAU_GEM_DOMAIN_GART, &info);
	bool ok = handle != 0 && map_count > 0 &&
	    map_count <= sizeof(maps) / sizeof(maps[0]);

	memset(maps, 0, sizeof(maps));
	for (size_t i = 0; ok && i < map_count; i++) {
		maps[i] = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, (off_t)info.map_handle);
		ok = maps[i] != MAP_FAILED;
		if (ok)
			*(volatile uint32_t *)maps[i] = (uint32_t)i;
	}
	for (size_t i = 0; i < map_count; i++) {
		if (maps[i] != NULL && maps[i] != MAP_FAILED)
			ok = munmap(maps[i], info.size) == 0 && ok;
	}
	gem_close(fd, handle);
	return ok;
}

static bool
test_mmap_after_gem_close(int fd)
{
	struct drm_nouveau_gem_info info;
	void *maps[2] = { MAP_FAILED, MAP_FAILED };
	uint32_t handle = gem_new(fd, NOUVEAU_GEM_DOMAIN_GART, &info);
	bool ok = handle != 0;

	for (size_t i = 0; ok && i < 2; i++) {
		maps[i] = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, (off_t)info.map_handle);
		ok = maps[i] != MAP_FAILED;
	}
	gem_close(fd, handle);
	if (ok) {
		*(volatile uint32_t *)maps[0] = 0x12345678;
		ok = *(volatile uint32_t *)maps[1] == 0x12345678;
	}
	for (size_t i = 0; i < 2; i++) {
		if (maps[i] != MAP_FAILED)
			ok = munmap(maps[i], info.size) == 0 && ok;
	}
	return ok;
}

int
main(int argc, char **argv)
{
	struct drm_nouveau_channel_alloc alloc;
	struct drm_nouveau_channel_free free_args;
	const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	int fd;
	unsigned failed = 0;

	if (argc > 1 && strcmp(argv[1], "--hold-mmap") == 0) {
		struct drm_nouveau_gem_info info;
		void *map;
		uint32_t handle;

		path = argc > 2 ? argv[2] : "/dev/dri/renderD128";
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			return 1;
		handle = gem_new(fd, NOUVEAU_GEM_DOMAIN_GART, &info);
		if (handle == 0) {
			close(fd);
			return 1;
		}
		map = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, (off_t)info.map_handle);
		gem_close(fd, handle);
		close(fd);
		if (map == MAP_FAILED)
			return 1;
		*(volatile uint32_t *)map = 1;
		printf("MMAP_READY\n");
		fflush(stdout);
		for (;;)
			pause();
	}
	if (argc > 1 && strcmp(argv[1], "--mmap-count") == 0) {
		size_t map_count;
		bool ok;

		if (argc < 3)
			return 1;
		map_count = strtoul(argv[2], NULL, 0);
		path = argc > 3 ? argv[3] : "/dev/dri/renderD128";
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			return 1;
		ok = test_repeated_mmap(fd, map_count);
		close(fd);
		printf("TEST mmap count=%zu              %s\n", map_count,
		    ok ? "PASS" : "FAIL");
		return ok ? 0 : 1;
	}

	fd = open(path, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return 1;
	}
	memset(&alloc, 0, sizeof(alloc));
	alloc.fb_ctxdma_handle = ~0u;
	alloc.tt_ctxdma_handle = NOUVEAU_FIFO_ENGINE_GR;
	if (xioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, &alloc) != 0) {
		fprintf(stderr, "channel alloc failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("TEST no-share reservation          %s\n",
	    test_no_share_reservation(fd, (uint32_t)alloc.channel) ? "PASS" :
	    (++failed, "FAIL"));
	printf("TEST PRIME self-only round-trip    %s\n",
	    test_prime_self_only(fd) ? "PASS" : (++failed, "FAIL"));
	printf("TEST repeated mmap lifetime        %s\n",
	    test_repeated_mmap(fd, 64) ? "PASS" : (++failed, "FAIL"));
	printf("TEST mmap after GEM close          %s\n",
	    test_mmap_after_gem_close(fd) ? "PASS" : (++failed, "FAIL"));
	memset(&free_args, 0, sizeof(free_args));
	free_args.channel = alloc.channel;
	(void)xioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_FREE, &free_args);
	close(fd);
	printf("summary: 4 run, %u failed\n", failed);
	return failed == 0 ? 0 : 1;
}
