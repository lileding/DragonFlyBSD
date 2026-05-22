/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM RPC helpers — RM_ALLOC / RM_CONTROL / FREE.
 *
 * Closely mirrors Linux nouveau drivers/gpu/drm/nouveau/nvkm/subdev/gsp/
 *   rm/r535/alloc.c   (alloc envelope; reused by r570)
 *   rm/r535/ctrl.c    (control envelope; reused by r570)
 *   rm/r570/client.c  (NV01_ROOT ctor — r570 override, uses pOsPidInfo)
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <linux/slab.h>

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include "nvkm_gsp_vmm.h"
/* === RM_ALLOC === */

void *
nvkm_gsp_rm_alloc_get(struct nvkm_gsp_object *parent, uint32_t handle,
    uint32_t oclass, uint32_t params_size, struct nvkm_gsp_object *new_obj)
{
	struct nvkm_gsp_client *client = parent->client;
	struct nvkm_softc *sc = client->sc;
	struct rpc_gsp_rm_alloc_v03_00 *rpc;

	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC,
	    sizeof(*rpc) + params_size);
	if (rpc == NULL)
		return (NULL);

	new_obj->client = client;
	new_obj->parent = parent;
	new_obj->handle = handle;

	rpc->hClient = client->object.handle;
	rpc->hParent = parent->handle;
	rpc->hObject = handle;
	rpc->hClass  = oclass;
	rpc->status  = 0;
	rpc->paramsSize = params_size;
	rpc->flags   = 0;
	return (rpc->params);
}

/* Recover the rpc envelope from the params pointer we returned to caller. */
static struct rpc_gsp_rm_alloc_v03_00 *
nvkm_gsp_rm_alloc_hdr(void *params)
{
	return (struct rpc_gsp_rm_alloc_v03_00 *)
	    ((uint8_t *)params - offsetof(struct rpc_gsp_rm_alloc_v03_00, params));
}

static struct rpc_gsp_rm_control_v03_00 *
nvkm_gsp_rm_ctrl_hdr(void *params)
{
	return (struct rpc_gsp_rm_control_v03_00 *)
	    ((uint8_t *)params - offsetof(struct rpc_gsp_rm_control_v03_00, params));
}

int
nvkm_gsp_rm_alloc_wr(struct nvkm_gsp_object *obj, void *params)
{
	struct nvkm_softc *sc = obj->client->sc;
	struct rpc_gsp_rm_alloc_v03_00 *rpc = nvkm_gsp_rm_alloc_hdr(params);
	struct rpc_gsp_rm_alloc_v03_00 *rep;
	int ret = 0;
	uint32_t expected_repc = sizeof(*rpc) + rpc->paramsSize;

	rep = nvkm_gsp_rpc_push(sc, rpc, NVKM_GSP_RPC_REPLY_RECV,
	    expected_repc);
	if (rep == NULL)
		return (EIO);

	if (rep->status != 0) {
		device_printf(sc->dev,
		    "gsp_rm: ALLOC cls=0x%x obj=0x%x parent=0x%x failed status=0x%x\n",
		    rpc->hClass, rpc->hObject, rpc->hParent, rep->status);
		ret = EIO;
	}
	nvkm_gsp_rpc_done(sc, rep);
	return (ret);
}

void
nvkm_gsp_rm_alloc_done(struct nvkm_gsp_object *obj, void *params)
{
	struct nvkm_softc *sc = obj->client->sc;
	struct rpc_gsp_rm_alloc_v03_00 *rpc = nvkm_gsp_rm_alloc_hdr(params);

	nvkm_gsp_rpc_done(sc, rpc);
}

int
nvkm_gsp_rm_free(struct nvkm_gsp_object *obj)
{
	struct nvkm_softc *sc = obj->client->sc;
	struct rpc_free_v03_00 *rpc;

	if (obj->handle == 0)
		return (0);

	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_FREE, sizeof(*rpc));
	if (rpc == NULL)
		return (EIO);
	rpc->params.hRoot = obj->client->object.handle;
	rpc->params.hObjectParent = 0;
	rpc->params.hObjectOld = obj->handle;
	rpc->params.status = 0;
	return (nvkm_gsp_rpc_wr(sc, rpc, NVKM_GSP_RPC_REPLY_RECV));
}

/* === RM_CONTROL === */

void *
nvkm_gsp_rm_ctrl_get(struct nvkm_gsp_object *obj, uint32_t cmd,
    uint32_t params_size)
{
	struct nvkm_gsp_client *client = obj->client;
	struct nvkm_softc *sc = client->sc;
	struct rpc_gsp_rm_control_v03_00 *rpc;

	rpc = nvkm_gsp_rpc_get(sc, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL,
	    sizeof(*rpc) + params_size);
	if (rpc == NULL)
		return (NULL);

	rpc->hClient = client->object.handle;
	rpc->hObject = obj->handle;
	rpc->cmd     = cmd;
	rpc->status  = 0;
	rpc->paramsSize = params_size;
	rpc->flags   = 0;
	return (rpc->params);
}

int
nvkm_gsp_rm_ctrl_rd(struct nvkm_gsp_object *obj, void **params, uint32_t repc)
{
	struct nvkm_softc *sc = obj->client->sc;
	struct rpc_gsp_rm_control_v03_00 *rpc = nvkm_gsp_rm_ctrl_hdr(*params);
	struct rpc_gsp_rm_control_v03_00 *rep;
	int ret = 0;
	uint32_t expected_repc = sizeof(*rpc) + repc;

	rep = nvkm_gsp_rpc_push(sc, rpc, NVKM_GSP_RPC_REPLY_RECV,
	    expected_repc);
	if (rep == NULL) {
		*params = NULL;
		return (EIO);
	}

	if (rep->status != 0) {
		device_printf(sc->dev,
		    "gsp_rm: CONTROL cmd=0x%x obj=0x%x failed status=0x%x\n",
		    rpc->cmd, rpc->hObject, rep->status);
		ret = EIO;
	}

	if (repc != 0)
		*params = rep->params;	/* caller frees via _done */
	else {
		nvkm_gsp_rpc_done(sc, rep);
		*params = NULL;
	}
	return (ret);
}

int
nvkm_gsp_rm_ctrl_wr(struct nvkm_gsp_object *obj, void *params)
{
	void *p = params;
	return (nvkm_gsp_rm_ctrl_rd(obj, &p, 0));
}

void
nvkm_gsp_rm_ctrl_done(struct nvkm_gsp_object *obj, void *params)
{
	struct nvkm_softc *sc = obj->client->sc;
	struct rpc_gsp_rm_control_v03_00 *rpc = nvkm_gsp_rm_ctrl_hdr(params);

	nvkm_gsp_rpc_done(sc, rpc);
}

/* === Client root === */

/* NV0000_ALLOC_PARAMETERS — r570 layout (open-rm 570.144).
 * NOTE: r535 lacks pOsPidInfo; we MUST use r570 layout against r570 firmware. */
struct NV0000_ALLOC_PARAMETERS_r570 {
	uint32_t hClient;
	uint32_t processID;
	char     processName[100];
	uint8_t  _pad_to_align8[4];	/* pOsPidInfo is 8-aligned */
	uint64_t pOsPidInfo;
};
#define NV01_ROOT	0x00000000U

int
nvkm_gsp_client_ctor(struct nvkm_softc *sc, uint32_t handle,
    struct nvkm_gsp_client *client)
{
	struct NV0000_ALLOC_PARAMETERS_r570 *args;
	int err;

	memset(client, 0, sizeof(*client));
	client->sc = sc;
	client->object.client = client;
	client->object.parent = NULL;
	client->object.handle = handle;

	args = nvkm_gsp_rm_alloc_get(&client->object, handle, NV01_ROOT,
	    sizeof(*args), &client->object);
	if (args == NULL)
		return (ENOMEM);

	args->hClient   = handle;
	args->processID = (uint32_t)~0u;
	strncpy(args->processName, "dfly-nvkm", sizeof(args->processName));
	args->pOsPidInfo = 0;

	err = nvkm_gsp_rm_alloc_wr(&client->object, args);
	if (err != 0) {
		device_printf(sc->dev,
		    "gsp_rm: client_ctor(handle=0x%x) failed err=%d\n",
		    handle, err);
		return (err);
	}
	device_printf(sc->dev,
	    "gsp_rm: client root allocated handle=0x%x\n", handle);
	return (0);
}

int
nvkm_gsp_client_dtor(struct nvkm_gsp_client *client)
{
	if (client->object.handle == 0)
		return (0);
	return (nvkm_gsp_rm_free(&client->object));
}

/* === NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE ===
 *
 * Linux nouveau queries this in r5xx fifo subdev oneinit. Stores
 * the per-channel CE fault method buffer size, which is then used
 * as args->mthdbufMem.size in channel alloc.
 *
 * Reply struct just has u32 size at offset 0 (open-rm 570.144). */
#define NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE	0x20802a08U

struct NV2080_CTRL_CE_GET_FAULT_METHOD_BUFFER_SIZE_PARAMS {
	uint32_t size;
};

int
nvkm_gsp_query_mthdbuf_size(struct nvkm_softc *sc)
{
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct NV2080_CTRL_CE_GET_FAULT_METHOD_BUFFER_SIZE_PARAMS *p;
	void *q;
	int err;

	if (sc->gsp_internal_subdevice == 0) {
		device_printf(sc->dev,
		    "gsp_rm: no internal subdevice handle\n");
		return (ENXIO);
	}

	/* Build a stack-local object pair so we can RM_CONTROL the
	 * GSP-internal subdevice without owning it. */
	memset(&tmp_client, 0, sizeof(tmp_client));
	tmp_client.sc = sc;
	tmp_client.object.client = &tmp_client;
	tmp_client.object.handle = sc->gsp_internal_client;
	tmp_subdev.client = &tmp_client;
	tmp_subdev.parent = NULL;
	tmp_subdev.handle = sc->gsp_internal_subdevice;

	p = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE, sizeof(*p));
	if (p == NULL)
		return (ENOMEM);
	q = p;
	err = nvkm_gsp_rm_ctrl_rd(&tmp_subdev, &q, sizeof(*p));
	if (err != 0 || q == NULL) {
		device_printf(sc->dev,
		    "gsp_rm: CE_GET_FAULT_METHOD_BUFFER_SIZE failed err=%d\n",
		    err);
		return (err ? err : EIO);
	}
	sc->mthdbuf_size = ((struct NV2080_CTRL_CE_GET_FAULT_METHOD_BUFFER_SIZE_PARAMS *)q)->size;
	nvkm_gsp_rm_ctrl_done(&tmp_subdev, q);

	device_printf(sc->dev,
	    "gsp_rm: CE mthdbuf_size = 0x%x\n", sc->mthdbuf_size);
	return (0);
}

/* === NV01_DEVICE_0 + NV20_SUBDEVICE_0 ===
 * No r570 override for these; structs match Linux nouveau r535/nvrm/device.h. */

#define NV01_DEVICE_0		0x00000080U
#define NV20_SUBDEVICE_0	0x00002080U

/* Nouveau handle scheme (drm/nouveau/nvkm/subdev/gsp/rm/handles.h). */
#define NVKM_RM_DEVICE		0xde1d0000u
#define NVKM_RM_SUBDEVICE	0x5d1d0000u

/* Open-rm uses NV_DECLARE_ALIGNED(NvU64 ..., 8); on x86_64 the default
 * alignof(u64)==8 so the compiler inserts 4 bytes of padding between
 * flags and vaSpaceSize automatically. Do NOT mark this packed. */
struct NV0080_ALLOC_PARAMETERS_r535 {
	uint32_t deviceId;
	uint32_t hClientShare;
	uint32_t hTargetClient;
	uint32_t hTargetDevice;
	int32_t  flags;
	uint64_t vaSpaceSize;
	uint64_t vaStartInternal;
	uint64_t vaLimitInternal;
	int32_t  vaMode;
};

struct NV2080_ALLOC_PARAMETERS_r535 {
	uint32_t subDeviceId;
};

int
nvkm_gsp_device_ctor(struct nvkm_gsp_client *client,
    struct nvkm_gsp_device *device)
{
	struct nvkm_softc *sc = client->sc;
	struct NV0080_ALLOC_PARAMETERS_r535 *dargs;
	struct NV2080_ALLOC_PARAMETERS_r535 *sargs;
	int err;

	memset(device, 0, sizeof(*device));

	dargs = nvkm_gsp_rm_alloc_get(&client->object, NVKM_RM_DEVICE,
	    NV01_DEVICE_0, sizeof(*dargs), &device->object);
	if (dargs == NULL)
		return (ENOMEM);
	dargs->hClientShare = client->object.handle;
	err = nvkm_gsp_rm_alloc_wr(&device->object, dargs);
	if (err != 0) {
		device_printf(sc->dev,
		    "gsp_rm: NV01_DEVICE alloc failed err=%d\n", err);
		return (err);
	}
	device_printf(sc->dev,
	    "gsp_rm: NV01_DEVICE_0 handle=0x%x ok\n", device->object.handle);

	sargs = nvkm_gsp_rm_alloc_get(&device->object, NVKM_RM_SUBDEVICE,
	    NV20_SUBDEVICE_0, sizeof(*sargs), &device->subdevice);
	if (sargs == NULL) {
		nvkm_gsp_rm_free(&device->object);
		return (ENOMEM);
	}
	sargs->subDeviceId = 0;
	err = nvkm_gsp_rm_alloc_wr(&device->subdevice, sargs);
	if (err != 0) {
		device_printf(sc->dev,
		    "gsp_rm: NV20_SUBDEVICE alloc failed err=%d\n", err);
		nvkm_gsp_rm_free(&device->object);
		return (err);
	}
	device_printf(sc->dev,
	    "gsp_rm: NV20_SUBDEVICE_0 handle=0x%x ok\n",
	    device->subdevice.handle);
	return (0);
}

int
nvkm_gsp_device_dtor(struct nvkm_gsp_device *device)
{
	nvkm_gsp_rm_free(&device->subdevice);
	nvkm_gsp_rm_free(&device->object);
	return (0);
}

/* === FERMI_VASPACE_A ===
 * No r570 override (.vmm is part of r535_alloc/r535_ctrl). Struct from
 * Linux nouveau r535/nvrm/vmm.h (open-rm 535.113.01). The same layout
 * is accepted by r570 GSP firmware. */

int
nvkm_gsp_vaspace_ctor(struct nvkm_gsp_device *device,
    struct nvkm_gsp_vaspace *vas)
{
	struct nvkm_gsp_client *client = device->object.client;
	struct nvkm_softc *sc = client->sc;
	struct NV_VASPACE_ALLOCATION_PARAMETERS_r535 *args;
	int err;

	memset(vas, 0, sizeof(*vas));
	args = nvkm_gsp_rm_alloc_get(&device->object, NVKM_RM_VASPACE,
	    FERMI_VASPACE_A, sizeof(*args), &vas->object);
	if (args == NULL)
		return (ENOMEM);

	args->index = NV_VASPACE_ALLOCATION_INDEX_GPU_NEW;
	/* Non-external vaspace; channel alloc still needs PDE copy via
	 * NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES — TODO. */
	err = nvkm_gsp_rm_alloc_wr(&vas->object, args);
	if (err != 0) {
		device_printf(sc->dev,
		    "gsp_rm: FERMI_VASPACE_A alloc failed err=%d\n", err);
		return (err);
	}
	device_printf(sc->dev,
	    "gsp_rm: FERMI_VASPACE_A handle=0x%x ok\n", vas->object.handle);
	return (0);
}

int
nvkm_gsp_vaspace_dtor(struct nvkm_gsp_vaspace *vas)
{
	return (nvkm_gsp_rm_free(&vas->object));
}

/* === chid pool (host-side, 2048 bits, chid 0 reserved) ===
 *
 * 2048-entry bitmap, chid 0 reserved (rsvd_chids=1 in r570_fifo).
 * Init: set bit 0. Alloc: find first 0-bit, set, return id.
 * Free: clear bit. Mirrors nouveau chid.c. */
void
nvkm_chid_init(struct nvkm_softc *sc)
{
	lwkt_token_init(&sc->chid_tok, "nvkm-chid");
	memset(sc->chid_used, 0, sizeof(sc->chid_used));
	sc->chid_used[0] |= 1ULL;  /* chid 0 reserved */
}

int
nvkm_chid_alloc(struct nvkm_softc *sc)
{
	int chid = -1;
	lwkt_gettoken(&sc->chid_tok);
	for (int w = 0; w < 32 && chid < 0; w++) {
		uint64_t v = sc->chid_used[w];
		if (v == ~0ULL) continue;
		for (int b = 0; b < 64; b++) {
			if (!(v & (1ULL << b))) {
				sc->chid_used[w] |= (1ULL << b);
				chid = w * 64 + b;
				break;
			}
		}
	}
	lwkt_reltoken(&sc->chid_tok);
	return chid;
}

void
nvkm_chid_free(struct nvkm_softc *sc, int chid)
{
	if (chid < 0 || chid >= 2048)
		return;
	lwkt_gettoken(&sc->chid_tok);
	sc->chid_used[chid >> 6] &= ~(1ULL << (chid & 63));
	lwkt_reltoken(&sc->chid_tok);
}

/* === VRAM bump allocator ===
 *
 * Carve out a fixed safe window inside the GPU's local VRAM. Eventually
 * this should be replaced with proper fbRegion parsing (see
 * r535_gsp_get_static_info_fb in Linux nouveau), but the FwSec-stitched
 * WPR2 region on our 2080 Ti lives near the very top of the 11 GiB
 * frame buffer (~0x2b7900000), so a range well below that is safe for
 * small driver allocations.
 */

#define NVKM_VRAM_BUMP_BASE	0x10000000ULL	/* 256 MiB into VRAM */
#define NVKM_VRAM_BUMP_SIZE	0x10000000ULL	/* 256 MiB window */

int
nvkm_gsp_vram_init(struct nvkm_softc *sc)
{
	uint64_t base, size;

	base = sc->fb_usable_base;
	size = sc->fb_usable_size;
	if (size == 0) {
		device_printf(sc->dev,
		    "gsp_rm: usable VRAM region unknown (static_info parse failed)\n");
		return (ENXIO);
	}

	/* Leave 64 MiB at the top of the chosen region as a safety margin
	 * (FwSec/booter footprint sits near the top of FB). */
	if (size > (64ULL << 20))
		size -= (64ULL << 20);

	/* nouveau allocates instmem from the TOP of VRAM (nvkm_ram_get
	 * with back=true -> nvkm_mm_tail). Stick to that pattern: bump
	 * downward from limit. base = lower bound; next = current top edge. */
	sc->vram_bump_base  = base;
	sc->vram_bump_next  = base + size;
	sc->vram_bump_limit = base + size;
	device_printf(sc->dev,
	    "gsp_rm: VRAM bump window 0x%llx..0x%llx (alloc top-down)\n",
	    (unsigned long long)sc->vram_bump_base,
	    (unsigned long long)sc->vram_bump_limit);
	return (0);
}

uint64_t
nvkm_gsp_vram_alloc(struct nvkm_softc *sc, uint64_t size, uint64_t align)
{
	uint64_t off;

	if (align == 0)
		align = 0x1000;	/* PAGE_SIZE */
	size = (size + align - 1) & ~(align - 1);

	/* Top-down bump: lower the next pointer by size, then align down. */
	if (sc->vram_bump_next < sc->vram_bump_base + size) {
		device_printf(sc->dev,
		    "gsp_rm: VRAM bump alloc exhausted (need 0x%llx)\n",
		    (unsigned long long)size);
		return (0);
	}
	off = (sc->vram_bump_next - size) & ~(align - 1);
	if (off < sc->vram_bump_base) {
		device_printf(sc->dev,
		    "gsp_rm: VRAM bump alloc exhausted (align mismatch)\n");
		return (0);
	}
	sc->vram_bump_next = off;
	device_printf(sc->dev,
	    "gsp_rm: VRAM alloc 0x%llx (size 0x%llx)\n",
	    (unsigned long long)off, (unsigned long long)size);
	return (off);
}

/* === KEPLER_CHANNEL_GROUP_A (TSG) ===
 * Engine-bound channel group. NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS is
 * defined in open-rm 570.144 src/common/sdk/nvidia/inc/nvos.h. */

#define KEPLER_CHANNEL_GROUP_A		0x0000a06cU
#define NVKM_RM_CHGRP			0xa06c0000u

struct NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS_r570 {
	uint32_t hObjectError;
	uint32_t hObjectEccError;
	uint32_t hVASpace;
	uint32_t engineType;
	uint8_t  bIsCallingContextVgpuPlugin;
	uint8_t  _pad[3];
};

int
nvkm_gsp_chgrp_ctor(struct nvkm_gsp_device *device,
    struct nvkm_gsp_vaspace *vas, uint32_t engine_type,
    struct nvkm_gsp_chgrp *grp)
{
	struct nvkm_softc *sc = device->object.client->sc;
	struct NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS_r570 *args;
	int err;

	memset(grp, 0, sizeof(*grp));
	args = nvkm_gsp_rm_alloc_get(&device->object, NVKM_RM_CHGRP,
	    KEPLER_CHANNEL_GROUP_A, sizeof(*args), &grp->object);
	if (args == NULL)
		return (ENOMEM);

	args->hVASpace = vas->object.handle;
	args->engineType = engine_type;

	err = nvkm_gsp_rm_alloc_wr(&grp->object, args);
	if (err != 0) {
		device_printf(sc->dev,
		    "gsp_rm: KEPLER_CHANNEL_GROUP_A engineType=0x%x failed err=%d\n",
		    engine_type, err);
		return (err);
	}
	device_printf(sc->dev,
	    "gsp_rm: KEPLER_CHANNEL_GROUP_A handle=0x%x engineType=0x%x ok\n",
	    grp->object.handle, engine_type);
	return (0);
}

int
nvkm_gsp_chgrp_dtor(struct nvkm_gsp_chgrp *grp)
{
	return (nvkm_gsp_rm_free(&grp->object));
}

/* === TURING_CHANNEL_GPFIFO_A ===
 *
 * Mirror of Linux nouveau drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/
 * fifo.c r570_chan_alloc. Struct from rm/r570/nvrm/fifo.h
 * (NV_CHANNEL_ALLOC_PARAMS). The r570 layout is slightly larger than
 * r535 (CC IV/nonce fields added) — use r570.
 *
 * gpFifoOffset is left at 0 for now: GSP only validates it at first
 * push/exec, and we're not pushing yet. mthdbufMem size is hard-coded
 * to 0x4000 (Turing default) instead of querying
 * NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE — TODO.
 */

#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>

#define TURING_CHANNEL_GPFIFO_A		0x0000c46fU
#define NVKM_RM_CHANNEL			0xf1f00000u  /* nouveau NVKM_RM_CHAN(0) */

#define NV_CHANNEL_INST_SIZE		0x1000U
#define NV_CHANNEL_USERD_SIZE		0x200U
#define NV_CHANNEL_RAMFC_SIZE		0x200U
/* mthdbuf size now queried from GSP at attach; sc->mthdbuf_size. */
#define NV_CHANNEL_GPFIFO_ENTRIES	0x80U

#define NV_MEMORY_DESC_ADDRSPACE_SYSMEM_COH	0U
#define NV_MEMORY_DESC_ADDRSPACE_SYSMEM_NONCOH	1U
#define NV_MEMORY_DESC_ADDRSPACE_VIDMEM		2U

/* Bit field positions extracted from r570/nvrm/fifo.h. */
#define NVOS04_FLAGS_CHANNEL_USERD_INDEX_PAGE_FIXED	(1U << 21)

#define NV_KERNELCHANNEL_INTERNALFLAGS_PRIV_USER	(0U << 0)
#define NV_KERNELCHANNEL_INTERNALFLAGS_PRIV_ADMIN	(1U << 0)
#define NV_KERNELCHANNEL_INTERNALFLAGS_ERRNOT_NONE	(1U << 2)
#define NV_KERNELCHANNEL_INTERNALFLAGS_ECCNOT_NONE	(1U << 4)

#define NV_MAX_SUBDEVICES	8
#define CC_CHAN_ALLOC_IV_SIZE_DWORD	3U
#define CC_CHAN_ALLOC_NONCE_SIZE_DWORD	8U

struct NV_MEMORY_DESC_PARAMS_r570 {
	uint64_t base;
	uint64_t size;
	uint32_t addressSpace;
	uint32_t cacheAttrib;
};

struct NV_CHANNEL_ALLOC_PARAMS_r570 {
	uint32_t hObjectError;
	uint32_t hObjectBuffer;
	uint64_t gpFifoOffset;
	uint32_t gpFifoEntries;
	uint32_t flags;
	uint32_t hContextShare;
	uint32_t hVASpace;
	uint32_t hUserdMemory[NV_MAX_SUBDEVICES];
	uint64_t userdOffset[NV_MAX_SUBDEVICES];
	uint32_t engineType;
	uint32_t cid;
	uint32_t subDeviceId;
	uint32_t hObjectEccError;
	struct NV_MEMORY_DESC_PARAMS_r570 instanceMem;
	struct NV_MEMORY_DESC_PARAMS_r570 userdMem;
	struct NV_MEMORY_DESC_PARAMS_r570 ramfcMem;
	struct NV_MEMORY_DESC_PARAMS_r570 mthdbufMem;
	uint32_t hPhysChannelGroup;
	uint32_t internalFlags;
	struct NV_MEMORY_DESC_PARAMS_r570 errorNotifierMem;
	struct NV_MEMORY_DESC_PARAMS_r570 eccErrorNotifierMem;
	uint32_t ProcessID;
	uint32_t SubProcessID;
	uint32_t encryptIv[CC_CHAN_ALLOC_IV_SIZE_DWORD];
	uint32_t decryptIv[CC_CHAN_ALLOC_IV_SIZE_DWORD];
	uint32_t hmacNonce[CC_CHAN_ALLOC_NONCE_SIZE_DWORD];
	uint32_t tpcConfigID;
};

static MALLOC_DEFINE(M_NVKM_MTHDBUF, "nvkm_mthdbuf", "nvkm CE method buffer");

int
nvkm_gsp_chan_ctor(struct nvkm_gsp_vmm *vmm,
    uint32_t engine_type, struct nvkm_gsp_chan *chan)
{
	struct nvkm_gsp_device *device = &vmm->device;
	struct nvkm_softc *sc = vmm->sc;
	struct NV_CHANNEL_ALLOC_PARAMS_r570 *args;
	int err;

	memset(chan, 0, sizeof(*chan));

	/* VRAM: inst block + USERD (separate pages). */
	chan->inst_vram  = nvkm_gsp_vram_alloc(sc, NV_CHANNEL_INST_SIZE, 0x1000);
	chan->userd_vram = nvkm_gsp_vram_alloc(sc, NV_CHANNEL_USERD_SIZE, 0x1000);
	if (chan->inst_vram == 0 || chan->userd_vram == 0) {
		device_printf(sc->dev,
		    "gsp_rm: channel VRAM alloc failed\n");
		return (ENOMEM);
	}

	/* sysmem: CE method buffer. Size queried at attach. */
	uint32_t mthdbuf_sz = sc->mthdbuf_size ?
	    sc->mthdbuf_size : 0x4000U;
	chan->mthdbuf_kva = contigmalloc(mthdbuf_sz, M_NVKM_MTHDBUF,
	    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, 0x1000, 0);
	if (chan->mthdbuf_kva == NULL) {
		device_printf(sc->dev,
		    "gsp_rm: mthdbuf contigmalloc failed\n");
		return (ENOMEM);
	}
	chan->mthdbuf_paddr = vtophys(chan->mthdbuf_kva);
	chan->mthdbuf_size = mthdbuf_sz;

	args = nvkm_gsp_rm_alloc_get(&device->object, NVKM_RM_CHANNEL,
	    TURING_CHANNEL_GPFIFO_A, sizeof(*args), &chan->object);
	if (args == NULL) {
		contigfree(chan->mthdbuf_kva, chan->mthdbuf_size,
		    M_NVKM_MTHDBUF);
		chan->mthdbuf_kva = NULL;
		return (ENOMEM);
	}

	/* gpFifoOffset stays 0 — GSP only validates it on first push. */
	args->gpFifoEntries = NV_CHANNEL_GPFIFO_ENTRIES;
	/* chid allocated from host pool (rsvd_chids=1 ->
	 * nvkm_chid_alloc starts at 1). Encode into
	 * USERD_INDEX_VALUE/PAGE_VALUE per r570/fifo.c:r570_chan_alloc. */
	chan->chid = nvkm_chid_alloc(sc);
	if (chan->chid < 0) {
		contigfree(chan->mthdbuf_kva, chan->mthdbuf_size,
		    M_NVKM_MTHDBUF);
		chan->mthdbuf_kva = NULL;
		return (ENOMEM);
	}
	{
		uint32_t userd_p = (uint32_t)chan->chid / 8u;
		uint32_t userd_i = (uint32_t)chan->chid % 8u;
		args->flags =
		    ((userd_i & 7u) << 8) |
		    ((userd_p & 0x1ffu) << 12) |
		    (1U << 21) /* USERD_INDEX_PAGE_FIXED */ |
		    (1U << 5) /* PRIVILEGED_CHANNEL_TRUE */;
	}
	args->hVASpace = vmm->vaspace.handle;
	args->engineType = engine_type;
	/* subDeviceId stays 0 — matches nouveau */

	args->instanceMem.base = chan->inst_vram;
	args->instanceMem.size = NV_CHANNEL_INST_SIZE;
	args->instanceMem.addressSpace = NV_MEMORY_DESC_ADDRSPACE_VIDMEM;
	args->instanceMem.cacheAttrib = 1;

	args->userdMem.base = chan->userd_vram;
	/* Cover the whole 4 KiB USERD page so GSP can pick any
	 * chid in [1, 7] (chid 0 is reserved per rsvd_chids=1
	 * in nouveau r570_fifo). Each slot is 0x200 bytes. */
	args->userdMem.size = 0x1000;
	args->userdMem.addressSpace = NV_MEMORY_DESC_ADDRSPACE_VIDMEM;
	args->userdMem.cacheAttrib = 1;

	args->ramfcMem.base = chan->inst_vram;	/* ramfc lives inside inst block */
	args->ramfcMem.size = NV_CHANNEL_RAMFC_SIZE;
	args->ramfcMem.addressSpace = NV_MEMORY_DESC_ADDRSPACE_VIDMEM;
	args->ramfcMem.cacheAttrib = 1;

	args->mthdbufMem.base = chan->mthdbuf_paddr;
	args->mthdbufMem.size = mthdbuf_sz;
	args->mthdbufMem.addressSpace = NV_MEMORY_DESC_ADDRSPACE_SYSMEM_NONCOH;
	args->mthdbufMem.cacheAttrib = 0;

	args->internalFlags =
	    NV_KERNELCHANNEL_INTERNALFLAGS_PRIV_ADMIN |
	    NV_KERNELCHANNEL_INTERNALFLAGS_ERRNOT_NONE |
	    NV_KERNELCHANNEL_INTERNALFLAGS_ECCNOT_NONE;

	err = nvkm_gsp_rm_alloc_wr(&chan->object, args);
	if (err != 0) {
		device_printf(sc->dev,
		    "gsp_rm: TURING_CHANNEL_GPFIFO_A alloc failed err=%d "
		    "(inst=0x%llx userd=0x%llx mthdbuf=0x%llx)\n", err,
		    (unsigned long long)chan->inst_vram,
		    (unsigned long long)chan->userd_vram,
		    (unsigned long long)chan->mthdbuf_paddr);
		contigfree(chan->mthdbuf_kva, chan->mthdbuf_size,
		    M_NVKM_MTHDBUF);
		chan->mthdbuf_kva = NULL;
		return (err);
	}

	device_printf(sc->dev,
	    "gsp_rm: TURING_CHANNEL_GPFIFO_A handle=0x%x engine=0x%x ok\n",
	    chan->object.handle, engine_type);
	return (0);
}

int
nvkm_gsp_chan_dtor(struct nvkm_gsp_chan *chan)
{
	struct nvkm_softc *sc = chan->object.client ?
	    chan->object.client->sc : NULL;
	int err = nvkm_gsp_rm_free(&chan->object);
	if (sc != NULL && chan->chid > 0)
		nvkm_chid_free(sc, chan->chid);
	if (chan->mthdbuf_kva != NULL) {
		contigfree(chan->mthdbuf_kva, chan->mthdbuf_size,
		    M_NVKM_MTHDBUF);
		chan->mthdbuf_kva = NULL;
	}
	return (err);
}
