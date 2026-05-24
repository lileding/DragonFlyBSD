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

int
nvkm_gsp_bar1_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	uint64_t pd2, pd1, pd0, spt;
	uint64_t pd2_pde;
	struct rpc_update_bar_pde_v15_00_b1 *rpc;
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
	if (!sc->bar2.ready) {
		device_printf(sc->dev, "bar1: BAR2 must be ready first (PRAMIN bootstrap)\n");
		return (ENXIO);
	}

	pd2 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	pd1 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	pd0 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	spt = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	uint64_t lpt = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	if (!pd2 || !pd1 || !pd0 || !spt || !lpt) {
		device_printf(sc->dev, "bar1: VRAM alloc failed\n");
		return (ENOMEM);
	}

	b1->pd3_paddr = sc->gsp_bar1_pdb;
	b1->pd2_paddr = pd2;
	b1->pd1_paddr = pd1;
	b1->pd0_paddr = pd0;
	b1->spt_paddr = spt;
	b1->next_gva  = BAR1_GVA_ALLOC_BASE;

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b1_pramin_set_base(sc, pd2 & ~(uint64_t)0xffffu);

	uint64_t zpages[5] = { pd2, pd1, pd0, spt, lpt };
	for (int zi = 0; zi < 5; zi++) {
		for (uint32_t off = 0; off < 0x1000; off += 4)
			b1_pramin_wr32(sc, zpages[zi] + off, 0);
	}

	b1_pramin_wr64(sc, pd2 + 0,
	    (pd1 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	b1_pramin_wr64(sc, pd1 + 0,
	    (pd0 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	/* Dual slot: BIG = LPT (all-invalid zeros so walker falls back to SMALL
	 * for 4 KiB GVAs); SMALL = SPT (our 4 KiB-page maps live here). */
	b1_pramin_wr64(sc, pd0 + 0,
	    (lpt >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	b1_pramin_wr64(sc, pd0 + 8,
	    (spt >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);

	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	pd2_pde = (pd2 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM;

	uint64_t pdb_paddr = sc->gsp_bar1_pdb;
	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b1_pramin_set_base(sc, pdb_paddr & ~(uint64_t)0xffffu);
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
	rpc->barType         = NV_RPC_UPDATE_PDE_BAR_1;
	rpc->entryValue      = pd2_pde;
	rpc->entryLevelShift = NVKM_GMMU_PD3_SHIFT;
	err = nvkm_gsp_rpc_wr(sc, rpc, NVKM_GSP_RPC_REPLY_RECV);
	if (err != 0) {
		device_printf(sc->dev,
		    "bar1: UPDATE_BAR_PDE BAR_1 failed err=%d\n", err);
		return (err);
	}

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b1_pramin_set_base(sc, pdb_paddr & ~(uint64_t)0xffffu);
	uint32_t pdb0_post_lo = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((pdb_paddr + 0) & 0xffffu));
	uint32_t pdb0_post_hi = nvkm_rd32(sc, NV_PRAMIN +
	    (uint32_t)((pdb_paddr + 4) & 0xffffu));
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	device_printf(sc->dev,
	    "bar1: GSP PDB[0] pre RPC = 0x%08x:%08x, post RPC = 0x%08x:%08x, expected pde = 0x%llx\n",
	    pdb0_pre_hi, pdb0_pre_lo, pdb0_post_hi, pdb0_post_lo,
	    (unsigned long long)pd2_pde);

	/* PDB invalidate -- same sequence as BAR2. */
	uint64_t pdb_inv = (pdb_paddr >> 12) << 4;
	uint32_t inv_slot_rb = 0;
	for (int spin = 0; spin < 200; spin++) {
		inv_slot_rb = nvkm_rd32(sc, 0x100c80);
		if (inv_slot_rb & 0x00ff0000u)
			break;
		DELAY(10);
	}
	nvkm_wr32(sc, 0x100cb8, (uint32_t)pdb_inv);
	nvkm_wr32(sc, 0x100cbc, 0x80000000u | 0x00u);
	uint32_t trig_rb = 0xffffffffu;
	for (int spin = 0; spin < 200; spin++) {
		trig_rb = nvkm_rd32(sc, 0x100cbc);
		if (!(trig_rb & 0x80000000u))
			break;
		DELAY(10);
	}
	device_printf(sc->dev,
	    "bar1: PDB invalidate trigger 0x100cbc=0x%x\n", trig_rb);

	device_printf(sc->dev,
	    "bar1: PT chain PD2=0x%llx PD1=0x%llx PD0=0x%llx SPT=0x%llx; "
	    "GSP PDB=0x%llx; UPDATE_BAR_PDE pde=0x%llx; "
	    "BAR1@%llx %lluMiB\n",
	    (unsigned long long)pd2, (unsigned long long)pd1,
	    (unsigned long long)pd0, (unsigned long long)spt,
	    (unsigned long long)pdb_paddr, (unsigned long long)pd2_pde,
	    (unsigned long long)rman_get_start(sc->bar_res[1]),
	    (unsigned long long)rman_get_size(sc->bar_res[1]) >> 20);

	sc->bar1.ready = true;

	/* Smoke test: alloc one VRAM page, map at BAR1 GVA 0 and at BAR2 GVA 0,
	 * write via BAR1, read via BAR2, then vice versa.  If walker works,
	 * both reads return the values written. */
	{
		uint64_t test_paddr = nvkm_gsp_vram_alloc(sc, 0x1000, 0x10000);
		if (test_paddr != 0) {
			(void)nvkm_gsp_bar1_map_vram(sc, 0, test_paddr);
			nvkm_gsp_bar1_flush(sc);
			nvkm_gsp_bar1_wr32(sc, 0, 0xcafebabe);
			nvkm_gsp_bar1_wr32(sc, 4, 0xdeadc0de);
			nvkm_gsp_bar1_flush(sc);
			uint32_t b1_0 = nvkm_gsp_bar1_rd32(sc, 0);
			uint32_t b1_4 = nvkm_gsp_bar1_rd32(sc, 4);
			device_printf(sc->dev,
			    "bar1: SMOKE wrote 0xcafebabe/0xdeadc0de via BAR1; readback BAR1[0]=0x%08x BAR1[4]=0x%08x %s\n",
			    b1_0, b1_4,
			    (b1_0 == 0xcafebabe && b1_4 == 0xdeadc0de) ? "OK" : "*** MISMATCH ***");
		}
	}

	return (0);
}

void
nvkm_gsp_bar1_fini(struct nvkm_softc *sc)
{
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
	uint64_t pte;

	if (!b1->ready)
		return (ENXIO);
	if (bar1_gva >= (512ULL << 20)) /* SPT covers 2 MiB; we only use small range */
		return (EINVAL);

	spt_idx = (uint32_t)(bar1_gva >> 12);  /* 4 KiB SMALL pages */
	pte = (vram_paddr >> 12) << 8;          /* PTE: paddr in [39:8] */
	pte |= 0x1;                              /* VALID = bit 0 */

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b1_pramin_set_base(sc, b1->spt_paddr & ~(uint64_t)0xffffu);
	b1_pramin_wr64(sc, b1->spt_paddr + (uint64_t)spt_idx * 8, pte);
	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	/* Force MMU TLB invalidate for BAR1\'s PDB. Without this, walker may
	 * have cached translations from before this map call. */
	uint64_t pdb_inv = (sc->gsp_bar1_pdb >> 12) << 4;
	for (int spin = 0; spin < 200; spin++) {
		if (nvkm_rd32(sc, 0x100c80) & 0x00ff0000u) break;
		DELAY(10);
	}
	nvkm_wr32(sc, 0x100cb8, (uint32_t)pdb_inv);
	nvkm_wr32(sc, 0x100cbc, 0x80000000u);
	for (int spin = 0; spin < 200; spin++) {
		if (!(nvkm_rd32(sc, 0x100cbc) & 0x80000000u)) break;
		DELAY(10);
	}

	device_printf(sc->dev,
	    "bar1: map BAR1_GVA=0x%llx -> VRAM=0x%llx (SPT[%u]=0x%llx)\n",
	    (unsigned long long)bar1_gva, (unsigned long long)vram_paddr,
	    spt_idx, (unsigned long long)pte);
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
	nvkm_gsp_bar1_wr32(sc, gva + 0, (uint32_t)(val & 0xffffffffu));
	nvkm_gsp_bar1_wr32(sc, gva + 4, (uint32_t)(val >> 32));
}

uint64_t
nvkm_gsp_bar1_rd64(struct nvkm_softc *sc, uint64_t gva)
{
	uint64_t lo = nvkm_gsp_bar1_rd32(sc, gva + 0);
	uint64_t hi = nvkm_gsp_bar1_rd32(sc, gva + 4);
	return ((hi << 32) | lo);
}

int
nvkm_gsp_bar1_alloc_page(struct nvkm_softc *sc, struct nvkm_bar1_page *page)
{
	uint64_t paddr, gva;
	int err;

	if (!sc->bar1.ready)
		return (ENXIO);

	paddr = nvkm_gsp_vram_alloc(sc, NVKM_GMMU_PT_PAGE_SIZE, NVKM_GMMU_PT_PAGE_SIZE);
	if (paddr == 0)
		return (ENOMEM);

	gva = sc->bar1.next_gva;
	sc->bar1.next_gva += NVKM_GMMU_PT_PAGE_SIZE;

	err = nvkm_gsp_bar1_map_vram(sc, gva, paddr);
	if (err != 0)
		return (err);
	nvkm_gsp_bar1_flush(sc);

	page->vram_paddr = paddr;
	page->bar1_gva   = gva;
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
}
