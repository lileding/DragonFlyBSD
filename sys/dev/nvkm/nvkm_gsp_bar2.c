/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM BAR2 vmm: a minimal host-side page-table chain so the CPU
 * can read/write any VRAM physical page via a PCIe BAR with full L2
 * coherency (PBDMA sees the writes).
 *
 * Mirrors Linux nouveau:
 *   - r535_bar_bar2_init / r535_bar_bar2_update_pde
 *     (drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/bar.c)
 *   - rpc_update_bar_pde_v15_00 (fn=70, r570/rpcfn.h)
 *   - PDE/PTE encoding from vmmgp100.c
 *
 * IMPORTANT: nouveau allocates BAR2 PT pages in VRAM (NVKM_MEM_TARGET_INST
 * -> VRAM on Pascal+). The PDE chain therefore uses aperture=VRAM(1) for
 * PD3[0]/PD2[0]/PD1[0]/PD0[0]. Leaf SPT entries map BAR2 GVA->VRAM with
 * aperture=VRAM. The Turing GMMU BAR2 walker appears to only honor VRAM
 * apertures in GSP-managed PT chains.
 *
 * All host writes to these VRAM PT pages go through PRAMIN before the
 * UPDATE_BAR_PDE RPC; after that we use BAR2 itself for further updates.
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/pmap.h>

/* RPC function from r570/rpcfn.h:86. */
#define NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE	70

/* NV_RPC_UPDATE_PDE_BAR_TYPE values from r535/nvrm/bar.h:12-16. */
#define NV_RPC_UPDATE_PDE_BAR_1		0
#define NV_RPC_UPDATE_PDE_BAR_2		1
#define NV_RPC_UPDATE_PDE_BAR_INVALID	2

struct rpc_update_bar_pde_v15_00 {
	uint32_t barType;
	uint8_t  _pad[4];	/* NV_ALIGN_BYTES(8) before entryValue */
	uint64_t entryValue;
	uint64_t entryLevelShift;
};

#define APER_VRAM	(1ULL << 1)
#define VALID		(1ULL << 0)

static __inline uint64_t
pde_vram(uint64_t paddr)
{
	return (paddr >> 4) | APER_VRAM;
}

static __inline uint64_t
pte_vram(uint64_t paddr)
{
	/* PTE VRAM aperture is 0 (PEER is 1). Different from PDE,
	 * where VRAM is 1. See tu102/dev_mmu.h NV_MMU_VER2_PTE_APERTURE. */
	return (paddr >> 4) | VALID;
}

/* Allocate one 4 KiB VRAM page from the bump allocator and PRAMIN-zero
 * it. Returns the VRAM paddr in pt->paddr; pt->kva stays NULL (we never
 * have a CPU mapping into VRAM PT — PRAMIN or BAR2 only). */
static int
bar2_pt_alloc(struct nvkm_softc *sc, struct nvkm_gsp_bar2_pt *pt)
{
	uint64_t p = nvkm_gsp_vram_alloc(sc, 0x1000, 0x1000);
	if (p == 0)
		return (ENOMEM);
	pt->paddr = p;
	pt->kva = NULL;

	lwkt_gettoken(&sc->gsp_tok);
	uint32_t saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(p >> 16));
	uint32_t off = (uint32_t)(p & 0xffffu);
	for (int j = 0; j < 0x1000; j += 4)
		nvkm_wr32(sc, NV_PRAMIN + off + j, 0);
	(void)nvkm_rd32(sc, NV_PRAMIN + off);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);
	return (0);
}

static void
bar2_pt_free(struct nvkm_gsp_bar2_pt *pt __unused)
{
	/* VRAM bump allocator has no free. */
}

/* Write one 64-bit value to a VRAM page via PRAMIN. Caller holds gsp_tok
 * and PRAMIN base has been set to (paddr_base >> 16). */
static __inline void
pramin_wr64(struct nvkm_softc *sc, uint32_t pramin_off, uint64_t val)
{
	nvkm_wr32(sc, NV_PRAMIN + pramin_off + 0, (uint32_t)(val & 0xffffffffu));
	nvkm_wr32(sc, NV_PRAMIN + pramin_off + 4, (uint32_t)(val >> 32));
}

int
nvkm_gsp_bar2_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	struct rpc_update_bar_pde_v15_00 *rpc;
	uint64_t pd3_pde;
	int err;

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

	/* Write PDEs via PRAMIN:
	 *   PD2[0] = PDE_VRAM(PD1)
	 *   PD1[0] = PDE_VRAM(PD0)
	 *   PD0[0].lo = PDE_VRAM(SPT); PD0[0].hi = 0 (no big-page LPT)
	 * SPT entries are written later by nvkm_gsp_bar2_map_vram. */
	lwkt_gettoken(&sc->gsp_tok);
	uint32_t saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);

	nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(b2->pd2.paddr >> 16));
	pramin_wr64(sc, (uint32_t)(b2->pd2.paddr & 0xffffu),
	    pde_vram(b2->pd1.paddr));

	nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(b2->pd1.paddr >> 16));
	pramin_wr64(sc, (uint32_t)(b2->pd1.paddr & 0xffffu),
	    pde_vram(b2->pd0.paddr));

	nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(b2->pd0.paddr >> 16));
	pramin_wr64(sc, (uint32_t)(b2->pd0.paddr & 0xffffu) + 0,
	    pde_vram(b2->spt.paddr));	/* PD0[0].lo = SPT (small page) */
	pramin_wr64(sc, (uint32_t)(b2->pd0.paddr & 0xffffu) + 8, 0); /* LPT */

	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	/* PD3[0] PDE points at our host-owned PD2 (in VRAM, aperture=VRAM). */
	pd3_pde = pde_vram(b2->pd2.paddr);

	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE,
	    sizeof(*rpc));
	if (rpc == NULL) {
		device_printf(sc->dev, "bar2: rpc_get failed\n");
		return (ENOMEM);
	}
	rpc->barType        = NV_RPC_UPDATE_PDE_BAR_2;
	rpc->entryValue     = pd3_pde;
	rpc->entryLevelShift = 47;	/* Pascal+ root PD level */

	err = nvkm_gsp_rpc_wr(sc, rpc, NVKM_GSP_RPC_REPLY_RECV);
	if (err != 0) {
		device_printf(sc->dev,
		    "bar2: UPDATE_BAR_PDE failed err=%d\n", err);
		return (err);
	}

	uint32_t pdb_reg = nvkm_rd32(sc, 0xb80f48u);
	device_printf(sc->dev,
	    "bar2: GPU BAR2 PDB reg @0xb80f48 = 0x%08x\n", pdb_reg);
	/* Belt-and-suspenders: PRAMIN-write our PDE into the GSP-owned
	 * PDB[0] as well. PDB paddr decoded from the GPU reg:
	 *   pdb_paddr = (reg & 0x3fffffff) << 12   (bits 30..0 hold addr>>12) */
	if ((pdb_reg & 0x80000000u) != 0) {
		uint64_t pdb_paddr = ((uint64_t)(pdb_reg & 0x3fffffffu)) << 12;
		lwkt_gettoken(&sc->gsp_tok);
		uint32_t s2 = nvkm_rd32(sc, NV_PBUS_PRAMIN);
		nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(pdb_paddr >> 16));
		uint32_t off2 = (uint32_t)(pdb_paddr & 0xffffu);
		uint32_t old_lo = nvkm_rd32(sc, NV_PRAMIN + off2 + 0);
		uint32_t old_hi = nvkm_rd32(sc, NV_PRAMIN + off2 + 4);
		pramin_wr64(sc, off2, pd3_pde);
		(void)nvkm_rd32(sc, NV_PRAMIN + off2);
		nvkm_wr32(sc, NV_PBUS_PRAMIN, s2);
		lwkt_reltoken(&sc->gsp_tok);
		device_printf(sc->dev,
		    "bar2: PDB @0x%llx PDE[0] %08x:%08x -> %016llx (PRAMIN)\n",
		    (unsigned long long)pdb_paddr, old_hi, old_lo,
		    (unsigned long long)pd3_pde);
	}
	device_printf(sc->dev,
	    "bar2: vmm ready VRAM PT (PD2=0x%llx PD1=0x%llx PD0=0x%llx "
	    "SPT=0x%llx, PD3[0]=0x%016llx, BAR3@%llx size %lluMiB)\n",
	    (unsigned long long)b2->pd2.paddr,
	    (unsigned long long)b2->pd1.paddr,
	    (unsigned long long)b2->pd0.paddr,
	    (unsigned long long)b2->spt.paddr,
	    (unsigned long long)pd3_pde,
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

/* Map one 4 KiB VRAM page into BAR2 at the given GVA. GVA must be
 * 4 KiB-aligned and < 2 MiB (we use a single SPT, 512 entries). */
int
nvkm_gsp_bar2_map_vram(struct nvkm_softc *sc, uint64_t bar2_gva,
    uint64_t vram_paddr)
{
	struct nvkm_gsp_bar2 *b2 = &sc->bar2;
	uint32_t spt_idx;

	if (!b2->ready)
		return (ENXIO);
	if ((bar2_gva & 0xfffULL) || (vram_paddr & 0xfffULL))
		return (EINVAL);
	if (bar2_gva >= 0x200000ULL)
		return (ERANGE);

	spt_idx = (uint32_t)(bar2_gva >> 12);

	/* Write the PTE via PRAMIN. SPT is VRAM, 8 bytes per entry. */
	lwkt_gettoken(&sc->gsp_tok);
	uint32_t saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(b2->spt.paddr >> 16));
	uint32_t off = (uint32_t)(b2->spt.paddr & 0xffffu) + spt_idx * 8;
	pramin_wr64(sc, off, pte_vram(vram_paddr));
	(void)nvkm_rd32(sc, NV_PRAMIN + off);
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	device_printf(sc->dev,
	    "bar2: map BAR2_GVA=0x%llx -> VRAM=0x%llx (SPT[%u]=PTE_VRAM)\n",
	    (unsigned long long)bar2_gva, (unsigned long long)vram_paddr,
	    spt_idx);
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
