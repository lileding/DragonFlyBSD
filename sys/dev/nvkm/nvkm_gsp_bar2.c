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
 * Layout: host owns PD2/PD1/PD0/SPT (all sysmem, SYS_COH aperture
 * with VOL bit so the GMMU walker reads them coherently). PD3 (root)
 * stays GSP-side; we hand over the single PD3[0] PDE value via the
 * UPDATE_BAR_PDE RPC.
 *
 * GVA layout in our BAR2 vmm (we use a tiny window):
 *   GVA 0x000000..0x1fffff -> 512 4 KiB slots; first slot for USERD
 */

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/pmap.h>

static MALLOC_DEFINE(M_NVKM_BAR2, "nvkm_bar2", "nvkm BAR2 PT pages");

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

/* PDE/PTE encoding (Pascal+) -- see vmmgp100.c:gp100_vmm_pde
 *   bit 0:    VALID  (PTE only; PDEs are valid when aperture != 0)
 *   bits 2:1: aperture (1=VRAM, 2=SYS_COH, 3=SYS_NCOH)
 *   bit 3:    VOL    (set for SYS_COH; tells walker to snoop)
 *   bits 39:4: physical address >> 4
 */
#define APER_VRAM	(1ULL << 1)
#define APER_SYS_COH	(2ULL << 1)
#define VOL		(1ULL << 3)
#define VALID		(1ULL << 0)

static __inline uint64_t
pde_sys_coh(uint64_t paddr)
{
	return (paddr >> 4) | APER_SYS_COH | VOL;
}

static __inline uint64_t
pte_vram(uint64_t paddr)
{
	return (paddr >> 4) | APER_VRAM | VALID;
}

/* Allocate one 4 KiB contiguous sysmem page, zero it, return KVA. */
static int
bar2_pt_alloc(struct nvkm_gsp_bar2_pt *pt)
{
	pt->kva = contigmalloc(0x1000, M_NVKM_BAR2,
	    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, 0x1000, 0);
	if (pt->kva == NULL)
		return (ENOMEM);
	pt->paddr = vtophys(pt->kva);
	return (0);
}

static void
bar2_pt_free(struct nvkm_gsp_bar2_pt *pt)
{
	if (pt->kva != NULL) {
		contigfree(pt->kva, 0x1000, M_NVKM_BAR2);
		pt->kva = NULL;
		pt->paddr = 0;
	}
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

	if ((err = bar2_pt_alloc(&b2->pd2)) ||
	    (err = bar2_pt_alloc(&b2->pd1)) ||
	    (err = bar2_pt_alloc(&b2->pd0)) ||
	    (err = bar2_pt_alloc(&b2->spt))) {
		bar2_pt_free(&b2->pd2);
		bar2_pt_free(&b2->pd1);
		bar2_pt_free(&b2->pd0);
		bar2_pt_free(&b2->spt);
		return (err);
	}

	/* Wire PD2[0] -> PD1, PD1[0] -> PD0, PD0[0].low -> SPT.
	 * PT pages are sysmem; aperture = SYS_COH so MMU walker uses
	 * snooped reads (x86 coherent). VOL bit set per nouveau.
	 *
	 * SPT entries are written later by nvkm_gsp_bar2_map_vram. */
	((volatile uint64_t *)b2->pd2.kva)[0] = pde_sys_coh(b2->pd1.paddr);
	((volatile uint64_t *)b2->pd1.kva)[0] = pde_sys_coh(b2->pd0.paddr);
	((volatile uint64_t *)b2->pd0.kva)[0] = pde_sys_coh(b2->spt.paddr);
	((volatile uint64_t *)b2->pd0.kva)[1] = 0;	/* no big-page LPT */
	cpu_sfence();

	/* PD3[0] PDE that GSP should program into the GPU's BAR2 PDB.
	 * Points at our host-owned PD2.
	 *
	 * entryLevelShift = 47 = the bit position of the root PD (PD3
	 * covers 2^47 = 128 TiB per entry on the Pascal+ 16K-page layout). */
	pd3_pde = pde_sys_coh(b2->pd2.paddr);

	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE,
	    sizeof(*rpc));
	if (rpc == NULL) {
		device_printf(sc->dev, "bar2: rpc_get failed\n");
		return (ENOMEM);
	}
	rpc->barType        = NV_RPC_UPDATE_PDE_BAR_2;
	rpc->entryValue     = pd3_pde;
	rpc->entryLevelShift = 47;

	err = nvkm_gsp_rpc_wr(sc, rpc, NVKM_GSP_RPC_REPLY_RECV);
	if (err != 0) {
		device_printf(sc->dev,
		    "bar2: UPDATE_BAR_PDE failed err=%d\n", err);
		return (err);
	}

	uint32_t pdb_reg = nvkm_rd32(sc, 0xb80f48u);
	device_printf(sc->dev,
	    "bar2: GPU BAR2 PDB reg @0xb80f48 = 0x%08x\n", pdb_reg);
	device_printf(sc->dev,
	    "bar2: vmm ready (PD2=0x%llx PD1=0x%llx PD0=0x%llx SPT=0x%llx, "
	    "PD3[0]=0x%016llx, BAR3@%llx size %lluMiB)\n",
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
 * 4 KiB-aligned and within range covered by our minimal PT (currently
 * 2 MiB = 512 SPT entries). */
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
	if (bar2_gva >= 0x200000ULL)	/* 2 MiB: 512 * 4 KiB */
		return (ERANGE);

	spt_idx = (uint32_t)(bar2_gva >> 12);
	((volatile uint64_t *)b2->spt.kva)[spt_idx] = pte_vram(vram_paddr);
	cpu_sfence();

	device_printf(sc->dev,
	    "bar2: map BAR2_GVA=0x%llx -> VRAM=0x%llx (SPT[%u]=0x%016llx)\n",
	    (unsigned long long)bar2_gva, (unsigned long long)vram_paddr,
	    spt_idx,
	    (unsigned long long)((volatile uint64_t *)b2->spt.kva)[spt_idx]);
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
