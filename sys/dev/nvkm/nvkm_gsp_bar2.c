/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM BAR2 vmm (port of nouveau r535_bar_bar2_init).
 *
 * Build a fresh BAR2 VMM root, bootstrap the whole BAR2 aperture, then
 * send UPDATE_BAR_PDE BAR_2 with the first root PDE.  This mirrors
 * nouveau's gf100_bar_oneinit_bar(..., NVKM_BAR2_INST) + nvkm_vmm_boot()
 * before r535_bar_bar2_init() hands vmm->pd->pt[0][0] to GSP-RM.
 *
 * BAR2 gives us L2-coherent VRAM read/write -- needed to manipulate
 * BAR1 PT pages (which are in VRAM and must be visible to the BAR1
 * walker via L2).
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include <vm/vm.h>
#include <vm/pmap.h>

static MALLOC_DEFINE(M_NVKM_BAR2_PT, "nvkm_bar2_pt", "nvkm BAR2 page tables");

#define NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE 70
#define NV_RPC_UPDATE_PDE_BAR_2             1

#define BAR2_PT_PD1	1
#define BAR2_PT_PD0	2
#define BAR2_PT_SPT	3

struct rpc_update_bar_pde_v15_00_b2 {
	uint32_t barType;
	uint8_t  _pad[4];
	uint64_t entryValue;
	uint64_t entryLevelShift;
};

static __inline void
b2_pramin_set_base(struct nvkm_softc *sc, uint64_t paddr)
{
	nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(paddr >> 16));
}

static __inline void
b2_pramin_wr32(struct nvkm_softc *sc, uint64_t paddr, uint32_t val)
{
	nvkm_wr32(sc, NV_PRAMIN + (uint32_t)(paddr & 0xffffu), val);
}

static __inline void
b2_pramin_wr64(struct nvkm_softc *sc, uint64_t paddr, uint64_t val)
{
	b2_pramin_wr32(sc, paddr + 0, (uint32_t)(val & 0xffffffffu));
	b2_pramin_wr32(sc, paddr + 4, (uint32_t)(val >> 32));
}

static void
nvkm_gsp_bar2_zero_vram_page_locked(struct nvkm_softc *sc, uint64_t paddr)
{
	uint32_t saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);

	b2_pramin_set_base(sc, paddr & ~(uint64_t)0xffffu);
	for (uint32_t off = 0; off < NVKM_GMMU_PT_PAGE_SIZE; off += 4)
		b2_pramin_wr32(sc, paddr + off, 0);
	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
}

static struct nvkm_gsp_bar2_pt *
nvkm_gsp_bar2_pt_find(struct nvkm_gsp_bar2 *b2, uint8_t level,
    uint32_t pd2_idx, uint32_t pd1_idx, uint32_t pd0_idx)
{
	struct nvkm_gsp_bar2_pt *pt;

	LIST_FOREACH(pt, &b2->pt_pages, link) {
		if (pt->level == level &&
		    pt->pd2_idx == pd2_idx &&
		    pt->pd1_idx == pd1_idx &&
		    pt->pd0_idx == pd0_idx)
			return (pt);
	}

	return (NULL);
}

static int
nvkm_gsp_bar2_pt_alloc(struct nvkm_softc *sc, uint8_t level,
    uint32_t pd2_idx, uint32_t pd1_idx, uint32_t pd0_idx,
    struct nvkm_gsp_bar2_pt **ppt)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	struct nvkm_gsp_bar2_pt *pt;
	uint64_t paddr;

	pt = nvkm_gsp_bar2_pt_find(b2, level, pd2_idx, pd1_idx, pd0_idx);
	if (pt != NULL) {
		*ppt = pt;
		return (0);
	}

	paddr = nvkm_gsp_vram_alloc(sc, NVKM_GMMU_PT_PAGE_SIZE,
	    NVKM_GMMU_PT_PAGE_SIZE);
	if (paddr == 0)
		return (ENOMEM);

	pt = kmalloc(sizeof(*pt), M_NVKM_BAR2_PT, M_WAITOK | M_ZERO);
	pt->level = level;
	pt->pd2_idx = pd2_idx;
	pt->pd1_idx = pd1_idx;
	pt->pd0_idx = pd0_idx;
	pt->paddr = paddr;
	LIST_INSERT_HEAD(&b2->pt_pages, pt, link);

	lwkt_gettoken(&sc->gsp_tok);
	nvkm_gsp_bar2_zero_vram_page_locked(sc, paddr);
	lwkt_reltoken(&sc->gsp_tok);

	*ppt = pt;
	return (0);
}

static int
nvkm_gsp_bar2_get_spt(struct nvkm_softc *sc, uint64_t bar2_gva,
    struct nvkm_gsp_bar2_pt **pspt)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	struct nvkm_gsp_bar2_pt *pd1_pt, *pd0_pt, *spt_pt;
	uint32_t pd2_idx, pd1_idx, pd0_idx;
	uint32_t saved;
	int err;

	pd2_idx = (uint32_t)((bar2_gva >> NVKM_GMMU_PD2_SHIFT) &
	    (NVKM_GMMU_PD2_ENTRIES - 1));
	pd1_idx = (uint32_t)((bar2_gva >> NVKM_GMMU_PD1_SHIFT) &
	    (NVKM_GMMU_PD1_ENTRIES - 1));
	pd0_idx = (uint32_t)((bar2_gva >> NVKM_GMMU_PD0_SHIFT) &
	    (NVKM_GMMU_PD0_ENTRIES - 1));

	err = nvkm_gsp_bar2_pt_alloc(sc, BAR2_PT_PD1, pd2_idx, 0, 0, &pd1_pt);
	if (err != 0)
		return (err);

	err = nvkm_gsp_bar2_pt_alloc(sc, BAR2_PT_PD0, pd2_idx, pd1_idx, 0,
	    &pd0_pt);
	if (err != 0)
		return (err);

	err = nvkm_gsp_bar2_pt_alloc(sc, BAR2_PT_SPT, pd2_idx, pd1_idx,
	    pd0_idx, &spt_pt);
	if (err != 0)
		return (err);

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);

	b2_pramin_set_base(sc, b2->pd2_paddr & ~(uint64_t)0xffffu);
	b2_pramin_wr64(sc, b2->pd2_paddr + pd2_idx * 8,
	    nvkm_pde_to_vram(pd1_pt->paddr));

	b2_pramin_set_base(sc, pd1_pt->paddr & ~(uint64_t)0xffffu);
	b2_pramin_wr64(sc, pd1_pt->paddr + pd1_idx * 8,
	    nvkm_pde_to_vram(pd0_pt->paddr));

	b2_pramin_set_base(sc, pd0_pt->paddr & ~(uint64_t)0xffffu);
	b2_pramin_wr64(sc, pd0_pt->paddr + pd0_idx * 16,
	    nvkm_pde_to_vram(spt_pt->paddr));
	b2_pramin_wr64(sc, pd0_pt->paddr + pd0_idx * 16 + 8, 0);

	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	if (b2->pd1_paddr == 0)
		b2->pd1_paddr = pd1_pt->paddr;
	if (b2->pd0_paddr == 0)
		b2->pd0_paddr = pd0_pt->paddr;
	if (b2->spt_paddr == 0)
		b2->spt_paddr = spt_pt->paddr;

	*pspt = spt_pt;
	return (0);
}

static int
nvkm_gsp_bar2_bootstrap(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	uint64_t gva;
	int err;

	/*
	 * nouveau bootstraps BAR2 with the smallest supported page size
	 * (tu102_vmm.page[] walks down to shift 12 before nvkm_vmm_boot()).
	 * Pre-create every PD0->SPT edge that covers the halved BAR2 window,
	 * instead of allocating the tree lazily after UPDATE_BAR_PDE.
	 */
	for (gva = 0; gva < b2->aperture_size; gva +=
	    (1ULL << NVKM_GMMU_PD0_SHIFT)) {
		struct nvkm_gsp_bar2_pt *spt_pt __unused;

		err = nvkm_gsp_bar2_get_spt(sc, gva, &spt_pt);
		if (err != 0)
			return (err);
	}

	return (0);
}

void
nvkm_gsp_bar2_invalidate(struct nvkm_softc *sc)
{
	uint32_t trig_rb = 0xffffffffu;

	/*
	 * Match nouveau TU102 BAR VMM flush.  Under GSP-RM, BAR2 updates are
	 * invalidated through the GSP-provided BAR2 PDB with PAGE_ALL,
	 * HUB_ONLY, and ALL_PDB set.
	 */
	nvkm_wr32(sc, 0xb830a0, (uint32_t)(sc->gsp_bar2_pdb >> 8));
	nvkm_wr32(sc, 0xb830a4, 0x00000000u);
	nvkm_wr32(sc, 0xb830b0, 0x80000000u | 0x00000007u);
	for (int spin = 0; spin < 200; spin++) {
		trig_rb = nvkm_rd32(sc, 0xb830b0);
		if (!(trig_rb & 0x80000000u))
			break;
		DELAY(10);
	}

#ifdef NVKM_DEBUG_BAR2
	device_printf(sc->dev,
	    "bar2: TU102 invalidate PDB=0x%llx 0xb830b0=0x%x\n",
	    (unsigned long long)sc->gsp_bar2_pdb, trig_rb);
#endif
}

int
nvkm_gsp_bar2_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	uint64_t pd2;
	uint64_t pd2_pde;
	struct rpc_update_bar_pde_v15_00_b2 *rpc;
	int err;
	uint32_t saved;

	if (sc->bar_res[3] == NULL) {
		device_printf(sc->dev, "bar2: PCIe BAR3 (BAR2) not mapped\n");
		return (ENXIO);
	}
	if (sc->gsp_bar2_pdb == 0) {
		device_printf(sc->dev, "bar2: GSP did not publish bar2PdeBase\n");
		return (ENXIO);
	}

	pd2 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	if (!pd2) {
		device_printf(sc->dev, "bar2: VRAM alloc failed\n");
		return (ENOMEM);
	}

	b2->pd3_paddr = sc->gsp_bar2_pdb;
	b2->pd2_paddr = pd2;
	b2->pd1_paddr = 0;
	b2->pd0_paddr = 0;
	b2->spt_paddr = 0;
	b2->aperture_size = rman_get_size(sc->bar_res[3]) >> 1;
	b2->next_gva  = BAR2_GVA_ALLOC_BASE;
	LIST_INIT(&b2->pt_pages);

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	nvkm_gsp_bar2_zero_vram_page_locked(sc, pd2);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	err = nvkm_gsp_bar2_bootstrap(sc);
	if (err != 0) {
		device_printf(sc->dev, "bar2: bootstrap failed err=%d\n", err);
		return (err);
	}

	pd2_pde = (pd2 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM;

	/* Diag: read GSP\'s BAR2 PDB[0] via PRAMIN before/after RPC. */
	uint64_t pdb_paddr = sc->gsp_bar2_pdb;
	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b2_pramin_set_base(sc, pdb_paddr & ~(uint64_t)0xffffu);
	uint32_t pdb0_pre_lo = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((pdb_paddr + 0) & 0xffffu));
	uint32_t pdb0_pre_hi = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((pdb_paddr + 4) & 0xffffu));
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE,
	    sizeof(*rpc));
	if (rpc == NULL)
		return (ENOMEM);
	rpc->barType         = NV_RPC_UPDATE_PDE_BAR_2;
	rpc->entryValue      = pd2_pde;
	rpc->entryLevelShift = NVKM_GMMU_PD3_SHIFT;
	err = nvkm_gsp_rpc_wr(sc, rpc, NVKM_GSP_RPC_REPLY_RECV);
	if (err != 0) {
		device_printf(sc->dev,
		    "bar2: UPDATE_BAR_PDE BAR_2 failed err=%d\n", err);
		return (err);
	}

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b2_pramin_set_base(sc, pdb_paddr & ~(uint64_t)0xffffu);
	uint32_t pdb0_post_lo = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((pdb_paddr + 0) & 0xffffu));
	uint32_t pdb0_post_hi = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((pdb_paddr + 4) & 0xffffu));
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

#ifdef NVKM_DEBUG_BAR2
	device_printf(sc->dev,
	    "bar2: GSP PDB[0] pre RPC = 0x%08x:%08x, post RPC = 0x%08x:%08x, expected pde = 0x%llx\n",
	    pdb0_pre_hi, pdb0_pre_lo, pdb0_post_hi, pdb0_post_lo,
	    (unsigned long long)pd2_pde);
#else
	(void)pdb0_pre_lo;
	(void)pdb0_pre_hi;
	(void)pdb0_post_lo;
	(void)pdb0_post_hi;
#endif

	nvkm_gsp_bar2_invalidate(sc);

	/* Read BAR2 inst reg (0xb80f48) -- the address walker uses as root. */
	uint32_t bar2_inst = nvkm_rd32(sc, 0xb80f48);
	uint32_t bar1_inst = nvkm_rd32(sc, 0xb80f40);
	uint64_t bar2_inst_paddr = ((uint64_t)(bar2_inst & 0x0fffffffu)) << 12;
	uint64_t bar1_inst_paddr = ((uint64_t)(bar1_inst & 0x0fffffffu)) << 12;
#ifdef NVKM_DEBUG_BAR2
	device_printf(sc->dev,
	    "bar2: 0xb80f48=0x%08x (inst paddr=0x%llx), 0xb80f40=0x%08x (inst paddr=0x%llx)\n",
	    bar2_inst, (unsigned long long)bar2_inst_paddr,
	    bar1_inst, (unsigned long long)bar1_inst_paddr);
#endif

	/* Re-verify whether GSP populated BAR1 inst[0x200] PDB ptr.
	 * Earlier conclusion was "GSP doesn\'t init BAR1" -- recheck. */
	if (bar1_inst_paddr != 0) {
		lwkt_gettoken(&sc->gsp_tok);
		saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
		b2_pramin_set_base(sc, bar1_inst_paddr & ~(uint64_t)0xffffu);
		uint32_t b1_lo = nvkm_rd32(sc, NV_PRAMIN +
		    (uint32_t)((bar1_inst_paddr + 0x200) & 0xffffu));
		uint32_t b1_hi = nvkm_rd32(sc, NV_PRAMIN +
		    (uint32_t)((bar1_inst_paddr + 0x204) & 0xffffu));
		nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
		lwkt_reltoken(&sc->gsp_tok);
#ifdef NVKM_DEBUG_BAR2
		device_printf(sc->dev,
		    "bar2: BAR1_inst[0x200..0x208] (PDB ptr) = 0x%08x:%08x %s\n",
		    b1_hi, b1_lo,
		    (b1_lo == 0 && b1_hi == 0) ? "*** UNINITIALIZED ***" : "(populated)");
#else
		(void)b1_lo;
		(void)b1_hi;
#endif
	}

	/* Read PDB pointer from BAR2 inst block (offset 0x200) via PRAMIN. */
	if (bar2_inst_paddr != 0) {
		lwkt_gettoken(&sc->gsp_tok);
		saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
		b2_pramin_set_base(sc, bar2_inst_paddr & ~(uint64_t)0xffffu);
		uint32_t inst_lo = nvkm_rd32(sc, NV_PRAMIN +
		    (uint32_t)((bar2_inst_paddr + 0x200) & 0xffffu));
		uint32_t inst_hi = nvkm_rd32(sc, NV_PRAMIN +
		    (uint32_t)((bar2_inst_paddr + 0x204) & 0xffffu));
		nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
		lwkt_reltoken(&sc->gsp_tok);
#ifdef NVKM_DEBUG_BAR2
		device_printf(sc->dev,
		    "bar2: BAR2_inst[0x200..0x208] (PDB ptr) = 0x%08x:%08x\n",
		    inst_hi, inst_lo);
#else
		(void)inst_lo;
		(void)inst_hi;
#endif
	}

	b2->ready = true;  /* enable map_vram + flush */

	/* Nouveau-style flush setup: map a guaranteed-valid VRAM page at
	 * BAR2_GVA_FLUSH (=0); flush() reads from that GVA to force walker
	 * re-walk after PT updates (port of r535_bar_bar2_init lines 100-114
	 * + r535_bar_flush). */
	{
		uint64_t flush_vram = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
		if (flush_vram != 0) {
			b2->flush_vram_paddr = flush_vram;
			(void)nvkm_gsp_bar2_map_vram(sc, BAR2_GVA_FLUSH, flush_vram);
			/* First flush: forces walker to walk our chain end-to-end. */
			nvkm_gsp_bar2_flush(sc);
		}
	}

	/* Immediate self-test (post-flush): map test VRAM page, write+read. */
	{
		uint64_t tv = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
		if (tv != 0) {
			(void)nvkm_gsp_bar2_map_vram(sc, 0x2000, tv);
			nvkm_gsp_bar2_flush(sc);
			nvkm_gsp_bar2_wr32(sc, 0x2000 + 0x10, 0xC0FFEE12u);
			nvkm_gsp_bar2_flush(sc);
			uint32_t rb = nvkm_gsp_bar2_rd32(sc, 0x2000 + 0x10);
#ifdef NVKM_DEBUG_BAR2
			device_printf(sc->dev,
			    "bar2_diag: post-flush test wr C0FFEE12, readback = 0x%08x (target VRAM 0x%llx)\n",
			    rb, (unsigned long long)tv);
#else
			(void)rb;
#endif
			b2->next_gva = 0x3000;
		}
	}

#ifdef NVKM_DEBUG_BAR2
	device_printf(sc->dev,
	    "bar2: PT chain PD2=0x%llx PD1=0x%llx PD0=0x%llx SPT=0x%llx; "
	    "GSP PDB=0x%llx; UPDATE_BAR_PDE pde=0x%llx; BAR2@%llx %lluMiB halve=%lluMiB\n",
	    (unsigned long long)pd2, (unsigned long long)b2->pd1_paddr,
	    (unsigned long long)b2->pd0_paddr, (unsigned long long)b2->spt_paddr,
	    (unsigned long long)b2->pd3_paddr,
	    (unsigned long long)pd2_pde,
	    (unsigned long long)rman_get_start(sc->bar_res[3]),
	    (unsigned long long)rman_get_size(sc->bar_res[3]) >> 20,
	    (unsigned long long)b2->aperture_size >> 20);
#endif
	b2->ready = true;
	return (0);
}

void
nvkm_gsp_bar2_fini(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar2_pt *pt;

	sc->bar2.ready = false;
	while ((pt = LIST_FIRST(&sc->bar2.pt_pages)) != NULL) {
		LIST_REMOVE(pt, link);
		kfree(pt, M_NVKM_BAR2_PT);
	}
}

int
nvkm_gsp_bar2_map_vram(struct nvkm_softc *sc, uint64_t bar2_gva,
    uint64_t vram_paddr)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	struct nvkm_gsp_bar2_pt *spt_pt;
	uint32_t spt_idx;
	uint64_t pte;
	uint32_t saved;
	int err;

	if (!b2->ready)
		return (ENXIO);
	if ((bar2_gva & (NVKM_GMMU_PT_PAGE_SIZE - 1)) ||
	    (vram_paddr & (NVKM_GMMU_PT_PAGE_SIZE - 1)))
		return (EINVAL);
	if (bar2_gva >= b2->aperture_size)
		return (ERANGE);

	err = nvkm_gsp_bar2_get_spt(sc, bar2_gva, &spt_pt);
	if (err != 0)
		return (err);

	spt_idx = (uint32_t)((bar2_gva >> NVKM_GMMU_SPT_SHIFT)
	    & (NVKM_GMMU_SPT_ENTRIES - 1));
	pte = ((uint64_t)vram_paddr >> NV_PT_ADDR_SHIFT) | NV_PTE_VALID;

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b2_pramin_set_base(sc, spt_pt->paddr & ~(uint64_t)0xffffu);
	b2_pramin_wr64(sc, spt_pt->paddr + spt_idx * 8, pte);
	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

#ifdef NVKM_DEBUG_BAR2
	device_printf(sc->dev,
	    "bar2: map BAR2_GVA=0x%llx -> VRAM=0x%llx (SPT[%u] page=0x%llx pte=0x%016llx)\n",
	    (unsigned long long)bar2_gva, (unsigned long long)vram_paddr,
	    spt_idx, (unsigned long long)spt_pt->paddr,
	    (unsigned long long)pte);
#endif
	return (0);
}

void
nvkm_gsp_bar2_wr32(struct nvkm_softc *sc, uint64_t bar2_gva, uint32_t val)
{
	bus_write_4(sc->bar_res[3], (bus_size_t)bar2_gva, val);
}

uint32_t
nvkm_gsp_bar2_rd32(struct nvkm_softc *sc, uint64_t bar2_gva)
{
	return bus_read_4(sc->bar_res[3], (bus_size_t)bar2_gva);
}


/* Nouveau r535_bar_flush equivalent: read from BAR2 GVA 0 via PCIe BAR3.
 * Walker translates the GVA through our PDB->PD2->PD1->PD0->SPT chain
 * and returns data from the flush_vram page. The read itself serves as
 * the "flush" -- it forces walker to walk the chain and refresh TLB. */
void
nvkm_gsp_bar2_flush(struct nvkm_softc *sc)
{
	if (!sc->bar2.ready)
		return;
	(void)bus_read_4(sc->bar_res[3], (bus_size_t)BAR2_GVA_FLUSH);
}

void
nvkm_gsp_bar2_wr64(struct nvkm_softc *sc, uint64_t bar2_gva, uint64_t val)
{
	bus_write_4(sc->bar_res[3], (bus_size_t)(bar2_gva + 0),
	    (uint32_t)(val & 0xffffffffu));
	bus_write_4(sc->bar_res[3], (bus_size_t)(bar2_gva + 4),
	    (uint32_t)(val >> 32));
}

uint64_t
nvkm_gsp_bar2_rd64(struct nvkm_softc *sc, uint64_t bar2_gva)
{
	uint32_t lo = bus_read_4(sc->bar_res[3], (bus_size_t)(bar2_gva + 0));
	uint32_t hi = bus_read_4(sc->bar_res[3], (bus_size_t)(bar2_gva + 4));
	return ((uint64_t)hi << 32) | lo;
}


int
nvkm_gsp_pramin_rd64(struct nvkm_softc *sc, uint64_t paddr, uint64_t *out)
{
	uint32_t saved;
	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b2_pramin_set_base(sc, paddr & ~(uint64_t)0xffffu);
	uint32_t lo = nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((paddr + 0) & 0xffffu));
	uint32_t hi = nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((paddr + 4) & 0xffffu));
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);
	*out = ((uint64_t)hi << 32) | lo;
	return (0);
}
