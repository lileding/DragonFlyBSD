/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reusable userspace smoke test for the nvgpu fake EXEC sync path.
 */

#include <sys/ioctl.h>
#include <sys/stat.h>
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

#define DRM_NOUVEAU_EXEC 0x12

#define DRM_NOUVEAU_SYNC_SYNCOBJ 0x0
#define DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ 0x1
#define DRM_NOUVEAU_SYNC_TYPE_MASK 0xf

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

#define DRM_IOCTL_NOUVEAU_EXEC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_EXEC, struct drm_nouveau_exec)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static unsigned tests_run;
static unsigned tests_failed;
static unsigned tests_skipped;
static bool current_skipped;
static const char *current_skip_reason;
static const char *device_path;
static unsigned iterations = 1;

static int64_t
abs_timeout_nsec(unsigned seconds)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return INT64_MAX;
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec +
	    (int64_t)seconds * 1000000000LL;
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

	if (device_path != NULL) {
		fd = open(device_path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			fprintf(stderr, "open %s failed: %s\n", device_path,
			    strerror(errno));
		return fd;
	}
	for (size_t i = 0; i < ARRAY_SIZE(paths); i++) {
		fd = open(paths[i], O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			device_path = paths[i];
			return fd;
		}
	}
	fprintf(stderr, "no DRM device found\n");
	return -1;
}

static bool
test_skip(const char *reason)
{
	current_skipped = true;
	current_skip_reason = reason;
	return true;
}

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

static uint32_t
syncobj_create(int fd, bool signaled)
{
	struct drm_syncobj_create args;

	memset(&args, 0, sizeof(args));
	args.flags = signaled ? DRM_SYNCOBJ_CREATE_SIGNALED : 0;
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
	(void)xioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args,
	    "DRM_IOCTL_SYNCOBJ_DESTROY");
}

static bool
syncobj_wait_binary(int fd, const uint32_t *handles, uint32_t count,
    unsigned seconds)
{
	struct drm_syncobj_wait args;

	memset(&args, 0, sizeof(args));
	args.handles = (uint64_t)(uintptr_t)handles;
	args.timeout_nsec = abs_timeout_nsec(seconds);
	args.count_handles = count;
	args.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	return xioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args,
	    "DRM_IOCTL_SYNCOBJ_WAIT") == 0;
}

static bool
syncobj_wait_timeline(int fd, const uint32_t *handles,
    const uint64_t *points, uint32_t count, unsigned seconds)
{
	struct drm_syncobj_timeline_wait args;

	memset(&args, 0, sizeof(args));
	args.handles = (uint64_t)(uintptr_t)handles;
	args.points = (uint64_t)(uintptr_t)points;
	args.timeout_nsec = abs_timeout_nsec(seconds);
	args.count_handles = count;
	args.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	return xioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args,
	    "DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT") == 0;
}

static bool
syncobj_signal_timeline(int fd, uint32_t handle, uint64_t point)
{
	struct drm_syncobj_timeline_array args;

	memset(&args, 0, sizeof(args));
	args.handles = (uint64_t)(uintptr_t)&handle;
	args.points = (uint64_t)(uintptr_t)&point;
	args.count_handles = 1;
	return xioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args,
	    "DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL") == 0;
}

static bool
syncobj_export_fd(int fd, uint32_t handle, int *out_fd)
{
	struct drm_syncobj_handle args;
	int ret;

	memset(&args, 0, sizeof(args));
	args.handle = handle;
	args.flags = 0;
	args.fd = -1;
	do {
		ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		if (errno != ENOSYS && errno != EOPNOTSUPP)
			fprintf(stderr,
			    "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD failed: errno=%d (%s)\n",
			    errno, strerror(errno));
		return false;
	}
	*out_fd = args.fd;
	return true;
}

static uint32_t
syncobj_import_fd(int fd, int shared_fd)
{
	struct drm_syncobj_handle args;

	memset(&args, 0, sizeof(args));
	args.flags = 0;
	args.fd = shared_fd;
	if (xioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args,
	    "DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE") != 0)
		return 0;
	return args.handle;
}

static bool
exec_submit(int fd, const struct drm_nouveau_sync *waits, uint32_t wait_count,
    const struct drm_nouveau_sync *sigs, uint32_t sig_count, uint32_t channel)
{
	struct drm_nouveau_exec exec;

	memset(&exec, 0, sizeof(exec));
	exec.channel = channel;
	exec.push_count = 0;
	exec.wait_count = wait_count;
	exec.sig_count = sig_count;
	exec.wait_ptr = (uint64_t)(uintptr_t)waits;
	exec.sig_ptr = (uint64_t)(uintptr_t)sigs;
	return xioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec,
	    "DRM_IOCTL_NOUVEAU_EXEC") == 0;
}

static bool
exec_expect_errno(int fd, const struct drm_nouveau_sync *waits,
    uint32_t wait_count, const struct drm_nouveau_sync *sigs,
    uint32_t sig_count, int expected_errno)
{
	struct drm_nouveau_exec exec;
	int ret;

	memset(&exec, 0, sizeof(exec));
	exec.wait_count = wait_count;
	exec.sig_count = sig_count;
	exec.wait_ptr = (uint64_t)(uintptr_t)waits;
	exec.sig_ptr = (uint64_t)(uintptr_t)sigs;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec);
	if (ret == 0) {
		fprintf(stderr, "DRM_IOCTL_NOUVEAU_EXEC unexpectedly succeeded\n");
		return false;
	}
	if (errno != expected_errno) {
		fprintf(stderr, "DRM_IOCTL_NOUVEAU_EXEC errno=%d expected=%d\n",
		    errno, expected_errno);
		return false;
	}
	return true;
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
test_binary_single(int fd)
{
	uint32_t out = syncobj_create(fd, false);
	struct drm_nouveau_sync sig = binary_sync(out);
	bool ok;

	ok = out != 0 && exec_submit(fd, NULL, 0, &sig, 1, 1) &&
	    syncobj_wait_binary(fd, &out, 1, 5);
	syncobj_destroy(fd, out);
	return ok;
}

static bool
test_binary_already_signaled_wait(int fd)
{
	uint32_t in = syncobj_create(fd, true);
	uint32_t out = syncobj_create(fd, false);
	struct drm_nouveau_sync wait = binary_sync(in);
	struct drm_nouveau_sync sig = binary_sync(out);
	bool ok;

	ok = in != 0 && out != 0 && exec_submit(fd, &wait, 1, &sig, 1, 2) &&
	    syncobj_wait_binary(fd, &out, 1, 5);
	syncobj_destroy(fd, in);
	syncobj_destroy(fd, out);
	return ok;
}

static bool
test_binary_chain(int fd)
{
	uint32_t a = syncobj_create(fd, false);
	uint32_t b = syncobj_create(fd, false);
	struct drm_nouveau_sync sig_a = binary_sync(a);
	struct drm_nouveau_sync wait_a = binary_sync(a);
	struct drm_nouveau_sync sig_b = binary_sync(b);
	bool ok;

	ok = a != 0 && b != 0 && exec_submit(fd, NULL, 0, &sig_a, 1, 3) &&
	    exec_submit(fd, &wait_a, 1, &sig_b, 1, 3) &&
	    syncobj_wait_binary(fd, &b, 1, 5);
	syncobj_destroy(fd, a);
	syncobj_destroy(fd, b);
	return ok;
}

static bool
test_binary_fanout(int fd)
{
	uint32_t root = syncobj_create(fd, false);
	uint32_t children[8];
	struct drm_nouveau_sync sig_root = binary_sync(root);
	struct drm_nouveau_sync wait_root = binary_sync(root);
	bool ok;

	memset(children, 0, sizeof(children));
	ok = root != 0 && exec_submit(fd, NULL, 0, &sig_root, 1, 4);
	for (size_t i = 0; ok && i < ARRAY_SIZE(children); i++) {
		struct drm_nouveau_sync sig_child;

		children[i] = syncobj_create(fd, false);
		sig_child = binary_sync(children[i]);
		ok = children[i] != 0 && exec_submit(fd, &wait_root, 1,
		    &sig_child, 1, 4);
	}
	if (ok)
		ok = syncobj_wait_binary(fd, children, ARRAY_SIZE(children), 5);
	for (size_t i = 0; i < ARRAY_SIZE(children); i++)
		syncobj_destroy(fd, children[i]);
	syncobj_destroy(fd, root);
	return ok;
}

static bool
test_binary_chain_stress(int fd)
{
	uint32_t handles[32];
	bool ok = true;

	memset(handles, 0, sizeof(handles));
	for (size_t i = 0; ok && i < ARRAY_SIZE(handles); i++) {
		struct drm_nouveau_sync wait;
		struct drm_nouveau_sync sig;
		uint32_t wait_count = 0;

		handles[i] = syncobj_create(fd, false);
		sig = binary_sync(handles[i]);
		if (i > 0) {
			wait = binary_sync(handles[i - 1]);
			wait_count = 1;
		}
		ok = handles[i] != 0 && exec_submit(fd,
		    wait_count ? &wait : NULL, wait_count, &sig, 1, 5);
	}
	if (ok)
		ok = syncobj_wait_binary(fd, &handles[ARRAY_SIZE(handles) - 1], 1, 5);
	for (size_t i = 0; i < ARRAY_SIZE(handles); i++)
		syncobj_destroy(fd, handles[i]);
	return ok;
}

static bool
test_timeline_chain(int fd)
{
	uint32_t t = syncobj_create(fd, false);
	struct drm_nouveau_sync sig1 = timeline_sync(t, 1);
	struct drm_nouveau_sync wait1 = timeline_sync(t, 1);
	struct drm_nouveau_sync sig2 = timeline_sync(t, 2);
	uint64_t point = 2;
	bool ok;

	ok = t != 0 && exec_submit(fd, NULL, 0, &sig1, 1, 6) &&
	    exec_submit(fd, &wait1, 1, &sig2, 1, 6) &&
	    syncobj_wait_timeline(fd, &t, &point, 1, 5);
	syncobj_destroy(fd, t);
	return ok;
}

static bool
test_timeline_already_signaled_wait(int fd)
{
	uint32_t t = syncobj_create(fd, false);
	struct drm_nouveau_sync wait7 = timeline_sync(t, 7);
	struct drm_nouveau_sync sig8 = timeline_sync(t, 8);
	uint64_t point = 8;
	bool ok;

	ok = t != 0 && syncobj_signal_timeline(fd, t, 7) &&
	    exec_submit(fd, &wait7, 1, &sig8, 1, 7) &&
	    syncobj_wait_timeline(fd, &t, &point, 1, 5);
	syncobj_destroy(fd, t);
	return ok;
}

static bool
test_mixed_multi_wait(int fd)
{
	uint32_t b = syncobj_create(fd, false);
	uint32_t t = syncobj_create(fd, false);
	uint32_t out = syncobj_create(fd, false);
	struct drm_nouveau_sync sig_b = binary_sync(b);
	struct drm_nouveau_sync sig_t2 = timeline_sync(t, 2);
	struct drm_nouveau_sync waits[2];
	struct drm_nouveau_sync sig_out = binary_sync(out);
	bool ok;

	waits[0] = binary_sync(b);
	waits[1] = timeline_sync(t, 2);
	ok = b != 0 && t != 0 && out != 0 &&
	    exec_submit(fd, NULL, 0, &sig_b, 1, 8) &&
	    exec_submit(fd, NULL, 0, &sig_t2, 1, 8) &&
	    exec_submit(fd, waits, 2, &sig_out, 1, 8) &&
	    syncobj_wait_binary(fd, &out, 1, 5);
	syncobj_destroy(fd, b);
	syncobj_destroy(fd, t);
	syncobj_destroy(fd, out);
	return ok;
}

static bool
test_cross_file_syncobj(int fd)
{
	int fd2 = open_device();
	int shared_fd = -1;
	uint32_t shared1 = 0, shared2 = 0, out2 = 0;
	struct drm_nouveau_sync sig_shared;
	struct drm_nouveau_sync wait_shared;
	struct drm_nouveau_sync sig_out;
	bool ok;

	if (fd2 < 0)
		return false;
	shared1 = syncobj_create(fd, false);
	ok = shared1 != 0;
	if (ok && !syncobj_export_fd(fd, shared1, &shared_fd)) {
		if (errno == ENOSYS || errno == EOPNOTSUPP) {
			ok = test_skip("syncobj fd export unsupported");
			goto out;
		}
		ok = false;
	}
	if (ok) {
		shared2 = syncobj_import_fd(fd2, shared_fd);
		out2 = syncobj_create(fd2, false);
	}
	if (shared_fd >= 0)
		close(shared_fd);
	sig_shared = binary_sync(shared1);
	wait_shared = binary_sync(shared2);
	sig_out = binary_sync(out2);
	ok = ok && shared2 != 0 && out2 != 0 &&
	    exec_submit(fd, NULL, 0, &sig_shared, 1, 9) &&
	    exec_submit(fd2, &wait_shared, 1, &sig_out, 1, 9) &&
	    syncobj_wait_binary(fd2, &out2, 1, 5);
out:
	syncobj_destroy(fd, shared1);
	syncobj_destroy(fd2, shared2);
	syncobj_destroy(fd2, out2);
	close(fd2);
	return ok;
}

static bool
test_invalid_wait_handle(int fd)
{
	uint32_t out = syncobj_create(fd, false);
	struct drm_nouveau_sync wait = binary_sync(0x7fffffffU);
	struct drm_nouveau_sync sig = binary_sync(out);
	bool ok;

	ok = out != 0 && exec_expect_errno(fd, &wait, 1, &sig, 1, ENOENT);
	syncobj_destroy(fd, out);
	return ok;
}

static bool
test_unsignaled_wait_without_fence(int fd)
{
	uint32_t in = syncobj_create(fd, false);
	uint32_t out = syncobj_create(fd, false);
	struct drm_nouveau_sync wait = binary_sync(in);
	struct drm_nouveau_sync sig = binary_sync(out);
	bool ok;

	ok = in != 0 && out != 0 &&
	    exec_expect_errno(fd, &wait, 1, &sig, 1, EINVAL);
	syncobj_destroy(fd, in);
	syncobj_destroy(fd, out);
	return ok;
}

static bool
test_close_with_pending_future(int fd_unused __attribute__((unused)))
{
	int fd = open_device();
	uint32_t out;
	struct drm_nouveau_sync sig;
	bool ok;

	if (fd < 0)
		return false;
	out = syncobj_create(fd, false);
	sig = binary_sync(out);
	ok = out != 0 && exec_submit(fd, NULL, 0, &sig, 1, 10);
	syncobj_destroy(fd, out);
	close(fd);
	usleep(50000);
	return ok;
}

struct test_case {
	const char *name;
	bool (*run)(int fd);
};

static const struct test_case tests[] = {
	{ "binary single exec signal", test_binary_single },
	{ "binary already-signaled wait", test_binary_already_signaled_wait },
	{ "binary chain", test_binary_chain },
	{ "binary fanout", test_binary_fanout },
	{ "binary chain stress", test_binary_chain_stress },
	{ "timeline chain", test_timeline_chain },
	{ "timeline already-signaled wait", test_timeline_already_signaled_wait },
	{ "mixed multi-wait", test_mixed_multi_wait },
	{ "cross-file syncobj", test_cross_file_syncobj },
	{ "invalid wait handle", test_invalid_wait_handle },
	{ "unsignaled wait without fence", test_unsignaled_wait_without_fence },
	{ "close with pending future", test_close_with_pending_future },
};

static void
run_one(int fd, const struct test_case *test)
{
	bool ok;

	tests_run++;
	current_skipped = false;
	current_skip_reason = NULL;
	printf("TEST %-34s", test->name);
	fflush(stdout);
	ok = test->run(fd);
	if (ok && current_skipped) {
		printf(" SKIP");
		if (current_skip_reason != NULL)
			printf(" (%s)", current_skip_reason);
		printf("\n");
		tests_skipped++;
	} else if (ok) {
		printf(" PASS\n");
	} else {
		printf(" FAIL\n");
		tests_failed++;
	}
}

static void
usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-d /dev/dri/renderD128] [-n loops]\n", prog);
}

int
main(int argc, char **argv)
{
	int fd;
	int ch;

	while ((ch = getopt(argc, argv, "d:n:h")) != -1) {
		switch (ch) {
		case 'd':
			device_path = optarg;
			break;
		case 'n': {
			char *endp = NULL;
			unsigned long value;

			errno = 0;
			value = strtoul(optarg, &endp, 10);
			if (errno != 0 || endp == optarg || *endp != '\0' ||
			    value == 0 || value > 1000000UL) {
				usage(argv[0]);
				return 2;
			}
			iterations = (unsigned)value;
			break;
		}
		case 'h':
		default:
			usage(argv[0]);
			return ch == 'h' ? 0 : 2;
		}
	}
	fd = open_device();
	if (fd < 0)
		return 1;
	printf("device: %s\n", device_path);
	for (unsigned iter = 0; iter < iterations; iter++) {
		if (iterations > 1)
			printf("iteration: %u/%u\n", iter + 1, iterations);
		for (size_t i = 0; i < ARRAY_SIZE(tests); i++)
			run_one(fd, &tests[i]);
	}
	close(fd);
	printf("summary: %u run, %u skipped, %u failed\n",
	    tests_run, tests_skipped, tests_failed);
	return tests_failed == 0 ? 0 : 1;
}
