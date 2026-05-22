/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM BAR1 vmm.
 *
 * BAR1 is the PCIe paged window the host uses to do L2-coherent VRAM
 * access. CPU writes to `bar_res[1] + bar1_gva` are translated by the
 * GMMU through this BAR1 vmm's page table chain and land in VRAM via
 * the GPU L2 -- so PBDMA's subsequent reads of the same VRAM page see
 * the new value.
 *
 * Design (kept aligned with the channel vmm we already proved out):
 *   - PD2/PD1/PD0/SPT all in sysmem (contigmalloc, host KVA).
 *   - PDE encoding SYS_COH + VOL; leaf PTE encoding VRAM (aperture 0).
 *   - GSP at boot pre-allocated a BAR1 PDB in VRAM and pointed the GPU
 *     BAR1 PDB register (0xb80f40) at it. We do NOT replace the PDB;
 *     we just overwrite PDB[0] PDE to point at our sysmem PD2.
 *   - PDB[0] is written via (a) UPDATE_BAR_PDE RPC barType=BAR_1, so
 *     GSP firmware updates the PDB L2-coherently from its side, and
 *     (b) PRAMIN-poke as a fallback (only safe if walker hasn't touched
 *     this PDB line yet; works for BAR1 because nothing on GSP path
 *     uses BAR1 before host).
 *
 * Reference: linux/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/bar.c
 *     :r535_bar_bar1_init -- nouveau wraps GSP-given PDB and adds
 *     mappings via nvkm_vmm_map. We simplify: own PDB[0] outright.
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/pmap.h>

static MALLOC_DEFINE(M_NVKM_BAR1, "nvkm_bar1", "nvkm BAR1 PT pages");

/* RPC fn -- r570/rpcfn.h:86 UPDATE_BAR_PDE. */
#define NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE	70
#define NV_RPC_UPDATE_PDE_BAR_1			0

struct rpc_update_bar_pde_v15_00 {
	uint32_t barType;
	uint8_t  _pad[4];
	uint64_t entryValue;
	uint64_t entryLevelShift;
};

static int
bar1_pt_alloc(struct nvkm_gsp_bar1_pt *pt)
{
	pt->kva = contigmalloc(NVKM_GMMU_PT_PAGE_SIZE, M_NVKM_BAR1,
	    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0,
	    NVKM_GMMU_PT_PAGE_SIZE, 0);
	if (pt->kva == NULL)
		return (ENOMEM);
	pt->paddr = vtophys(pt->kva);
	return (0);
}

static void
bar1_pt_free(struct nvkm_gsp_bar1_pt *pt)
{
	if (pt->kva != NULL) {
		contigfree(pt->kva, NVKM_GMMU_PT_PAGE_SIZE, M_NVKM_BAR1);
		pt->kva = NULL;
		pt->paddr = 0;
	}
}

/* Caller holds sc->gsp_tok. */
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

static __inline uint64_t
pramin_rd64(struct nvkm_softc *sc, uint32_t pramin_off)
{
	uint32_t lo = nvkm_rd32(sc, NV_PRAMIN + pramin_off + 0);
	uint32_t hi = nvkm_rd32(sc, NV_PRAMIN + pramin_off + 4);
	return ((uint64_t)hi << 32) | lo;
}

int
nvkm_gsp_bar1_init(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;
	struct rpc_update_bar_pde_v15_00 *rpc;
	uint64_t pdb_paddr;
	uint64_t pdb0_existing, pd3_pde;
	int err;
	uint32_t saved;

	if (sc->bar_res[1] == NULL) {
		device_printf(sc->dev,
		    "bar1: PCIe BAR1 not mapped\n");
		return (ENXIO);
	}
	if (sc->gsp_bar1_pdb == 0) {
		device_printf(sc->dev,
		    "bar1: GSP did not publish bar1PdeBase\n");
		return (ENXIO);
	}

	if ((err = bar1_pt_alloc(&b1->pd2)) ||
	    (err = bar1_pt_alloc(&b1->pd1)) ||
	    (err = bar1_pt_alloc(&b1->pd0)) ||
	    (err = bar1_pt_alloc(&b1->spt))) {
		bar1_pt_free(&b1->pd2);
		bar1_pt_free(&b1->pd1);
		bar1_pt_free(&b1->pd0);
		bar1_pt_free(&b1->spt);
		return (err);
	}

	/* PT chain (all sysmem). Walker reads sysmem via PCIe coherent
	 * snoop -- host KVA writes are immediately visible.
	 *
	 * Aperture SYS_COH + VOL; reference vmmgp100.c:237-251 case
	 * NVKM_MEM_TARGET_HOST. */
	((volatile uint64_t *)b1->pd2.kva)[0] =
	    nvkm_pde_to_sysmem(b1->pd1.paddr);
	((volatile uint64_t *)b1->pd1.kva)[0] =
	    nvkm_pde_to_sysmem(b1->pd0.paddr);
	((volatile uint64_t *)b1->pd0.kva)[0] =
	    nvkm_pde_to_sysmem(b1->spt.paddr);
	/* PD0 entry is dual (16 bytes): low = small-page (SPT), high =
	 * big-page (LPT). We only use small pages. */
	((volatile uint64_t *)b1->pd0.kva)[1] = 0;
	cpu_sfence();

	/* Inject PDB[0] = PDE_to_sysmem(our PD2). The PDB itself is the
	 * VRAM page GSP gave us. Two writers:
	 *   1. PRAMIN-poke (bypasses L2; works iff walker hasn't read
	 *      this cache line yet -- our gamble for BAR1).
	 *   2. UPDATE_BAR_PDE RPC barType=BAR_1: GSP firmware writes the
	 *      PDB[0] from inside the GPU, L2-coherently. */
	pdb_paddr = sc->gsp_bar1_pdb;
	pd3_pde = nvkm_pde_to_sysmem(b1->pd2.paddr);

	lwkt_gettoken(&sc->gsp_tok);
	saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
	pramin_set_base(sc, pdb_paddr);
	pdb0_existing = pramin_rd64(sc,
	    (uint32_t)(pdb_paddr & 0xffffu));
	pramin_wr64(sc, (uint32_t)(pdb_paddr & 0xffffu), pd3_pde);
	(void)nvkm_rd32(sc, NV_PRAMIN);  /* flush */
	nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
	lwkt_reltoken(&sc->gsp_tok);

	device_printf(sc->dev,
	    "bar1: PDB @0x%llx, PDB[0] was 0x%016llx, now 0x%016llx (PRAMIN)\n",
	    (unsigned long long)pdb_paddr,
	    (unsigned long long)pdb0_existing,
	    (unsigned long long)pd3_pde);

	/* RPC: tell GSP-RM about our PD3[0] PDE so it can mirror the
	 * write L2-coherently. r535/bar.c:53 r535_bar_bar2_update_pde
	 * (we send the BAR_1 variant). */
	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE,
	    sizeof(*rpc));
	if (rpc == NULL) {
		device_printf(sc->dev, "bar1: rpc_get failed\n");
		return (ENOMEM);
	}
	rpc->barType         = NV_RPC_UPDATE_PDE_BAR_1;
	rpc->entryValue      = pd3_pde;
	rpc->entryLevelShift = NVKM_GMMU_PD3_SHIFT;
	err = nvkm_gsp_rpc_wr(sc, rpc, NVKM_GSP_RPC_REPLY_RECV);
	if (err != 0) {
		device_printf(sc->dev,
		    "bar1: UPDATE_BAR_PDE BAR_1 failed err=%d\n", err);
		return (err);
	}

	device_printf(sc->dev,
	    "bar1: vmm ready PD2=0x%llx PD1=0x%llx PD0=0x%llx SPT=0x%llx, "
	    "BAR1@%llx %lluMiB\n",
	    (unsigned long long)b1->pd2.paddr,
	    (unsigned long long)b1->pd1.paddr,
	    (unsigned long long)b1->pd0.paddr,
	    (unsigned long long)b1->spt.paddr,
	    (unsigned long long)rman_get_start(sc->bar_res[1]),
	    (unsigned long long)rman_get_size(sc->bar_res[1]) >> 20);
	b1->ready = true;
	return (0);
}

void
nvkm_gsp_bar1_fini(struct nvkm_softc *sc)
{
	struct nvkm_gsp_bar1 *b1 = &sc->bar1;

	bar1_pt_free(&b1->spt);
	bar1_pt_free(&b1->pd0);
	bar1_pt_free(&b1->pd1);
	bar1_pt_free(&b1->pd2);
	b1->ready = false;
}

/* Map one 4 KiB VRAM page into BAR1 at the given GVA. GVA range is
 * limited to a single SPT page = 2 MiB for now. */
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

	/* Leaf PTE: aperture=VRAM (=0 for PTE, tu102/dev_mmu.h:80),
	 * VALID=1. No VOL for VRAM. */
	pte = ((uint64_t)vram_paddr >> NV_PT_ADDR_SHIFT) | NV_PTE_VALID;
	((volatile uint64_t *)b1->spt.kva)[spt_idx] = pte;
	cpu_sfence();

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
