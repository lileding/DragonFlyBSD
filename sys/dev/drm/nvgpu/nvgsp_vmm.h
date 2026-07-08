/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP VMM backend boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_VMM_H_
#define _NVGSP_VMM_H_

#include <sys/stdint.h>

struct nvgpu_device;
struct nvgsp_vmm;

/* Create kernel/GSP GPUVA state before channels. */
int nvgsp_vmm_init_kernel(struct nvgpu_device *gpu);
/* Destroy kernel/GSP GPUVA state after channels stop. */
void nvgsp_vmm_fini_kernel(struct nvgpu_device *gpu);
/* Create VMM state for the golden channel. */
int nvgsp_vmm_create_golden(struct nvgpu_device *gpu);
/* Destroy VMM state for the golden channel. */
void nvgsp_vmm_destroy_golden(struct nvgpu_device *gpu);
/* Map submission support pages before channel publication. */
int nvgsp_vmm_map_submit_pages(struct nvgpu_device *gpu);
/* Create a per-process user VMM.  out receives owned storage destroyed by destroy_user. */
int nvgsp_vmm_create_user(struct nvgpu_device *gpu, uint32_t client_handle,
    struct nvgsp_vmm **out);
/* Destroy a per-process user VMM after all channels using it are gone. */
void nvgsp_vmm_destroy_user(struct nvgsp_vmm *vmm);

#endif /* _NVGSP_VMM_H_ */
