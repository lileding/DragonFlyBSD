/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM VMM: bundles a per-VA-space NV01_ROOT client + NV01_DEVICE_0
 * + NV20_SUBDEVICE_0 + FERMI_VASPACE_A together with the three host
 * sysmem PT pages (PD3, PD2, PD1) for the 512 MiB server-managed
 * window at GPU VA 0x100000000.
 *
 * Mirrors Linux nouveau's per-vmm client+device pattern (see
 * refs/nouveau_r570_gmmu.md §3-4 for the full picture).
 */
#ifndef _NVKM_GSP_VMM_H_
#define _NVKM_GSP_VMM_H_

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"

struct nvkm_bo;
struct nvkm_gsp_vmm_sparse_unmap_plan;

/*
 * nvkm_gsp_vmm_sparse_unmap_clear_fn
 *
 * Ownership:
 *   Borrows one prepared sparse-clear range.  The callback must not retain
 *   pointers into the VMM sparse-unmap plan.
 *
 * Lifetime:
 *   The scalar va, size, and page_shift values are valid only for the callback
 *   invocation.  They describe prepared clear ranges that remain owned by the
 *   sparse-unmap plan.
 *
 * Threading:
 *   Called while the caller owns the sparse-unmap plan.  The callback must not
 *   mutate the plan or take VMM ownership from it.
 */
typedef void (*nvkm_gsp_vmm_sparse_unmap_clear_fn)(void *arg,
	    uint64_t va, uint64_t size, uint8_t page_shift);

struct nvkm_gsp_vmm_dirty_range {
	uint64_t	start;
	uint64_t	end;
};

/*
 * nvkm_gsp_vmm_dirty_set
 *
 * Ownership:
 *   Borrows a caller-owned array of dirty ranges.  The backend does not retain
 *   the array or any VMM/remap ownership from it.
 *
 * Lifetime:
 *   The range array must remain valid for the duration of
 *   nvkm_gsp_vmm_flush_dirty().  Ranges are scalar [start,end) GPU VA facts
 *   for PTE/PDE writes already performed by the enclosing commit.
 *
 * Threading:
 *   The caller owns the higher-level remap serialization.  The backend takes
 *   vmm->tok while flushing BAR1 writes and issuing the hardware invalidate.
 */
struct nvkm_gsp_vmm_dirty_set {
	const struct nvkm_gsp_vmm_dirty_range *ranges;
	uint32_t	range_count;
	uint64_t	page_count;
	int		overflow;
};

/* Per-vmm GMMU page-table chain. Three sysmem 4 KiB pages, one each
 * for PD3 / PD2 / PD1; the 512 MiB RM-managed range falls inside
 * PD1 entry 8 (VA 0x100000000 / 512MiB = 8). */
struct nvkm_gsp_vmm_pt {
	struct nvkm_bar1_page	page;  /* VRAM page mapped into BAR1 */
};

struct nvkm_gsp_vmm_pd1 {
	LIST_ENTRY(nvkm_gsp_vmm_pd1) link;
	uint32_t		pd2_idx;
	struct nvkm_bar1_page	page;
};
LIST_HEAD(nvkm_gsp_vmm_pd1_list, nvkm_gsp_vmm_pd1);

enum nvkm_gsp_vmm_pd0_slot_state {
	NVKM_GSP_VMM_PD0_SLOT_EMPTY = 0,
	NVKM_GSP_VMM_PD0_SLOT_CHILD,
	NVKM_GSP_VMM_PD0_SLOT_VALID_2M,
	NVKM_GSP_VMM_PD0_SLOT_SPARSE_2M,
};

struct nvkm_gsp_vmm_pd0 {
	LIST_ENTRY(nvkm_gsp_vmm_pd0) link;
	LIST_ENTRY(nvkm_gsp_vmm_pd0) lookup_link;
	struct nvkm_bar1_page	*pd1_page;
	uint32_t		pd2_idx;
	uint32_t		pd1_idx;
	uint32_t		refcount;
	uint32_t		valid_2m_count;
	uint32_t		sparse_2m_count;
	uint8_t			slot_state[NVKM_GMMU_PD0_ENTRIES];
	struct nvkm_bar1_page	page;
};
LIST_HEAD(nvkm_gsp_vmm_pd0_list, nvkm_gsp_vmm_pd0);
LIST_HEAD(nvkm_gsp_vmm_pd0_lookup_list, nvkm_gsp_vmm_pd0);

#define NVKM_GSP_VMM_SPT_MASK_WORDS	(NVKM_GMMU_SPT_ENTRIES / 64)

struct nvkm_gsp_vmm_user_pt {
	LIST_ENTRY(nvkm_gsp_vmm_user_pt) link;
	LIST_ENTRY(nvkm_gsp_vmm_user_pt) lookup_link;
	struct nvkm_gsp_vmm_pd0 *pd0;
	uint32_t		pd2_idx;
	uint32_t		pd1_idx;
	uint32_t		pd0_idx;
	uint32_t		valid_pte_count;
	uint32_t		valid_lpte_count;
	uint32_t		sparse_pte_count;
	uint32_t		sparse_lpte_count;
	uint64_t		valid_spt_mask[NVKM_GSP_VMM_SPT_MASK_WORDS];
	uint64_t		sparse_spt_mask[NVKM_GSP_VMM_SPT_MASK_WORDS];
	uint32_t		valid_lpt_mask;
	uint32_t		sparse_lpt_mask;
	struct nvkm_bar1_page	lpt;
	struct nvkm_bar1_page	spt;
};
LIST_HEAD(nvkm_gsp_vmm_user_pt_list, nvkm_gsp_vmm_user_pt);
LIST_HEAD(nvkm_gsp_vmm_user_pt_lookup_list, nvkm_gsp_vmm_user_pt);

#define NVKM_GSP_VMM_PD0_HASH_BITS	8
#define NVKM_GSP_VMM_PD0_HASH_SIZE	(1U << NVKM_GSP_VMM_PD0_HASH_BITS)
#define NVKM_GSP_VMM_USER_PT_HASH_BITS	10
#define NVKM_GSP_VMM_USER_PT_HASH_SIZE	(1U << NVKM_GSP_VMM_USER_PT_HASH_BITS)

struct nvkm_gsp_vmm_sparse_region {
	LIST_ENTRY(nvkm_gsp_vmm_sparse_region) link;
	uint64_t		addr;
	uint64_t		size;
	uint8_t			page_shift;
};
LIST_HEAD(nvkm_gsp_vmm_sparse_region_list, nvkm_gsp_vmm_sparse_region);

struct nvkm_gsp_vmm {
	struct nvkm_softc	*sc;
	struct lwkt_token	 tok;	/* protects PT writes + VA alloc */

	struct nvkm_gsp_client	 client;	/* NV01_ROOT */
	struct nvkm_gsp_device	 device;	/* NV01_DEVICE_0 + subdevice */
	struct nvkm_gsp_object	 vaspace;	/* FERMI_VASPACE_A */

	/* PT levels: [0]=PD3 (root), [1]=PD2, [2]=PD1.
	 * PD3[0] -> PD2_paddr; PD2[0] -> PD1_paddr; PD1[8] empty (GSP fills). */
	struct nvkm_gsp_vmm_pt	 pt[3];

	/* The server-managed window: VA range [rm_va_base, rm_va_base+rm_va_size). */
	uint64_t		 rm_va_base;
	uint64_t		 rm_va_size;

	struct nvkm_gsp_vmm_pd1_list user_pd1_pages;
	struct nvkm_gsp_vmm_pd0_list user_pd0_pages;
	struct nvkm_gsp_vmm_user_pt_list user_pt_pages;
	struct nvkm_gsp_vmm_pd0_lookup_list
	    user_pd0_lookup[NVKM_GSP_VMM_PD0_HASH_SIZE];
	struct nvkm_gsp_vmm_user_pt_lookup_list
	    user_pt_lookup[NVKM_GSP_VMM_USER_PT_HASH_SIZE];
	struct nvkm_gsp_vmm_sparse_region_list sparse_regions;
	struct nvkm_dmamem sparse_page;

	/* Next free submit GVA slot inside this VMM's CLIENT_BASE window.
	 * Per-VMM (not global): different VMMs are separate address spaces,
	 * so slot 0 is reusable across them and the counter cannot overflow
	 * the global submit window. */
	uint32_t		submit_gva_slot;
};

struct nvkm_gsp_vmm_pte_info {
	uint64_t	va;
	uint64_t	pte;
	uint64_t	lpte;
	uint32_t	pd2_idx;
	uint32_t	pd1_idx;
	uint32_t	pd0_idx;
	uint32_t	lpt_idx;
	uint32_t	spt_idx;
	uint8_t		has_pt;
};

/* Construct a complete VMM: allocate client/device/subdevice/vaspace,
 * allocate three host sysmem PT pages, encode PD3[0] and PD2[0], then
 * issue NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES.
 *
 * On success the vmm is fully usable as a parent for channel allocs. */
int	 nvkm_gsp_vmm_ctor(struct nvkm_softc *sc, uint32_t client_handle,
	    struct nvkm_gsp_vmm *vmm);

/* Tear down. Frees PT pages + RM resources. */
void	 nvkm_gsp_vmm_dtor(struct nvkm_gsp_vmm *vmm);

int	 nvkm_gsp_vmm_map_sysmem(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    vm_paddr_t paddr, uint64_t size);
int	 nvkm_gsp_vmm_map_sysmem_kva(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    void *kva, uint64_t size, uint8_t kind);
int	 nvkm_gsp_vmm_map_vram(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t paddr, uint64_t size);
int	 nvkm_gsp_vmm_map_vram_flags(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t paddr, uint64_t size, uint8_t priv, uint8_t ro,
	    uint8_t kind);
int	 nvkm_gsp_vmm_unmap(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t size);
int	 nvkm_gsp_vmm_map_sparse(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t size);
int	 nvkm_gsp_vmm_unmap_sparse(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t size);
int	 nvkm_gsp_vmm_has_sparse_region(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size);
int	 nvkm_gsp_vmm_prepare_sparse_region(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size,
	    struct nvkm_gsp_vmm_sparse_region **pregion);
int	 nvkm_gsp_vmm_prepare_sparse_region_page(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size, uint8_t page_shift,
	    struct nvkm_gsp_vmm_sparse_region **pregion);
int	 nvkm_gsp_vmm_alloc_sparse_region(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size,
	    struct nvkm_gsp_vmm_sparse_region **pregion);
int	 nvkm_gsp_vmm_alloc_sparse_region_page(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size, uint8_t page_shift,
	    struct nvkm_gsp_vmm_sparse_region **pregion);
int	 nvkm_gsp_vmm_commit_sparse_noflush(struct nvkm_gsp_vmm *vmm,
	    struct nvkm_gsp_vmm_sparse_region *region);
int	 nvkm_gsp_vmm_check_sparse_regions_commit(struct nvkm_gsp_vmm *vmm,
	    struct nvkm_gsp_vmm_sparse_region **regions,
	    uint32_t region_count, uint64_t replace_addr,
	    uint64_t replace_size);
void	 nvkm_gsp_vmm_abort_sparse_region(struct nvkm_gsp_vmm *vmm,
	    struct nvkm_gsp_vmm_sparse_region *region);

/* Publish pending PT writes + invalidate the VMM TLB.  Caller need not
 * hold vmm->tok.  The *_noflush variants below write PTEs without it so a
 * batch (VM_BIND) flushes once via this helper. */
void	 nvkm_gsp_vmm_flush(struct nvkm_gsp_vmm *vmm);
/*
 * nvkm_gsp_vmm_flush_dirty
 *
 * Ownership:
 *   Borrows vmm and a caller-owned dirty set.  Dirty ranges are consumed only
 *   as scalar invalidate inputs and are not retained.
 *
 * Lifetime:
 *   The dirty set must remain alive for the call.  On return, backend PTE/PDE
 *   writes covered by the dirty set have passed the required visibility and
 *   hardware invalidate boundary.
 *
 * Threading:
 *   Caller owns higher-level remap/job ordering.  The backend takes vmm->tok
 *   internally while flushing BAR1 writes and issuing the TU102 invalidate.
 */
void	 nvkm_gsp_vmm_flush_dirty(struct nvkm_gsp_vmm *vmm,
	    const struct nvkm_gsp_vmm_dirty_set *dirty);
int	 nvkm_gsp_vmm_ensure_pt_range(struct nvkm_gsp_vmm *vmm, uint64_t va,
		    uint64_t size);
int	 nvkm_gsp_vmm_ensure_pd0_range(struct nvkm_gsp_vmm *vmm, uint64_t va,
		    uint64_t size);
int	 nvkm_gsp_vmm_check_prepared_pt_range(struct nvkm_gsp_vmm *vmm,
		    uint64_t va, uint64_t size, uint8_t page_shift);
int	 nvkm_gsp_vmm_map_sysmem_noflush(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    vm_paddr_t paddr, uint64_t size);
int	 nvkm_gsp_vmm_map_sysmem_kva_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, void *kva, uint64_t size, uint8_t kind);
int	 nvkm_gsp_vmm_map_sysmem_bo_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, const struct nvkm_bo *bo, uint64_t bo_offset,
	    uint64_t size, uint8_t kind);
int	 nvkm_gsp_vmm_map_sysmem_bo_prepared_noflush(
		    struct nvkm_gsp_vmm *vmm, uint64_t va, const struct nvkm_bo *bo,
		    uint64_t bo_offset, uint64_t size, uint8_t kind);
int	 nvkm_gsp_vmm_map_sysmem_paddrs_prepared_noflush(
		    struct nvkm_gsp_vmm *vmm, uint64_t va,
		    const vm_paddr_t *paddrs, uint32_t page_count, uint8_t kind);
int	 nvkm_gsp_vmm_map_sysmem_paddrs_page_prepared_noflush(
		    struct nvkm_gsp_vmm *vmm, uint64_t va,
		    const vm_paddr_t *paddrs, uint32_t page_count,
		    uint8_t kind, uint8_t page_shift);
int	 nvkm_gsp_vmm_map_vram_flags_noflush(struct nvkm_gsp_vmm *vmm,
		    uint64_t va, uint64_t paddr, uint64_t size, uint8_t priv,
		    uint8_t ro, uint8_t kind);
int	 nvkm_gsp_vmm_map_vram_flags_page_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t paddr, uint64_t size, uint8_t priv,
	    uint8_t ro, uint8_t kind, uint8_t page_shift);
int	 nvkm_gsp_vmm_map_vram_flags_page_prepared_noflush(
	    struct nvkm_gsp_vmm *vmm, uint64_t va, uint64_t paddr,
	    uint64_t size, uint8_t priv, uint8_t ro, uint8_t kind,
	    uint8_t page_shift);
int	 nvkm_gsp_vmm_promote_vram_64k_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t paddr, uint64_t size, uint8_t priv,
	    uint8_t ro, uint8_t kind);
int	 nvkm_gsp_vmm_promote_vram_2m_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t paddr, uint64_t size, uint8_t priv,
	    uint8_t ro, uint8_t kind);
int	 nvkm_gsp_vmm_promote_sysmem_2m_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, const vm_paddr_t *paddrs, uint32_t page_count,
	    uint8_t kind);
int	 nvkm_gsp_vmm_prepare_split_vram_2m(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, struct nvkm_gsp_vmm_user_pt **ppt);
int	 nvkm_gsp_vmm_check_split_vram_2m(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, struct nvkm_gsp_vmm_user_pt *pt);
int	 nvkm_gsp_vmm_commit_split_vram_2m_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, struct nvkm_gsp_vmm_user_pt *pt);
void	 nvkm_gsp_vmm_abort_split_vram_2m(struct nvkm_gsp_vmm *vmm,
	    struct nvkm_gsp_vmm_user_pt *pt);
int	 nvkm_gsp_vmm_unmap_noflush(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t size);
int	 nvkm_gsp_vmm_check_unmap_valid_range(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size, int preserve_target_pts);
int	 nvkm_gsp_vmm_check_unmap_valid_range_page(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size, int preserve_target_pts,
	    uint8_t preserve_page_shift);
int	 nvkm_gsp_vmm_unmap_valid_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size);
int	 nvkm_gsp_vmm_unmap_valid_preserve_page_noflush(
	    struct nvkm_gsp_vmm *vmm, uint64_t va, uint64_t size,
	    uint8_t preserve_page_shift);
int	 nvkm_gsp_vmm_unmap_valid_preserve_pt_noflush(
	    struct nvkm_gsp_vmm *vmm, uint64_t va, uint64_t size);
int	 nvkm_gsp_vmm_check_clear_pd0_target_range(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size);
int	 nvkm_gsp_vmm_check_clear_pd0_target_range_allow_sparse(
	    struct nvkm_gsp_vmm *vmm, uint64_t va, uint64_t size);
int	 nvkm_gsp_vmm_clear_pd0_target_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size);
int	 nvkm_gsp_vmm_map_sparse_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size);
int	 nvkm_gsp_vmm_unmap_sparse_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size);
int	 nvkm_gsp_vmm_prepare_unmap_sparse_range(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size,
	    struct nvkm_gsp_vmm_sparse_unmap_plan **pplan);
int	 nvkm_gsp_vmm_prepare_unmap_sparse_range_page(
	    struct nvkm_gsp_vmm *vmm, uint64_t va, uint64_t size,
	    uint8_t clear_page_shift, int preserve_target_pts,
	    struct nvkm_gsp_vmm_sparse_unmap_plan **pplan);
int	 nvkm_gsp_vmm_commit_unmap_sparse_range_noflush(
	    struct nvkm_gsp_vmm *vmm,
	    struct nvkm_gsp_vmm_sparse_unmap_plan *plan);
/*
 * nvkm_gsp_vmm_sparse_unmap_plan_for_each_clear
 *
 * Ownership:
 *   Borrows the prepared sparse-unmap plan and calls fn for each clear range.
 *   The function does not consume or publish any plan ownership.
 *
 * Lifetime:
 *   The caller must invoke this before fini releases the plan.  It is safe both
 *   before and after commit because clear-range descriptors stay linked until
 *   fini.
 *
 * Threading:
 *   Does not take vmm->tok and does not mutate VMM state.  The caller must
 *   provide the same external serialization that protects the plan lifetime.
 */
void	 nvkm_gsp_vmm_sparse_unmap_plan_for_each_clear(
	    const struct nvkm_gsp_vmm_sparse_unmap_plan *plan,
	    nvkm_gsp_vmm_sparse_unmap_clear_fn fn, void *arg);
int	 nvkm_gsp_vmm_check_unmap_sparse_range_prepared(
	    struct nvkm_gsp_vmm *vmm,
	    const struct nvkm_gsp_vmm_sparse_unmap_plan *plan,
	    uint64_t va, uint64_t size, uint8_t page_shift);
void	 nvkm_gsp_vmm_fini_unmap_sparse_range(struct nvkm_gsp_vmm *vmm,
	    struct nvkm_gsp_vmm_sparse_unmap_plan *plan);
int	 nvkm_gsp_vmm_unmap_sparse_range_noflush(struct nvkm_gsp_vmm *vmm,
	    uint64_t va, uint64_t size);
void	 nvkm_gsp_vmm_read_pte(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    struct nvkm_gsp_vmm_pte_info *info);
void	 nvkm_gsp_vmm_debug_dump_pte(struct nvkm_gsp_vmm *vmm, uint64_t va);

#endif /* _NVKM_GSP_VMM_H_ */
