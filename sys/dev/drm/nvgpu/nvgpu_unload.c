/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unload admission gate for the native NVIDIA GPU driver.
 */

#include "nvgpu_unload.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"

#include <drm/drmP.h>

#include <sys/kernel.h>
#include <sys/thread.h>

static MALLOC_DEFINE(M_NVGPU_UNLOAD, "nvgpu_unload", "nvgpu unload gate");

struct nvgpu_unload_state {
	struct lwkt_token token;
	bool unloading;
	uint32_t open_count;
	uint32_t last_open_count;
	uint32_t last_file_count;
	uint32_t last_mmap_count;
	uint32_t last_sched_count;
	uint32_t busy_count;
};

/* Owned by drm.ko; serializes DRM open, close, and lastclose. */
extern struct lock drm_global_mutex;
/* Initialize unload admission state before DRM users can open the device. */
int
nvgpu_unload_init(struct nvgpu_device *gpu)
{
	struct nvgpu_unload_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state == NULL)
		return (ENOMEM);
	lwkt_token_init(&state->token, "nvgpuunl");
	nvgpu_device_set_unload_state(gpu, state);
	nvgpu_log(NVGPU_LOG_DEBUG, "unload init\n");
	return (0);
}

/* Release unload admission state after DRM is unpublished and all users are gone. */
void
nvgpu_unload_fini(struct nvgpu_device *gpu)
{
	struct nvgpu_unload_state *state;

	state = nvgpu_device_unload_state(gpu);
	if (state == NULL)
		return;
	if (state->open_count != 0)
		nvgpu_log(NVGPU_LOG_INFO,
		    "unload fini with %u open files\n", state->open_count);
	nvgpu_device_set_unload_state(gpu, NULL);
	kfree(state);
}

/* Admit one DRM file open unless unload has started. */
int
nvgpu_unload_file_open(struct nvgpu_device *gpu)
{
	struct nvgpu_unload_state *state;
	int error = 0;

	state = nvgpu_device_unload_state(gpu);
	if (state == NULL)
		return (ENXIO);
	lwkt_gettoken(&state->token);
	if (state->unloading) {
		error = EBUSY;
	} else {
		state->open_count++;
		nvgpu_log(NVGPU_LOG_DEBUG,
		    "file open count=%u\n", state->open_count);
	}
	lwkt_reltoken(&state->token);
	return (error);
}

/* Drop one DRM file reference previously admitted by nvgpu_unload_file_open(). */
void
nvgpu_unload_file_close(struct nvgpu_device *gpu)
{
	struct nvgpu_unload_state *state;

	state = nvgpu_device_unload_state(gpu);
	if (state == NULL)
		return;
	lwkt_gettoken(&state->token);
	if (state->open_count == 0) {
		nvgpu_log(NVGPU_LOG_INFO, "file close without open\n");
	} else {
		state->open_count--;
		nvgpu_log(NVGPU_LOG_DEBUG,
		    "file close count=%u\n", state->open_count);
	}
	lwkt_reltoken(&state->token);
}

/* Start unload admission after proving the DRM core has no live users. */
int
nvgpu_unload_begin(struct nvgpu_device *gpu)
{
	struct nvgpu_unload_state *state;
	struct drm_device *ddev;
	struct drm_file *file_priv;
	uint32_t file_count = 0;
	uint32_t mmap_count = 0;
	uint32_t sched_count = 0;
	int error = 0;

	state = nvgpu_device_unload_state(gpu);
	if (state == NULL)
		return (0);
	ddev = nvgpu_device_drm_dev(gpu);
	if (ddev == NULL) {
		lwkt_gettoken(&state->token);
		state->unloading = true;
		lwkt_reltoken(&state->token);
		return (0);
	}

	/*
	 * Match the legacy driver gate: the decision is made while DRM open and
	 * close are serialized.  A racing open is either visible in open_count or
	 * reaches nvdrm_open() after unloading is set and gets refused.
	 */
	mutex_lock(&drm_global_mutex);
	mutex_lock(&ddev->filelist_mutex);
	list_for_each_entry(file_priv, &ddev->filelist, lhead)
		file_count++;
	mutex_unlock(&ddev->filelist_mutex);

	lwkt_gettoken(&state->token);
	if (ddev->open_count != 0 || file_count != 0 || mmap_count != 0 ||
	    sched_count != 0) {
		state->busy_count++;
		state->last_open_count = ddev->open_count;
		state->last_file_count = file_count;
		state->last_mmap_count = mmap_count;
		state->last_sched_count = sched_count;
		error = EBUSY;
	} else {
		state->unloading = true;
	}
	lwkt_reltoken(&state->token);
	mutex_unlock(&drm_global_mutex);

	if (error != 0) {
		nvgpu_log(NVGPU_LOG_INFO,
		    "unload busy: open=%u files=%u mmap=%u sched=%u\n",
		    state->last_open_count, state->last_file_count,
		    state->last_mmap_count, state->last_sched_count);
	} else {
		nvgpu_log(NVGPU_LOG_DEBUG, "unload admitted\n");
	}
	return (error);
}
