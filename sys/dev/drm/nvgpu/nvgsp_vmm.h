/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP VMM backend boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_VMM_H_
#define _NVGSP_VMM_H_

struct nvgpu_device;

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

#endif /* _NVGSP_VMM_H_ */
