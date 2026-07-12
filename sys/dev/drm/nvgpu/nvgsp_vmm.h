/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP VMM backend boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_VMM_H_
#define _NVGSP_VMM_H_

#include <sys/stdint.h>
#include <vm/vm.h>

struct nvgpu_bo;
struct nvgpu_device;
struct nvgsp_vmm;
struct nvgsp_vmm_sparse_region;
struct nvgsp_vmm_sparse_unmap_plan;
struct nvgsp_vmm_user_pt;

typedef void (*nvgsp_vmm_sparse_unmap_clear_fn)(void *arg,
    uint64_t va, uint64_t size, uint8_t page_shift);

struct nvgsp_vmm_dirty_range {
	uint64_t start;
	uint64_t end;
};

struct nvgsp_vmm_dirty_set {
	const struct nvgsp_vmm_dirty_range *ranges;
	uint32_t range_count;
	uint64_t page_count;
	int overflow;
};

struct nvgsp_vmm_pte_info {
	uint64_t va;
	uint64_t pte;
	uint64_t lpte;
	uint32_t pd2_idx;
	uint32_t pd1_idx;
	uint32_t pd0_idx;
	uint32_t lpt_idx;
	uint32_t spt_idx;
	uint8_t has_pt;
};

/*
 * VMM operation contract
 *
 * Public operations serialize their backend tracker with the VMM token and
 * may sleep while allocating page tables.  Functions ending in _noflush update
 * page-table state without a final GMMU invalidate; callers batch those calls
 * and finish with nvgsp_vmm_flush() or nvgsp_vmm_flush_dirty().  Prepared
 * commit functions consume state allocated by their matching prepare function;
 * abort/fini functions consume prepared state when commit is not performed.
 */

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
/* Map a sysmem range and flush the VMM before returning. */
int nvgsp_vmm_map_sysmem(struct nvgsp_vmm *vmm, uint64_t va,
    uint64_t paddr, uint64_t size);
/* Map already prepared sysmem PTEs without the final GMMU flush. */
int nvgsp_vmm_map_sysmem_noflush(struct nvgsp_vmm *vmm, uint64_t va,
    vm_paddr_t paddr, uint64_t size);
int nvgsp_vmm_map_sysmem_bo_prepared_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, const struct nvgpu_bo *bo, uint64_t bo_offset,
    uint64_t size, uint8_t kind);
int nvgsp_vmm_map_sysmem_paddrs_page_prepared_noflush(
    struct nvgsp_vmm *vmm, uint64_t va, const vm_paddr_t *paddrs,
    uint32_t page_count, uint8_t kind, uint8_t page_shift);
/* Map a VRAM range and flush the VMM before returning. */
int nvgsp_vmm_map_vram(struct nvgsp_vmm *vmm, uint64_t va,
    uint64_t paddr, uint64_t size, uint8_t kind);
/* Map or promote prepared VRAM/sysmem PTEs without the final GMMU flush. */
int nvgsp_vmm_map_vram_flags_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t paddr, uint64_t size, uint8_t priv,
    uint8_t ro, uint8_t kind);
int nvgsp_vmm_map_vram_flags_page_prepared_noflush(
    struct nvgsp_vmm *vmm, uint64_t va, uint64_t paddr, uint64_t size,
    uint8_t priv, uint8_t ro, uint8_t kind, uint8_t page_shift);
int nvgsp_vmm_promote_vram_64k_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t paddr, uint64_t size, uint8_t priv,
    uint8_t ro, uint8_t kind);
int nvgsp_vmm_promote_vram_2m_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t paddr, uint64_t size, uint8_t priv,
    uint8_t ro, uint8_t kind);
int nvgsp_vmm_promote_sysmem_2m_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, const vm_paddr_t *paddrs, uint32_t page_count,
    uint8_t kind);
/* Clear a GPUVA range and flush the VMM before returning. */
int nvgsp_vmm_unmap(struct nvgsp_vmm *vmm, uint64_t va, uint64_t size);
/* Validate or clear prepared page-table ranges without the final flush. */
int nvgsp_vmm_unmap_valid_page_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size, uint8_t page_shift);
int nvgsp_vmm_unmap_valid_preserve_page_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size, uint8_t preserve_page_shift);
int nvgsp_vmm_clear_conflict_preserve_page_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size, uint8_t preserve_page_shift);
int nvgsp_vmm_check_unmap_valid_range_page(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size, int preserve_target_pts,
    uint8_t preserve_page_shift);
int nvgsp_vmm_check_clear_pd0_target_range(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size);
int nvgsp_vmm_check_clear_pd0_target_range_allow_sparse(
    struct nvgsp_vmm *vmm, uint64_t va, uint64_t size);
int nvgsp_vmm_clear_pd0_target_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size);
int nvgsp_vmm_ensure_pt_range(struct nvgsp_vmm *vmm, uint64_t va,
    uint64_t size);
int nvgsp_vmm_ensure_pd0_range(struct nvgsp_vmm *vmm, uint64_t va,
    uint64_t size);
int nvgsp_vmm_check_prepared_pt_range(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size, uint8_t page_shift);

/* Prepare, validate, commit, or abort one 2 MiB VRAM split. */
int nvgsp_vmm_prepare_split_vram_2m(struct nvgsp_vmm *vmm,
    uint64_t va, struct nvgsp_vmm_user_pt **pt);
int nvgsp_vmm_check_split_vram_2m(struct nvgsp_vmm *vmm,
    uint64_t va, struct nvgsp_vmm_user_pt *pt);
int nvgsp_vmm_commit_split_vram_2m_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, struct nvgsp_vmm_user_pt *pt);
void nvgsp_vmm_abort_split_vram_2m(struct nvgsp_vmm *vmm,
    struct nvgsp_vmm_user_pt *pt);

/* Prepare and commit sparse metadata; abort consumes an uncommitted region. */
int nvgsp_vmm_has_sparse_region(struct nvgsp_vmm *vmm, uint64_t va,
    uint64_t size);
int nvgsp_vmm_prepare_sparse_region_page(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size, uint8_t page_shift,
    struct nvgsp_vmm_sparse_region **region);
int nvgsp_vmm_commit_sparse_noflush(struct nvgsp_vmm *vmm,
    struct nvgsp_vmm_sparse_region *region);
int nvgsp_vmm_check_sparse_regions_commit(struct nvgsp_vmm *vmm,
    struct nvgsp_vmm_sparse_region **regions, uint32_t region_count,
    uint64_t replace_addr, uint64_t replace_size);
void nvgsp_vmm_abort_sparse_region(struct nvgsp_vmm *vmm,
    struct nvgsp_vmm_sparse_region *region);

/*
 * Build and consume sparse-unmap plans.  prepare returns an owned plan;
 * commit updates hardware/tracker state, and fini always releases the plan.
 */
int nvgsp_vmm_prepare_unmap_sparse_range_page(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size, uint8_t clear_page_shift,
    int preserve_target_pts, struct nvgsp_vmm_sparse_unmap_plan **plan);
int nvgsp_vmm_prepare_overwrite_sparse_range_page(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size, uint8_t clear_page_shift,
    struct nvgsp_vmm_sparse_unmap_plan **plan);
int nvgsp_vmm_prepare_metadata_sparse_range(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size,
    struct nvgsp_vmm_sparse_unmap_plan **plan);
int nvgsp_vmm_commit_unmap_sparse_range_noflush(struct nvgsp_vmm *vmm,
    struct nvgsp_vmm_sparse_unmap_plan *plan);
int nvgsp_vmm_unmap_sparse_range_noflush(struct nvgsp_vmm *vmm,
    uint64_t va, uint64_t size);
int nvgsp_vmm_commit_unmap_sparse_range_conflict_noflush(
    struct nvgsp_vmm *vmm, struct nvgsp_vmm_sparse_unmap_plan *plan);
int nvgsp_vmm_sparse_unmap_plan_wrote_hw(
    const struct nvgsp_vmm_sparse_unmap_plan *plan);
void nvgsp_vmm_sparse_unmap_plan_for_each_clear(
    const struct nvgsp_vmm_sparse_unmap_plan *plan,
    nvgsp_vmm_sparse_unmap_clear_fn fn, void *arg);
int nvgsp_vmm_check_unmap_sparse_range_prepared(struct nvgsp_vmm *vmm,
    const struct nvgsp_vmm_sparse_unmap_plan *plan, uint64_t va,
    uint64_t size, uint8_t page_shift);
void nvgsp_vmm_fini_unmap_sparse_range(struct nvgsp_vmm *vmm,
    struct nvgsp_vmm_sparse_unmap_plan *plan);

/* Publish pending PTE writes globally or only for the supplied dirty ranges. */
void nvgsp_vmm_flush(struct nvgsp_vmm *vmm);
void nvgsp_vmm_flush_dirty(struct nvgsp_vmm *vmm,
    const struct nvgsp_vmm_dirty_set *dirty);
/* Create a per-process user VMM.  out receives owned storage destroyed by destroy_user. */
int nvgsp_vmm_create_user(struct nvgpu_device *gpu, uint32_t client_handle,
    struct nvgsp_vmm **out);
/* Destroy a per-process user VMM after all channels using it are gone. */
void nvgsp_vmm_destroy_user(struct nvgsp_vmm *vmm);

#endif /* _NVGSP_VMM_H_ */
