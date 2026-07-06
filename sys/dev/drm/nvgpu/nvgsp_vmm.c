/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP VMM backend boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_vmm.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"
#include "nvgsp_rm.h"

#define NVGSP_VMM_CALL(expr) do { \
	int error__ = (expr); \
	if (error__ != 0) \
		return (error__); \
} while (0)

/* Create a backend GPU virtual address space. */
static int
nvgsp_vmm_create_vaspace(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vmm create vaspace\n");
	return (0);
}

/* Create backend page tables for a VMM. */
static int
nvgsp_vmm_create_page_tables(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vmm create page tables\n");
	return (0);
}

/* Create the kernel GPU virtual address space. */
/* Create kernel/GSP GPUVA state before channels. */
int
nvgsp_vmm_init_kernel(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vmm init kernel\n");
	NVGSP_VMM_CALL(nvgsp_rm_client_create(gpu));
	NVGSP_VMM_CALL(nvgsp_rm_device_create(gpu));
	NVGSP_VMM_CALL(nvgsp_rm_subdevice_create(gpu));
	NVGSP_VMM_CALL(nvgsp_vmm_create_vaspace(gpu));
	NVGSP_VMM_CALL(nvgsp_vmm_create_page_tables(gpu));
	NVGSP_VMM_CALL(nvgsp_rm_create_usermode_object(gpu));
	return (0);
}

/* Destroy the kernel GPU virtual address space. */
/* Destroy kernel/GSP GPUVA state after channels stop. */
void
nvgsp_vmm_fini_kernel(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vmm fini kernel\n");
}

/* Create VMM state for the golden channel. */
int
nvgsp_vmm_create_golden(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vmm create golden\n");
	return (0);
}

/* Destroy VMM state for the golden channel. */
void
nvgsp_vmm_destroy_golden(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vmm destroy golden\n");
}

/* Map submission support pages into the backend VMM. */
/* Map submission support pages before channel publication. */
int
nvgsp_vmm_map_submit_pages(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "vmm map submit pages\n");
	return (0);
}
