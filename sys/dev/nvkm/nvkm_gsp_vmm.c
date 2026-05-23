/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM VMM constructor: per-VA-space client/device tree + PT pages
 * + COPY_SERVER_RESERVED_PDES. See refs/nouveau_r570_gmmu.md.
 *
 * The chain reproduced here matches Linux nouveau r535_mmu_vaspace_new
 * with external=false. Page sizes assume Turing's 5-level GMMU with
 * a 512 MiB-per-PD1-entry layout.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/pmap.h>

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include "nvkm_gsp_vmm.h"

static MALLOC_DEFINE(M_NVKM_VMM, "nvkm_vmm", "nvkm GMMU page table pages");

/* The fixed split — both endpoints are documented in
 * Linux nouveau rm/r535/nvrm/vmm.h:
 *   SPLIT_VAS_SERVER_RM_MANAGED_VA_START  0x100000000  (4 GiB)
 *   SPLIT_VAS_SERVER_RM_MANAGED_VA_SIZE       0x20000000 (512 MiB)
 */
/* deprecated: replaced by NVKM_VMM_RM_BASE in nvkm_priv.h */
/* deprecated: replaced by NVKM_VMM_RM_SIZE in nvkm_priv.h */

/* gp100 PDE encoding:
 *   bits [2:1] aperture (1=VRAM, 2=SYS_COH, 3=SYS_NCOH)
 *   bit  [3]   VOL (set for SYS_COH)
 *   bits [39:4] (paddr >> 4)
 *   Non-zero aperture = PDE valid.
 */
#define NVKM_PDE_APERTURE_SYS_COH	(2ULL << 1)
#define NVKM_PDE_VOL			(1ULL << 3)


/* NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES — params from
 * Linux nouveau rm/r535/nvrm/vmm.h. Up to GMMU_FMT_MAX_LEVELS=6
 * level entries. We use 3 (PD3, PD2, PD1). */
#define NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES	0x90f10106U
/* aperture values for NV90F1_CTRL_..._PDES levels[i].aperture
 * (RPC enum, NOT the PDE-encoding bits). r535/nvrm/vmm.h: same as
 * GMMU_APERTURE enum values without the shift. */
#define NV_COPY_PDE_APERTURE_INVALID	0
#define NV_COPY_PDE_APERTURE_VIDMEM	1
#define NV_COPY_PDE_APERTURE_SYS_COH	2
#define NV_COPY_PDE_APERTURE_SYS_NCOH	3

struct NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_LEVEL {
	uint64_t physAddress;
	uint64_t size;		/* bytes occupied at this level */
	uint32_t aperture;
	uint32_t pageShift;
};
struct NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS {
	uint32_t hSubDevice;	/* 0 -> use subDeviceId */
	uint32_t subDeviceId;
	uint64_t pageSize;
	uint64_t virtAddrLo;
	uint64_t virtAddrHi;
	uint32_t numLevelsToCopy;
	uint8_t  _pad[4];
	struct NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_LEVEL levels[6];
};

static int
nvkm_gsp_vmm_pt_alloc(struct nvkm_softc *sc, struct nvkm_gsp_vmm_pt *pt)
{
	return (nvkm_gsp_bar1_alloc_page(sc, &pt->page));
}

static void
nvkm_gsp_vmm_pt_free(struct nvkm_softc *sc, struct nvkm_gsp_vmm_pt *pt)
{
	nvkm_gsp_bar1_free_page(sc, &pt->page);
}

static int
nvkm_gsp_vmm_copy_pdes(struct nvkm_gsp_vmm *vmm)
{
	struct nvkm_softc *sc = vmm->sc;
	struct NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS *ctrl;
	void *p;
	int err;

	ctrl = nvkm_gsp_rm_ctrl_get(&vmm->vaspace,
	    NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES,
	    sizeof(*ctrl));
	if (ctrl == NULL)
		return (ENOMEM);

	ctrl->hSubDevice = 0;
	ctrl->subDeviceId = 1;	/* one-hot for subdevice 0 */
	ctrl->pageSize = 1ULL << 29;	/* 512 MiB — the leaf for PD1 */
	ctrl->virtAddrLo = vmm->rm_va_base;
	ctrl->virtAddrHi = vmm->rm_va_base + vmm->rm_va_size - 1;
	ctrl->numLevelsToCopy = 3;

	/* PD3 — root. Holds 4 entries of 8 bytes each (2-bit index). */
	ctrl->levels[0].physAddress = (uint64_t)vmm->pt[0].page.vram_paddr;
	ctrl->levels[0].size        = (1ULL << 2) * 8;	/* 32 bytes used */
	ctrl->levels[0].aperture    = NV_COPY_PDE_APERTURE_VIDMEM;
	ctrl->levels[0].pageShift   = 47;

	/* PD2 — 512 entries × 8 bytes = 4 KiB. */
	ctrl->levels[1].physAddress = (uint64_t)vmm->pt[1].page.vram_paddr;
	ctrl->levels[1].size        = (1ULL << 9) * 8;	/* 4096 */
	ctrl->levels[1].aperture    = NV_COPY_PDE_APERTURE_VIDMEM;
	ctrl->levels[1].pageShift   = 38;

	/* PD1 — 512 entries × 8 bytes = 4 KiB. */
	ctrl->levels[2].physAddress = (uint64_t)vmm->pt[2].page.vram_paddr;
	ctrl->levels[2].size        = (1ULL << 9) * 8;	/* 4096 */
	ctrl->levels[2].aperture    = NV_COPY_PDE_APERTURE_VIDMEM;
	ctrl->levels[2].pageShift   = 29;

	p = ctrl;
	err = nvkm_gsp_rm_ctrl_rd(&vmm->vaspace, &p, 0);
	if (err != 0) {
		device_printf(sc->dev,
		    "gsp_rm: VASPACE_COPY_SERVER_RESERVED_PDES failed err=%d\n",
		    err);
		return (err);
	}
	device_printf(sc->dev,
	    "gsp_rm: COPY_SERVER_RESERVED_PDES ok (va=0x%llx+0x%llx, "
	    "PD3=0x%llx PD2=0x%llx PD1=0x%llx)\n",
	    (unsigned long long)vmm->rm_va_base,
	    (unsigned long long)vmm->rm_va_size,
	    (unsigned long long)vmm->pt[0].page.vram_paddr,
	    (unsigned long long)vmm->pt[1].page.vram_paddr,
	    (unsigned long long)vmm->pt[2].page.vram_paddr);
	return (0);
}

int
nvkm_gsp_vmm_ctor(struct nvkm_softc *sc, uint32_t client_handle,
    struct nvkm_gsp_vmm *vmm)
{
	int err, i;

	memset(vmm, 0, sizeof(*vmm));
	vmm->sc = sc;
	vmm->rm_va_base = NVKM_VMM_RM_BASE;
	vmm->rm_va_size = NVKM_VMM_RM_SIZE;
	lwkt_token_init(&vmm->tok, "nvkm-vmm");

	/* 1) Client + device + subdevice. */
	err = nvkm_gsp_client_ctor(sc, client_handle, &vmm->client);
	if (err != 0)
		return (err);
	err = nvkm_gsp_device_ctor(&vmm->client, &vmm->device);
	if (err != 0)
		goto fail_client;

	/* 2) Three host sysmem PT pages: PD3, PD2, PD1. */
	for (i = 0; i < 3; i++) {
		err = nvkm_gsp_vmm_pt_alloc(sc, &vmm->pt[i]);
		if (err != 0) {
			device_printf(sc->dev,
			    "gsp_rm: PT page %d alloc failed\n", i);
			goto fail_pt;
		}
	}

	/* 3) Encode PD3[0] = PDE pointing at PD2; PD2[0] = PDE pointing at PD1.
	 *    The 512 MiB at VA 0x100000000 lands in PD1[8] — GSP fills that
	 *    entry itself when it makes RM-internal allocations.
	 *
	 *    PT pages are write-back cacheable sysmem, x86 has PCIe cache
	 *    snoop, so SYS_COH + VOL is the right aperture. */
	/* PD3[0] -> PD2 ; PD2[0] -> PD1. Both PT pages in sysmem; write
	 * via host KVA. The GMMU walker reads sysmem via PCIe coherent
	 * snoop on x86, so these writes are immediately visible. */
	/* PT chain in VRAM, written via BAR1 (L2-coherent). PDE aperture
	 * is VIDMEM (not SYS_COH+VOL). */
	nvkm_gsp_bar1_wr64(sc, vmm->pt[0].page.bar1_gva + 0,
	    nvkm_pde_to_vram(vmm->pt[1].page.vram_paddr));
	nvkm_gsp_bar1_wr64(sc, vmm->pt[1].page.bar1_gva + 0,
	    nvkm_pde_to_vram(vmm->pt[2].page.vram_paddr));

	device_printf(sc->dev,
	    "gsp_rm: PT chain (VRAM) PD3=0x%llx@bar1=0x%llx PD2=0x%llx@bar1=0x%llx PD1=0x%llx@bar1=0x%llx\n",
	    (unsigned long long)vmm->pt[0].page.vram_paddr,
	    (unsigned long long)vmm->pt[0].page.bar1_gva,
	    (unsigned long long)vmm->pt[1].page.vram_paddr,
	    (unsigned long long)vmm->pt[1].page.bar1_gva,
	    (unsigned long long)vmm->pt[2].page.vram_paddr,
	    (unsigned long long)vmm->pt[2].page.bar1_gva);

	/* 4) Allocate FERMI_VASPACE_A (non-external, server-managed PDE flavour). */
	{
		struct NV_VASPACE_ALLOCATION_PARAMETERS_r535 *args;

		args = nvkm_gsp_rm_alloc_get(&vmm->device.object,
		    NVKM_RM_VASPACE, FERMI_VASPACE_A, sizeof(*args),
		    &vmm->vaspace);
		if (args == NULL) {
			err = ENOMEM;
			goto fail_pt;
		}
		args->index = NV_VASPACE_ALLOCATION_INDEX_GPU_NEW;
		args->flags = 0;	/* server-managed */
		err = nvkm_gsp_rm_alloc_wr(&vmm->vaspace, args);
		if (err != 0) {
			device_printf(sc->dev,
			    "gsp_rm: FERMI_VASPACE_A alloc failed err=%d\n",
			    err);
			goto fail_pt;
		}
	}

	/* 5) Hand the PT chain to GSP via COPY_SERVER_RESERVED_PDES. */
	err = nvkm_gsp_vmm_copy_pdes(vmm);
	if (err != 0)
		goto fail_vaspace;

	device_printf(sc->dev,
	    "gsp_rm: VMM ready (client=0x%x device=0x%x vaspace=0x%x)\n",
	    vmm->client.object.handle, vmm->device.object.handle,
	    vmm->vaspace.handle);

	/* Allocate TURING_USERMODE_A at device level (nouveau allocates this
	 * once at drm init). GSP may gate doorbell delivery on its presence. */
	{
		struct nvkm_gsp_object usermode_obj;
		void *up = nvkm_gsp_rm_alloc_get(&vmm->device.subdevice,
		    0xc4610000u, 0x0000c461u, 0, &usermode_obj);
		if (up != NULL) {
			int uerr = nvkm_gsp_rm_alloc_wr(&usermode_obj, up);
			device_printf(sc->dev,
			    "gsp_rm: TURING_USERMODE_A handle=0x%x err=%d (device-level)\n",
			    usermode_obj.handle, uerr);
		}
	}

	return (0);

fail_vaspace:
	nvkm_gsp_rm_free(&vmm->vaspace);
fail_pt:
	for (i = 0; i < 3; i++)
		nvkm_gsp_vmm_pt_free(vmm->sc, &vmm->pt[i]);
	nvkm_gsp_device_dtor(&vmm->device);
fail_client:
	nvkm_gsp_client_dtor(&vmm->client);
	return (err);
}

void
nvkm_gsp_vmm_dtor(struct nvkm_gsp_vmm *vmm)
{
	int i;

	if (vmm->vaspace.handle != 0)
		nvkm_gsp_rm_free(&vmm->vaspace);
	for (i = 0; i < 3; i++)
		nvkm_gsp_vmm_pt_free(vmm->sc, &vmm->pt[i]);
	nvkm_gsp_device_dtor(&vmm->device);
	nvkm_gsp_client_dtor(&vmm->client);
}
