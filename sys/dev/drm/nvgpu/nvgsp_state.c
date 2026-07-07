/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP backend state boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_state.h"
#include "nvgsp_priv.h"

static MALLOC_DEFINE(M_NVGSP_STATE, "nvgsp_state", "nvgsp backend state");

/* Return the borrowed GSP state stored on the physical GPU object. */
struct nvgsp_state *
nvgsp_state_get(struct nvgpu_device *gpu)
{
	if (gpu == NULL)
		return (NULL);
	return (nvgpu_device_gsp(gpu));
}

/* Allocate CPU-side GSP state.  The GPU owns the returned object until state_fini. */
int
nvgsp_state_init(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp;

	if (nvgpu_device_gsp(gpu) != NULL)
		return (0);

	gsp = kmalloc(sizeof(*gsp), M_NVGSP_STATE, M_WAITOK | M_ZERO);
	gsp->gpu = gpu;
	gsp->dev = nvgpu_device_dev(gpu);
	gsp->chip = nvgpu_device_chip(gpu);
	lwkt_token_init(&gsp->gsp_tok, "nvgsp");
	LIST_INIT(&gsp->gsp_pending);
	nvgpu_device_set_gsp(gpu, gsp);
	nvgpu_log(NVGPU_LOG_DEBUG, "gsp state initialized for %s\n", gsp->chip->chip);
	return (0);
}

/* Release CPU-side GSP state after all firmware/runtime resources are gone. */
void
nvgsp_state_fini(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp;

	gsp = nvgsp_state_get(gpu);
	if (gsp == NULL)
		return;
	if (gsp->gsp != NULL || gsp->sec2 != NULL || gsp->gsp_shm.kva != NULL ||
	    gsp->wpr_meta.kva != NULL || gsp->vbios != NULL) {
		nvgpu_log(NVGPU_LOG_INFO,
		    "gsp state still owns resources at state_fini; shutdown path incomplete\n");
	}
	nvgpu_device_set_gsp(gpu, NULL);
	kfree(gsp, M_NVGSP_STATE);
}

/* Query static GPU information from GSP. */
int
nvgsp_state_query_static_info(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);

	if (gsp == NULL)
		return (ENXIO);
	return (nvgsp_static_query_info(gsp));
}

/* Query the method-buffer size from GSP. */
int
nvgsp_state_query_mthdbuf_size(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);

	if (gsp == NULL)
		return (ENXIO);
	/*
	 * The old driver used a conservative fixed method buffer until channel
	 * bring-up queried the RM path.  Keep that boot contract here; the
	 * channel module will replace this with the real RPC while being moved.
	 */
	gsp->mthdbuf_size = 0x4000;
	nvgpu_log(NVGPU_LOG_DEBUG, "gsp method buffer size defaulted to %u\n",
	    gsp->mthdbuf_size);
	return (0);
}

/* Retrieve the interrupt table from GSP. */
int
nvgsp_state_get_intr_table(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);

	if (gsp == NULL)
		return (ENXIO);
	/*
	 * Interrupt table parsing belongs to nvgpu_intr.  Keep the masks zeroed
	 * until that module is moved off the skeleton.
	 */
	memset(gsp->gsp_nonstall_leaf_mask, 0, sizeof(gsp->gsp_nonstall_leaf_mask));
	memset(gsp->gsp_stall_leaf_mask, 0, sizeof(gsp->gsp_stall_leaf_mask));
	memset(gsp->gsp_engine_leaf_mask, 0, sizeof(gsp->gsp_engine_leaf_mask));
	memset(gsp->gsp_disp_leaf_mask, 0, sizeof(gsp->gsp_disp_leaf_mask));
	return (0);
}

/* Enable doorbells before channel submission. */
int
nvgsp_state_enable_doorbell(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	uint32_t v;

	if (gsp == NULL)
		return (ENXIO);
	v = nvgsp_rd32(gsp, 0x00b65000);
	nvgsp_wr32(gsp, 0x00b65000, v | 0x80000000u);
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "gsp doorbell enable 0xb65000 was 0x%08x now 0x%08x\n",
	    v, v | 0x80000000u);
	return (0);
}
