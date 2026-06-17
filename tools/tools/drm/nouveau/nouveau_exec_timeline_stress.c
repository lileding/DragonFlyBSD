/*
 * nouveau_exec_timeline_stress - stress EXEC timeline signal teardown.
 *
 * Build:
 *   cc -Wall -Wextra \
 *      -I/home/lileding/src/nvkm/sys/dev/drm/include \
 *      -I/home/lileding/src/nvkm/sys/dev/drm/include/uapi/drm \
 *      -o /var/tmp/nouveau_exec_timeline_stress nouveau_exec_timeline_stress.c
 *
 * Run:
 *   /var/tmp/nouveau_exec_timeline_stress [/dev/dri/renderD128] [iterations]
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

#define DRM_NOUVEAU_CHANNEL_ALLOC	0x02
#define DRM_NOUVEAU_CHANNEL_FREE	0x03
#define DRM_NOUVEAU_EXEC		0x12

#define NOUVEAU_FIFO_ENGINE_GR		0x01
#define DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ	0x1

#ifndef ETIME
#define ETIME ETIMEDOUT
#endif

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

#define DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_ALLOC, \
	    struct drm_nouveau_channel_alloc)
#define DRM_IOCTL_NOUVEAU_CHANNEL_FREE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_FREE, \
	    struct drm_nouveau_channel_free)
#define DRM_IOCTL_NOUVEAU_EXEC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_EXEC, struct drm_nouveau_exec)

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

static int
channel_alloc(int fd, int32_t *channel)
{
	struct drm_nouveau_channel_alloc req;

	memset(&req, 0, sizeof(req));
	req.fb_ctxdma_handle = ~0u;
	req.tt_ctxdma_handle = NOUVEAU_FIFO_ENGINE_GR;
	if (ioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, &req) != 0) {
		fprintf(stderr, "NOUVEAU_CHANNEL_ALLOC failed: %s\n",
		    strerror(errno));
		return (-1);
	}

	*channel = req.channel;
	return (0);
}

static void
channel_free(int fd, int32_t channel)
{
	struct drm_nouveau_channel_free req;

	if (channel < 0)
		return;

	memset(&req, 0, sizeof(req));
	req.channel = channel;
	if (ioctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_FREE, &req) != 0)
		fprintf(stderr, "NOUVEAU_CHANNEL_FREE channel=%d failed: %s\n",
		    channel, strerror(errno));
}

/*
 * exec_signal_timeline()
 *
 * Ownership:
 *   Borrows fd and does not own the syncobj handle.  The kernel takes its own
 *   references to the done fence and syncobj while queuing the EXEC job.
 *
 * Lifetime:
 *   The caller may destroy the syncobj immediately after this returns; that is
 *   the lifetime edge this stress test is meant to exercise.
 *
 * Threading:
 *   Single-threaded userspace submit.  Kernel completion runs asynchronously.
 */
static int
exec_signal_timeline(int fd, int32_t channel, uint32_t handle, uint64_t point)
{
	struct drm_nouveau_sync sig;
	struct drm_nouveau_exec exec;

	memset(&sig, 0, sizeof(sig));
	sig.flags = DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ;
	sig.handle = handle;
	sig.timeline_value = point;

	memset(&exec, 0, sizeof(exec));
	exec.channel = (uint32_t)channel;
	exec.sig_count = 1;
	exec.sig_ptr = (uint64_t)(uintptr_t)&sig;

	if (ioctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec) != 0) {
		fprintf(stderr, "NOUVEAU_EXEC point=%llu failed: %s\n",
		    (unsigned long long)point, strerror(errno));
		return (-1);
	}

	return (0);
}

static int
syncobj_timeline_poll(int fd, uint32_t handle, uint64_t point,
    uint64_t *etime_count)
{
	struct drm_syncobj_timeline_wait req;
	uint32_t handles[1];
	uint64_t points[1];

	handles[0] = handle;
	points[0] = point;
	memset(&req, 0, sizeof(req));
	req.handles = (uint64_t)(uintptr_t)handles;
	req.points = (uint64_t)(uintptr_t)points;
	req.count_handles = 1;
	req.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	req.timeout_nsec = 0;

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &req) == 0)
		return (0);
	if (errno == ETIME) {
		(*etime_count)++;
		return (0);
	}

	fprintf(stderr, "TIMELINE_WAIT point=%llu failed: %s\n",
	    (unsigned long long)point, strerror(errno));
	return (-1);
}

int
main(int argc, char **argv)
{
	const char *node = "/dev/dri/renderD128";
	unsigned long iterations = 20000;
	uint64_t etime_count = 0;
	int32_t channel = -1;
	int fd;
	int ret = 1;

	if (argc > 1)
		node = argv[1];
	if (argc > 2) {
		char *end = NULL;

		errno = 0;
		iterations = strtoul(argv[2], &end, 0);
		if (errno != 0 || end == argv[2] || *end != '\0' ||
		    iterations == 0) {
			fprintf(stderr, "invalid iteration count: %s\n", argv[2]);
			return (1);
		}
	}

	fd = open_drm_node(node);
	if (fd < 0)
		return (1);

	if (channel_alloc(fd, &channel) != 0)
		goto out;

	for (unsigned long i = 1; i <= iterations; i++) {
		uint32_t handle = 0;

		if (syncobj_create(fd, &handle) != 0)
			goto out;
		if (exec_signal_timeline(fd, channel, handle, i) != 0) {
			syncobj_destroy(fd, handle);
			goto out;
		}
		if (syncobj_timeline_poll(fd, handle, i, &etime_count) != 0) {
			syncobj_destroy(fd, handle);
			goto out;
		}
		syncobj_destroy(fd, handle);
	}

	printf("PASS: %lu EXEC timeline points, %llu timeout polls\n",
	    iterations, (unsigned long long)etime_count);
	ret = 0;
out:
	channel_free(fd, channel);
	close(fd);
	return (ret);
}
