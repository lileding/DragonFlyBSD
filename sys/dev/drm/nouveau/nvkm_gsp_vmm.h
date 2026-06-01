/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM VMM: bundles a per-VA-space NV01_ROOT client + NV01_DEVICE_0
 * + NV20_SUBDEVICE_0 + FERMI_VASPACE_A together with the three host
 * sysmem PT pages (PD3, PD2, PD1) for the 512 MiB server-managed
 * window at GPU VA 0x100000000.
 *
 * Mirrors Linux nouveau's per-vmm client+device pattern (see
 * refs/nouveau_r570_gmmu.md §3-4 for the full picture).
 */
#ifndef _NVKM_GSP_VMM_H_
#define _NVKM_GSP_VMM_H_

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"

/* Per-vmm GMMU page-table chain. Three sysmem 4 KiB pages, one each
 * for PD3 / PD2 / PD1; the 512 MiB RM-managed range falls inside
 * PD1 entry 8 (VA 0x100000000 / 512MiB = 8). */
struct nvkm_gsp_vmm_pt {
	struct nvkm_bar1_page	page;  /* VRAM page mapped into BAR1 */
};

struct nvkm_gsp_vmm_pd1 {
	LIST_ENTRY(nvkm_gsp_vmm_pd1) link;
	uint32_t		pd2_idx;
	struct nvkm_bar1_page	page;
};
LIST_HEAD(nvkm_gsp_vmm_pd1_list, nvkm_gsp_vmm_pd1);

struct nvkm_gsp_vmm_pd0 {
	LIST_ENTRY(nvkm_gsp_vmm_pd0) link;
	struct nvkm_bar1_page	*pd1_page;
	uint32_t		pd2_idx;
	uint32_t		pd1_idx;
	uint32_t		refcount;
	struct nvkm_bar1_page	page;
};
LIST_HEAD(nvkm_gsp_vmm_pd0_list, nvkm_gsp_vmm_pd0);

struct nvkm_gsp_vmm_user_pt {
	LIST_ENTRY(nvkm_gsp_vmm_user_pt) link;
	struct nvkm_gsp_vmm_pd0 *pd0;
	uint32_t		pd2_idx;
	uint32_t		pd1_idx;
	uint32_t		pd0_idx;
	uint32_t		valid_pte_count;
	struct nvkm_bar1_page	lpt;
	struct nvkm_bar1_page	spt;
};
LIST_HEAD(nvkm_gsp_vmm_user_pt_list, nvkm_gsp_vmm_user_pt);

struct nvkm_gsp_vmm_sparse_region {
	LIST_ENTRY(nvkm_gsp_vmm_sparse_region) link;
	uint64_t		addr;
	uint64_t		size;
};
LIST_HEAD(nvkm_gsp_vmm_sparse_region_list, nvkm_gsp_vmm_sparse_region);

struct nvkm_gsp_vmm {
	struct nvkm_softc	*sc;
	struct lwkt_token	 tok;	/* protects PT writes + VA alloc */

	struct nvkm_gsp_client	 client;	/* NV01_ROOT */
	struct nvkm_gsp_device	 device;	/* NV01_DEVICE_0 + subdevice */
	struct nvkm_gsp_object	 vaspace;	/* FERMI_VASPACE_A */

	/* PT levels: [0]=PD3 (root), [1]=PD2, [2]=PD1.
	 * PD3[0] -> PD2_paddr; PD2[0] -> PD1_paddr; PD1[8] empty (GSP fills). */
	struct nvkm_gsp_vmm_pt	 pt[3];

	/* The server-managed window: VA range [rm_va_base, rm_va_base+rm_va_size). */
	uint64_t		 rm_va_base;
	uint64_t		 rm_va_size;

	struct nvkm_gsp_vmm_pd1_list user_pd1_pages;
	struct nvkm_gsp_vmm_pd0_list user_pd0_pages;
	struct nvkm_gsp_vmm_user_pt_list user_pt_pages;
	struct nvkm_gsp_vmm_sparse_region_list sparse_regions;
	struct nvkm_dmamem sparse_page;
};

struct nvkm_gsp_vmm_pte_info {
	uint64_t	va;
	uint64_t	pte;
	uint32_t	pd2_idx;
	uint32_t	pd1_idx;
	uint32_t	pd0_idx;
	uint32_t	spt_idx;
	uint8_t		has_pt;
};

/* Construct a complete VMM: allocate client/device/subdevice/vaspace,
 * allocate three host sysmem PT pages, encode PD3[0] and PD2[0], then
 * issue NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES.
 *
 * On success the vmm is fully usable as a parent for channel allocs. */
int	 nvkm_gsp_vmm_ctor(struct nvkm_softc *sc, uint32_t client_handle,
	    struct nvkm_gsp_vmm *vmm);

/* Tear down. Frees PT pages + RM resources. */
void	 nvkm_gsp_vmm_dtor(struct nvkm_gsp_vmm *vmm);

int	 nvkm_gsp_vmm_map_sysmem(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    vm_paddr_t paddr, uint64_t size);
int	 nvkm_gsp_vmm_map_sysmem_kva(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    void *kva, uint64_t size);
int	 nvkm_gsp_vmm_map_vram(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t paddr, uint64_t size);
int	 nvkm_gsp_vmm_map_vram_flags(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t paddr, uint64_t size, uint8_t priv, uint8_t ro);
int	 nvkm_gsp_vmm_unmap(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t size);
int	 nvkm_gsp_vmm_map_sparse(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t size);
int	 nvkm_gsp_vmm_unmap_sparse(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    uint64_t size);
void	 nvkm_gsp_vmm_read_pte(struct nvkm_gsp_vmm *vmm, uint64_t va,
	    struct nvkm_gsp_vmm_pte_info *info);
void	 nvkm_gsp_vmm_debug_dump_pte(struct nvkm_gsp_vmm *vmm, uint64_t va);

#endif /* _NVKM_GSP_VMM_H_ */
