/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM BAR1 vmm (port of nouveau r535_bar_bar1_init).
 *
 * GSP allocates a BAR1 PDB at boot and publishes its physaddr via
 * static_info bar1PdeBase. Walker reads PDB via GPU L2 -- so any write
 * to PDB[0] must be L2-coherent. PRAMIN bypasses L2 and is silently lost.
 *
 * nouveau's solution:
 *   - Pre-build a fresh PT chain (PD2/PD1/PD0/SPT) in VRAM. These are
 *     fresh pages walker has never touched, so first L2 fetch == DRAM
 *     contents. PRAMIN-writing those pages at init time IS visible to
 *     the walker the first time it reaches them.
 *   - Send UPDATE_BAR_PDE RPC with PDE pointing at our fresh PD2.
 *     GSP firmware writes the PDE into its BAR1 PDB from inside the
 *     GPU (L2-coherent path).
 *
 * For new SPT entries added later: we use PRAMIN-write while no BAR1
 * access has touched the SPT page yet. After first BAR1 access, the
 * SPT page is L2-cached and subsequent PRAMIN writes are lost.
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/pmap.h>

/* RPC fn 70 = UPDATE_BAR_PDE. */
#define NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE 70
#define NV_RPC_UPDATE_PDE_BAR_1             0
#define NV_RPC_UPDATE_PDE_BAR_2             1

struct rpc_update_bar_pde_v15_00 {
	uint32_t barType;
	uint8_t  _pad[4];
	uint64_t entryValue;
	uint64_t entryLevelShift;
};

/* === PRAMIN helpers (window = 64 KiB) === */

static __inline void
pramin_set_base(struct nvkm_softc *sc, uint64_t paddr)
{
	nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(paddr >> 16));
}

static __inline void
pramin_wr32(struct nvkm_softc *sc, uint64_t paddr, uint32_t val)
{
	nvkm_wr32(sc, NV_PRAMIN + (uint32_t)(paddr & 0xffffu), val);
}

static __inline void
pramin_wr64(struct nvkm_softc *sc, uint64_t paddr, uint64_t val)
{
	pramin_wr32(sc, paddr + 0, (uint32_t)(val & 0xffffffffu));
	pramin_wr32(sc, paddr + 4, (uint32_t)(val >> 32));
}

/* === init === */

int
nvkm_gsp_bar1_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint64_t pd2, pd1, pd0, spt;
	uint64_t pd2_pde;
	struct rpc_update_bar_pde_v15_00 *rpc;
	int err;
	uint32_t saved;

	if (sc->bar_res[1] == NULL) {
		device_printf(sc->dev, "bar1: PCIe BAR1 not mapped\n");
		return (ENXIO);
	}
	if (sc->gsp_bar1_pdb == 0) {
		device_printf(sc->dev, "bar1: GSP did not publish bar1PdeBase\n");
		return (ENXIO);
	}

	/* Fresh VRAM PT chain (PD3 is GSP's PDB itself; we build PD2 down). */
	pd2 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	pd1 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	pd0 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	spt = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	if (!pd2 || !pd1 || !pd0 || !spt) {
		device_printf(sc->dev, "bar1: VRAM alloc failed\n");
		return (ENOMEM);
	}

	b1->pd3_paddr = sc->gsp_bar1_pdb;  /* root = GSP's PDB */
	b1->pd2_paddr = pd2;
	b1->pd1_paddr = pd1;
	b1->pd0_paddr = pd0;
	b1->spt_paddr = spt;
	b1->next_gva  = BAR1_GVA_ALLOC_BASE;

	/* Pages are fresh (walker has never accessed). PRAMIN-write each.
	 * vram_alloc returned adjacent pages -> single 64 KiB PRAMIN window. */
	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	pramin_set_base(sc, pd2 & ~(uint64_t)0xffffu);

	/* Zero our 4 fresh pages then install the chain. */
	uint64_t zpages[4] = { pd2, pd1, pd0, spt };
	for (int zi = 0; zi < 4; zi++) {
		for (uint32_t off = 0; off < 0x1000; off += 4)
			pramin_wr32(sc, zpages[zi] + off, 0);
	}

	/* PDE encoding: VRAM aperture bits 2:1 = 01 (= 1<<1 = 2), no VOL. */
	pramin_wr64(sc, pd2 + 0,
	    (pd1 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	pramin_wr64(sc, pd1 + 0,
	    (pd0 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	/* PD0 dual entry: [0]=small (SPT), [1]=big (none). */
	pramin_wr64(sc, pd0 + 0,
	    (spt >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	pramin_wr64(sc, pd0 + 8, 0);
	/* SPT starts empty; entries via bar1_alloc_page / map_vram. */

	(void)nvkm_rd32(sc, NV_PRAMIN);  /* flush PRAMIN posted writes */
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	/* Send UPDATE_BAR_PDE RPC: GSP firmware writes our PDE into its
	 * BAR1 PDB[0] from inside the GPU (L2-coherent). */
	pd2_pde = (pd2 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM;
	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE,
	    sizeof(*rpc));
	if (rpc == NULL)
		return (ENOMEM);
	rpc->barType         = NV_RPC_UPDATE_PDE_BAR_1;
	rpc->entryValue      = pd2_pde;
	rpc->entryLevelShift = NVKM_GMMU_PD3_SHIFT;
	err = nvkm_gsp_rpc_wr(sc, rpc, NVKM_GSP_RPC_REPLY_RECV);
	if (err != 0) {
		device_printf(sc->dev,
		    "bar1: UPDATE_BAR_PDE BAR_1 failed err=%d\n", err);
		return (err);
	}

	device_printf(sc->dev,
	    "bar1: VRAM PT chain PD2=0x%llx PD1=0x%llx PD0=0x%llx SPT=0x%llx; "
	    "GSP PDB=0x%llx; UPDATE_BAR_PDE installed pde=0x%llx\n",
	    (unsigned long long)pd2, (unsigned long long)pd1,
	    (unsigned long long)pd0, (unsigned long long)spt,
	    (unsigned long long)b1->pd3_paddr,
	    (unsigned long long)pd2_pde);
	b1->ready = true;
	return (0);
}

void
nvkm_gsp_bar1_fini(struct nvkm_softc *sc)
{
	sc->bar1.ready = false;
}

int
nvkm_gsp_bar1_map_vram(struct nvkm_softc *sc, uint64_t bar1_gva,
    uint64_t vram_paddr)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint32_t spt_idx;
	uint64_t pte;
	uint32_t saved;

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

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	pramin_set_base(sc, b1->spt_paddr & ~(uint64_t)0xffffu);
	pramin_wr64(sc, b1->spt_paddr + spt_idx * 8, pte);
	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	device_printf(sc->dev,
	    "bar1: map BAR1_GVA=0x%llx -> VRAM=0x%llx (SPT[%u]=0x%016llx)\n",
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

/* Diag stub (referenced from chan_ctor diag block). */
void
nvkm_gsp_bar1_dump_pt(struct nvkm_softc *sc, uint64_t target_paddr,
    uint32_t target_off)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint32_t saved;

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	pramin_set_base(sc, b1->spt_paddr & ~(uint64_t)0xffffu);
	uint32_t s0lo = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((b1->spt_paddr + 0) & 0xffffu));
	uint32_t s0hi = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((b1->spt_paddr + 4) & 0xffffu));
	device_printf(sc->dev,
	    "bar1_diag: SPT[0] PRAMIN readback = 0x%08x:%08x\n", s0hi, s0lo);
	pramin_set_base(sc, target_paddr & ~(uint64_t)0xffffu);
	uint32_t v = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((target_paddr + target_off) & 0xffffu));
	device_printf(sc->dev,
	    "bar1_diag: VRAM 0x%llx+0x%x via PRAMIN = 0x%08x\n",
	    (unsigned long long)target_paddr, target_off, v);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);
}
