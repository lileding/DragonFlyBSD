/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM VMM constructor: per-VA-space client/device tree + PT pages
 * + COPY_SERVER_RESERVED_PDES. See refs/nouveau_r570_gmmu.md.
 *
 * The chain reproduced here matches Linux nouveau r535_mmu_vaspace_new
 * with external=false. Page sizes assume Turing's 5-level GMMU with
 * a 512 MiB-per-PD1-entry layout.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/pmap.h>

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include "nvkm_gsp_vmm.h"

static MALLOC_DEFINE(M_NVKM_VMM, "nvkm_vmm", "nvkm GMMU page table pages");

/* The fixed split — both endpoints are documented in
 * Linux nouveau rm/r535/nvrm/vmm.h:
 *   SPLIT_VAS_SERVER_RM_MANAGED_VA_START  0x100000000  (4 GiB)
 *   SPLIT_VAS_SERVER_RM_MANAGED_VA_SIZE       0x20000000 (512 MiB)
 */
/* deprecated: replaced by NVKM_VMM_RM_BASE in nvkm_priv.h */
/* deprecated: replaced by NVKM_VMM_RM_SIZE in nvkm_priv.h */

/* gp100 PDE encoding:
 *   bits [2:1] aperture (1=VRAM, 2=SYS_COH, 3=SYS_NCOH)
 *   bit  [3]   VOL (set for SYS_COH)
 *   bits [39:4] (paddr >> 4)
 *   Non-zero aperture = PDE valid.
 */
#define NVKM_PDE_APERTURE_SYS_COH	(2ULL << 1)
#define NVKM_PDE_VOL			(1ULL << 3)


/* NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES — params from
 * Linux nouveau rm/r535/nvrm/vmm.h. Up to GMMU_FMT_MAX_LEVELS=6
 * level entries. We use 3 (PD3, PD2, PD1). */
#define NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES	0x90f10106U
/* aperture values for NV90F1_CTRL_..._PDES levels[i].aperture
 * (RPC enum, NOT the PDE-encoding bits). r535/nvrm/vmm.h: same as
 * GMMU_APERTURE enum values without the shift. */
#define NV_COPY_PDE_APERTURE_INVALID	0
#define NV_COPY_PDE_APERTURE_VIDMEM	1
#define NV_COPY_PDE_APERTURE_SYS_COH	2
#define NV_COPY_PDE_APERTURE_SYS_NCOH	3

struct NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_LEVEL {
	uint64_t physAddress;
	uint64_t size;		/* bytes occupied at this level */
	uint32_t aperture;
	uint32_t pageShift;
};
struct NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS {
	uint32_t hSubDevice;	/* 0 -> use subDeviceId */
	uint32_t subDeviceId;
	uint64_t pageSize;
	uint64_t virtAddrLo;
	uint64_t virtAddrHi;
	uint32_t numLevelsToCopy;
	uint8_t  _pad[4];
	struct NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_LEVEL levels[6];
};

static int
nvkm_gsp_vmm_pt_alloc(struct nvkm_softc *sc, struct nvkm_gsp_vmm_pt *pt)
{
	return (nvkm_gsp_bar1_alloc_page_kind(sc, &pt->page,
	    NVKM_VRAM_VMM_PT, pt));
}

static void
nvkm_gsp_vmm_pt_free(struct nvkm_softc *sc, struct nvkm_gsp_vmm_pt *pt)
{
	nvkm_gsp_bar1_free_page(sc, &pt->page);
}

static void
nvkm_gsp_vmm_zero_bar1_page(struct nvkm_softc *sc, const struct nvkm_bar1_page *p)
{
	for (uint32_t off = 0; off < NVKM_GMMU_PT_PAGE_SIZE; off += 4)
		nvkm_gsp_bar1_wr32(sc, p->bar1_gva + off, 0);
	nvkm_gsp_bar1_flush(sc);
}

static void
nvkm_gsp_vmm_sparse_bar1_page(struct nvkm_softc *sc,
    const struct nvkm_bar1_page *p, uint64_t pte)
{
	for (uint32_t off = 0; off < NVKM_GMMU_PT_PAGE_SIZE; off += 8)
		nvkm_gsp_bar1_wr64(sc, p->bar1_gva + off, pte);
	nvkm_gsp_bar1_flush(sc);
}

static void
nvkm_gsp_vmm_invalidate(struct nvkm_gsp_vmm *vmm)
{
	struct nvkm_softc *sc = vmm->sc;
	uint64_t pdb = vmm->pt[0].page.vram_paddr;
	uint32_t trig_rb = 0xffffffffu;

	/* Match nouveau tu102_vmm_flush(): target this VMM root PDB and
	 * invalidate all pages. BAR1/BAR2 have their own ALL_PDB path. */
	nvkm_wr32(sc, 0xb830a0, (uint32_t)(pdb >> 8));
	nvkm_wr32(sc, 0xb830a4, 0x00000000u);
	nvkm_wr32(sc, 0xb830b0, 0x80000000u | 0x00000001u);
	for (int spin = 0; spin < 200000; spin++) {
		trig_rb = nvkm_rd32(sc, 0xb830b0);
		if ((trig_rb & 0x80000000u) == 0)
			break;
		DELAY(10);
	}
	if ((trig_rb & 0x80000000u) != 0) {
		device_printf(sc->dev,
		    "gsp_vmm: invalidate timeout pdb=0x%llx trig=0x%08x\n",
		    (unsigned long long)pdb, trig_rb);
	}
}

static struct nvkm_gsp_vmm_pd0 *
nvkm_gsp_vmm_pd0_find(struct nvkm_gsp_vmm *vmm, uint32_t pd1_idx)
{
	struct nvkm_gsp_vmm_pd0 *pd0;

	LIST_FOREACH(pd0, &vmm->user_pd0_pages, link) {
		if (pd0->pd1_idx == pd1_idx)
			return (pd0);
	}
	return (NULL);
}

static int
nvkm_gsp_vmm_pd0_get(struct nvkm_gsp_vmm *vmm, uint32_t pd1_idx,
    struct nvkm_gsp_vmm_pd0 **ppd0)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_vmm_pd0 *pd0;
	int err;

	pd0 = nvkm_gsp_vmm_pd0_find(vmm, pd1_idx);
	if (pd0 != NULL) {
		nvkm_gsp_bar1_wr64(sc, vmm->pt[2].page.bar1_gva + pd1_idx * 8,
		    nvkm_pde_to_vram(pd0->page.vram_paddr));
		nvkm_gsp_bar1_flush(sc);
		*ppd0 = pd0;
		return (0);
	}

	pd0 = kmalloc(sizeof(*pd0), M_NVKM_VMM, M_WAITOK | M_ZERO);
	pd0->pd1_idx = pd1_idx;
	err = nvkm_gsp_bar1_alloc_page_kind(sc, &pd0->page,
	    NVKM_VRAM_VMM_PT, pd0);
	if (err != 0) {
		kfree(pd0, M_NVKM_VMM);
		return (err);
	}

	nvkm_gsp_vmm_zero_bar1_page(sc, &pd0->page);
	nvkm_gsp_bar1_wr64(sc, vmm->pt[2].page.bar1_gva + pd1_idx * 8,
	    nvkm_pde_to_vram(pd0->page.vram_paddr));
	nvkm_gsp_bar1_flush(sc);
	LIST_INSERT_HEAD(&vmm->user_pd0_pages, pd0, link);
	*ppd0 = pd0;
	return (0);
}

static struct nvkm_gsp_vmm_user_pt *
nvkm_gsp_vmm_user_pt_find(struct nvkm_gsp_vmm *vmm, uint32_t pd1_idx,
    uint32_t pd0_idx)
{
	struct nvkm_gsp_vmm_user_pt *pt;

	LIST_FOREACH(pt, &vmm->user_pt_pages, link) {
		if (pt->pd1_idx == pd1_idx && pt->pd0_idx == pd0_idx)
			return (pt);
	}
	return (NULL);
}

static int
nvkm_gsp_vmm_user_pt_get(struct nvkm_gsp_vmm *vmm, uint64_t va,
    struct nvkm_gsp_vmm_user_pt **ppt)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_vmm_user_pt *pt;
	struct nvkm_gsp_vmm_pd0 *pd0;
	uint32_t pd1_idx, pd0_idx;
	int err;

	pd1_idx = (uint32_t)((va >> NVKM_GMMU_PD1_SHIFT) &
	    (NVKM_GMMU_PD1_ENTRIES - 1));
	pd0_idx = (uint32_t)((va >> NVKM_GMMU_PD0_SHIFT) &
	    (NVKM_GMMU_PD0_ENTRIES - 1));

	pt = nvkm_gsp_vmm_user_pt_find(vmm, pd1_idx, pd0_idx);
	if (pt != NULL) {
		nvkm_gsp_bar1_wr64(sc,
		    vmm->pt[2].page.bar1_gva + pd1_idx * 8,
		    nvkm_pde_to_vram(pt->pd0->page.vram_paddr));
		nvkm_gsp_bar1_wr64(sc,
		    pt->pd0->page.bar1_gva + (pd0_idx * 2 + 0) * 8,
		    nvkm_pde_to_vram(pt->lpt.vram_paddr));
		nvkm_gsp_bar1_wr64(sc,
		    pt->pd0->page.bar1_gva + (pd0_idx * 2 + 1) * 8,
		    nvkm_pde_to_vram(pt->spt.vram_paddr));
		nvkm_gsp_bar1_flush(sc);
		*ppt = pt;
		return (0);
	}

	err = nvkm_gsp_vmm_pd0_get(vmm, pd1_idx, &pd0);
	if (err != 0)
		return (err);

	pt = kmalloc(sizeof(*pt), M_NVKM_VMM, M_WAITOK | M_ZERO);
	pt->pd0 = pd0;
	pt->pd1_idx = pd1_idx;
	pt->pd0_idx = pd0_idx;
	pt->valid_pte_count = 0;

	err = nvkm_gsp_bar1_alloc_page_kind(sc, &pt->lpt,
	    NVKM_VRAM_VMM_PT, pt);
	if (err != 0)
		goto fail;
	err = nvkm_gsp_bar1_alloc_page_kind(sc, &pt->spt,
	    NVKM_VRAM_VMM_PT, pt);
	if (err != 0)
		goto fail;

	nvkm_gsp_vmm_zero_bar1_page(sc, &pt->lpt);
	nvkm_gsp_vmm_sparse_bar1_page(sc, &pt->spt,
	    nvkm_pte_to_sysmem(vmm->sparse_page.paddr));

	/* PD1 owns one shared PD0 page. Each PD0 entry gets its own dual
	 * BIG/SMALL PDE pair, so mappings in the same 512 MiB range do not
	 * overwrite earlier PD0 entries. */
	nvkm_gsp_bar1_wr64(sc, pd0->page.bar1_gva + (pd0_idx * 2 + 0) * 8,
	    nvkm_pde_to_vram(pt->lpt.vram_paddr));
	nvkm_gsp_bar1_wr64(sc, pd0->page.bar1_gva + (pd0_idx * 2 + 1) * 8,
	    nvkm_pde_to_vram(pt->spt.vram_paddr));
	nvkm_gsp_bar1_flush(sc);
	pd0->refcount++;

	LIST_INSERT_HEAD(&vmm->user_pt_pages, pt, link);
	*ppt = pt;
	return (0);

fail:
	if (pt->spt.vram_paddr != 0)
		nvkm_gsp_bar1_free_page(sc, &pt->spt);
	if (pt->lpt.vram_paddr != 0)
		nvkm_gsp_bar1_free_page(sc, &pt->lpt);
	kfree(pt, M_NVKM_VMM);
	return (err);
}

static int
nvkm_gsp_vmm_write_pte(struct nvkm_gsp_vmm *vmm, uint64_t va, uint64_t pte)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_vmm_user_pt *pt;
	uint32_t spt_idx;
	uint64_t old_pte, sparse_pte;
	int err;

	err = nvkm_gsp_vmm_user_pt_get(vmm, va, &pt);
	if (err != 0)
		return (err);

	spt_idx = (uint32_t)((va >> NVKM_GMMU_SPT_SHIFT) &
	    (NVKM_GMMU_SPT_ENTRIES - 1));
	old_pte = nvkm_gsp_bar1_rd64(sc, pt->spt.bar1_gva + spt_idx * 8);
	nvkm_gsp_bar1_wr64(sc, pt->spt.bar1_gva + spt_idx * 8, pte);
	sparse_pte = nvkm_pte_to_sysmem(vmm->sparse_page.paddr);
	if ((old_pte == 0 || old_pte == sparse_pte) &&
	    pte != 0 && pte != sparse_pte) {
		pt->valid_pte_count++;
	} else if (old_pte != 0 && old_pte != sparse_pte &&
	    (pte == 0 || pte == sparse_pte) && pt->valid_pte_count > 0) {
		pt->valid_pte_count--;
	}
	return (0);
}

static void
nvkm_gsp_vmm_reclaim_empty_pt(struct nvkm_gsp_vmm *vmm,
    struct nvkm_gsp_vmm_user_pt *pt)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_vmm_pd0 *pd0 = pt->pd0;

	if (pt->valid_pte_count != 0)
		return;

	nvkm_gsp_bar1_wr64(sc,
	    pd0->page.bar1_gva + (pt->pd0_idx * 2 + 0) * 8, 0);
	nvkm_gsp_bar1_wr64(sc,
	    pd0->page.bar1_gva + (pt->pd0_idx * 2 + 1) * 8, 0);
	LIST_REMOVE(pt, link);
	nvkm_gsp_bar1_free_page(sc, &pt->spt);
	nvkm_gsp_bar1_free_page(sc, &pt->lpt);
	kfree(pt, M_NVKM_VMM);

	if (pd0->refcount > 0)
		pd0->refcount--;
	if (pd0->refcount != 0)
		return;

	nvkm_gsp_bar1_wr64(sc, vmm->pt[2].page.bar1_gva +
	    pd0->pd1_idx * 8, 0);
	LIST_REMOVE(pd0, link);
	nvkm_gsp_bar1_free_page(sc, &pd0->page);
	kfree(pd0, M_NVKM_VMM);
}

static int
nvkm_gsp_vmm_write_sparse(struct nvkm_gsp_vmm *vmm, uint64_t va,
    uint64_t size)
{
	uint64_t off;
	int err;

	for (off = 0; off < size; off += NVKM_GMMU_PT_PAGE_SIZE) {
		err = nvkm_gsp_vmm_write_pte(vmm, va + off,
		    nvkm_pte_to_sysmem(vmm->sparse_page.paddr));
		if (err != 0)
			return (err);
	}
	return (0);
}

void
nvkm_gsp_vmm_debug_dump_pte(struct nvkm_gsp_vmm *vmm, uint64_t va)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_vmm_user_pt *pt;
	uint32_t pd1_idx, pd0_idx, spt_idx;
	uint64_t pte;

	pd1_idx = (uint32_t)((va >> NVKM_GMMU_PD1_SHIFT) &
	    (NVKM_GMMU_PD1_ENTRIES - 1));
	pd0_idx = (uint32_t)((va >> NVKM_GMMU_PD0_SHIFT) &
	    (NVKM_GMMU_PD0_ENTRIES - 1));
	spt_idx = (uint32_t)((va >> NVKM_GMMU_SPT_SHIFT) &
	    (NVKM_GMMU_SPT_ENTRIES - 1));

	lwkt_gettoken(&vmm->tok);
	pt = nvkm_gsp_vmm_user_pt_find(vmm, pd1_idx, pd0_idx);
	if (pt == NULL) {
		device_printf(sc->dev,
		    "gsp_vmm: pte va=0x%016jx pd1=%u pd0=%u spt=%u missing-pt sparse=0x%016jx\n",
		    (uintmax_t)va, pd1_idx, pd0_idx, spt_idx,
		    (uintmax_t)vmm->sparse_page.paddr);
		lwkt_reltoken(&vmm->tok);
		return;
	}

	pte = (uint64_t)nvkm_gsp_bar1_rd32(sc, pt->spt.bar1_gva + spt_idx * 8);
	pte |= (uint64_t)nvkm_gsp_bar1_rd32(sc,
	    pt->spt.bar1_gva + spt_idx * 8 + 4) << 32;
	device_printf(sc->dev,
	    "gsp_vmm: pte va=0x%016jx pd1=%u pd0=%u spt=%u pte=0x%016jx sparse=0x%016jx sparse_pte=0x%016jx\n",
	    (uintmax_t)va, pd1_idx, pd0_idx, spt_idx, (uintmax_t)pte,
	    (uintmax_t)vmm->sparse_page.paddr,
	    (uintmax_t)nvkm_pte_to_sysmem(vmm->sparse_page.paddr));
	lwkt_reltoken(&vmm->tok);
}

int
nvkm_gsp_vmm_map_sysmem(struct nvkm_gsp_vmm *vmm, uint64_t va,
    vm_paddr_t paddr, uint64_t size)
{
	struct nvkm_softc *sc = vmm->sc;
	uint64_t off;
	int err;

	if ((va | paddr | size) & (NVKM_GMMU_PT_PAGE_SIZE - 1))
		return (EINVAL);

	lwkt_gettoken(&vmm->tok);
	for (off = 0; off < size; off += NVKM_GMMU_PT_PAGE_SIZE) {
		uint64_t gva = va + off;

		err = nvkm_gsp_vmm_write_pte(vmm, gva,
		    nvkm_pte_to_sysmem((uint64_t)paddr + off));
		if (err != 0) {
			lwkt_reltoken(&vmm->tok);
			return (err);
		}
	}
	nvkm_gsp_bar1_flush(sc);
	nvkm_gsp_vmm_invalidate(vmm);
	lwkt_reltoken(&vmm->tok);
	return (0);
}

int
nvkm_gsp_vmm_map_sysmem_kva(struct nvkm_gsp_vmm *vmm, uint64_t va,
    void *kva, uint64_t size)
{
	struct nvkm_softc *sc = vmm->sc;
	uint64_t off;
	int err;

	if (((uintptr_t)kva | va | size) & (NVKM_GMMU_PT_PAGE_SIZE - 1))
		return (EINVAL);

	lwkt_gettoken(&vmm->tok);
	for (off = 0; off < size; off += NVKM_GMMU_PT_PAGE_SIZE) {
		uint64_t gva = va + off;
		vm_paddr_t paddr = vtophys((uint8_t *)kva + off);

		err = nvkm_gsp_vmm_write_pte(vmm, gva,
		    nvkm_pte_to_sysmem((uint64_t)paddr));
		if (err != 0) {
			lwkt_reltoken(&vmm->tok);
			return (err);
		}
	}
	nvkm_gsp_bar1_flush(sc);
	nvkm_gsp_vmm_invalidate(vmm);
	lwkt_reltoken(&vmm->tok);
	return (0);
}

int
nvkm_gsp_vmm_map_vram_flags(struct nvkm_gsp_vmm *vmm, uint64_t va,
    uint64_t paddr, uint64_t size, uint8_t priv, uint8_t ro)
{
	struct nvkm_softc *sc = vmm->sc;
	uint64_t off;
	int err;

	if ((va | paddr | size) & (NVKM_GMMU_PT_PAGE_SIZE - 1))
		return (EINVAL);

	lwkt_gettoken(&vmm->tok);
	for (off = 0; off < size; off += NVKM_GMMU_PT_PAGE_SIZE) {
		uint64_t gva = va + off;

		err = nvkm_gsp_vmm_write_pte(vmm, gva,
		    nvkm_pte_to_vram_flags(paddr + off, priv, ro));
		if (err != 0) {
			lwkt_reltoken(&vmm->tok);
			return (err);
		}
	}
	nvkm_gsp_bar1_flush(sc);
	nvkm_gsp_vmm_invalidate(vmm);
	lwkt_reltoken(&vmm->tok);
	return (0);
}

int
nvkm_gsp_vmm_map_vram(struct nvkm_gsp_vmm *vmm, uint64_t va,
    uint64_t paddr, uint64_t size)
{
	return (nvkm_gsp_vmm_map_vram_flags(vmm, va, paddr, size, 0, 0));
}

int
nvkm_gsp_vmm_unmap(struct nvkm_gsp_vmm *vmm, uint64_t va, uint64_t size)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_vmm_user_pt *pt;
	uint64_t off;
	int err;

	if ((va | size) & (NVKM_GMMU_PT_PAGE_SIZE - 1))
		return (EINVAL);

	lwkt_gettoken(&vmm->tok);
	for (off = 0; off < size; off += NVKM_GMMU_PT_PAGE_SIZE) {
		uint64_t gva = va + off;
		uint32_t pd1_idx = (uint32_t)((gva >> NVKM_GMMU_PD1_SHIFT) &
		    (NVKM_GMMU_PD1_ENTRIES - 1));
		uint32_t pd0_idx = (uint32_t)((gva >> NVKM_GMMU_PD0_SHIFT) &
		    (NVKM_GMMU_PD0_ENTRIES - 1));

		pt = nvkm_gsp_vmm_user_pt_find(vmm, pd1_idx, pd0_idx);
		if (pt != NULL) {
			err = nvkm_gsp_vmm_write_pte(vmm, gva,
			    nvkm_pte_to_sysmem(vmm->sparse_page.paddr));
			if (err != 0) {
				lwkt_reltoken(&vmm->tok);
				return (err);
			}
			nvkm_gsp_vmm_reclaim_empty_pt(vmm, pt);
		}
	}
	nvkm_gsp_bar1_flush(sc);
	nvkm_gsp_vmm_invalidate(vmm);
	lwkt_reltoken(&vmm->tok);
	return (0);
}

int
nvkm_gsp_vmm_map_sparse(struct nvkm_gsp_vmm *vmm, uint64_t va, uint64_t size)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_vmm_sparse_region *region;
	int err;

	if ((va | size) & (NVKM_GMMU_PT_PAGE_SIZE - 1))
		return (EINVAL);

	region = kmalloc(sizeof(*region), M_NVKM_VMM, M_WAITOK | M_ZERO);
	region->addr = va;
	region->size = size;

	lwkt_gettoken(&vmm->tok);
	LIST_INSERT_HEAD(&vmm->sparse_regions, region, link);
	err = nvkm_gsp_vmm_write_sparse(vmm, va, size);
	if (err != 0) {
		LIST_REMOVE(region, link);
		kfree(region, M_NVKM_VMM);
		lwkt_reltoken(&vmm->tok);
		return (err);
	}
	nvkm_gsp_bar1_flush(sc);
	nvkm_gsp_vmm_invalidate(vmm);
	lwkt_reltoken(&vmm->tok);
	return (0);
}

int
nvkm_gsp_vmm_unmap_sparse(struct nvkm_gsp_vmm *vmm, uint64_t va, uint64_t size)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_vmm_sparse_region *region;
	struct nvkm_gsp_vmm_user_pt *pt;
	uint64_t off;
	int err;

	if ((va | size) & (NVKM_GMMU_PT_PAGE_SIZE - 1))
		return (EINVAL);

	lwkt_gettoken(&vmm->tok);
	LIST_FOREACH(region, &vmm->sparse_regions, link) {
		if (region->addr == va && region->size == size)
			break;
	}
	if (region == NULL) {
		lwkt_reltoken(&vmm->tok);
		return (ENOENT);
	}
	LIST_REMOVE(region, link);
	kfree(region, M_NVKM_VMM);

	for (off = 0; off < size; off += NVKM_GMMU_PT_PAGE_SIZE) {
		uint64_t gva = va + off;
		uint32_t pd1_idx = (uint32_t)((gva >> NVKM_GMMU_PD1_SHIFT) &
		    (NVKM_GMMU_PD1_ENTRIES - 1));
		uint32_t pd0_idx = (uint32_t)((gva >> NVKM_GMMU_PD0_SHIFT) &
		    (NVKM_GMMU_PD0_ENTRIES - 1));

		pt = nvkm_gsp_vmm_user_pt_find(vmm, pd1_idx, pd0_idx);
		if (pt == NULL)
			continue;

		err = nvkm_gsp_vmm_write_pte(vmm, gva, 0);
		if (err != 0) {
			lwkt_reltoken(&vmm->tok);
			return (err);
		}
		nvkm_gsp_vmm_reclaim_empty_pt(vmm, pt);
	}
	nvkm_gsp_bar1_flush(sc);
	nvkm_gsp_vmm_invalidate(vmm);
	lwkt_reltoken(&vmm->tok);
	return (0);
}


void
nvkm_gsp_vmm_snapshot(struct nvkm_gsp_vmm *vmm, uint32_t *pd0_count,
    uint32_t *pt_count, uint64_t *valid_pte_count,
    uint32_t *sparse_region_count)
{
	struct nvkm_gsp_vmm_pd0 *pd0;
	struct nvkm_gsp_vmm_user_pt *pt;
	struct nvkm_gsp_vmm_sparse_region *region;

	*pd0_count = 0;
	*pt_count = 0;
	*valid_pte_count = 0;
	*sparse_region_count = 0;
	if (vmm == NULL)
		return;

	lwkt_gettoken(&vmm->tok);
	LIST_FOREACH(pd0, &vmm->user_pd0_pages, link)
		(*pd0_count)++;
	LIST_FOREACH(pt, &vmm->user_pt_pages, link) {
		(*pt_count)++;
		*valid_pte_count += pt->valid_pte_count;
	}
	LIST_FOREACH(region, &vmm->sparse_regions, link)
		(*sparse_region_count)++;
	lwkt_reltoken(&vmm->tok);
}

static int
nvkm_gsp_vmm_copy_pdes(struct nvkm_gsp_vmm *vmm)
{
	struct nvkm_softc *sc = vmm->sc;
	struct NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS *ctrl;
	void *p;
	int err;

	ctrl = nvkm_gsp_rm_ctrl_get(&vmm->vaspace,
	    NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES,
	    sizeof(*ctrl));
	if (ctrl == NULL)
		return (ENOMEM);

	ctrl->hSubDevice = 0;
	ctrl->subDeviceId = 0;
	ctrl->pageSize = 1ULL << 29;	/* 512 MiB — the leaf for PD1 */
	ctrl->virtAddrLo = vmm->rm_va_base;
	ctrl->virtAddrHi = vmm->rm_va_base + vmm->rm_va_size - 1;
	ctrl->numLevelsToCopy = 3;

	/* PD3 — root. Holds 4 entries of 8 bytes each (2-bit index). */
	ctrl->levels[0].physAddress = (uint64_t)vmm->pt[0].page.vram_paddr;
	ctrl->levels[0].size        = (1ULL << 2) * 8;	/* 32 bytes used */
	ctrl->levels[0].aperture    = NV_COPY_PDE_APERTURE_VIDMEM;
	ctrl->levels[0].pageShift   = 47;

	/* PD2 — 512 entries × 8 bytes = 4 KiB. */
	ctrl->levels[1].physAddress = (uint64_t)vmm->pt[1].page.vram_paddr;
	ctrl->levels[1].size        = (1ULL << 9) * 8;	/* 4096 */
	ctrl->levels[1].aperture    = NV_COPY_PDE_APERTURE_VIDMEM;
	ctrl->levels[1].pageShift   = 38;

	/* PD1 — 512 entries × 8 bytes = 4 KiB. */
	ctrl->levels[2].physAddress = (uint64_t)vmm->pt[2].page.vram_paddr;
	ctrl->levels[2].size        = (1ULL << 9) * 8;	/* 4096 */
	ctrl->levels[2].aperture    = NV_COPY_PDE_APERTURE_VIDMEM;
	ctrl->levels[2].pageShift   = 29;

	p = ctrl;
	err = nvkm_gsp_rm_ctrl_rd(&vmm->vaspace, &p, 0);
	if (err != 0) {
		device_printf(sc->dev,
		    "gsp_rm: VASPACE_COPY_SERVER_RESERVED_PDES failed err=%d\n",
		    err);
		return (err);
	}
	device_printf(sc->dev,
	    "gsp_rm: COPY_SERVER_RESERVED_PDES ok (va=0x%llx+0x%llx, "
	    "PD3=0x%llx PD2=0x%llx PD1=0x%llx)\n",
	    (unsigned long long)vmm->rm_va_base,
	    (unsigned long long)vmm->rm_va_size,
	    (unsigned long long)vmm->pt[0].page.vram_paddr,
	    (unsigned long long)vmm->pt[1].page.vram_paddr,
	    (unsigned long long)vmm->pt[2].page.vram_paddr);
	return (0);
}

int
nvkm_gsp_vmm_ctor(struct nvkm_softc *sc, uint32_t client_handle,
    struct nvkm_gsp_vmm *vmm)
{
	int err, i;

	memset(vmm, 0, sizeof(*vmm));
	vmm->sc = sc;
	vmm->rm_va_base = NVKM_VMM_RM_BASE;
	vmm->rm_va_size = NVKM_VMM_RM_SIZE;
	lwkt_token_init(&vmm->tok, "nvkm-vmm");
	LIST_INIT(&vmm->user_pd0_pages);
	LIST_INIT(&vmm->user_pt_pages);
	LIST_INIT(&vmm->sparse_regions);

	err = nvkm_dmamem_alloc(sc, NVKM_GMMU_PT_PAGE_SIZE,
	    NVKM_GMMU_PT_PAGE_SIZE, &vmm->sparse_page);
	if (err != 0)
		return (err);

	/* 1) Client + device + subdevice. */
	err = nvkm_gsp_client_ctor(sc, client_handle, &vmm->client);
	if (err != 0)
		goto fail_sparse_page;
	err = nvkm_gsp_device_ctor(&vmm->client, &vmm->device);
	if (err != 0)
		goto fail_client;

	/* 2) Three host sysmem PT pages: PD3, PD2, PD1. */
	for (i = 0; i < 3; i++) {
		err = nvkm_gsp_vmm_pt_alloc(sc, &vmm->pt[i]);
		if (err != 0) {
			device_printf(sc->dev,
			    "gsp_rm: PT page %d alloc failed\n", i);
			goto fail_pt;
		}
	}

	/* 3) Encode PD3[0] = PDE pointing at PD2; PD2[0] = PDE pointing at PD1.
	 *    The 512 MiB at VA 0x100000000 lands in PD1[8] — GSP fills that
	 *    entry itself when it makes RM-internal allocations.
	 *
	 *    PT pages are write-back cacheable sysmem, x86 has PCIe cache
	 *    snoop, so SYS_COH + VOL is the right aperture. */
	/* PD3[0] -> PD2 ; PD2[0] -> PD1. Both PT pages in sysmem; write
	 * via host KVA. The GMMU walker reads sysmem via PCIe coherent
	 * snoop on x86, so these writes are immediately visible. */
	/* PT chain in VRAM, written via BAR1 (L2-coherent). PDE aperture
	 * is VIDMEM (not SYS_COH+VOL). */
	nvkm_gsp_bar1_wr64(sc, vmm->pt[0].page.bar1_gva + 0,
	    nvkm_pde_to_vram(vmm->pt[1].page.vram_paddr));
	nvkm_gsp_bar1_wr64(sc, vmm->pt[1].page.bar1_gva + 0,
	    nvkm_pde_to_vram(vmm->pt[2].page.vram_paddr));

	device_printf(sc->dev,
	    "gsp_rm: PT chain (VRAM) PD3=0x%llx@bar1=0x%llx PD2=0x%llx@bar1=0x%llx PD1=0x%llx@bar1=0x%llx\n",
	    (unsigned long long)vmm->pt[0].page.vram_paddr,
	    (unsigned long long)vmm->pt[0].page.bar1_gva,
	    (unsigned long long)vmm->pt[1].page.vram_paddr,
	    (unsigned long long)vmm->pt[1].page.bar1_gva,
	    (unsigned long long)vmm->pt[2].page.vram_paddr,
	    (unsigned long long)vmm->pt[2].page.bar1_gva);

	/* 4) Allocate FERMI_VASPACE_A (non-external, server-managed PDE flavour). */
	{
		struct NV_VASPACE_ALLOCATION_PARAMETERS_r535 *args;

		args = nvkm_gsp_rm_alloc_get(&vmm->device.object,
		    nvkm_gsp_client_child_handle(&vmm->client,
		    NVKM_RM_VASPACE), FERMI_VASPACE_A, sizeof(*args),
		    &vmm->vaspace);
		if (args == NULL) {
			err = ENOMEM;
			goto fail_pt;
		}
		args->index = NV_VASPACE_ALLOCATION_INDEX_GPU_NEW;
		args->flags = 0;	/* server-managed */
		err = nvkm_gsp_rm_alloc_wr(&vmm->vaspace, args);
		if (err != 0) {
			device_printf(sc->dev,
			    "gsp_rm: FERMI_VASPACE_A alloc failed err=%d\n",
			    err);
			goto fail_pt;
		}
	}

	/* 5) Hand the PT chain to GSP via COPY_SERVER_RESERVED_PDES. */
	err = nvkm_gsp_vmm_copy_pdes(vmm);
	if (err != 0)
		goto fail_vaspace;

	device_printf(sc->dev,
	    "gsp_rm: VMM ready (client=0x%x device=0x%x vaspace=0x%x)\n",
	    vmm->client.object.handle, vmm->device.object.handle,
	    vmm->vaspace.handle);

	/* Allocate TURING_USERMODE_A at device level (nouveau allocates this
	 * once at drm init). GSP may gate doorbell delivery on its presence. */
	{
		struct nvkm_gsp_object usermode_obj;
		void *up = nvkm_gsp_rm_alloc_get(&vmm->device.subdevice,
		    nvkm_gsp_client_child_handle(&vmm->client, 0xc4610000u),
		    0x0000c461u, 0, &usermode_obj);
		if (up != NULL) {
			int uerr = nvkm_gsp_rm_alloc_wr(&usermode_obj, up);
			device_printf(sc->dev,
			    "gsp_rm: TURING_USERMODE_A handle=0x%x err=%d (device-level)\n",
			    usermode_obj.handle, uerr);
			/* Dump BAR0 regs post-USERMODE_A to compare with Fedora. */
			uint32_t um0 = nvkm_rd32(sc, 0xbb0000);
			uint32_t um80 = nvkm_rd32(sc, 0xbb0080);
			uint32_t um84 = nvkm_rd32(sc, 0xbb0084);
#ifdef NVKM_DEBUG_USERMODE_DIAG
			device_printf(sc->dev,
			    "fed_diag: USERMODE[0]=0x%08x TIME=%08x:%08x (Fedora: 0xc461)\n",
			    um0, um84, um80);
#else
			(void)um80;
			(void)um84;
#endif
			/* If 0, GSP didn\'t write class id -- write it ourselves. */
			if (um0 == 0) {
				nvkm_wr32(sc, 0xbb0000, 0xc461u);
				uint32_t um0b = nvkm_rd32(sc, 0xbb0000);
#ifdef NVKM_DEBUG_USERMODE_DIAG
				device_printf(sc->dev,
				    "fed_diag: wrote 0xc461 to USERMODE[0], readback = 0x%08x\n",
				    um0b);
#else
				(void)um0b;
#endif
			}
		}
	}

	return (0);

fail_vaspace:
	nvkm_gsp_rm_free(&vmm->vaspace);
fail_pt:
	for (i = 0; i < 3; i++)
		nvkm_gsp_vmm_pt_free(vmm->sc, &vmm->pt[i]);
	nvkm_gsp_device_dtor(&vmm->device);
fail_client:
	nvkm_gsp_client_dtor(&vmm->client);
fail_sparse_page:
	nvkm_dmamem_free(sc, &vmm->sparse_page);
	return (err);
}

void
nvkm_gsp_vmm_dtor(struct nvkm_gsp_vmm *vmm)
{
	struct nvkm_gsp_vmm_user_pt *pt;
	struct nvkm_gsp_vmm_pd0 *pd0;
	struct nvkm_gsp_vmm_sparse_region *region;
	int i;

	while ((region = LIST_FIRST(&vmm->sparse_regions)) != NULL) {
		LIST_REMOVE(region, link);
		kfree(region, M_NVKM_VMM);
	}
	while ((pt = LIST_FIRST(&vmm->user_pt_pages)) != NULL) {
		LIST_REMOVE(pt, link);
		nvkm_gsp_bar1_free_page(vmm->sc, &pt->spt);
		nvkm_gsp_bar1_free_page(vmm->sc, &pt->lpt);
		kfree(pt, M_NVKM_VMM);
	}
	while ((pd0 = LIST_FIRST(&vmm->user_pd0_pages)) != NULL) {
		LIST_REMOVE(pd0, link);
		nvkm_gsp_bar1_free_page(vmm->sc, &pd0->page);
		kfree(pd0, M_NVKM_VMM);
	}
	if (vmm->vaspace.handle != 0)
		nvkm_gsp_rm_free(&vmm->vaspace);
	for (i = 0; i < 3; i++)
		nvkm_gsp_vmm_pt_free(vmm->sc, &vmm->pt[i]);
	nvkm_dmamem_free(vmm->sc, &vmm->sparse_page);
	nvkm_gsp_device_dtor(&vmm->device);
	nvkm_gsp_client_dtor(&vmm->client);
}
