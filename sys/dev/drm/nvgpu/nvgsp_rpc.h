/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP RPC transport boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_RPC_H_
#define _NVGSP_RPC_H_

struct nvgpu_device;

/* Initialize GSP RPC transport state before early RPCs. */
int nvgsp_rpc_init(struct nvgpu_device *gpu);
/* Queue early system-info RPC during GSP boot. */
int nvgsp_rpc_enqueue_system_info_preinit(struct nvgpu_device *gpu);
/* Queue early registry RPC during GSP boot. */
int nvgsp_rpc_enqueue_registry_preinit(struct nvgpu_device *gpu);
/* Notify GSP that the driver is unloading; may wait for RPC completion. */
int nvgsp_rpc_send_unloading_guest_driver(struct nvgpu_device *gpu);

#endif /* _NVGSP_RPC_H_ */
