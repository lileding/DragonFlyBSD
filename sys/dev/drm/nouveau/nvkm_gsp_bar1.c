/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM BAR1 vmm (port of nouveau r535_bar_bar1_init).
 *
 * Mirror of nvkm_gsp_bar2.c but writes our PDE into GSP\'s BAR1 PDB[0]
 * via UPDATE_BAR_PDE BAR_1. GVA->VRAM read/write goes through PCIe BAR1
 * (bar_res[1] = 256 MiB) — L2-coherent like BAR2.
 *
 * Earlier conclusion that "GSP doesn\'t init BAR1" was wrong: GSP DOES
 * populate inst[0x200] PDB ptr from static_info bar1PdeBase, and the
 * walker is functional. We can replace its PDB with our own chain
 * exactly like BAR2.
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include <drm/drmP.h>
#include <vm/vm.h>
#include <vm/pmap.h>

#define NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE 70
#define NV_RPC_UPDATE_PDE_BAR_1             0

struct rpc_update_bar_pde_v15_00_b1 {
	uint32_t barType;
	uint8_t  _pad[4];
	uint64_t entryValue;
	uint64_t entryLevelShift;
};

static __inline void
b1_pramin_set_base(struct nvkm_softc *sc, uint64_t paddr)
{
	nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(paddr >> 16));
}

static __inline void
b1_pramin_wr32(struct nvkm_softc *sc, uint64_t paddr, uint32_t val)
{
	nvkm_wr32(sc, NV_PRAMIN + (uint32_t)(paddr & 0xffffu), val);
}

static __inline void
b1_pramin_wr64(struct nvkm_softc *sc, uint64_t paddr, uint64_t val)
{
	b1_pramin_wr32(sc, paddr + 0, (uint32_t)(val & 0xffffffffu));
	b1_pramin_wr32(sc, paddr + 4, (uint32_t)(val >> 32));
}

static __inline uint64_t
b1_pramin_rd64(struct nvkm_softc *sc, uint64_t paddr)
{
	uint32_t lo, hi;

	lo = nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((paddr + 0) & 0xffffu));
	hi = nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((paddr + 4) & 0xffffu));
	return (((uint64_t)hi << 32) | lo);
}

void
nvkm_gsp_bar1_invalidate(struct nvkm_softc *sc)
{
	uint32_t trig_rb = 0xffffffffu;

	/*
	 * Nouveau TU102 BAR VMM flush uses HUB MMU invalidate registers:
	 *   0xb830a0 = PDB >> 8
	 *   0xb830a4 = upper PDB
	 *   0xb830b0 = TRIGGER | PAGE_ALL | HUB_ONLY | ALL_PDB
	 */
	nvkm_wr32(sc, 0xb830a0, (uint32_t)(sc->gsp_bar1_pdb >> 8));
	nvkm_wr32(sc, 0xb830a4, 0x00000000u);
	nvkm_wr32(sc, 0xb830b0, 0x80000000u | 0x00000007u);
	for (int spin = 0; spin < 200; spin++) {
		trig_rb = nvkm_rd32(sc, 0xb830b0);
		if (!(trig_rb & 0x80000000u))
			break;
		DELAY(10);
	}

#ifdef NVKM_DEBUG_BAR1
	nvkm_debugf(sc->dev,
	    "bar1: TU102 invalidate PDB=0x%llx 0xb830b0=0x%x\n",
	    (unsigned long long)sc->gsp_bar1_pdb, trig_rb);
#endif
}

int
nvkm_gsp_bar1_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint64_t spt;
	uint64_t gsp_pd2, gsp_pd1, gsp_pd0;
	uint64_t spt_pde;
	uint64_t pd0_big, pd0_small;
	uint64_t pd0_big_pre, pd0_small_pre;
	uint64_t pd0_big_post, pd0_small_post;
	uint32_t saved;

	if (sc->bar_res[1] == NULL) {
		nvkm_debugf(sc->dev, "bar1: PCIe BAR1 not mapped\n");
		return (ENXIO);
	}
	if (sc->gsp_bar1_pdb == 0) {
		nvkm_debugf(sc->dev, "bar1: GSP did not publish bar1PdeBase\n");
		return (ENXIO);
	}
	if (!sc->bar2.ready) {
		nvkm_debugf(sc->dev, "bar1: BAR2 must be ready first (PRAMIN bootstrap)\n");
		return (ENXIO);
	}

	/* nouveau-style inheritance: don't allocate our own PD2/PD1/PD0,
	 * don't issue UPDATE_BAR_PDE BAR_1 RPC. GSP already built a BAR1 PT
	 * chain rooted at sc->gsp_bar1_pdb with mappings for GSP's own use
	 * (e.g. its internal channel inst blocks, runlist VRAM, etc). If we
	 * overwrite GSP's PD3[0], those GSP-internal BAR1 mappings break and
	 * GSP can't access its own state — silently page-faulting on inst
	 * reads, leaving PBDMA unable to schedule channels.
	 *
	 * Instead: walk GSP's chain (PD3[0]->PD2[0]->PD1[0]->PD0), allocate
	 * our own SPT pages, and mount them on a fixed high BAR1 PD0 range.
	 * This gives the driver a 64 MiB BAR1 window for host writes to VRAM
	 * page-table pages without disturbing GSP's low BAR1 mappings.
	 */
	uint64_t pdb_paddr = sc->gsp_bar1_pdb;
	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);

	/* Read GSP PD3[0] = first 8 bytes of GSP PD3 page. */
	b1_pramin_set_base(sc, pdb_paddr & ~(uint64_t)0xffffu);
	uint32_t pd3_lo = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((pdb_paddr + 0) & 0xffffu));
	uint32_t pd3_hi = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((pdb_paddr + 4) & 0xffffu));
	uint64_t pd3_0 = ((uint64_t)pd3_hi << 32) | pd3_lo;
	gsp_pd2 = (pd3_0 & ~(uint64_t)0xffull) << 4;

	/* Read GSP PD2[0]. */
	b1_pramin_set_base(sc, gsp_pd2 & ~(uint64_t)0xffffu);
	uint32_t pd2_lo = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((gsp_pd2 + 0) & 0xffffu));
	uint32_t pd2_hi = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((gsp_pd2 + 4) & 0xffffu));
	uint64_t pd2_0 = ((uint64_t)pd2_hi << 32) | pd2_lo;
	gsp_pd1 = (pd2_0 & ~(uint64_t)0xffull) << 4;

	/* Read GSP PD1[0]. */
	b1_pramin_set_base(sc, gsp_pd1 & ~(uint64_t)0xffffu);
	uint32_t pd1_lo = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((gsp_pd1 + 0) & 0xffffu));
	uint32_t pd1_hi = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((gsp_pd1 + 4) & 0xffffu));
	uint64_t pd1_0 = ((uint64_t)pd1_hi << 32) | pd1_lo;
	gsp_pd0 = (pd1_0 & ~(uint64_t)0xffull) << 4;

	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

#ifdef NVKM_DEBUG_BAR1
	nvkm_debugf(sc->dev,
	    "bar1: walked GSP PT chain: PD3=0x%llx -> PD2=0x%llx -> PD1=0x%llx -> PD0=0x%llx\n",
	    (unsigned long long)pdb_paddr, (unsigned long long)gsp_pd2,
	    (unsigned long long)gsp_pd1, (unsigned long long)gsp_pd0);
#endif

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	for (uint32_t slot = BAR1_PD0_MANAGED_FIRST;
	    slot <= BAR1_PD0_MANAGED_LAST; slot++) {
		uint32_t idx = slot - BAR1_PD0_MANAGED_FIRST;

		/* Alloc OUR SPT.  Each SPT covers one 2 MiB BAR1 PD0 slot. */
		spt = nvkm_gsp_vram_alloc_kind(sc, 0x1000, 0x1000,
		    NVKM_VRAM_BAR1_SPT, &sc->bar1);
		if (spt == 0) {
			nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
			lwkt_reltoken(&sc->gsp_tok);
			nvkm_debugf(sc->dev, "bar1: SPT alloc failed slot=%u\n", slot);
			return (ENOMEM);
		}

		/* Zero our SPT via PRAMIN. */
		b1_pramin_set_base(sc, spt & ~(uint64_t)0xffffu);
		for (uint32_t off = 0; off < 0x1000; off += 4)
			b1_pramin_wr32(sc, spt + off, 0);

		/* Write GSP PD0[slot] as a full dual PDE, like nouveau's
		 * gp100_vmm_pd0_pde() VMM_WO128() path.  We only install a
		 * 4 KiB SMALL SPT, so BIG is invalid and SMALL points at ours.
		 */
		spt_pde = (spt >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM;
		pd0_big = gsp_pd0 + slot * 16 + 0;
		pd0_small = gsp_pd0 + slot * 16 + 8;
		b1_pramin_set_base(sc, pd0_big & ~(uint64_t)0xffffu);
		pd0_big_pre = b1_pramin_rd64(sc, pd0_big);
		pd0_small_pre = b1_pramin_rd64(sc, pd0_small);
		b1_pramin_wr64(sc, pd0_big, 0);
		b1_pramin_wr64(sc, pd0_small, spt_pde);
		(void)nvkm_rd32(sc, NV_PRAMIN);
		pd0_big_post = b1_pramin_rd64(sc, pd0_big);
		pd0_small_post = b1_pramin_rd64(sc, pd0_small);
		b1->spt_paddr[idx] = spt;

#ifdef NVKM_DEBUG_BAR1
		nvkm_debugf(sc->dev,
		    "bar1: mounted OUR SPT 0x%llx at GSP PD0[%u] BIG 0x%llx->0x%llx SMALL 0x%llx->0x%llx\n",
		    (unsigned long long)spt, slot,
		    (unsigned long long)pd0_big_pre,
		    (unsigned long long)pd0_big_post,
		    (unsigned long long)pd0_small_pre,
		    (unsigned long long)pd0_small_post);
#endif
	}
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	b1->pd3_paddr = sc->gsp_bar1_pdb;
	b1->pd2_paddr = gsp_pd2;   /* shared with GSP, informational */
	b1->pd1_paddr = gsp_pd1;
	b1->pd0_paddr = gsp_pd0;
	b1->next_gva  = BAR1_GVA_ALLOC_BASE;
	memset(b1->gva_used, 0, sizeof(b1->gva_used));
	b1->fictitious_start = rman_get_start(sc->bar_res[1]) +
	    BAR1_GVA_ALLOC_BASE;
	b1->fictitious_end = b1->fictitious_start +
	    (vm_paddr_t)BAR1_GVA_ALLOC_PAGES * NVKM_GMMU_PT_PAGE_SIZE;
	if (!b1->fictitious_registered) {
		int r;

		r = vm_phys_fictitious_reg_range(b1->fictitious_start,
		    b1->fictitious_end, VM_MEMATTR_WRITE_COMBINING);
		if (r != 0) {
			nvkm_debugf(sc->dev,
			    "bar1: fictitious range 0x%llx-0x%llx failed err=%d\n",
			    (unsigned long long)b1->fictitious_start,
			    (unsigned long long)b1->fictitious_end, r);
			return (r);
		}
		b1->fictitious_registered = true;
	}

#ifdef NVKM_DEBUG_BAR1
	nvkm_debugf(sc->dev,
	    "bar1: mounted OUR SPT window PD0[%u..%u]; GVA base 0x%llx\n",
	    BAR1_PD0_MANAGED_FIRST, BAR1_PD0_MANAGED_LAST,
	    (unsigned long long)b1->next_gva);
#endif

	nvkm_gsp_bar1_invalidate(sc);

#ifdef NVKM_DEBUG_BAR1
	nvkm_debugf(sc->dev,
	    "bar1: inheriting GSP PT chain PD2=0x%llx PD1=0x%llx PD0=0x%llx; "
	    "our SPT window mounted on GSP PD0[%u..%u].SMALL; "
	    "BAR1@%llx %lluMiB\n",
	    (unsigned long long)gsp_pd2, (unsigned long long)gsp_pd1,
	    (unsigned long long)gsp_pd0, BAR1_PD0_MANAGED_FIRST,
	    BAR1_PD0_MANAGED_LAST,
	    (unsigned long long)rman_get_start(sc->bar_res[1]),
	    (unsigned long long)rman_get_size(sc->bar_res[1]) >> 20);
#endif

	sc->bar1.ready = true;

	/* SMOKE test removed: GVA=0 maps through GSP's own PT chain
	 * (our SPT window is mounted at high PD0 slots). Writing
	 * BAR1[0] would write to whatever GSP has at SPT[0] — likely a
	 * critical GSP-internal page. Don't go there. */

	return (0);
}

void
nvkm_gsp_bar1_fini(struct nvkm_softc *sc)
{
	if (sc->bar1.fictitious_registered) {
		vm_phys_fictitious_unreg_range(sc->bar1.fictitious_start,
		    sc->bar1.fictitious_end);
		sc->bar1.fictitious_registered = false;
	}
	sc->bar1.ready = false;
}

/* Map a 4 KiB VRAM page at a specific BAR1 GVA by writing the SPT entry. */
int
nvkm_gsp_bar1_map_vram(struct nvkm_softc *sc, uint64_t bar1_gva,
    uint64_t vram_paddr)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint32_t saved;
	uint32_t spt_idx;
	uint32_t pd0_idx;
	uint64_t spt;
	uint64_t pte;

	if (!b1->ready)
		return (ENXIO);
	if (bar1_gva >= (512ULL << 20)) /* BAR1 is 256 MiB on TU102 */
		return (EINVAL);

	/* Our SPT is mounted at GSP PD0[127].SMALL, covering the final
	 * 2 MiB of BAR1.  Match nouveau's VMM iterator index split:
	 *   SPT index = (gva >> 12) & 0x1ff
	 *   PD0 index = (gva >> 21) & 0xff
	 * The old code used gva >> 12 directly, which wrote far beyond the
	 * 512-entry SPT for GVAs like 0xfe07000.
	 */
	pd0_idx = (uint32_t)((bar1_gva >> 21) & 0xffu);
	if (pd0_idx < BAR1_PD0_MANAGED_FIRST ||
	    pd0_idx > BAR1_PD0_MANAGED_LAST)
		return (EINVAL);

	spt_idx = (uint32_t)((bar1_gva >> 12) & 0x1ffu);  /* 4 KiB SMALL pages */
	pte = (vram_paddr >> 12) << 8;          /* PTE: paddr in [39:8] */
	pte |= 0x1;                              /* VALID = bit 0 */

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	spt = b1->spt_paddr[pd0_idx - BAR1_PD0_MANAGED_FIRST];
	b1_pramin_set_base(sc, spt & ~(uint64_t)0xffffu);
	b1_pramin_wr64(sc, spt + (uint64_t)spt_idx * 8, pte);
	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	nvkm_gsp_bar1_invalidate(sc);

#ifdef NVKM_DEBUG_BAR1
	nvkm_debugf(sc->dev,
	    "bar1: map BAR1_GVA=0x%llx -> VRAM=0x%llx (SPT[%u]=0x%llx)\n",
	    (unsigned long long)bar1_gva, (unsigned long long)vram_paddr,
	    spt_idx, (unsigned long long)pte);
#endif
	return (0);
}

static int
nvkm_gsp_bar1_clear_gva(struct nvkm_softc *sc, uint64_t bar1_gva)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint32_t saved;
	uint32_t spt_idx;
	uint32_t pd0_idx;
	uint64_t spt;

	if (!b1->ready)
		return (ENXIO);
	if (bar1_gva >= (512ULL << 20)) /* BAR1 is 256 MiB on TU102 */
		return (EINVAL);

	pd0_idx = (uint32_t)((bar1_gva >> 21) & 0xffu);
	if (pd0_idx < BAR1_PD0_MANAGED_FIRST ||
	    pd0_idx > BAR1_PD0_MANAGED_LAST)
		return (EINVAL);

	spt_idx = (uint32_t)((bar1_gva >> 12) & 0x1ffu);

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	spt = b1->spt_paddr[pd0_idx - BAR1_PD0_MANAGED_FIRST];
	b1_pramin_set_base(sc, spt & ~(uint64_t)0xffffu);
	b1_pramin_wr64(sc, spt + (uint64_t)spt_idx * 8, 0);
	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	return (0);
}

/* Flush: read from BAR1 to force walker re-walk + L2 sync. */
void
nvkm_gsp_bar1_flush(struct nvkm_softc *sc)
{
	if (!sc->bar1.ready || sc->bar_res[1] == NULL)
		return;
	(void)bus_read_4(sc->bar_res[1], 0);
}

void
nvkm_gsp_bar1_wr32(struct nvkm_softc *sc, uint64_t gva, uint32_t val)
{
	if (!sc->bar1.ready)
		return;
	bus_write_4(sc->bar_res[1], gva, val);
}

uint32_t
nvkm_gsp_bar1_rd32(struct nvkm_softc *sc, uint64_t gva)
{
	if (!sc->bar1.ready)
		return (0xdeadbeef);
	return (bus_read_4(sc->bar_res[1], gva));
}

void
nvkm_gsp_bar1_wr64(struct nvkm_softc *sc, uint64_t gva, uint64_t val)
{
	if (!sc->bar1.ready)
		return;
	bus_write_8(sc->bar_res[1], gva, val);
}

/*
 * nvkm_gsp_bar1_set_region64()
 *
 * Ownership:
 *   Borrows sc and its BAR1 resource; it does not retain references or change
 *   BAR1 virtual address ownership.
 *
 * Lifetime:
 *   The caller must ensure [gva, gva + count * 8) is currently mapped in BAR1
 *   for the full duration of the call.
 *
 * Threading:
 *   Performs synchronous MMIO writes.  Callers provide higher-level
 *   serialization for the page table or object being updated.
 */
void
nvkm_gsp_bar1_set_region64(struct nvkm_softc *sc, uint64_t gva, uint64_t val,
    uint32_t count)
{
	uint32_t i;

	if (!sc->bar1.ready || count == 0)
		return;
	for (i = 0; i < count; i++)
		bus_write_8(sc->bar_res[1], gva + (uint64_t)i * 8, val);
}

uint64_t
nvkm_gsp_bar1_rd64(struct nvkm_softc *sc, uint64_t gva)
{
	if (!sc->bar1.ready)
		return (0xdeadbeefdeadbeefULL);
	return (bus_read_8(sc->bar_res[1], gva));
}

static bool
nvkm_gsp_bar1_gva_used(struct nvkm_gsp_bar1 *b1, uint32_t idx)
{
	return ((b1->gva_used[idx / 8] & (1u << (idx % 8))) != 0);
}

static void
nvkm_gsp_bar1_gva_set(struct nvkm_gsp_bar1 *b1, uint32_t idx,
    bool used)
{
	uint8_t bit = 1u << (idx % 8);

	if (used)
		b1->gva_used[idx / 8] |= bit;
	else
		b1->gva_used[idx / 8] &= ~bit;
}

static int
nvkm_gsp_bar1_alloc_gva(struct nvkm_gsp_bar1 *b1, uint64_t *pgva)
{
	uint32_t start, idx;

	start = (uint32_t)((b1->next_gva - BAR1_GVA_ALLOC_BASE) /
	    NVKM_GMMU_PT_PAGE_SIZE);
	if (start >= BAR1_GVA_ALLOC_PAGES)
		start = 0;

	for (uint32_t i = 0; i < BAR1_GVA_ALLOC_PAGES; i++) {
		idx = (start + i) % BAR1_GVA_ALLOC_PAGES;
		if (nvkm_gsp_bar1_gva_used(b1, idx))
			continue;

		nvkm_gsp_bar1_gva_set(b1, idx, true);
		*pgva = BAR1_GVA_ALLOC_BASE +
		    (uint64_t)idx * NVKM_GMMU_PT_PAGE_SIZE;
		b1->next_gva = BAR1_GVA_ALLOC_BASE +
		    (uint64_t)((idx + 1) % BAR1_GVA_ALLOC_PAGES) *
		    NVKM_GMMU_PT_PAGE_SIZE;
		return (0);
	}

	return (ENOSPC);
}

static int
nvkm_gsp_bar1_alloc_gva_range(struct nvkm_gsp_bar1 *b1, uint32_t pages,
    uint64_t *pgva)
{
	uint32_t start;

	if (pages == 0 || pages > BAR1_GVA_ALLOC_PAGES)
		return (EINVAL);

	start = (uint32_t)((b1->next_gva - BAR1_GVA_ALLOC_BASE) /
	    NVKM_GMMU_PT_PAGE_SIZE);
	if (start >= BAR1_GVA_ALLOC_PAGES)
		start = 0;

	for (uint32_t i = 0; i < BAR1_GVA_ALLOC_PAGES; i++) {
		uint32_t idx = (start + i) % BAR1_GVA_ALLOC_PAGES;
		bool used = false;

		if (idx + pages > BAR1_GVA_ALLOC_PAGES)
			continue;

		for (uint32_t page = 0; page < pages; page++) {
			if (nvkm_gsp_bar1_gva_used(b1, idx + page)) {
				used = true;
				break;
			}
		}
		if (used)
			continue;

		for (uint32_t page = 0; page < pages; page++)
			nvkm_gsp_bar1_gva_set(b1, idx + page, true);

		*pgva = BAR1_GVA_ALLOC_BASE +
		    (uint64_t)idx * NVKM_GMMU_PT_PAGE_SIZE;
		b1->next_gva = BAR1_GVA_ALLOC_BASE +
		    (uint64_t)((idx + pages) % BAR1_GVA_ALLOC_PAGES) *
		    NVKM_GMMU_PT_PAGE_SIZE;
		return (0);
	}

	return (ENOSPC);
}

static void
nvkm_gsp_bar1_free_gva(struct nvkm_gsp_bar1 *b1, uint64_t gva)
{
	uint32_t idx;

	if (gva < BAR1_GVA_ALLOC_BASE ||
	    gva >= BAR1_GVA_ALLOC_BASE +
	    (uint64_t)BAR1_GVA_ALLOC_PAGES * NVKM_GMMU_PT_PAGE_SIZE ||
	    (gva & (NVKM_GMMU_PT_PAGE_SIZE - 1)) != 0)
		return;

	idx = (uint32_t)((gva - BAR1_GVA_ALLOC_BASE) /
	    NVKM_GMMU_PT_PAGE_SIZE);
	nvkm_gsp_bar1_gva_set(b1, idx, false);
}

static void
nvkm_gsp_bar1_free_gva_range(struct nvkm_gsp_bar1 *b1, uint64_t gva,
    uint32_t pages)
{
	uint32_t idx;

	if (pages == 0 ||
	    gva < BAR1_GVA_ALLOC_BASE ||
	    gva >= BAR1_GVA_ALLOC_BASE +
	    (uint64_t)BAR1_GVA_ALLOC_PAGES * NVKM_GMMU_PT_PAGE_SIZE ||
	    (gva & (NVKM_GMMU_PT_PAGE_SIZE - 1)) != 0)
		return;

	idx = (uint32_t)((gva - BAR1_GVA_ALLOC_BASE) /
	    NVKM_GMMU_PT_PAGE_SIZE);
	if (idx + pages > BAR1_GVA_ALLOC_PAGES)
		return;

	for (uint32_t page = 0; page < pages; page++)
		nvkm_gsp_bar1_gva_set(b1, idx + page, false);
}


void
nvkm_gsp_bar1_count_gva(struct nvkm_softc *sc, uint32_t *used,
    uint32_t *total)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint32_t count = 0;

	for (uint32_t i = 0; i < BAR1_GVA_ALLOC_PAGES; i++) {
		if (nvkm_gsp_bar1_gva_used(b1, i))
			count++;
	}
	*used = count;
	*total = BAR1_GVA_ALLOC_PAGES;
}

int
nvkm_gsp_bar1_alloc_page_kind(struct nvkm_softc *sc,
    struct nvkm_bar1_page *page, enum nvkm_vram_kind kind, void *owner)
{
	uint64_t paddr, gva;
	int err;

	if (!sc->bar1.ready)
		return (ENXIO);

	err = nvkm_gsp_bar1_alloc_gva(&sc->bar1, &gva);
	if (err != 0)
		return (err);

	paddr = nvkm_gsp_vram_alloc_kind(sc, NVKM_GMMU_PT_PAGE_SIZE,
	    NVKM_GMMU_PT_PAGE_SIZE, kind, owner);
	if (paddr == 0) {
		nvkm_gsp_bar1_free_gva(&sc->bar1, gva);
		return (ENOMEM);
	}

	err = nvkm_gsp_bar1_map_vram(sc, gva, paddr);
	if (err != 0) {
		nvkm_gsp_vram_free_kind(sc, paddr, kind, owner);
		nvkm_gsp_bar1_free_gva(&sc->bar1, gva);
		return (err);
	}
	nvkm_gsp_bar1_flush(sc);

	page->vram_paddr = paddr;
	page->bar1_gva   = gva;
	page->kind = kind;
	page->owner = owner;
	return (0);
}

int
nvkm_gsp_bar1_alloc_page(struct nvkm_softc *sc, struct nvkm_bar1_page *page)
{
	return (nvkm_gsp_bar1_alloc_page_kind(sc, page, NVKM_VRAM_BAR1_PAGE,
	    &sc->bar1));
}

void
nvkm_gsp_bar1_free_page(struct nvkm_softc *sc, struct nvkm_bar1_page *page)
{
	if (page->bar1_gva != 0) {
		if (nvkm_gsp_bar1_clear_gva(sc, page->bar1_gva) == 0)
			nvkm_gsp_bar1_invalidate(sc);
		nvkm_gsp_bar1_free_gva(&sc->bar1, page->bar1_gva);
	}
	if (page->vram_paddr != 0)
		nvkm_gsp_vram_free_kind(sc, page->vram_paddr, page->kind,
		    page->owner);
	page->vram_paddr = 0;
	page->bar1_gva   = 0;
	page->kind = NVKM_VRAM_UNKNOWN;
	page->owner = NULL;
}

/*
 * Map an EXISTING 4 KiB VRAM page (already-allocated paddr) into BAR1 at the
 * next free GVA, returning the GVA for host reads/writes. Unlike
 * nvkm_gsp_bar1_alloc_page, this does not allocate VRAM -- the caller owns the
 * paddr (e.g. the GSP-handed display instance RAM). paddr must be 4 KiB
 * aligned. Release the GVA with nvkm_gsp_bar1_unmap_existing.
 */
int
nvkm_gsp_bar1_map_existing(struct nvkm_softc *sc, uint64_t paddr, uint64_t *pgva)
{
	uint64_t gva;
	int err;

	if (!sc->bar1.ready)
		return (ENXIO);

	err = nvkm_gsp_bar1_alloc_gva(&sc->bar1, &gva);
	if (err != 0)
		return (err);

	err = nvkm_gsp_bar1_map_vram(sc, gva, paddr);
	if (err != 0) {
		nvkm_gsp_bar1_free_gva(&sc->bar1, gva);
		return (err);
	}
	nvkm_gsp_bar1_flush(sc);

	*pgva = gva;
	return (0);
}

int
nvkm_gsp_bar1_map_existing_range(struct nvkm_softc *sc, uint64_t paddr,
    uint64_t size, uint64_t *pgva)
{
	uint64_t gva;
	uint32_t pages;
	int err;

	if (!sc->bar1.ready)
		return (ENXIO);
	if (pgva == NULL || size == 0 ||
	    (paddr & (NVKM_GMMU_PT_PAGE_SIZE - 1)) != 0)
		return (EINVAL);

	pages = (uint32_t)((size + NVKM_GMMU_PT_PAGE_SIZE - 1) /
	    NVKM_GMMU_PT_PAGE_SIZE);
	if (pages == 0 || pages > BAR1_GVA_ALLOC_PAGES)
		return (EINVAL);

	err = nvkm_gsp_bar1_alloc_gva_range(&sc->bar1, pages, &gva);
	if (err != 0)
		return (err);

	for (uint32_t page = 0; page < pages; page++) {
		err = nvkm_gsp_bar1_map_vram(sc,
		    gva + (uint64_t)page * NVKM_GMMU_PT_PAGE_SIZE,
		    paddr + (uint64_t)page * NVKM_GMMU_PT_PAGE_SIZE);
		if (err != 0) {
			for (uint32_t clear = 0; clear < page; clear++) {
				(void)nvkm_gsp_bar1_clear_gva(sc,
				    gva + (uint64_t)clear *
				    NVKM_GMMU_PT_PAGE_SIZE);
			}
			if (page != 0)
				nvkm_gsp_bar1_invalidate(sc);
			nvkm_gsp_bar1_free_gva_range(&sc->bar1, gva, pages);
			return (err);
		}
	}
	nvkm_gsp_bar1_flush(sc);

	*pgva = gva;
	return (0);
}

void
nvkm_gsp_bar1_unmap_existing(struct nvkm_softc *sc, uint64_t gva)
{
	if (gva != 0) {
		if (nvkm_gsp_bar1_clear_gva(sc, gva) == 0)
			nvkm_gsp_bar1_invalidate(sc);
		nvkm_gsp_bar1_free_gva(&sc->bar1, gva);
	}
}

void
nvkm_gsp_bar1_unmap_existing_range(struct nvkm_softc *sc, uint64_t gva,
    uint64_t size)
{
	uint32_t pages;
	bool cleared = false;

	if (gva == 0 || size == 0)
		return;

	pages = (uint32_t)((size + NVKM_GMMU_PT_PAGE_SIZE - 1) /
	    NVKM_GMMU_PT_PAGE_SIZE);
	for (uint32_t page = 0; page < pages; page++) {
		if (nvkm_gsp_bar1_clear_gva(sc,
		    gva + (uint64_t)page * NVKM_GMMU_PT_PAGE_SIZE) == 0)
			cleared = true;
	}
	if (cleared)
		nvkm_gsp_bar1_invalidate(sc);
	nvkm_gsp_bar1_free_gva_range(&sc->bar1, gva, pages);
}

void
nvkm_gsp_bar1_dump_pt(struct nvkm_softc *sc __unused,
    uint64_t target_paddr __unused, uint32_t target_off __unused)
{
}
