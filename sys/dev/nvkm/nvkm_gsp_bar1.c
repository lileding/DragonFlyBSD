/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM BAR1 vmm (port of nouveau r535_bar_bar1_init).
 *
 * Strategy mirrors nouveau verbatim:
 *   1. Adopt GSP\'s BAR1 PDB at sc->gsp_bar1_pdb (root of BAR1 walker chain).
 *   2. Allocate fresh VRAM pages for PD2, PD1, PD0, SPT.
 *   3. Map every PT page + the GSP PDB into BAR2 at reserved GVAs so we can
 *      L2-coherently write them.
 *   4. Write the PT chain via BAR2:
 *         GSP BAR1 PDB[0] = pde_to_vram(our PD2)
 *         our PD2[0]       = pde_to_vram(our PD1)
 *         our PD1[0]       = pde_to_vram(our PD0)
 *         our PD0[0]       = pde_to_vram(our SPT)     // small
 *         our PD0[1]       = 0                         // big (none)
 *   5. bar2_flush() after every batch of writes.
 *   6. Eventually send UPDATE_BAR_PDE BAR_1 RPC (belt + suspenders -- GSP
 *      side may also trigger walker invalidate, even if RPC is STUB host-side).
 *
 * Later, nvkm_gsp_bar1_map_vram(gva, vram) writes SPT[idx] via BAR2 and
 * flushes.
 *
 * Reference: linux/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/bar.c
 *            :r535_bar_bar1_init.
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
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

/* Map one VRAM page into BAR2 at the next free GVA; return its BAR2 GVA. */
static uint64_t
bar1_map_into_bar2(struct nvkm_softc *sc, uint64_t vram_paddr)
{
	uint64_t gva = sc->bar2.next_gva;
	sc->bar2.next_gva += 0x1000;
	if (nvkm_gsp_bar2_map_vram(sc, gva, vram_paddr) != 0)
		return 0;
	return gva;
}

int
nvkm_gsp_bar1_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint64_t pd2, pd1, pd0, spt;
	uint64_t pd2_b2, pd1_b2, pd0_b2, spt_b2, pdb_b2;
	uint64_t pd2_pde;
	struct rpc_update_bar_pde_v15_00_b1 *rpc;
	int err;

	if (sc->bar_res[1] == NULL) {
		device_printf(sc->dev, "bar1: PCIe BAR1 not mapped\n");
		return (ENXIO);
	}
	if (!sc->bar2.ready) {
		device_printf(sc->dev, "bar1: BAR2 not ready (required)\n");
		return (ENXIO);
	}
	if (sc->gsp_bar1_pdb == 0) {
		device_printf(sc->dev, "bar1: GSP did not publish bar1PdeBase\n");
		return (ENXIO);
	}

	/* Diag: read BAR1 inst block PDB ptr to verify it matches sc->gsp_bar1_pdb. */
	{
		uint32_t bar1_inst_reg = nvkm_rd32(sc, 0xb80f40);
		uint64_t bar1_inst = ((uint64_t)(bar1_inst_reg & 0x0fffffffu)) << 12;
		uint64_t pdb_b2_tmp = nvkm_gsp_bar2_map_vram(sc, sc->bar2.next_gva, bar1_inst);
		(void)pdb_b2_tmp;
		uint64_t inst_b2_gva = sc->bar2.next_gva;
		sc->bar2.next_gva += 0x1000;
		nvkm_gsp_bar2_flush(sc);
		uint32_t inst_pdb_lo = nvkm_gsp_bar2_rd32(sc, inst_b2_gva + 0x200);
		uint32_t inst_pdb_hi = nvkm_gsp_bar2_rd32(sc, inst_b2_gva + 0x204);
		uint64_t inst_pdb_raw = ((uint64_t)inst_pdb_hi << 32) | inst_pdb_lo;
		uint64_t inst_pdb_paddr = inst_pdb_raw & ~(uint64_t)0xfff;  /* strip aper/flags */
		device_printf(sc->dev,
		    "bar1: 0xb80f40=0x%08x -> inst paddr=0x%llx; inst[0x200]=0x%llx -> PDB paddr=0x%llx; sc->gsp_bar1_pdb=0x%llx\n",
		    bar1_inst_reg, (unsigned long long)bar1_inst,
		    (unsigned long long)inst_pdb_raw,
		    (unsigned long long)inst_pdb_paddr,
		    (unsigned long long)sc->gsp_bar1_pdb);
	}

	/* Also dump first u64 of sc->gsp_bar1_pdb in case it\'s itself an inst block. */
	{
		uint64_t b1pdb_b2 = sc->bar2.next_gva;
		sc->bar2.next_gva += 0x1000;
		(void)nvkm_gsp_bar2_map_vram(sc, b1pdb_b2, sc->gsp_bar1_pdb);
		nvkm_gsp_bar2_flush(sc);
		uint32_t v0_lo = nvkm_gsp_bar2_rd32(sc, b1pdb_b2 + 0);
		uint32_t v0_hi = nvkm_gsp_bar2_rd32(sc, b1pdb_b2 + 4);
		uint32_t v200_lo = nvkm_gsp_bar2_rd32(sc, b1pdb_b2 + 0x200);
		uint32_t v200_hi = nvkm_gsp_bar2_rd32(sc, b1pdb_b2 + 0x204);
		device_printf(sc->dev,
		    "bar1: gsp_bar1_pdb @ 0x%llx contents: [0]=0x%08x:%08x [0x200]=0x%08x:%08x\n",
		    (unsigned long long)sc->gsp_bar1_pdb,
		    v0_hi, v0_lo, v200_hi, v200_lo);
	}

	/* Step 1: allocate fresh PT pages in VRAM. */
	pd2 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	pd1 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	pd0 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	spt = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	if (!pd2 || !pd1 || !pd0 || !spt) {
		device_printf(sc->dev, "bar1: VRAM alloc failed\n");
		return (ENOMEM);
	}

	b1->pd3_paddr = sc->gsp_bar1_pdb;
	b1->pd2_paddr = pd2;
	b1->pd1_paddr = pd1;
	b1->pd0_paddr = pd0;
	b1->spt_paddr = spt;
	b1->next_gva  = BAR1_GVA_ALLOC_BASE;

	/* Step 2: map each PT page + GSP\'s BAR1 PDB into BAR2 so we can
	 * L2-coherently read/write them. */
	pd2_b2 = bar1_map_into_bar2(sc, pd2);
	pd1_b2 = bar1_map_into_bar2(sc, pd1);
	pd0_b2 = bar1_map_into_bar2(sc, pd0);
	spt_b2 = bar1_map_into_bar2(sc, spt);
	pdb_b2 = bar1_map_into_bar2(sc, sc->gsp_bar1_pdb);
	if (!pd2_b2 || !pd1_b2 || !pd0_b2 || !spt_b2 || !pdb_b2) {
		device_printf(sc->dev, "bar1: BAR2 map failed\n");
		return (ENXIO);
	}
	b1->spt_bar2_gva = spt_b2;	/* used by bar1_map_vram */

	nvkm_gsp_bar2_flush(sc);

	/* Step 3: zero our 4 PT pages via BAR2 (L2-coherent). */
	uint64_t zpages[4] = { pd2_b2, pd1_b2, pd0_b2, spt_b2 };
	for (int zi = 0; zi < 4; zi++)
		for (uint32_t off = 0; off < 0x1000; off += 4)
			nvkm_gsp_bar2_wr32(sc, zpages[zi] + off, 0);

	nvkm_gsp_bar2_flush(sc);

	/* Step 4: write the PT chain via BAR2. PDE encoding =
	 * (paddr >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM (no VOL). */
	pd2_pde = (pd2 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM;
	nvkm_gsp_bar2_wr64(sc, pd2_b2 + 0,
	    (pd1 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	nvkm_gsp_bar2_wr64(sc, pd1_b2 + 0,
	    (pd0 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	/* PD0 dual entry: low = small (SPT), high = big (LPT none). */
	nvkm_gsp_bar2_wr64(sc, pd0_b2 + 0,
	    (spt >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	nvkm_gsp_bar2_wr64(sc, pd0_b2 + 8, 0);

	/* Write GSP\'s BAR1 PDB[0] = pde_to_vram(our PD2). Walker rooted at
	 * GSP\'s PDB now traverses our chain. */
	nvkm_gsp_bar2_wr64(sc, pdb_b2 + 0, pd2_pde);

	nvkm_gsp_bar2_flush(sc);

	/* Verify: read PDB[0] back via BAR2 (L2-coherent) -- should show
	 * our PDE if write landed in L2. */
	uint64_t pdb0_check = nvkm_gsp_bar2_rd64(sc, pdb_b2 + 0);
	device_printf(sc->dev,
	    "bar1: PDB[0] readback via BAR2 = 0x%llx (expected 0x%llx)\n",
	    (unsigned long long)pdb0_check, (unsigned long long)pd2_pde);

	/* BAR1 walker invalidate: toggle 0xb80f40 bit 31 (BAR1_BLOCK enable).
	 * Per tu102_bar.c:tu102_bar_bar1_init -- write same inst addr with
	 * enable bit set. We don\'t need to repoint, just touching the reg
	 * may force walker re-init. */
	uint32_t bar1_reg = nvkm_rd32(sc, 0xb80f40);
	nvkm_wr32(sc, 0xb80f40, bar1_reg & ~0x80000000u);  /* disable */
	DELAY(10);
	nvkm_wr32(sc, 0xb80f40, bar1_reg);                  /* re-enable same inst */
	device_printf(sc->dev,
	    "bar1: toggled 0xb80f40 (was 0x%08x) to force walker re-init\n",
	    bar1_reg);

	/* Step 5: belt + suspenders -- send UPDATE_BAR_PDE BAR_1 RPC. May be
	 * STUB on TU10X host side but GSP firmware may still process it
	 * (e.g., to invalidate walker TLB). */
	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE,
	    sizeof(*rpc));
	if (rpc != NULL) {
		rpc->barType         = NV_RPC_UPDATE_PDE_BAR_1;
		rpc->entryValue      = pd2_pde;
		rpc->entryLevelShift = NVKM_GMMU_PD3_SHIFT;
		err = nvkm_gsp_rpc_wr(sc, rpc, NVKM_GSP_RPC_REPLY_RECV);
		if (err != 0)
			device_printf(sc->dev,
			    "bar1: UPDATE_BAR_PDE BAR_1 returned err=%d (non-fatal)\n",
			    err);
	}

	device_printf(sc->dev,
	    "bar1: PT via BAR2: PD2=0x%llx@b2=0x%llx PD1=0x%llx@b2=0x%llx "
	    "PD0=0x%llx@b2=0x%llx SPT=0x%llx@b2=0x%llx; GSP PDB=0x%llx@b2=0x%llx; "
	    "pd2_pde=0x%llx\n",
	    (unsigned long long)pd2, (unsigned long long)pd2_b2,
	    (unsigned long long)pd1, (unsigned long long)pd1_b2,
	    (unsigned long long)pd0, (unsigned long long)pd0_b2,
	    (unsigned long long)spt, (unsigned long long)spt_b2,
	    (unsigned long long)b1->pd3_paddr, (unsigned long long)pdb_b2,
	    (unsigned long long)pd2_pde);

	b1->ready = true;

	/* Sanity test: map a fresh VRAM page at BAR1 GVA 0x1000, write+read. */
	{
		uint64_t tv = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
		if (tv != 0) {
			(void)nvkm_gsp_bar1_map_vram(sc, 0x1000, tv);
			nvkm_gsp_bar1_wr32(sc, 0x1000 + 0x10, 0xFEEDFACEu);
			uint32_t rb = nvkm_gsp_bar1_rd32(sc, 0x1000 + 0x10);
			device_printf(sc->dev,
			    "bar1_diag: wr FEEDFACE @ BAR1 GVA 0x1010, readback = 0x%08x\n", rb);
		}
	}

	return (0);
}

void
nvkm_gsp_bar1_fini(struct nvkm_softc *sc)
{
	sc->bar1.ready = false;
}

/* Install SPT entry via BAR2 (L2-coherent). Caller may need bar_flush after
 * batch. */
int
nvkm_gsp_bar1_map_vram(struct nvkm_softc *sc, uint64_t bar1_gva,
    uint64_t vram_paddr)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint32_t spt_idx;
	uint64_t pte;

	if (!b1->ready)
		return (ENXIO);
	if ((bar1_gva & (NVKM_GMMU_PT_PAGE_SIZE - 1)) ||
	    (vram_paddr & (NVKM_GMMU_PT_PAGE_SIZE - 1)))
		return (EINVAL);
	if ((bar1_gva >> NVKM_GMMU_SPT_SHIFT) >= NVKM_GMMU_SPT_ENTRIES)
		return (ERANGE);

	spt_idx = (uint32_t)((bar1_gva >> NVKM_GMMU_SPT_SHIFT)
	    & (NVKM_GMMU_SPT_ENTRIES - 1));
	pte = ((uint64_t)vram_paddr >> NV_PT_ADDR_SHIFT) | NV_PTE_VALID;

	/* Write SPT[idx] via BAR2 (L2-coherent), then flush so BAR1 walker
	 * sees the new entry. */
	nvkm_gsp_bar2_wr64(sc, b1->spt_bar2_gva + spt_idx * 8, pte);
	nvkm_gsp_bar2_flush(sc);

	device_printf(sc->dev,
	    "bar1: map BAR1_GVA=0x%llx -> VRAM=0x%llx (SPT[%u]=0x%016llx via BAR2)\n",
	    (unsigned long long)bar1_gva, (unsigned long long)vram_paddr,
	    spt_idx, (unsigned long long)pte);
	return (0);
}

void
nvkm_gsp_bar1_wr32(struct nvkm_softc *sc, uint64_t bar1_gva, uint32_t val)
{
	bus_write_4(sc->bar_res[1], (bus_size_t)bar1_gva, val);
}

uint32_t
nvkm_gsp_bar1_rd32(struct nvkm_softc *sc, uint64_t bar1_gva)
{
	return bus_read_4(sc->bar_res[1], (bus_size_t)bar1_gva);
}

void
nvkm_gsp_bar1_wr64(struct nvkm_softc *sc, uint64_t bar1_gva, uint64_t val)
{
	bus_write_4(sc->bar_res[1], (bus_size_t)(bar1_gva + 0),
	    (uint32_t)(val & 0xffffffffu));
	bus_write_4(sc->bar_res[1], (bus_size_t)(bar1_gva + 4),
	    (uint32_t)(val >> 32));
}

uint64_t
nvkm_gsp_bar1_rd64(struct nvkm_softc *sc, uint64_t bar1_gva)
{
	uint32_t lo = bus_read_4(sc->bar_res[1], (bus_size_t)(bar1_gva + 0));
	uint32_t hi = bus_read_4(sc->bar_res[1], (bus_size_t)(bar1_gva + 4));
	return ((uint64_t)hi << 32) | lo;
}

int
nvkm_gsp_bar1_alloc_page(struct nvkm_softc *sc, struct nvkm_bar1_page *page)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint64_t paddr;
	int err;

	if (!b1->ready)
		return (ENXIO);
	if ((b1->next_gva >> NVKM_GMMU_SPT_SHIFT) >= NVKM_GMMU_SPT_ENTRIES)
		return (ENOSPC);

	paddr = nvkm_gsp_vram_alloc(sc, NVKM_GMMU_PT_PAGE_SIZE,
	    NVKM_GMMU_PT_PAGE_SIZE);
	if (paddr == 0)
		return (ENOMEM);

	page->vram_paddr = paddr;
	page->bar1_gva   = b1->next_gva;
	b1->next_gva += NVKM_GMMU_PT_PAGE_SIZE;

	err = nvkm_gsp_bar1_map_vram(sc, page->bar1_gva, page->vram_paddr);
	if (err != 0) {
		page->vram_paddr = 0;
		page->bar1_gva   = 0;
		return (err);
	}
	return (0);
}

void
nvkm_gsp_bar1_free_page(struct nvkm_softc *sc __unused,
    struct nvkm_bar1_page *page)
{
	page->vram_paddr = 0;
	page->bar1_gva   = 0;
}

void
nvkm_gsp_bar1_dump_pt(struct nvkm_softc *sc __unused,
    uint64_t target_paddr __unused, uint32_t target_off __unused)
{
	/* No longer used. */
}
