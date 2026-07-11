/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reusable userspace smoke and stress test for nouveau VM_BIND semantics.
 */

#include <sys/ioctl.h>
#include <sys/types.h>

#include <libdrm/drm.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DRM_NOUVEAU_VM_BIND 0x11
#define DRM_NOUVEAU_GEM_NEW 0x40

#define NOUVEAU_GEM_DOMAIN_VRAM (1u << 1)
#define NOUVEAU_GEM_DOMAIN_GART (1u << 2)
#define NOUVEAU_GEM_DOMAIN_NO_SHARE (1u << 5)

#define DRM_NOUVEAU_SYNC_SYNCOBJ 0x0
#define DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ 0x1
#define DRM_NOUVEAU_VM_BIND_OP_MAP 0x0
#define DRM_NOUVEAU_VM_BIND_OP_UNMAP 0x1
#define DRM_NOUVEAU_VM_BIND_SPARSE (1u << 8)
#define DRM_NOUVEAU_VM_BIND_RUN_ASYNC 0x1

#define PAGE_4K 0x1000ULL
#define PAGE_64K 0x10000ULL
#define PAGE_2M 0x200000ULL
#define TEST_VA 0x200000000ULL

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

struct drm_nouveau_sync {
	uint32_t flags;
	uint32_t handle;
	uint64_t timeline_value;
};

struct drm_nouveau_vm_bind_op {
	uint32_t op;
	uint32_t flags;
	uint32_t handle;
	uint32_t pad;
	uint64_t addr;
	uint64_t bo_offset;
	uint64_t range;
};

struct drm_nouveau_vm_bind {
	uint32_t op_count;
	uint32_t flags;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t op_ptr;
};

#define DRM_IOCTL_NOUVEAU_GEM_NEW \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_NEW, \
	    struct drm_nouveau_gem_new)
#define DRM_IOCTL_NOUVEAU_VM_BIND \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_VM_BIND, \
	    struct drm_nouveau_vm_bind)

struct fixture {
	uint32_t vram;
	uint32_t gart;
};

static unsigned tests_run;
static unsigned tests_failed;
static const char *device_path;
static unsigned iterations = 1;

static int
xioctl(int fd, unsigned long request, void *arg, const char *name)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		fprintf(stderr, "%s failed: errno=%d (%s)\n", name, errno,
		    strerror(errno));
	return ret;
}

static int
open_device(void)
{
	static const char *paths[] = {
		"/dev/dri/renderD128",
		"/dev/dri/card0",
		"/dev/dri/card1",
	};
	int fd;

	if (device_path != NULL)
		return open(device_path, O_RDWR | O_CLOEXEC);
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fd = open(paths[i], O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			device_path = paths[i];
			return fd;
		}
	}
	return -1;
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
	if (xioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args,
	    "DRM_IOCTL_SYNCOBJ_CREATE") != 0)
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
	(void)ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

static bool
syncobj_wait_binary(int fd, uint32_t handle)
{
	struct drm_syncobj_wait args;

	memset(&args, 0, sizeof(args));
	args.handles = (uint64_t)(uintptr_t)&handle;
	args.timeout_nsec = abs_timeout_nsec(5);
	args.count_handles = 1;
	args.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	return xioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args,
	    "DRM_IOCTL_SYNCOBJ_WAIT") == 0;
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
	return xioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args,
	    "DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT") == 0;
}

static uint32_t
gem_new(int fd, uint64_t size, uint32_t domain, uint32_t align)
{
	struct drm_nouveau_gem_new args;

	memset(&args, 0, sizeof(args));
	args.info.size = size;
	args.info.domain = domain | NOUVEAU_GEM_DOMAIN_NO_SHARE;
	args.align = align;
	if (xioctl(fd, DRM_IOCTL_NOUVEAU_GEM_NEW, &args,
	    "DRM_IOCTL_NOUVEAU_GEM_NEW") != 0)
		return 0;
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
	(void)ioctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

static struct drm_nouveau_sync
binary_sync(uint32_t handle)
{
	struct drm_nouveau_sync sync;

	memset(&sync, 0, sizeof(sync));
	sync.flags = DRM_NOUVEAU_SYNC_SYNCOBJ;
	sync.handle = handle;
	return sync;
}

static struct drm_nouveau_sync
timeline_sync(uint32_t handle, uint64_t point)
{
	struct drm_nouveau_sync sync;

	memset(&sync, 0, sizeof(sync));
	sync.flags = DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ;
	sync.handle = handle;
	sync.timeline_value = point;
	return sync;
}

static bool
vm_bind(int fd, struct drm_nouveau_vm_bind_op *ops, uint32_t op_count,
	uint32_t flags, struct drm_nouveau_sync *waits, uint32_t wait_count,
	struct drm_nouveau_sync *signals, uint32_t signal_count)
{
	struct drm_nouveau_vm_bind args;

	memset(&args, 0, sizeof(args));
	args.op_count = op_count;
	args.flags = flags;
	args.wait_count = wait_count;
	args.sig_count = signal_count;
	args.wait_ptr = (uint64_t)(uintptr_t)waits;
	args.sig_ptr = (uint64_t)(uintptr_t)signals;
	args.op_ptr = (uint64_t)(uintptr_t)ops;
	return xioctl(fd, DRM_IOCTL_NOUVEAU_VM_BIND, &args,
	    "DRM_IOCTL_NOUVEAU_VM_BIND") == 0;
}

static bool
vm_bind_expect_errno(int fd, struct drm_nouveau_vm_bind_op *ops,
	uint32_t op_count, uint32_t flags, struct drm_nouveau_sync *waits,
	uint32_t wait_count, struct drm_nouveau_sync *signals,
	uint32_t signal_count, int expected)
{
	struct drm_nouveau_vm_bind args;
	int ret;

	memset(&args, 0, sizeof(args));
	args.op_count = op_count;
	args.flags = flags;
	args.wait_count = wait_count;
	args.sig_count = signal_count;
	args.wait_ptr = (uint64_t)(uintptr_t)waits;
	args.sig_ptr = (uint64_t)(uintptr_t)signals;
	args.op_ptr = (uint64_t)(uintptr_t)ops;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_NOUVEAU_VM_BIND, &args);
	return ret < 0 && errno == expected;
}

static struct drm_nouveau_vm_bind_op
map_op(uint32_t handle, uint64_t va, uint64_t bo_offset, uint64_t size,
	uint32_t flags)
{
	struct drm_nouveau_vm_bind_op op;

	memset(&op, 0, sizeof(op));
	op.op = DRM_NOUVEAU_VM_BIND_OP_MAP;
	op.flags = flags;
	op.handle = handle;
	op.addr = va;
	op.bo_offset = bo_offset;
	op.range = size;
	return op;
}

static struct drm_nouveau_vm_bind_op
unmap_op(uint64_t va, uint64_t size, uint32_t flags)
{
	struct drm_nouveau_vm_bind_op op;

	memset(&op, 0, sizeof(op));
	op.op = DRM_NOUVEAU_VM_BIND_OP_UNMAP;
	op.flags = flags;
	op.addr = va;
	op.range = size;
	return op;
}

static bool
test_sync_map_unmap(int fd, const struct fixture *f)
{
	struct drm_nouveau_vm_bind_op op;

	op = map_op(f->vram, TEST_VA, 0, 2 * PAGE_2M, 0);
	if (!vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0))
		return false;
	op = unmap_op(TEST_VA, 2 * PAGE_2M, 0);
	return vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0);
}

static bool
test_overlap_split_replace(int fd, const struct fixture *f)
{
	struct drm_nouveau_vm_bind_op op;
	uint64_t va = TEST_VA + 0x1000000ULL;

	op = map_op(f->vram, va, 0, 2 * PAGE_2M, 0);
	if (!vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0))
		return false;
	op = map_op(f->gart, va + PAGE_2M, 0, PAGE_64K, 0);
	if (!vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0))
		return false;
	op = unmap_op(va + PAGE_2M + 3 * PAGE_4K, PAGE_4K, 0);
	if (!vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0))
		return false;
	op = unmap_op(va, 2 * PAGE_2M, 0);
	return vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0);
}

static bool
test_sparse_replace(int fd, const struct fixture *f)
{
	struct drm_nouveau_vm_bind_op op;
	uint64_t va = TEST_VA + 0x2000000ULL;

	op = map_op(0, va, 0, PAGE_2M, DRM_NOUVEAU_VM_BIND_SPARSE);
	if (!vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0))
		return false;
	op = map_op(f->vram, va + PAGE_64K, 0, PAGE_64K, 0);
	if (!vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0))
		return false;
	op = unmap_op(va, PAGE_2M, DRM_NOUVEAU_VM_BIND_SPARSE);
	return vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0);
}

static bool
test_multi_op_batch(int fd, const struct fixture *f)
{
	struct drm_nouveau_vm_bind_op ops[4];
	uint64_t va = TEST_VA + 0x3000000ULL;

	ops[0] = map_op(f->vram, va, 0, PAGE_2M, 0);
	ops[1] = map_op(f->gart, va + PAGE_2M, 0, PAGE_64K, 0);
	ops[2] = unmap_op(va + PAGE_64K, PAGE_64K, 0);
	ops[3] = unmap_op(va, PAGE_2M + PAGE_64K, 0);
	return vm_bind(fd, ops, 4, 0, NULL, 0, NULL, 0);
}

static bool
test_async_binary_chain(int fd, const struct fixture *f)
{
	struct drm_nouveau_vm_bind_op first, second;
	struct drm_nouveau_sync sig_a, wait_a, sig_b;
	uint32_t a = syncobj_create(fd);
	uint32_t b = syncobj_create(fd);
	uint64_t va = TEST_VA + 0x4000000ULL;
	bool ok;

	first = map_op(f->vram, va, 0, PAGE_64K, 0);
	second = unmap_op(va, PAGE_64K, 0);
	sig_a = binary_sync(a);
	wait_a = binary_sync(a);
	sig_b = binary_sync(b);
	ok = a != 0 && b != 0 && vm_bind(fd, &first, 1,
	    DRM_NOUVEAU_VM_BIND_RUN_ASYNC, NULL, 0, &sig_a, 1) &&
	    vm_bind(fd, &second, 1, DRM_NOUVEAU_VM_BIND_RUN_ASYNC,
	    &wait_a, 1, &sig_b, 1) && syncobj_wait_binary(fd, b);
	syncobj_destroy(fd, a);
	syncobj_destroy(fd, b);
	return ok;
}

static bool
test_async_timeline_chain(int fd, const struct fixture *f)
{
	struct drm_nouveau_vm_bind_op first, second;
	struct drm_nouveau_sync sig1, wait1, sig2;
	uint32_t timeline = syncobj_create(fd);
	uint64_t va = TEST_VA + 0x5000000ULL;
	bool ok;

	first = map_op(f->gart, va, 0, PAGE_64K, 0);
	second = unmap_op(va, PAGE_64K, 0);
	sig1 = timeline_sync(timeline, 1);
	wait1 = timeline_sync(timeline, 1);
	sig2 = timeline_sync(timeline, 2);
	ok = timeline != 0 && vm_bind(fd, &first, 1,
	    DRM_NOUVEAU_VM_BIND_RUN_ASYNC, NULL, 0, &sig1, 1) &&
	    vm_bind(fd, &second, 1, DRM_NOUVEAU_VM_BIND_RUN_ASYNC,
	    &wait1, 1, &sig2, 1) &&
	    syncobj_wait_timeline(fd, timeline, 2);
	syncobj_destroy(fd, timeline);
	return ok;
}

static bool
test_empty_async_signal(int fd, const struct fixture *f __attribute__((unused)))
{
	struct drm_nouveau_sync sig;
	uint32_t out = syncobj_create(fd);
	bool ok;

	sig = binary_sync(out);
	ok = out != 0 && vm_bind(fd, NULL, 0,
	    DRM_NOUVEAU_VM_BIND_RUN_ASYNC, NULL, 0, &sig, 1) &&
	    syncobj_wait_binary(fd, out);
	syncobj_destroy(fd, out);
	return ok;
}

static bool
test_validation(int fd, const struct fixture *f)
{
	struct drm_nouveau_vm_bind_op op;
	struct drm_nouveau_sync sig;
	uint32_t out = syncobj_create(fd);
	bool ok = out != 0;

	sig = binary_sync(out);
	op = map_op(f->vram, TEST_VA, 0, PAGE_4K, 0);
	ok = ok && vm_bind_expect_errno(fd, &op, 1, 2, NULL, 0, NULL, 0,
	    EINVAL);
	ok = ok && vm_bind_expect_errno(fd, &op, 1, 0, NULL, 0, &sig, 1,
	    EINVAL);
	op.op = 99;
	ok = ok && vm_bind_expect_errno(fd, &op, 1, 0, NULL, 0, NULL, 0,
	    EINVAL);
	op = map_op(f->vram, TEST_VA + 1, 0, PAGE_4K, 0);
	ok = ok && vm_bind_expect_errno(fd, &op, 1, 0, NULL, 0, NULL, 0,
	    EINVAL);
	op = map_op(f->vram, TEST_VA, 0, 0, 0);
	ok = ok && vm_bind_expect_errno(fd, &op, 1, 0, NULL, 0, NULL, 0,
	    EINVAL);
	op = map_op(0x7fffffffU, TEST_VA, 0, PAGE_4K, 0);
	ok = ok && vm_bind_expect_errno(fd, &op, 1, 0, NULL, 0, NULL, 0,
	    ENOENT);
	op = map_op(f->vram, TEST_VA, 4 * PAGE_2M, PAGE_4K, 0);
	ok = ok && vm_bind_expect_errno(fd, &op, 1, 0, NULL, 0, NULL, 0,
	    EINVAL);
	syncobj_destroy(fd, out);
	return ok;
}

static bool
test_repeated_map_unmap(int fd, const struct fixture *f)
{
	struct drm_nouveau_vm_bind_op op;
	uint64_t va = TEST_VA + 0x6000000ULL;

	for (unsigned i = 0; i < 256; i++) {
		op = map_op((i & 1) ? f->vram : f->gart, va, 0, PAGE_64K, 0);
		if (!vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0))
			return false;
		op = unmap_op(va, PAGE_64K, 0);
		if (!vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0))
			return false;
	}
	return true;
}

static bool
test_mapping_after_gem_close(int fd, const struct fixture *f __attribute__((unused)))
{
	struct drm_nouveau_vm_bind_op op;
	uint64_t va = TEST_VA + 0x7000000ULL;
	uint32_t handle;

	handle = gem_new(fd, PAGE_64K, NOUVEAU_GEM_DOMAIN_GART, PAGE_64K);
	if (handle == 0)
		return false;
	op = map_op(handle, va, 0, PAGE_64K, 0);
	if (!vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0)) {
		gem_close(fd, handle);
		return false;
	}
	gem_close(fd, handle);
	op = unmap_op(va, PAGE_64K, 0);
	return vm_bind(fd, &op, 1, 0, NULL, 0, NULL, 0);
}

struct test_case {
	const char *name;
	bool (*run)(int fd, const struct fixture *fixture);
};

static const struct test_case tests[] = {
	{ "sync map/unmap", test_sync_map_unmap },
	{ "overlap split/replace", test_overlap_split_replace },
	{ "sparse replace/unmap", test_sparse_replace },
	{ "multi-op batch", test_multi_op_batch },
	{ "async binary chain", test_async_binary_chain },
	{ "async timeline chain", test_async_timeline_chain },
	{ "empty async signal", test_empty_async_signal },
	{ "validation errors", test_validation },
	{ "repeated map/unmap", test_repeated_map_unmap },
	{ "mapping after GEM close", test_mapping_after_gem_close },
};

static void
run_one(int fd, const struct fixture *fixture, const struct test_case *test)
{
	bool ok;

	tests_run++;
	printf("TEST %-30s", test->name);
	fflush(stdout);
	ok = test->run(fd, fixture);
	if (ok) {
		printf(" PASS\n");
	} else {
		printf(" FAIL\n");
		tests_failed++;
	}
}

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "usage: %s [-d /dev/dri/renderD128] [-n loops] [-t test-index]\n",
	    prog);
}

int
main(int argc, char **argv)
{
	struct fixture fixture;
	int fd, ch, selected = -1;

	while ((ch = getopt(argc, argv, "d:n:t:h")) != -1) {
		switch (ch) {
		case 'd':
			device_path = optarg;
			break;
		case 'n':
			iterations = (unsigned)strtoul(optarg, NULL, 10);
			if (iterations == 0)
				return 2;
			break;
		case 't':
			selected = (int)strtol(optarg, NULL, 10);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return ch == 'h' ? 0 : 2;
		}
	}
	if (selected >= (int)(sizeof(tests) / sizeof(tests[0]))) {
		usage(argv[0]);
		return 2;
	}
	fd = open_device();
	if (fd < 0) {
		fprintf(stderr, "open DRM device failed: %s\n", strerror(errno));
		return 1;
	}
	memset(&fixture, 0, sizeof(fixture));
	fixture.vram = gem_new(fd, 4 * PAGE_2M, NOUVEAU_GEM_DOMAIN_VRAM,
	    PAGE_2M);
	fixture.gart = gem_new(fd, 4 * PAGE_2M, NOUVEAU_GEM_DOMAIN_GART,
	    PAGE_64K);
	if (fixture.vram == 0 || fixture.gart == 0) {
		fprintf(stderr, "failed to create VM_BIND test BOs\n");
		return 1;
	}
	printf("device: %s vram=%u gart=%u\n", device_path, fixture.vram,
	    fixture.gart);
	for (unsigned iter = 0; iter < iterations; iter++) {
		if (selected >= 0) {
			run_one(fd, &fixture, &tests[selected]);
		} else {
			for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
				run_one(fd, &fixture, &tests[i]);
		}
	}
	gem_close(fd, fixture.vram);
	gem_close(fd, fixture.gart);
	close(fd);
	printf("summary: %u run, %u failed\n", tests_run, tests_failed);
	return tests_failed == 0 ? 0 : 1;
}
