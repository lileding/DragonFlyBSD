/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM BAR2 vmm — strict nouveau mirror.
 *
 * PT chain (PD2/PD1/PD0/SPT) is ALL in VRAM (NVKM_MEM_TARGET_INST).
 *   - nouveau ref: nvkm/subdev/mmu/base.c:208 nvkm_mmu_ptc_get uses
 *     NVKM_MEM_TARGET_INST for PT pages.
 * PDE aperture for VRAM = 1 (no VOL).
 *   - nouveau ref: nvkm/subdev/mmu/vmmgp100.c:243 gp100_vmm_pde, case
 *     NVKM_MEM_TARGET_VRAM does `*data |= 1ULL << 1`.
 * PTE aperture for VRAM = 0 (just VALID + address).
 *   - nouveau ref: nvkm/subdev/mmu/vmmgf100.c:327 gf100_vmm_aper
 *     case NVKM_MEM_TARGET_VRAM returns 0; gp100_vmm_pgt_pte writes
 *     `data = (addr>>4) | map->type` with `map->type |= aper<<1 | 1`.
 *   - hw ref: tu102/dev_mmu.h:80 NV_MMU_PTE_APERTURE_VIDEO_MEMORY = 0.
 *
 * Bootstrap (PT writes happen-once, fresh VRAM) uses PRAMIN backdoor.
 *   - nouveau ref: nvkm/subdev/instmem/nv50.c:57-73 nv50_instobj_wr32_slow
 *     writes via NV_PBUS_PRAMIN + (BAR0 + 0x700000 + offset).
 *
 * Runtime SPT updates (new BAR2 mappings) write via BAR2 itself.
 *   - nouveau ref: nv50_instobj_kmap self-maps inst memory into BAR2,
 *     then nv50_instobj_wr32 uses iowrite32_native through that KVA.
 *   - We do the moral equivalent: PRAMIN-write one SPT entry during
 *     init that self-maps the SPT page at BAR2 GVA SPT_SELF_GVA, then
 *     all subsequent SPT entry writes go through that BAR2 KVA.
 *
 * Invariant after init: a single VRAM PT page is touched ONLY via
 *     PRAMIN (bootstrap) OR ONLY via BAR2 (runtime). Never mixed.
 *     Mixed access on the same cache line causes the GMMU walker to
 *     see stale data because PRAMIN bypasses GPU L2.
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/pmap.h>

static MALLOC_DEFINE(M_NVKM_BAR2, "nvkm_bar2", "nvkm BAR2 PT pages");

/* RPC bits (nvkm/subdev/gsp/rm/r535/nvrm/bar.h:13-28, r570/rpcfn.h:86). */
#define NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE	70
#define NV_RPC_UPDATE_PDE_BAR_2			1

struct rpc_update_bar_pde_v15_00 {
	uint32_t barType;
	uint8_t  _pad[4];		/* NV_ALIGN_BYTES(8) before entryValue */
	uint64_t entryValue;
	uint64_t entryLevelShift;
};

/* PDE encoding -- VRAM target. */
static __inline uint64_t
pde_vram(uint64_t paddr)
{
	return (paddr >> 4) | (1ULL << 1);	/* aperture=VRAM=1, no VOL */
}

/* Leaf PTE encoding -- VRAM target (PTE aperture for VRAM = 0). */
static __inline uint64_t
pte_vram(uint64_t paddr)
{
	return (paddr >> 4) | 1ULL;		/* aperture=0, VALID=1 */
}

/* Reserve one SPT slot to self-map the SPT page itself, so post-init
 * SPT updates can be written through BAR2 (L2-coherent). */
#define SPT_SELF_SLOT		1U
#define SPT_SELF_GVA		(SPT_SELF_SLOT << 12)	/* = 0x1000 */

/* PRAMIN helpers -- caller holds sc->gsp_tok. */
static __inline void
pramin_set_base(struct nvkm_softc *sc, uint64_t paddr)
{
	nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(paddr >> 16));
}

static __inline void
pramin_wr64(struct nvkm_softc *sc, uint32_t pramin_off, uint64_t val)
{
	nvkm_wr32(sc, NV_PRAMIN + pramin_off + 0,
	    (uint32_t)(val & 0xffffffffu));
	nvkm_wr32(sc, NV_PRAMIN + pramin_off + 4,
	    (uint32_t)(val >> 32));
}

static int
bar2_pt_alloc(struct nvkm_softc *sc, struct nvkm_gsp_bar2_pt *pt)
{
	pt->paddr = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	if (pt->paddr == 0)
		return (ENOMEM);
	pt->kva = NULL;		/* VRAM PT has no host KVA */
	return (0);
}

static void
bar2_pt_free(struct nvkm_gsp_bar2_pt *pt __unused)
{
	/* VRAM bump allocator has no free. */
}

int
nvkm_gsp_bar2_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	struct rpc_update_bar_pde_v15_00 *rpc;
	uint64_t pd3_pde;
	int err;
	uint32_t saved;

	if (sc->bar_res[3] == NULL) {
		device_printf(sc->dev,
		    "bar2: PCIe BAR3 (== GPU BAR2) not mapped\n");
		return (ENXIO);
	}

	if ((err = bar2_pt_alloc(sc, &b2->pd2)) ||
	    (err = bar2_pt_alloc(sc, &b2->pd1)) ||
	    (err = bar2_pt_alloc(sc, &b2->pd0)) ||
	    (err = bar2_pt_alloc(sc, &b2->spt)))
		return (err);

	/* All bootstrap PT writes via PRAMIN. */
	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);

	/* Zero PD2/PD1/PD0/SPT pages (4 KiB each). */
	for (int k = 0; k < 4; k++) {
		uint64_t p = (k == 0) ? b2->pd2.paddr :
		             (k == 1) ? b2->pd1.paddr :
		             (k == 2) ? b2->pd0.paddr : b2->spt.paddr;
		pramin_set_base(sc, p);
		uint32_t off0 = (uint32_t)(p & 0xffffu);
		for (int b = 0; b < 0x1000; b += 4)
			nvkm_wr32(sc, NV_PRAMIN + off0 + b, 0);
	}

	/* PD2[0] -> PD1; PD1[0] -> PD0; PD0[0].small -> SPT, .big = 0. */
	pramin_set_base(sc, b2->pd2.paddr);
	pramin_wr64(sc, (uint32_t)(b2->pd2.paddr & 0xffffu),
	    pde_vram(b2->pd1.paddr));

	pramin_set_base(sc, b2->pd1.paddr);
	pramin_wr64(sc, (uint32_t)(b2->pd1.paddr & 0xffffu),
	    pde_vram(b2->pd0.paddr));

	pramin_set_base(sc, b2->pd0.paddr);
	pramin_wr64(sc, (uint32_t)(b2->pd0.paddr & 0xffffu) + 0,
	    pde_vram(b2->spt.paddr));	/* small page side */
	pramin_wr64(sc, (uint32_t)(b2->pd0.paddr & 0xffffu) + 8, 0); /* LPT=0 */

	/* SPT self-map: SPT[SELF_SLOT] = pte_vram(SPT_paddr).
	 * After UPDATE_BAR_PDE, writes to BAR3 + SPT_SELF_GVA + idx*8
	 * walk PD3->PD2->PD1->PD0->SPT[SELF_SLOT]=PTE->VRAM[SPT_paddr+...]
	 * i.e. they directly update the SPT page via the L2-coherent
	 * BAR2 path. */
	pramin_set_base(sc, b2->spt.paddr);
	pramin_wr64(sc, (uint32_t)(b2->spt.paddr & 0xffffu) + SPT_SELF_SLOT * 8,
	    pte_vram(b2->spt.paddr));

	(void)nvkm_rd32(sc, NV_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	/* Hand GSP the PD3[0] PDE pointing at our host-owned PD2. */
	pd3_pde = pde_vram(b2->pd2.paddr);

	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE,
	    sizeof(*rpc));
	if (rpc == NULL) {
		device_printf(sc->dev, "bar2: rpc_get failed\n");
		return (ENOMEM);
	}
	rpc->barType         = NV_RPC_UPDATE_PDE_BAR_2;
	rpc->entryValue      = pd3_pde;
	rpc->entryLevelShift = 47;	/* gp100_vmm_16[0].shift -- root PD3 */

	err = nvkm_gsp_rpc_wr(sc, rpc, NVKM_GSP_RPC_REPLY_RECV);
	if (err != 0) {
		device_printf(sc->dev,
		    "bar2: UPDATE_BAR_PDE failed err=%d\n", err);
		return (err);
	}

	/* UPDATE_BAR_PDE may be a no-op on Turing (journal §18). Also
	 * inject our PD3[0] PDE into the PDB GSP allocated, then force
	 * the GPU to re-read its PDB cache by re-writing 0xb80f48 with
	 * the current value (mirrors tu102_bar_bar2_init,
	 * bar/tu102.c:46-53). */
	uint32_t pdb_reg = nvkm_rd32(sc, 0xb80f48u);
	if ((pdb_reg & 0x80000000u) != 0) {
		uint64_t pdb_paddr = ((uint64_t)(pdb_reg & 0x3fffffffu)) << 12;
		lwkt_gettoken(&sc->gsp_tok);
		uint32_t s2 = nvkm_rd32(sc, NV_PBUS_PRAMIN);
		pramin_set_base(sc, pdb_paddr);
		pramin_wr64(sc, (uint32_t)(pdb_paddr & 0xffffu), pd3_pde);
		(void)nvkm_rd32(sc, NV_PRAMIN);
		nvkm_wr32(sc, NV_PBUS_PRAMIN, s2);
		lwkt_reltoken(&sc->gsp_tok);

		/* Disable BAR2, then re-enable with the same PDB paddr.
		 * Forces GPU to re-fetch PDB[0] from VRAM (PRAMIN-written). */
		uint32_t v = nvkm_rd32(sc, 0xb80f48u);
		nvkm_wr32(sc, 0xb80f48u, v & ~0x80000000u);
		(void)nvkm_rd32(sc, 0xb80f48u);
		nvkm_wr32(sc, 0xb80f48u, v);
		(void)nvkm_rd32(sc, 0xb80f48u);
		/* tu102_bar_bar2_wait: poll 0xb80f50 bits[3:2]==0. */
		for (int j = 0; j < 200; j++) {
			if ((nvkm_rd32(sc, 0xb80f50u) & 0xcu) == 0)
				break;
			DELAY(1000);
		}
	}

	device_printf(sc->dev,
	    "bar2: vmm ready (VRAM PT) PD2=0x%llx PD1=0x%llx PD0=0x%llx "
	    "SPT=0x%llx PD3[0]=0x%016llx self_gva=0x%x BAR3@%llx %lluMiB\n",
	    (unsigned long long)b2->pd2.paddr,
	    (unsigned long long)b2->pd1.paddr,
	    (unsigned long long)b2->pd0.paddr,
	    (unsigned long long)b2->spt.paddr,
	    (unsigned long long)pd3_pde,
	    (unsigned)SPT_SELF_GVA,
	    (unsigned long long)rman_get_start(sc->bar_res[3]),
	    (unsigned long long)rman_get_size(sc->bar_res[3]) >> 20);
	b2->ready = true;
	return (0);
}

void
nvkm_gsp_bar2_fini(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;

	bar2_pt_free(&b2->spt);
	bar2_pt_free(&b2->pd0);
	bar2_pt_free(&b2->pd1);
	bar2_pt_free(&b2->pd2);
	b2->ready = false;
}

/* Map one 4 KiB VRAM page into BAR2 at the given GVA. SPT entry write
 * goes through BAR2 itself (L2-coherent) using the SPT self-mapping. */
int
nvkm_gsp_bar2_map_vram(struct nvkm_softc *sc, uint64_t bar2_gva,
    uint64_t vram_paddr)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	uint32_t spt_idx;
	uint64_t pte;
	bus_size_t spt_self_off;

	if (!b2->ready)
		return (ENXIO);
	if ((bar2_gva & 0xfffULL) || (vram_paddr & 0xfffULL))
		return (EINVAL);
	if (bar2_gva >= 0x200000ULL)	/* 2 MiB == 512 SPT entries */
		return (ERANGE);
	if ((bar2_gva >> 12) == SPT_SELF_SLOT)
		return (EBUSY);		/* reserved for SPT self-map */

	spt_idx = (uint32_t)(bar2_gva >> 12);
	pte = pte_vram(vram_paddr);

	/* Write SPT[spt_idx] through the BAR2 self-mapping (L2-coherent).
	 * Walker translates SPT_SELF_GVA+spt_idx*8 -> SPT_paddr+spt_idx*8. */
	spt_self_off = (bus_size_t)(SPT_SELF_GVA + spt_idx * 8);
	bus_write_4(sc->bar_res[3], spt_self_off + 0,
	    (uint32_t)(pte & 0xffffffffu));
	bus_write_4(sc->bar_res[3], spt_self_off + 4,
	    (uint32_t)(pte >> 32));
	/* Read-back flush. */
	(void)bus_read_4(sc->bar_res[3], spt_self_off);

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
