/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP VMM backend boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_vmm.h"
#include "nvgsp_priv.h"
#include "nvgsp_rm.h"

static MALLOC_DEFINE(M_NVGSP_VMM, "nvgsp_vmm", "nvgsp VMM state");

#define FERMI_VASPACE_A		0x000090f1u
#define TURING_USERMODE_A	0x0000c461u
#define NVGSP_RM_VASPACE	0x90f10000u
#define NVGSP_RM_USERMODE	0xc4610000u

#define NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES 0x90f10106u
#define NV_COPY_PDE_APERTURE_VIDMEM 1u

struct nvgsp_copy_pdes_level {
	uint64_t physAddress;
	uint64_t size;
	uint32_t aperture;
	uint32_t pageShift;
};

struct nvgsp_copy_pdes_params {
	uint32_t hSubDevice;
	uint32_t subDeviceId;
	uint64_t pageSize;
	uint64_t virtAddrLo;
	uint64_t virtAddrHi;
	uint32_t numLevelsToCopy;
	uint8_t pad[4];
	struct nvgsp_copy_pdes_level levels[6];
};

struct nvgsp_vaspace_params {
	uint32_t index;
	int32_t flags;
	uint64_t vaSize;
	uint64_t vaStartInternal;
	uint64_t vaLimitInternal;
	uint32_t bigPageSize;
	uint8_t pad[4];
	uint64_t vaBase;
};

static uint32_t
nvgsp_vmm_get_child_handle(const struct nvgsp_client *client, uint32_t base)
{
	return (base | (client->object.handle & 0x00000fffu));
}

static uint32_t
nvgsp_vmm_get_pd2_idx(uint64_t va)
{
	return ((uint32_t)((va >> NVGSP_GMMU_PD2_SHIFT) &
	    (NVGSP_GMMU_PD2_ENTRIES - 1)));
}

static uint32_t
nvgsp_vmm_get_pd1_idx(uint64_t va)
{
	return ((uint32_t)((va >> NVGSP_GMMU_PD1_SHIFT) &
	    (NVGSP_GMMU_PD1_ENTRIES - 1)));
}

static uint32_t
nvgsp_vmm_get_pd0_idx(uint64_t va)
{
	return ((uint32_t)((va >> NVGSP_GMMU_PD0_SHIFT) &
	    (NVGSP_GMMU_PD0_ENTRIES - 1)));
}

static uint32_t
nvgsp_vmm_get_spt_idx(uint64_t va)
{
	return ((uint32_t)((va >> NVGSP_GMMU_SPT_SHIFT) &
	    (NVGSP_GMMU_SPT_ENTRIES - 1)));
}

static int
nvgsp_vmm_alloc_pt(struct nvgsp_state *gsp, struct nvgsp_vmm_pt *pt)
{
	return (nvgsp_bar_alloc_bar1_page_kind(gsp, &pt->page,
	    NVGSP_VRAM_VMM_PT, pt));
}

static void
nvgsp_vmm_free_pt(struct nvgsp_state *gsp, struct nvgsp_vmm_pt *pt)
{
	nvgsp_bar_free_bar1_page(gsp, &pt->page);
}

static void
nvgsp_vmm_zero_bar1_page(struct nvgsp_state *gsp,
    const struct nvgsp_bar1_page *page)
{
	nvgsp_bar_set_bar1_region64(gsp, page->bar1_gva, 0,
	    NVGSP_GMMU_PT_PAGE_SIZE / sizeof(uint64_t));
	nvgsp_bar_flush_bar1(gsp);
}

static struct nvgsp_bar1_page *
nvgsp_vmm_get_pd1_base_page(struct nvgsp_vmm *vmm, uint32_t pd2_idx)
{
	struct nvgsp_state *gsp = vmm->gsp;
	struct nvgsp_vmm_pd1 *pd1;
	int error;

	if (pd2_idx == 0)
		return (&vmm->pt[2].page);

	LIST_FOREACH(pd1, &vmm->user_pd1_pages, link) {
		if (pd1->pd2_idx == pd2_idx)
			return (&pd1->page);
	}

	pd1 = kmalloc(sizeof(*pd1), M_NVGSP_VMM, M_WAITOK | M_ZERO);
	pd1->pd2_idx = pd2_idx;
	error = nvgsp_bar_alloc_bar1_page_kind(gsp, &pd1->page,
	    NVGSP_VRAM_VMM_PT, pd1);
	if (error != 0) {
		kfree(pd1, M_NVGSP_VMM);
		return (NULL);
	}

	nvgsp_vmm_zero_bar1_page(gsp, &pd1->page);
	nvgsp_bar_wr64_bar1(gsp, vmm->pt[1].page.bar1_gva + pd2_idx * 8,
	    nvgsp_pde_to_vram(pd1->page.vram_paddr));
	nvgsp_bar_flush_bar1(gsp);
	LIST_INSERT_HEAD(&vmm->user_pd1_pages, pd1, link);
	return (&pd1->page);
}

static struct nvgsp_vmm_pd0 *
nvgsp_vmm_find_pd0(struct nvgsp_vmm *vmm, uint32_t pd2_idx, uint32_t pd1_idx)
{
	struct nvgsp_vmm_pd0 *pd0;

	LIST_FOREACH(pd0, &vmm->user_pd0_pages, link) {
		if (pd0->pd2_idx == pd2_idx && pd0->pd1_idx == pd1_idx)
			return (pd0);
	}
	return (NULL);
}

static int
nvgsp_vmm_get_pd0(struct nvgsp_vmm *vmm, uint32_t pd2_idx, uint32_t pd1_idx,
    struct nvgsp_vmm_pd0 **out)
{
	struct nvgsp_state *gsp = vmm->gsp;
	struct nvgsp_bar1_page *pd1_page;
	struct nvgsp_vmm_pd0 *pd0;
	int error;

	pd0 = nvgsp_vmm_find_pd0(vmm, pd2_idx, pd1_idx);
	if (pd0 != NULL) {
		*out = pd0;
		return (0);
	}

	pd1_page = nvgsp_vmm_get_pd1_base_page(vmm, pd2_idx);
	if (pd1_page == NULL)
		return (ENOMEM);

	pd0 = kmalloc(sizeof(*pd0), M_NVGSP_VMM, M_WAITOK | M_ZERO);
	pd0->pd1_page = pd1_page;
	pd0->pd2_idx = pd2_idx;
	pd0->pd1_idx = pd1_idx;
	error = nvgsp_bar_alloc_bar1_page_kind(gsp, &pd0->page,
	    NVGSP_VRAM_VMM_PT, pd0);
	if (error != 0) {
		kfree(pd0, M_NVGSP_VMM);
		return (error);
	}

	nvgsp_vmm_zero_bar1_page(gsp, &pd0->page);
	nvgsp_bar_wr64_bar1(gsp, pd1_page->bar1_gva + pd1_idx * 8,
	    nvgsp_pde_to_vram(pd0->page.vram_paddr));
	nvgsp_bar_flush_bar1(gsp);
	LIST_INSERT_HEAD(&vmm->user_pd0_pages, pd0, link);
	*out = pd0;
	return (0);
}

static struct nvgsp_vmm_user_pt *
nvgsp_vmm_find_user_pt(struct nvgsp_vmm *vmm, uint32_t pd2_idx,
    uint32_t pd1_idx, uint32_t pd0_idx)
{
	struct nvgsp_vmm_user_pt *pt;

	LIST_FOREACH(pt, &vmm->user_pt_pages, link) {
		if (pt->pd2_idx == pd2_idx && pt->pd1_idx == pd1_idx &&
		    pt->pd0_idx == pd0_idx)
			return (pt);
	}
	return (NULL);
}

static void
nvgsp_vmm_write_pd0_child_slot(struct nvgsp_state *gsp,
    const struct nvgsp_vmm_pd0 *pd0, uint32_t pd0_idx, uint64_t big_pde,
    uint64_t small_pde)
{
	nvgsp_bar_wr64_bar1(gsp, pd0->page.bar1_gva + (pd0_idx * 2 + 0) * 8,
	    big_pde);
	nvgsp_bar_wr64_bar1(gsp, pd0->page.bar1_gva + (pd0_idx * 2 + 1) * 8,
	    small_pde);
}

static int
nvgsp_vmm_get_user_pt(struct nvgsp_vmm *vmm, uint64_t va,
    struct nvgsp_vmm_user_pt **out)
{
	struct nvgsp_state *gsp = vmm->gsp;
	struct nvgsp_vmm_user_pt *pt;
	struct nvgsp_vmm_pd0 *pd0;
	uint32_t pd2_idx, pd1_idx, pd0_idx;
	int error;

	pd2_idx = nvgsp_vmm_get_pd2_idx(va);
	pd1_idx = nvgsp_vmm_get_pd1_idx(va);
	pd0_idx = nvgsp_vmm_get_pd0_idx(va);
	pt = nvgsp_vmm_find_user_pt(vmm, pd2_idx, pd1_idx, pd0_idx);
	if (pt != NULL) {
		*out = pt;
		return (0);
	}

	error = nvgsp_vmm_get_pd0(vmm, pd2_idx, pd1_idx, &pd0);
	if (error != 0)
		return (error);

	pt = kmalloc(sizeof(*pt), M_NVGSP_VMM, M_WAITOK | M_ZERO);
	pt->pd0 = pd0;
	pt->pd2_idx = pd2_idx;
	pt->pd1_idx = pd1_idx;
	pt->pd0_idx = pd0_idx;

	error = nvgsp_bar_alloc_bar1_page_kind(gsp, &pt->lpt,
	    NVGSP_VRAM_VMM_PT, pt);
	if (error != 0)
		goto fail;
	error = nvgsp_bar_alloc_bar1_page_kind(gsp, &pt->spt,
	    NVGSP_VRAM_VMM_PT, pt);
	if (error != 0)
		goto fail;

	nvgsp_vmm_zero_bar1_page(gsp, &pt->lpt);
	nvgsp_vmm_zero_bar1_page(gsp, &pt->spt);
	nvgsp_vmm_write_pd0_child_slot(gsp, pd0, pd0_idx,
	    nvgsp_pde_to_vram(pt->lpt.vram_paddr),
	    nvgsp_pde_to_vram(pt->spt.vram_paddr));
	nvgsp_bar_flush_bar1(gsp);

	LIST_INSERT_HEAD(&vmm->user_pt_pages, pt, link);
	*out = pt;
	return (0);

fail:
	if (pt->spt.vram_paddr != 0)
		nvgsp_bar_free_bar1_page(gsp, &pt->spt);
	if (pt->lpt.vram_paddr != 0)
		nvgsp_bar_free_bar1_page(gsp, &pt->lpt);
	kfree(pt, M_NVGSP_VMM);
	return (error);
}

static struct nvgsp_vmm_user_pt *
nvgsp_vmm_lookup_user_pt_va(struct nvgsp_vmm *vmm, uint64_t va)
{
	return (nvgsp_vmm_find_user_pt(vmm, nvgsp_vmm_get_pd2_idx(va),
	    nvgsp_vmm_get_pd1_idx(va), nvgsp_vmm_get_pd0_idx(va)));
}

static int
nvgsp_vmm_write_4k_pte(struct nvgsp_vmm *vmm, uint64_t va, uint64_t pte)
{
	struct nvgsp_vmm_user_pt *pt;
	int error;

	error = nvgsp_vmm_get_user_pt(vmm, va, &pt);
	if (error != 0)
		return (error);
	nvgsp_bar_wr64_bar1(vmm->gsp, pt->spt.bar1_gva + nvgsp_vmm_get_spt_idx(va) * 8,
	    pte);
	return (0);
}

static void
nvgsp_vmm_invalidate(struct nvgsp_vmm *vmm)
{
	struct nvgsp_state *gsp = vmm->gsp;
	uint64_t pdb = vmm->pt[0].page.vram_paddr;
	uint32_t trigger = 0xffffffffu;

	nvgsp_wr32(gsp, 0x00b830a0, (uint32_t)(pdb >> 8));
	nvgsp_wr32(gsp, 0x00b830a4, 0);
	nvgsp_wr32(gsp, 0x00b830b0, 0x80000001u);
	for (int spin = 0; spin < 200000; spin++) {
		trigger = nvgsp_rd32(gsp, 0x00b830b0);
		if ((trigger & 0x80000000u) == 0)
			break;
		DELAY(10);
	}
	if ((trigger & 0x80000000u) != 0) {
		nvgpu_log(NVGPU_LOG_INFO,
		    "vmm invalidate timeout pdb=0x%llx trigger=0x%08x\n",
		    (unsigned long long)pdb, trigger);
	}
}

void
nvgsp_vmm_flush(struct nvgsp_vmm *vmm)
{
	if (vmm == NULL || vmm->gsp == NULL)
		return;
	nvgsp_bar_flush_bar1(vmm->gsp);
	nvgsp_vmm_invalidate(vmm);
}

static int
nvgsp_vmm_construct_vaspace(struct nvgsp_vmm *vmm)
{
	struct nvgsp_vaspace_params *args;
	int error;

	memset(&vmm->vaspace, 0, sizeof(vmm->vaspace));
	args = nvgsp_rm_get_alloc(&vmm->device.object,
	    nvgsp_vmm_get_child_handle(&vmm->client, NVGSP_RM_VASPACE),
	    FERMI_VASPACE_A, sizeof(*args), &vmm->vaspace);
	if (args == NULL)
		return (ENOMEM);
	args->index = 0;
	args->flags = 0;
	error = nvgsp_rm_write_alloc(&vmm->vaspace, args);
	if (error != 0)
		memset(&vmm->vaspace, 0, sizeof(vmm->vaspace));
	return (error);
}

static int
nvgsp_vmm_copy_pdes(struct nvgsp_vmm *vmm)
{
	struct nvgsp_copy_pdes_params *ctrl;
	void *reply;
	int error;

	ctrl = nvgsp_rm_get_ctrl(&vmm->vaspace,
	    NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES, sizeof(*ctrl));
	if (ctrl == NULL)
		return (ENOMEM);

	ctrl->hSubDevice = 0;
	ctrl->subDeviceId = 0;
	ctrl->pageSize = 1ULL << NVGSP_GMMU_PD1_SHIFT;
	ctrl->virtAddrLo = vmm->rm_va_base;
	ctrl->virtAddrHi = vmm->rm_va_base + vmm->rm_va_size - 1;
	ctrl->numLevelsToCopy = 3;
	ctrl->levels[0].physAddress = vmm->pt[0].page.vram_paddr;
	ctrl->levels[0].size = (1ULL << 2) * 8;
	ctrl->levels[0].aperture = NV_COPY_PDE_APERTURE_VIDMEM;
	ctrl->levels[0].pageShift = NVGSP_GMMU_PD3_SHIFT;
	ctrl->levels[1].physAddress = vmm->pt[1].page.vram_paddr;
	ctrl->levels[1].size = NVGSP_GMMU_PD2_ENTRIES * 8;
	ctrl->levels[1].aperture = NV_COPY_PDE_APERTURE_VIDMEM;
	ctrl->levels[1].pageShift = NVGSP_GMMU_PD2_SHIFT;
	ctrl->levels[2].physAddress = vmm->pt[2].page.vram_paddr;
	ctrl->levels[2].size = NVGSP_GMMU_PD1_ENTRIES * 8;
	ctrl->levels[2].aperture = NV_COPY_PDE_APERTURE_VIDMEM;
	ctrl->levels[2].pageShift = NVGSP_GMMU_PD1_SHIFT;

	reply = ctrl;
	error = nvgsp_rm_read_ctrl(&vmm->vaspace, &reply, 0);
	return (error);
}

static int
nvgsp_vmm_construct_usermode(struct nvgsp_vmm *vmm)
{
	struct nvgsp_state *gsp = vmm->gsp;
	void *args;
	int error;

	memset(&vmm->usermode, 0, sizeof(vmm->usermode));
	args = nvgsp_rm_get_alloc(&vmm->device.subdevice,
	    nvgsp_vmm_get_child_handle(&vmm->client, NVGSP_RM_USERMODE),
	    TURING_USERMODE_A, 0, &vmm->usermode);
	if (args == NULL)
		return (ENOMEM);
	error = nvgsp_rm_write_alloc(&vmm->usermode, args);
	if (error != 0)
		return (error);
	if (nvgsp_rd32(gsp, 0x00bb0000) == 0)
		nvgsp_wr32(gsp, 0x00bb0000, TURING_USERMODE_A);
	return (0);
}

static int
nvgsp_vmm_construct(struct nvgsp_state *gsp, uint32_t client_handle,
    struct nvgsp_vmm *vmm)
{
	int error, i;

	memset(vmm, 0, sizeof(*vmm));
	vmm->gsp = gsp;
	vmm->rm_va_base = NVGSP_VMM_RM_BASE;
	vmm->rm_va_size = NVGSP_VMM_RM_SIZE;
	lwkt_token_init(&vmm->tok, "nvgsp-vmm");
	LIST_INIT(&vmm->user_pd1_pages);
	LIST_INIT(&vmm->user_pd0_pages);
	LIST_INIT(&vmm->user_pt_pages);
	LIST_INIT(&vmm->sparse_regions);

	error = nvgsp_dma_alloc_dmamem(gsp, NVGSP_GMMU_PT_PAGE_SIZE,
	    NVGSP_GMMU_PT_PAGE_SIZE, &vmm->sparse_page);
	if (error != 0)
		return (error);
	error = nvgsp_rm_construct_client(gsp, client_handle, &vmm->client);
	if (error != 0)
		goto fail_sparse_page;
	error = nvgsp_rm_construct_device(&vmm->client, &vmm->device);
	if (error != 0)
		goto fail_client;

	for (i = 0; i < 3; i++) {
		error = nvgsp_vmm_alloc_pt(gsp, &vmm->pt[i]);
		if (error != 0)
			goto fail_pt;
	}

	nvgsp_bar_wr64_bar1(gsp, vmm->pt[0].page.bar1_gva,
	    nvgsp_pde_to_vram(vmm->pt[1].page.vram_paddr));
	nvgsp_bar_wr64_bar1(gsp, vmm->pt[1].page.bar1_gva,
	    nvgsp_pde_to_vram(vmm->pt[2].page.vram_paddr));
	nvgsp_bar_flush_bar1(gsp);

	error = nvgsp_vmm_construct_vaspace(vmm);
	if (error != 0)
		goto fail_pt;
	error = nvgsp_vmm_copy_pdes(vmm);
	if (error != 0)
		goto fail_vaspace;
	error = nvgsp_vmm_construct_usermode(vmm);
	if (error != 0)
		goto fail_vaspace;

	nvgpu_log(NVGPU_LOG_DEBUG,
	    "vmm ready client=0x%x vaspace=0x%x pd3=0x%llx pd2=0x%llx pd1=0x%llx\n",
	    vmm->client.object.handle, vmm->vaspace.handle,
	    (unsigned long long)vmm->pt[0].page.vram_paddr,
	    (unsigned long long)vmm->pt[1].page.vram_paddr,
	    (unsigned long long)vmm->pt[2].page.vram_paddr);
	return (0);

fail_vaspace:
	nvgsp_rm_free(&vmm->vaspace);
fail_pt:
	for (i = 0; i < 3; i++)
		nvgsp_vmm_free_pt(gsp, &vmm->pt[i]);
	nvgsp_rm_destroy_device(&vmm->device);
fail_client:
	nvgsp_rm_destroy_client(&vmm->client);
fail_sparse_page:
	nvgsp_dma_free_dmamem(gsp, &vmm->sparse_page);
	return (error);
}

static void
nvgsp_vmm_destroy(struct nvgsp_vmm *vmm)
{
	struct nvgsp_vmm_sparse_region *region;
	struct nvgsp_vmm_user_pt *pt;
	struct nvgsp_vmm_pd0 *pd0;
	struct nvgsp_vmm_pd1 *pd1;
	int i;

	if (vmm == NULL || vmm->gsp == NULL)
		return;

	while ((region = LIST_FIRST(&vmm->sparse_regions)) != NULL) {
		LIST_REMOVE(region, link);
		kfree(region, M_NVGSP_VMM);
	}
	while ((pt = LIST_FIRST(&vmm->user_pt_pages)) != NULL) {
		LIST_REMOVE(pt, link);
		nvgsp_bar_free_bar1_page(vmm->gsp, &pt->spt);
		nvgsp_bar_free_bar1_page(vmm->gsp, &pt->lpt);
		kfree(pt, M_NVGSP_VMM);
	}
	while ((pd0 = LIST_FIRST(&vmm->user_pd0_pages)) != NULL) {
		LIST_REMOVE(pd0, link);
		nvgsp_bar_free_bar1_page(vmm->gsp, &pd0->page);
		kfree(pd0, M_NVGSP_VMM);
	}
	while ((pd1 = LIST_FIRST(&vmm->user_pd1_pages)) != NULL) {
		LIST_REMOVE(pd1, link);
		nvgsp_bar_free_bar1_page(vmm->gsp, &pd1->page);
		kfree(pd1, M_NVGSP_VMM);
	}
	nvgsp_rm_free(&vmm->usermode);
	nvgsp_rm_free(&vmm->vaspace);
	for (i = 0; i < 3; i++)
		nvgsp_vmm_free_pt(vmm->gsp, &vmm->pt[i]);
	nvgsp_dma_free_dmamem(vmm->gsp, &vmm->sparse_page);
	nvgsp_rm_destroy_device(&vmm->device);
	nvgsp_rm_destroy_client(&vmm->client);
}

int
nvgsp_vmm_map_sysmem_noflush(struct nvgsp_vmm *vmm, uint64_t va,
    vm_paddr_t paddr, uint64_t size)
{
	uint64_t off;
	int error;

	if (vmm == NULL || ((va | paddr | size) & (NVGSP_GMMU_PT_PAGE_SIZE - 1)) != 0)
		return (EINVAL);

	lwkt_gettoken(&vmm->tok);
	for (off = 0; off < size; off += NVGSP_GMMU_PT_PAGE_SIZE) {
		error = nvgsp_vmm_write_4k_pte(vmm, va + off,
		    nvgsp_pte_to_sysmem((uint64_t)paddr + off));
		if (error != 0) {
			lwkt_reltoken(&vmm->tok);
			return (error);
		}
	}
	lwkt_reltoken(&vmm->tok);
	return (0);
}

int
nvgsp_vmm_map_vram_flags_noflush(struct nvgsp_vmm *vmm, uint64_t va,
    uint64_t paddr, uint64_t size, uint8_t priv, uint8_t ro, uint8_t kind)
{
	uint64_t off;
	int error;

	if (vmm == NULL || ((va | paddr | size) & (NVGSP_GMMU_PT_PAGE_SIZE - 1)) != 0)
		return (EINVAL);

	lwkt_gettoken(&vmm->tok);
	for (off = 0; off < size; off += NVGSP_GMMU_PT_PAGE_SIZE) {
		error = nvgsp_vmm_write_4k_pte(vmm, va + off,
		    nvgsp_pte_to_vram_flags(paddr + off, priv, ro, kind));
		if (error != 0) {
			lwkt_reltoken(&vmm->tok);
			return (error);
		}
	}
	lwkt_reltoken(&vmm->tok);
	return (0);
}

int
nvgsp_vmm_unmap(struct nvgsp_vmm *vmm, uint64_t va, uint64_t size)
{
	struct nvgsp_vmm_user_pt *pt;
	uint64_t off;

	if (vmm == NULL || ((va | size) & (NVGSP_GMMU_PT_PAGE_SIZE - 1)) != 0)
		return (EINVAL);

	lwkt_gettoken(&vmm->tok);
	for (off = 0; off < size; off += NVGSP_GMMU_PT_PAGE_SIZE) {
		pt = nvgsp_vmm_lookup_user_pt_va(vmm, va + off);
		if (pt == NULL)
			continue;
		nvgsp_bar_wr64_bar1(vmm->gsp,
		    pt->spt.bar1_gva + nvgsp_vmm_get_spt_idx(va + off) * 8, 0);
	}
	lwkt_reltoken(&vmm->tok);
	nvgsp_vmm_flush(vmm);
	return (0);
}

int
nvgsp_vmm_init_kernel(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	int error;

	if (gsp == NULL)
		return (ENXIO);
	if (gsp->kernel_vmm != NULL)
		return (0);

	gsp->kernel_vmm = kmalloc(sizeof(*gsp->kernel_vmm), M_NVGSP_VMM,
	    M_WAITOK | M_ZERO);
	error = nvgsp_vmm_construct(gsp, 0xc1d00001u, gsp->kernel_vmm);
	if (error != 0) {
		kfree(gsp->kernel_vmm, M_NVGSP_VMM);
		gsp->kernel_vmm = NULL;
	}
	return (error);
}

void
nvgsp_vmm_fini_kernel(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);

	if (gsp == NULL || gsp->kernel_vmm == NULL)
		return;
	nvgsp_vmm_destroy(gsp->kernel_vmm);
	kfree(gsp->kernel_vmm, M_NVGSP_VMM);
	gsp->kernel_vmm = NULL;
}

int
nvgsp_vmm_create_golden(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	int error;

	if (gsp == NULL)
		return (ENXIO);
	if (gsp->golden_vmm != NULL)
		return (0);

	gsp->golden_vmm = kmalloc(sizeof(*gsp->golden_vmm), M_NVGSP_VMM,
	    M_WAITOK | M_ZERO);
	error = nvgsp_vmm_construct(gsp, 0xc1d00002u, gsp->golden_vmm);
	if (error != 0) {
		kfree(gsp->golden_vmm, M_NVGSP_VMM);
		gsp->golden_vmm = NULL;
	}
	return (error);
}

void
nvgsp_vmm_destroy_golden(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);

	if (gsp == NULL || gsp->golden_vmm == NULL)
		return;
	nvgsp_vmm_destroy(gsp->golden_vmm);
	kfree(gsp->golden_vmm, M_NVGSP_VMM);
	gsp->golden_vmm = NULL;
}

int
nvgsp_vmm_map_submit_pages(struct nvgpu_device *gpu)
{
	(void)gpu;
	return (0);
}
