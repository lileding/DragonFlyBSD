/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP BAR aperture boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_BAR_H_
#define _NVGSP_BAR_H_

#include <sys/stdint.h>

struct nvgpu_device;
struct nvgsp_state;

/* Initialize BAR2 backend state during boot; gpu is borrowed. */
int nvgsp_bar_init_bar2(struct nvgpu_device *gpu);
/* Release BAR2 state after all BAR2 users have stopped. */
void nvgsp_bar_fini_bar2(struct nvgpu_device *gpu);
/* Initialize BAR1 backend state during boot; gpu is borrowed. */
int nvgsp_bar_init_bar1(struct nvgpu_device *gpu);
/* Release BAR1 state after CPU mappings have drained. */
void nvgsp_bar_fini_bar1(struct nvgpu_device *gpu);
/* Map channel instance memory through BAR1.  Caller serializes BAR1 updates. */
int nvgsp_bar_map_bar1_inst(struct nvgpu_device *gpu);
/* Map USERD pages through BAR1 before channel publication. */
int nvgsp_bar_map_bar1_userd(struct nvgpu_device *gpu);

/*
 * Map one existing contiguous allocation through BAR1.
 *
 * On success pgva receives the owned GPU virtual address of the mapping.  The
 * caller keeps the physical storage alive and must unmap the same range before
 * releasing it.  These operations may sleep while updating VMM state.
 */
int nvgsp_bar_map_bar1_existing_range(struct nvgsp_state *gsp,
    uint64_t paddr, uint64_t size, uint64_t *pgva);
void nvgsp_bar_unmap_bar1_existing_range(struct nvgsp_state *gsp,
    uint64_t gva, uint64_t size);

/*
 * Map or unmap count pages from existing physical storage through BAR1.
 *
 * gvas is caller-owned storage.  A successful map fills every entry; unmap
 * consumes those mappings but not the array or physical pages.
 */
int nvgsp_bar_map_bar1_existing_scatter(struct nvgsp_state *gsp,
    uint64_t paddr, uint64_t size, uint64_t *gvas, uint32_t count);
void nvgsp_bar_unmap_bar1_existing_scatter(struct nvgsp_state *gsp,
    uint64_t *gvas, uint32_t count);

#endif /* _NVGSP_BAR_H_ */
