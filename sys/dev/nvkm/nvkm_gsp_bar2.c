/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM BAR2 vmm (port of nouveau r535_bar_bar2_init).
 *
 * Build a fresh VRAM PT chain (PD2/PD1/PD0/SPT), PRAMIN-write it, then
 * send UPDATE_BAR_PDE BAR_2 RPC so GSP firmware installs our PD2-pointing
 * PDE into GSP\'s own BAR2 PDB.  The BAR2 walker (rooted at GSP\'s PDB)
 * then traverses our chain on every access.
 *
 * BAR2 gives us L2-coherent VRAM read/write -- needed to manipulate
 * BAR1 PT pages (which are in VRAM and must be visible to the BAR1
 * walker via L2).
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include <vm/vm.h>
#include <vm/pmap.h>

#define NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE 70
#define NV_RPC_UPDATE_PDE_BAR_2             1

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

int
nvkm_gsp_bar2_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	uint64_t pd2, pd1, pd0, spt;
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
	pd1 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	pd0 = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	spt = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	if (!pd2 || !pd1 || !pd0 || !spt) {
		device_printf(sc->dev, "bar2: VRAM alloc failed\n");
		return (ENOMEM);
	}

	b2->pd3_paddr = sc->gsp_bar2_pdb;
	b2->pd2_paddr = pd2;
	b2->pd1_paddr = pd1;
	b2->pd0_paddr = pd0;
	b2->spt_paddr = spt;
	b2->next_gva  = BAR2_GVA_ALLOC_BASE;

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b2_pramin_set_base(sc, pd2 & ~(uint64_t)0xffffu);

	uint64_t zpages[4] = { pd2, pd1, pd0, spt };
	for (int zi = 0; zi < 4; zi++) {
		for (uint32_t off = 0; off < 0x1000; off += 4)
			b2_pramin_wr32(sc, zpages[zi] + off, 0);
	}

	b2_pramin_wr64(sc, pd2 + 0,
	    (pd1 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	b2_pramin_wr64(sc, pd1 + 0,
	    (pd0 >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	b2_pramin_wr64(sc, pd0 + 0,
	    (spt >> NV_PT_ADDR_SHIFT) | NV_PDE_APERTURE_VRAM);
	b2_pramin_wr64(sc, pd0 + 8, 0);

	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

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

	device_printf(sc->dev,
	    "bar2: GSP PDB[0] pre RPC = 0x%08x:%08x, post RPC = 0x%08x:%08x, expected pde = 0x%llx\n",
	    pdb0_pre_hi, pdb0_pre_lo, pdb0_post_hi, pdb0_post_lo,
	    (unsigned long long)pd2_pde);

	/* Full TLB+PDB invalidate, per gf100_vmm_invalidate (vmmgf100.c).
	 * Sequence:
	 *   1. Wait for free slot: 0x100c80 & 0x00ff0000 != 0
	 *   2. Write PDB addr | aperture to 0x100cb8
	 *   3. Write 0x80000000 | type to 0x100cbc (REPLAY_NONE=0)
	 *   4. Wait trigger clear: 0x100cbc & 0x80000000 == 0
	 */
	uint64_t pdb_inv = (pdb_paddr >> 12) << 4;  /* aperture VRAM = 0 */
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
	    "bar2: PDB invalidate: slot 0x100c80=0x%x, 0x100cb8=0x%x, trigger 0x100cbc=0x%x\n",
	    inv_slot_rb, (uint32_t)pdb_inv, trig_rb);

	/* Read BAR2 inst reg (0xb80f48) -- the address walker uses as root. */
	uint32_t bar2_inst = nvkm_rd32(sc, 0xb80f48);
	uint32_t bar1_inst = nvkm_rd32(sc, 0xb80f40);
	uint64_t bar2_inst_paddr = ((uint64_t)(bar2_inst & 0x0fffffffu)) << 12;
	uint64_t bar1_inst_paddr = ((uint64_t)(bar1_inst & 0x0fffffffu)) << 12;
	device_printf(sc->dev,
	    "bar2: 0xb80f48=0x%08x (inst paddr=0x%llx), 0xb80f40=0x%08x (inst paddr=0x%llx)\n",
	    bar2_inst, (unsigned long long)bar2_inst_paddr,
	    bar1_inst, (unsigned long long)bar1_inst_paddr);

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
		device_printf(sc->dev,
		    "bar2: BAR2_inst[0x200..0x208] (PDB ptr) = 0x%08x:%08x\n",
		    inst_hi, inst_lo);
	}

	/* Immediate self-test: map test VRAM page into BAR2, write+read.
	 * This runs RIGHT after PDB[0] update -- before anything else has
	 * touched BAR2, so walker TLB has no stale entries. */
	{
		uint64_t tv = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
		if (tv != 0) {
			b2->ready = true;  /* enable map_vram early */
			(void)nvkm_gsp_bar2_map_vram(sc, 0x1000, tv);
			nvkm_gsp_bar2_wr32(sc, 0x1000 + 0x10, 0xC0FFEE12u);
			uint32_t rb = nvkm_gsp_bar2_rd32(sc, 0x1000 + 0x10);
			device_printf(sc->dev,
			    "bar2_diag: immediate test wr C0FFEE12, readback = 0x%08x (target VRAM 0x%llx)\n",
			    rb, (unsigned long long)tv);
			b2->next_gva = 0x2000;  /* skip the test page */
		}
	}

	device_printf(sc->dev,
	    "bar2: PT chain PD2=0x%llx PD1=0x%llx PD0=0x%llx SPT=0x%llx; "
	    "GSP PDB=0x%llx; UPDATE_BAR_PDE pde=0x%llx; BAR2@%llx %lluMiB\n",
	    (unsigned long long)pd2, (unsigned long long)pd1,
	    (unsigned long long)pd0, (unsigned long long)spt,
	    (unsigned long long)b2->pd3_paddr,
	    (unsigned long long)pd2_pde,
	    (unsigned long long)rman_get_start(sc->bar_res[3]),
	    (unsigned long long)rman_get_size(sc->bar_res[3]) >> 20);
	b2->ready = true;
	return (0);
}

void
nvkm_gsp_bar2_fini(struct nvkm_softc *sc)
{
	sc->bar2.ready = false;
}

int
nvkm_gsp_bar2_map_vram(struct nvkm_softc *sc, uint64_t bar2_gva,
    uint64_t vram_paddr)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	uint32_t spt_idx;
	uint64_t pte;
	uint32_t saved;

	if (!b2->ready)
		return (ENXIO);
	if ((bar2_gva & (NVKM_GMMU_PT_PAGE_SIZE - 1)) ||
	    (vram_paddr & (NVKM_GMMU_PT_PAGE_SIZE - 1)))
		return (EINVAL);
	if ((bar2_gva >> NVKM_GMMU_SPT_SHIFT) >= NVKM_GMMU_SPT_ENTRIES)
		return (ERANGE);

	spt_idx = (uint32_t)((bar2_gva >> NVKM_GMMU_SPT_SHIFT)
	    & (NVKM_GMMU_SPT_ENTRIES - 1));
	pte = ((uint64_t)vram_paddr >> NV_PT_ADDR_SHIFT) | NV_PTE_VALID;

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	b2_pramin_set_base(sc, b2->spt_paddr & ~(uint64_t)0xffffu);
	b2_pramin_wr64(sc, b2->spt_paddr + spt_idx * 8, pte);
	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	device_printf(sc->dev,
	    "bar2: map BAR2_GVA=0x%llx -> VRAM=0x%llx (SPT[%u]=0x%016llx)\n",
	    (unsigned long long)bar2_gva, (unsigned long long)vram_paddr,
	    spt_idx, (unsigned long long)pte);
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
