/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP RPC transport boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_rpc.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


int
nvgsp_rpc_init(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "rpc init\n");
	return (0);
}

/* Queue the early system-info RPC. */
/* Queue early system-info RPC during GSP boot. */
int
nvgsp_rpc_prequeue_system_info(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "rpc prequeue system info\n");
	return (0);
}

/* Queue the early registry RPC. */
/* Queue early registry RPC during GSP boot. */
int
nvgsp_rpc_prequeue_registry(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "rpc prequeue registry\n");
	return (0);
}

/* Notify GSP that the guest driver is unloading. */
/* Notify GSP that the driver is unloading; may wait for RPC completion. */
int
nvgsp_rpc_unloading_guest_driver(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "rpc unloading guest driver\n");
	return (0);
}
