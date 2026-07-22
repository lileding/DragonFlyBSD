/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reusable userspace test for nvgpu EXEC, sync, and VM ordering.
 */

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <libdrm/drm.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DRM_NOUVEAU_CHANNEL_ALLOC 0x02
#define DRM_NOUVEAU_CHANNEL_FREE 0x03
#define DRM_NOUVEAU_VM_BIND 0x11
#define DRM_NOUVEAU_EXEC 0x12
#define DRM_NOUVEAU_GEM_NEW 0x40

#define NOUVEAU_FIFO_ENGINE_GR 0x01
#define DRM_NOUVEAU_VM_BIND_RUN_ASYNC 0x1

#define DRM_NOUVEAU_SYNC_SYNCOBJ 0x0
#define DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ 0x1
#define DRM_NOUVEAU_SYNC_TYPE_MASK 0xf
#define NOUVEAU_GEM_DOMAIN_GART (1u << 2)

#define DMA_BUF_SYNC_READ (1u << 0)
#define DMA_BUF_SYNC_WRITE (2u << 0)
#define DMA_BUF_SYNC_RW (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)

struct dma_buf_export_sync_file {
	uint32_t flags;
	int32_t fd;
};

struct dma_buf_import_sync_file {
	uint32_t flags;
	int32_t fd;
};

#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE \
	_IOWR('b', 2, struct dma_buf_export_sync_file)
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE \
	_IOW('b', 3, struct dma_buf_import_sync_file)

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

struct drm_nouveau_vm_bind {
	uint32_t op_count;
	uint32_t flags;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t op_ptr;
};

struct drm_nouveau_exec_push {
	uint64_t va;
	uint32_t va_len;
	uint32_t flags;
};

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

#define DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_ALLOC, \
	    struct drm_nouveau_channel_alloc)
#define DRM_IOCTL_NOUVEAU_CHANNEL_FREE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_FREE, \
	    struct drm_nouveau_channel_free)
#define DRM_IOCTL_NOUVEAU_VM_BIND \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_VM_BIND, \
	    struct drm_nouveau_vm_bind)

#define DRM_IOCTL_NOUVEAU_EXEC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_EXEC, struct drm_nouveau_exec)
#define DRM_IOCTL_NOUVEAU_GEM_NEW \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_GEM_NEW, \
	    struct drm_nouveau_gem_new)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static unsigned tests_run;
static unsigned tests_failed;
static unsigned tests_skipped;
static bool current_skipped;
static const char *current_skip_reason;
static const char *device_path;
static unsigned iterations = 1;
static uint32_t real_channel;

static int xioctl(int fd, unsigned long request, void *arg, const char *name);

static bool
channel_alloc_one(int fd, uint32_t *channel)
{
	struct drm_nouveau_channel_alloc args;

	memset(&args, 0, sizeof(args));
	args.fb_ctxdma_handle = ~0u;
	args.tt_ctxdma_handle = NOUVEAU_FIFO_ENGINE_GR;
	if (xioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, &args,
	    "DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC") != 0)
		return false;
	*channel = (uint32_t)args.channel;
	return true;
}

static void
channel_free_one(int fd, uint32_t channel)
{
	struct drm_nouveau_channel_free args;

	if (channel == 0)
		return;
	memset(&args, 0, sizeof(args));
	args.channel = (int32_t)channel;
	(void)xioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_FREE, &args,
	    "DRM_IOCTL_NOUVEAU_CHANNEL_FREE");
}

static bool
channel_alloc(int fd)
{
	return channel_alloc_one(fd, &real_channel);
}

static void
channel_free(int fd)
{
	channel_free_one(fd, real_channel);
	real_channel = 0;
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
syncobj_export_sync_file(int fd, uint32_t handle, int *sync_fd)
{
	struct drm_syncobj_handle args;

	memset(&args, 0, sizeof(args));
	args.handle = handle;
	args.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
	args.fd = -1;
	if (xioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args,
	    "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD(sync_file)") != 0)
		return false;
	*sync_fd = args.fd;
	return true;
}

static bool
syncobj_import_sync_file(int fd, uint32_t handle, int sync_fd)
{
	struct drm_syncobj_handle args;

	memset(&args, 0, sizeof(args));
	args.handle = handle;
	args.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
	args.fd = sync_fd;
	return xioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args,
	    "DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE(sync_file)") == 0;
}

static uint32_t
gem_new(int fd)
{
	struct drm_nouveau_gem_new args;

	memset(&args, 0, sizeof(args));
	args.info.size = 0x10000;
	args.info.domain = NOUVEAU_GEM_DOMAIN_GART;
	args.align = 0x1000;
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
	(void)xioctl(fd, DRM_IOCTL_GEM_CLOSE, &args, "DRM_IOCTL_GEM_CLOSE");
}

static bool
exec_submit_on_channel(int fd, const struct drm_nouveau_sync *waits,
    uint32_t wait_count, const struct drm_nouveau_sync *sigs,
    uint32_t sig_count, uint32_t channel)
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
exec_submit(int fd, const struct drm_nouveau_sync *waits, uint32_t wait_count,
    const struct drm_nouveau_sync *sigs, uint32_t sig_count,
    uint32_t sequence __attribute__((unused)))
{
	return exec_submit_on_channel(fd, waits, wait_count, sigs, sig_count,
	    real_channel);
}

static bool
vm_bind_empty_async(int fd, const struct drm_nouveau_sync *sigs,
    uint32_t sig_count)
{
	struct drm_nouveau_vm_bind bind;

	memset(&bind, 0, sizeof(bind));
	bind.flags = DRM_NOUVEAU_VM_BIND_RUN_ASYNC;
	bind.sig_count = sig_count;
	bind.sig_ptr = (uint64_t)(uintptr_t)sigs;
	return xioctl(fd, DRM_IOCTL_NOUVEAU_VM_BIND, &bind,
	    "DRM_IOCTL_NOUVEAU_VM_BIND") == 0;
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
test_ring_saturation(int fd)
{
	uint32_t handles[256];
	bool ok = true;

	memset(handles, 0, sizeof(handles));
	for (size_t i = 0; ok && i < ARRAY_SIZE(handles); i++) {
		struct drm_nouveau_sync sig;

		handles[i] = syncobj_create(fd, false);
		sig = binary_sync(handles[i]);
		ok = handles[i] != 0 && exec_submit(fd, NULL, 0, &sig, 1, 5);
	}
	if (ok)
		ok = syncobj_wait_binary(fd, handles, ARRAY_SIZE(handles), 10);
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
	uint32_t channel2 = 0;
	struct drm_nouveau_sync sig_shared;
	struct drm_nouveau_sync wait_shared;
	struct drm_nouveau_sync sig_out;
	bool ok;

	if (fd2 < 0)
		return false;
	if (!channel_alloc_one(fd2, &channel2)) {
		close(fd2);
		return false;
	}
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
	    exec_submit_on_channel(fd2, &wait_shared, 1, &sig_out, 1,
	    channel2) &&
	    syncobj_wait_binary(fd2, &out2, 1, 5);
out:
	syncobj_destroy(fd, shared1);
	syncobj_destroy(fd2, shared2);
	syncobj_destroy(fd2, out2);
	channel_free_one(fd2, channel2);
	close(fd2);
	return ok;
}

static bool
test_cross_file_timeline_syncobj(int fd)
{
	int fd2 = open_device();
	int shared_fd = -1;
	uint32_t shared1 = 0, shared2 = 0, out2 = 0;
	uint32_t channel2 = 0;
	struct drm_nouveau_sync sig_shared;
	struct drm_nouveau_sync wait_shared;
	struct drm_nouveau_sync sig_out;
	uint64_t point = 2;
	bool ok;

	if (fd2 < 0)
		return false;
	if (!channel_alloc_one(fd2, &channel2)) {
		close(fd2);
		return false;
	}
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
	sig_shared = timeline_sync(shared1, 1);
	wait_shared = timeline_sync(shared2, 1);
	sig_out = timeline_sync(out2, 2);
	ok = ok && shared2 != 0 && out2 != 0 &&
	    exec_submit(fd, NULL, 0, &sig_shared, 1, 9) &&
	    exec_submit_on_channel(fd2, &wait_shared, 1, &sig_out, 1,
	    channel2) && syncobj_wait_timeline(fd2, &out2, &point, 1, 5);
out:
	if (shared_fd >= 0)
		close(shared_fd);
	syncobj_destroy(fd, shared1);
	syncobj_destroy(fd2, shared2);
	syncobj_destroy(fd2, out2);
	channel_free_one(fd2, channel2);
	close(fd2);
	return ok;
}

static bool
test_dmabuf_fence_array_wait(int fd)
{
	struct dma_buf_import_sync_file import;
	struct dma_buf_export_sync_file export;
	struct drm_prime_handle prime;
	uint32_t fence_a = 0, fence_b = 0, array = 0, out = 0;
	uint32_t bo = 0;
	int sync_a = -1, sync_b = -1, array_fd = -1, dmabuf_fd = -1;
	struct drm_nouveau_sync sig_a, sig_b, wait_array, sig_out;
	bool ok = true;

	for (unsigned i = 0; ok && i < 64; i++)
		ok = vm_bind_empty_async(fd, NULL, 0);
	fence_a = syncobj_create(fd, false);
	fence_b = syncobj_create(fd, false);
	array = syncobj_create(fd, false);
	out = syncobj_create(fd, false);
	sig_a = binary_sync(fence_a);
	sig_b = binary_sync(fence_b);
	wait_array = binary_sync(array);
	sig_out = binary_sync(out);
	ok = ok && fence_a != 0 && fence_b != 0 && array != 0 && out != 0 &&
	    exec_submit(fd, NULL, 0, &sig_a, 1, real_channel) &&
	    exec_submit(fd, NULL, 0, &sig_b, 1, real_channel) &&
	    syncobj_export_sync_file(fd, fence_a, &sync_a) &&
	    syncobj_export_sync_file(fd, fence_b, &sync_b);
	bo = gem_new(fd);
	memset(&prime, 0, sizeof(prime));
	prime.handle = bo;
	prime.flags = DRM_CLOEXEC;
	prime.fd = -1;
	if (ok && bo != 0 && xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime,
	    "DRM_IOCTL_PRIME_HANDLE_TO_FD") == 0)
		dmabuf_fd = prime.fd;
	else
		ok = false;
	memset(&import, 0, sizeof(import));
	import.flags = DMA_BUF_SYNC_WRITE;
	import.fd = sync_a;
	if (ok && xioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import,
	    "DMA_BUF_IOCTL_IMPORT_SYNC_FILE(write)") != 0)
		ok = false;
	import.flags = DMA_BUF_SYNC_READ;
	import.fd = sync_b;
	if (ok && xioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import,
	    "DMA_BUF_IOCTL_IMPORT_SYNC_FILE(read)") != 0)
		ok = false;
	memset(&export, 0, sizeof(export));
	export.flags = DMA_BUF_SYNC_RW;
	export.fd = -1;
	if (ok && xioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export,
	    "DMA_BUF_IOCTL_EXPORT_SYNC_FILE") == 0)
		array_fd = export.fd;
	else
		ok = false;
	ok = ok && syncobj_import_sync_file(fd, array, array_fd) &&
	    exec_submit(fd, &wait_array, 1, &sig_out, 1, real_channel) &&
	    syncobj_wait_binary(fd, &out, 1, 10);
	if (array_fd >= 0)
		close(array_fd);
	if (dmabuf_fd >= 0)
		close(dmabuf_fd);
	if (sync_a >= 0)
		close(sync_a);
	if (sync_b >= 0)
		close(sync_b);
	gem_close(fd, bo);
	syncobj_destroy(fd, fence_a);
	syncobj_destroy(fd, fence_b);
	syncobj_destroy(fd, array);
	syncobj_destroy(fd, out);
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
	uint32_t channel = 0;
	uint32_t out[80];
	bool ok = true;

	if (fd < 0)
		return false;
	memset(out, 0, sizeof(out));
	ok = channel_alloc_one(fd, &channel);
	for (size_t i = 0; ok && i < ARRAY_SIZE(out); i++) {
		struct drm_nouveau_sync sig;

		out[i] = syncobj_create(fd, false);
		sig = binary_sync(out[i]);
		ok = out[i] != 0 && exec_submit_on_channel(fd, NULL, 0, &sig, 1,
		    channel);
	}
	for (size_t i = 0; i < ARRAY_SIZE(out); i++)
		syncobj_destroy(fd, out[i]);
	close(fd);
	usleep(200000);
	return ok;
}

static bool
test_channel_reuse_after_free(int fd)
{
	for (unsigned i = 0; i < 128; i++) {
		struct drm_nouveau_channel_free free_args;
		uint32_t channel;

		if (!channel_alloc_one(fd, &channel))
			return false;
		memset(&free_args, 0, sizeof(free_args));
		free_args.channel = (int32_t)channel;
		if (xioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_FREE, &free_args,
		    "DRM_IOCTL_NOUVEAU_CHANNEL_FREE") != 0)
			return false;
	}
	return true;
}

#define CHANNEL_CREATE_THREADS 16u

struct channel_create_race {
	atomic_uint ready;
	atomic_uint allocated;
	atomic_bool start;
	atomic_bool release;
};

struct channel_create_thread {
	struct channel_create_race *race;
	int fd;
	uint32_t channel;
	bool ok;
};

static void *
channel_create_thread(void *argument)
{
	struct channel_create_thread *thread = argument;
	struct channel_create_race *race = thread->race;
	struct drm_nouveau_sync signal;
	uint32_t syncobj;

	atomic_fetch_add_explicit(&race->ready, 1, memory_order_release);
	while (!atomic_load_explicit(&race->start, memory_order_acquire))
		sched_yield();
	thread->ok = channel_alloc_one(thread->fd, &thread->channel);
	atomic_fetch_add_explicit(&race->allocated, 1, memory_order_release);
	while (!atomic_load_explicit(&race->release, memory_order_acquire))
		sched_yield();
	if (thread->ok) {
		syncobj = syncobj_create(thread->fd, false);
		signal = binary_sync(syncobj);
		thread->ok = syncobj != 0 && exec_submit_on_channel(thread->fd,
		    NULL, 0, &signal, 1, thread->channel) &&
		    syncobj_wait_binary(thread->fd, &syncobj, 1, 10);
		syncobj_destroy(thread->fd, syncobj);
		channel_free_one(thread->fd, thread->channel);
	}
	return NULL;
}

static bool
test_concurrent_first_channel_create(int fd_unused __attribute__((unused)))
{
	struct channel_create_thread contexts[CHANNEL_CREATE_THREADS];
	struct channel_create_race race;
	pthread_t threads[CHANNEL_CREATE_THREADS];
	unsigned created;
	bool ok;
	int fd;

	fd = open_device();
	if (fd < 0)
		return false;
	memset(&race, 0, sizeof(race));
	memset(contexts, 0, sizeof(contexts));
	atomic_init(&race.ready, 0);
	atomic_init(&race.allocated, 0);
	atomic_init(&race.start, false);
	atomic_init(&race.release, false);
	created = 0;
	for (; created < CHANNEL_CREATE_THREADS; created++) {
		contexts[created].race = &race;
		contexts[created].fd = fd;
		if (pthread_create(&threads[created], NULL,
		    channel_create_thread, &contexts[created]) != 0)
			break;
	}
	while (atomic_load_explicit(&race.ready, memory_order_acquire) != created)
		sched_yield();
	atomic_store_explicit(&race.start, true, memory_order_release);
	while (atomic_load_explicit(&race.allocated, memory_order_acquire) != created)
		sched_yield();
	atomic_store_explicit(&race.release, true, memory_order_release);
	ok = created == CHANNEL_CREATE_THREADS;
	for (unsigned i = 0; i < created; i++) {
		if (pthread_join(threads[i], NULL) != 0 || !contexts[i].ok)
			ok = false;
	}
	close(fd);
	return ok;
}

static bool
test_cross_file_concurrent_channel_submit(int fd_unused __attribute__((unused)))
{
	struct channel_create_thread contexts[CHANNEL_CREATE_THREADS];
	struct channel_create_race race;
	pthread_t threads[CHANNEL_CREATE_THREADS];
	unsigned created;
	bool ok;

	memset(&race, 0, sizeof(race));
	memset(contexts, 0, sizeof(contexts));
	atomic_init(&race.ready, 0);
	atomic_init(&race.allocated, 0);
	atomic_init(&race.start, false);
	atomic_init(&race.release, false);
	created = 0;
	for (; created < CHANNEL_CREATE_THREADS; created++) {
		contexts[created].race = &race;
		contexts[created].fd = open_device();
		if (contexts[created].fd < 0)
			break;
		if (pthread_create(&threads[created], NULL,
		    channel_create_thread, &contexts[created]) != 0) {
			close(contexts[created].fd);
			break;
		}
	}
	while (atomic_load_explicit(&race.ready, memory_order_acquire) != created)
		sched_yield();
	atomic_store_explicit(&race.start, true, memory_order_release);
	while (atomic_load_explicit(&race.allocated, memory_order_acquire) != created)
		sched_yield();
	atomic_store_explicit(&race.release, true, memory_order_release);
	ok = created == CHANNEL_CREATE_THREADS;
	for (unsigned i = 0; i < created; i++) {
		if (pthread_join(threads[i], NULL) != 0 || !contexts[i].ok)
			ok = false;
		close(contexts[i].fd);
	}
	return ok;
}

static bool
test_channel_free_with_pending_exec(int fd)
{
	struct drm_nouveau_channel_free free_args;
	struct drm_nouveau_exec rejected;
	uint32_t channel = 0;
	uint32_t out[80];
	bool ok = true;
	int ret;

	memset(out, 0, sizeof(out));
	ok = channel_alloc_one(fd, &channel);
	for (size_t i = 0; ok && i < ARRAY_SIZE(out); i++) {
		struct drm_nouveau_sync sig;

		out[i] = syncobj_create(fd, false);
		sig = binary_sync(out[i]);
		ok = out[i] != 0 && exec_submit_on_channel(fd, NULL, 0, &sig, 1,
		    channel);
	}
	memset(&free_args, 0, sizeof(free_args));
	free_args.channel = (int32_t)channel;
	if (ok)
		ok = xioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_FREE, &free_args,
		    "DRM_IOCTL_NOUVEAU_CHANNEL_FREE") == 0;
	memset(&rejected, 0, sizeof(rejected));
	rejected.channel = channel;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &rejected);
	if (ok)
		ok = ret != 0 && errno == ENOENT;
	if (ok)
		ok = syncobj_wait_binary(fd, out, ARRAY_SIZE(out), 10);
	for (size_t i = 0; i < ARRAY_SIZE(out); i++)
		syncobj_destroy(fd, out[i]);
	return ok;
}

static bool
test_exec_bind_exec_chain(int fd)
{
	uint32_t exec0 = syncobj_create(fd, false);
	uint32_t bind = syncobj_create(fd, false);
	uint32_t exec1 = syncobj_create(fd, false);
	uint32_t handles[3] = { exec0, bind, exec1 };
	struct drm_nouveau_sync exec0_sig = binary_sync(exec0);
	struct drm_nouveau_sync bind_sig = binary_sync(bind);
	struct drm_nouveau_sync exec1_sig = binary_sync(exec1);
	bool ok;

	ok = exec0 != 0 && bind != 0 && exec1 != 0 &&
	    exec_submit(fd, NULL, 0, &exec0_sig, 1, real_channel) &&
	    vm_bind_empty_async(fd, &bind_sig, 1) &&
	    exec_submit(fd, NULL, 0, &exec1_sig, 1, real_channel) &&
	    syncobj_wait_binary(fd, handles, ARRAY_SIZE(handles), 5);
	syncobj_destroy(fd, exec0);
	syncobj_destroy(fd, bind);
	syncobj_destroy(fd, exec1);
	return ok;
}

static bool
test_exec_bind_exec_bind_exec_chain(int fd)
{
	uint32_t handles[5];
	struct drm_nouveau_sync signals[5];
	bool ok = true;

	memset(handles, 0, sizeof(handles));
	for (size_t i = 0; i < ARRAY_SIZE(handles); i++) {
		handles[i] = syncobj_create(fd, false);
		signals[i] = binary_sync(handles[i]);
		ok = ok && handles[i] != 0;
	}
	ok = ok && exec_submit(fd, NULL, 0, &signals[0], 1, real_channel) &&
	    vm_bind_empty_async(fd, &signals[1], 1) &&
	    exec_submit(fd, NULL, 0, &signals[2], 1, real_channel) &&
	    vm_bind_empty_async(fd, &signals[3], 1) &&
	    exec_submit(fd, NULL, 0, &signals[4], 1, real_channel) &&
	    syncobj_wait_binary(fd, handles, ARRAY_SIZE(handles), 10);
	for (size_t i = 0; i < ARRAY_SIZE(handles); i++)
		syncobj_destroy(fd, handles[i]);
	return ok;
}

static bool
test_close_with_pending_exec_bind_chains(int fd_unused __attribute__((unused)))
{
	int fd = open_device();
	uint32_t channel = 0;
	bool ok = true;

	if (fd < 0)
		return false;
	ok = channel_alloc_one(fd, &channel);
	for (unsigned i = 0; ok && i < 32; i++) {
		uint32_t exec = syncobj_create(fd, false);
		uint32_t bind = syncobj_create(fd, false);
		struct drm_nouveau_sync exec_sig = binary_sync(exec);
		struct drm_nouveau_sync bind_sig = binary_sync(bind);

		ok = exec != 0 && bind != 0 &&
		    exec_submit_on_channel(fd, NULL, 0, &exec_sig, 1, channel) &&
		    vm_bind_empty_async(fd, &bind_sig, 1);
		syncobj_destroy(fd, exec);
		syncobj_destroy(fd, bind);
	}
	close(fd);
	usleep(500000);
	return ok;
}

static bool
test_invalid_channel(int fd)
{
	struct drm_nouveau_exec exec;
	uint32_t out = syncobj_create(fd, false);
	struct drm_nouveau_sync sig = binary_sync(out);
	struct drm_syncobj_wait wait;
	bool ok;

	memset(&exec, 0, sizeof(exec));
	exec.channel = 0x7fffffffU;
	exec.sig_count = 1;
	exec.sig_ptr = (uint64_t)(uintptr_t)&sig;
	errno = 0;
	ok = out != 0 && ioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec) != 0 &&
	    errno == ENOENT;
	memset(&wait, 0, sizeof(wait));
	wait.handles = (uint64_t)(uintptr_t)&out;
	wait.timeout_nsec = abs_timeout_nsec(0);
	wait.count_handles = 1;
	wait.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	if (ok)
		ok = ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait) != 0;
	syncobj_destroy(fd, out);
	return ok;
}

static bool
test_invalid_push_pointer(int fd)
{
	struct drm_nouveau_exec exec;
	uint32_t out = syncobj_create(fd, false);
	struct drm_nouveau_sync sig = binary_sync(out);
	struct drm_syncobj_wait wait;
	bool ok;

	memset(&exec, 0, sizeof(exec));
	exec.channel = real_channel;
	exec.push_count = 1;
	exec.push_ptr = 1;
	exec.sig_count = 1;
	exec.sig_ptr = (uint64_t)(uintptr_t)&sig;
	errno = 0;
	ok = out != 0 && ioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec) != 0 &&
	    errno == EFAULT;
	memset(&wait, 0, sizeof(wait));
	wait.handles = (uint64_t)(uintptr_t)&out;
	wait.timeout_nsec = abs_timeout_nsec(0);
	wait.count_handles = 1;
	wait.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	if (ok)
		ok = ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait) != 0;
	syncobj_destroy(fd, out);
	return ok;
}

static bool
test_invalid_push_flags(int fd)
{
	struct drm_nouveau_exec_push push;
	struct drm_nouveau_exec exec;

	memset(&push, 0, sizeof(push));
	push.va = 0x1000;
	push.va_len = 4;
	push.flags = 2;
	memset(&exec, 0, sizeof(exec));
	exec.channel = real_channel;
	exec.push_count = 1;
	exec.push_ptr = (uint64_t)(uintptr_t)&push;
	errno = 0;
	return ioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec) != 0 &&
	    errno == EINVAL;
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
	{ "ring saturation", test_ring_saturation },
	{ "timeline chain", test_timeline_chain },
	{ "timeline already-signaled wait", test_timeline_already_signaled_wait },
	{ "mixed multi-wait", test_mixed_multi_wait },
	{ "cross-file syncobj", test_cross_file_syncobj },
	{ "cross-file timeline syncobj", test_cross_file_timeline_syncobj },
	{ "dma-buf fence-array wait", test_dmabuf_fence_array_wait },
	{ "invalid wait handle", test_invalid_wait_handle },
	{ "unsignaled wait without fence", test_unsignaled_wait_without_fence },
	{ "close with pending future", test_close_with_pending_future },
	{ "channel reuse after free", test_channel_reuse_after_free },
	{ "concurrent channel create and submit",
	    test_concurrent_first_channel_create },
	{ "cross-file concurrent channel submit",
	    test_cross_file_concurrent_channel_submit },
	{ "channel free with pending exec", test_channel_free_with_pending_exec },
	{ "exec bind exec chain", test_exec_bind_exec_chain },
	{ "exec bind exec bind exec chain", test_exec_bind_exec_bind_exec_chain },
	{ "close with pending exec bind chains",
	    test_close_with_pending_exec_bind_chains },
	{ "invalid channel", test_invalid_channel },
	{ "invalid push pointer", test_invalid_push_pointer },
	{ "invalid push flags", test_invalid_push_flags },
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
	if (!channel_alloc(fd)) {
		close(fd);
		return 1;
	}
	printf("device: %s\n", device_path);
	for (unsigned iter = 0; iter < iterations; iter++) {
		if (iterations > 1)
			printf("iteration: %u/%u\n", iter + 1, iterations);
		for (size_t i = 0; i < ARRAY_SIZE(tests); i++)
			run_one(fd, &tests[i]);
	}
	channel_free(fd);
	close(fd);
	printf("summary: %u run, %u skipped, %u failed\n",
	    tests_run, tests_skipped, tests_failed);
	return tests_failed == 0 ? 0 : 1;
}
