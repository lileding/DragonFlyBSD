/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP channel backend boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_channel.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"
#include "nvgsp_bar.h"
#include "nvgsp_rm.h"
#include "nvgsp_vmm.h"
#include "nvgsp_vram.h"

#define NVGSP_CHANNEL_CALL(expr) do { \
	int error__ = (expr); \
	if (error__ != 0) \
		return (error__); \
} while (0)

/* Allocate a channel identifier. */
static int
nvgsp_channel_alloc_chid(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel alloc chid\n");
	return (0);
}

/* Allocate pages used by channel submission. */
static int
nvgsp_channel_alloc_submit_pages(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel alloc submit pages\n");
	return (0);
}

/* Allocate the channel object through RM. */
static int
nvgsp_channel_rm_alloc(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel rm alloc\n");
	return (0);
}

/* Bind the channel to its target GPU engine. */
static int
nvgsp_channel_bind_engine(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel bind engine\n");
	return (0);
}

/* Schedule the channel for GPU execution. */
static int
nvgsp_channel_schedule(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel schedule\n");
	return (0);
}

/* Fetch the token required for work submission. */
static int
nvgsp_channel_get_work_submit_token(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel get work submit token\n");
	return (0);
}

/* Allocate resources specific to the golden channel. */
static int
nvgsp_channel_alloc_golden(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel alloc golden\n");
	NVGSP_CHANNEL_CALL(nvgsp_vram_alloc_golden(gpu));
	return (0);
}

/* Promote a graphics context to usable state. */
static int
nvgsp_channel_promote_gr_context(struct nvgpu_device *gpu, int golden)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel promote gr context\n");
	(void)golden;
	return (0);
}

/* Allocate an RM graphics object for a channel. */
static int
nvgsp_channel_alloc_graphics_object(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel alloc graphics object\n");
	return (0);
}

/* Free an RM graphics object for a channel. */
static int
nvgsp_channel_free_graphics_object(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel free graphics object\n");
	return (nvgsp_rm_free_graphics_object(gpu));
}

/* Create the bootstrap channel. */
/* Create the bootstrap channel during boot. */
int
nvgsp_channel_create_bootstrap(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel create bootstrap\n");
	NVGSP_CHANNEL_CALL(nvgsp_channel_alloc_chid(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_vram_alloc_channel_inst(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_vram_alloc_userd(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_channel_alloc_submit_pages(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_vmm_map_submit_pages(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_bar1_map_inst(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_bar1_map_userd(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_channel_rm_alloc(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_channel_bind_engine(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_channel_schedule(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_channel_get_work_submit_token(gpu));
	return (0);
}

/* Destroy the bootstrap channel. */
/* Destroy the bootstrap channel after submissions have stopped. */
void
nvgsp_channel_destroy_bootstrap(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel destroy bootstrap\n");
}

/* Create and promote the golden channel. */
/* Create the golden channel used as a template for user channels. */
int
nvgsp_channel_create_golden(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel create golden\n");
	NVGSP_CHANNEL_CALL(nvgsp_vmm_create_golden(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_channel_alloc_golden(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_channel_promote_gr_context(gpu, 1));
	NVGSP_CHANNEL_CALL(nvgsp_channel_alloc_graphics_object(gpu));
	NVGSP_CHANNEL_CALL(nvgsp_channel_free_graphics_object(gpu));
	return (0);
}

/* Destroy the golden channel. */
/* Destroy golden channel state after user channel creation has stopped. */
void
nvgsp_channel_destroy_golden(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "channel destroy golden\n");
	nvgsp_vmm_destroy_golden(gpu);
}
