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

/* NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE (0x20800a5c).
 * Mirror of nouveau r535/gsp.c:r535_gsp_intr_get_table. nouveau treats this
 * as mandatory in postinit -- the RPC registers the host as the intr
 * receiver on GSP side and is paired with a BAR0+0x110004 = 0x40 write to
 * enable hardware delivery of the GSP-managed intr line. Without it, GSP
 * does not forward channel/PBDMA events to host. */
#define NV2080_CTRL_INTERNAL_INTR_MAX_TABLE_SIZE 128
#define NV2080_INTR_CATEGORY_ENUM_COUNT          7
#define NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE 0x20800a5cu

struct NV2080_CTRL_INTERNAL_INTR_GET_KERNEL_TABLE_ENTRY_dfly {
	uint16_t engineIdx;
	uint16_t _pad;
	uint32_t pmcIntrMask;
	uint32_t vectorStall;
	uint32_t vectorNonStall;
};

struct NV2080_CTRL_INTERNAL_INTR_GET_KERNEL_TABLE_PARAMS_dfly {
	uint32_t tableLen;
	struct NV2080_CTRL_INTERNAL_INTR_GET_KERNEL_TABLE_ENTRY_dfly
	       table[NV2080_CTRL_INTERNAL_INTR_MAX_TABLE_SIZE];
	uint8_t subtreeMap[NV2080_INTR_CATEGORY_ENUM_COUNT * 2];
};

int
nvkm_gsp_intr_get_kernel_table(struct nvkm_softc *sc)
{
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct NV2080_CTRL_INTERNAL_INTR_GET_KERNEL_TABLE_PARAMS_dfly *p;
	void *q;
	int err;

	if (sc->gsp_internal_subdevice == 0)
		return (ENXIO);

	memset(&tmp_client, 0, sizeof(tmp_client));
	tmp_client.sc = sc;
	tmp_client.object.client = &tmp_client;
	tmp_client.object.handle = sc->gsp_internal_client;
	tmp_subdev.client = &tmp_client;
	tmp_subdev.parent = NULL;
	tmp_subdev.handle = sc->gsp_internal_subdevice;

	p = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE, sizeof(*p));
	if (p == NULL)
		return (ENOMEM);
	q = p;
	err = nvkm_gsp_rm_ctrl_rd(&tmp_subdev, &q, sizeof(*p));
	if (err != 0 || q == NULL) {
		device_printf(sc->dev,
		    "gsp_rm: INTR_GET_KERNEL_TABLE failed err=%d\n", err);
		return (err ? err : EIO);
	}
	{
		struct NV2080_CTRL_INTERNAL_INTR_GET_KERNEL_TABLE_PARAMS_dfly *r = q;
		device_printf(sc->dev,
		    "gsp_rm: INTR_GET_KERNEL_TABLE tableLen=%u\n",
		    r->tableLen);
		for (uint32_t i = 0; i < r->tableLen && i < 8u; i++) {
			device_printf(sc->dev,
			    "  [%u] engineIdx=%3u mask=0x%08x stall=%u nonStall=%u\n",
			    i, r->table[i].engineIdx, r->table[i].pmcIntrMask,
			    r->table[i].vectorStall, r->table[i].vectorNonStall);
		}
		/* Match Fedora 44 nouveau\'s EN_SET pattern exactly. Over-enabling
		 * triggers spurious GSP intr we never service. */
		nvkm_wr32(sc, 0xb81200u, 0x00031c80u);  /* leaf[0] */
		nvkm_wr32(sc, 0xb81210u, 0x0c000000u);  /* leaf[4] */
		device_printf(sc->dev,
		    "gsp_rm: intr_allow Fedora-pattern leaf[0]=0x00031c80 leaf[4]=0x0c000000\n");
	}
	nvkm_gsp_rm_ctrl_done(&tmp_subdev, q);

	/* Pair with the hardware enable nouveau does immediately after
	 * (r535/gsp.c:319). BAR0+0x110004 = 0x40 -- specific GSP intr line. */
	nvkm_wr32(sc, 0x00110004u, 0x00000040u);
	device_printf(sc->dev,
	    "gsp_rm: enabled GSP intr (BAR0+0x110004 = 0x40)\n");
	return (0);
}

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
	device_printf(sc->dev,
	    "gsp_rm: vram_init: sc->fb_usable_base=0x%llx size=0x%llx mthdbuf=0x%x\n",
	    (unsigned long long)sc->fb_usable_base,
	    (unsigned long long)sc->fb_usable_size,
	    sc->mthdbuf_size);
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

/* === GPU submit smoke test ===
 * Mirrors the chan_init + push + kick path nouveau runs from userspace
 * for Volta+ GPFIFO channels, but driven entirely from kernel:
 * host writes PT, builds push, kicks via PRAMIN/USERD, rings doorbell.
 */
/* === Submit BO GVAs (client-managed VMM range) ===
 * Three 4-KiB sysmem BOs mapped at consecutive GVAs starting at
 * NVKM_VMM_CLIENT_BASE. PD1 entry index = CLIENT_BASE >> PD1_SHIFT;
 * choosing CLIENT_BASE = 16 GiB lands them on PD1[32], well clear of
 * the server-reserved PD1[8]. */
#define SUBMIT_GVA_PUSHBUF	(NVKM_VMM_CLIENT_BASE + 0x0000ULL)
#define SUBMIT_GVA_GPFIFO	(NVKM_VMM_CLIENT_BASE + 0x1000ULL)
#define SUBMIT_GVA_SEMA		(NVKM_VMM_CLIENT_BASE + 0x2000ULL)

/* === Sem release payload === */
#define SEM_PAYLOAD		0xdeadbeefu

/* === NVC36F method push header constants (clc36f.h:95-128) ===
 * Header dword format (push906f.h:23-49):
 *   bits 28:16 method-data count
 *   bits 31:29 SEC_OP (INC_METHOD = 1)
 *   bits 15:13 subchannel (host methods = 0)
 *   bits 11:0  method address >> 2
 * Methods:
 *   0x5c SEM_ADDR_LO, 0x60 SEM_ADDR_HI, 0x64 SEM_PAYLOAD_LO,
 *   0x6c SEM_EXECUTE.
 */
#define NVC06F_DMA_SEC_OP_INC_METHOD	1u
#define NVC06F_GP_ENTRY1_LENGTH_SHIFT	10  /* clc06f.h:217 LENGTH 30:10 */
#define NVC36F_SEM_ADDR_LO_OFFSET	0x5c
#define NVC36F_SEM_EXECUTE_OFFSET	0x6c
#define NVC36F_PUSH_HDR_SEM_ADDR_TRIPLET	\
	((NVC06F_DMA_SEC_OP_INC_METHOD << 29) | (3u << 16) \
	 | (NVC36F_SEM_ADDR_LO_OFFSET >> 2))
#define NVC36F_PUSH_HDR_SEM_EXECUTE	\
	((NVC06F_DMA_SEC_OP_INC_METHOD << 29) | (1u << 16) \
	 | (NVC36F_SEM_EXECUTE_OFFSET >> 2))
#define NVC36F_SEM_EXECUTE_RELEASE	1u  /* clc36f.h:106 OPERATION=RELEASE */

#define SUBMIT_PUSH_DWORDS	6  /* total dwords in our push */

/* === USERD slot layout (gv100_chan_userd.size = 0x200) === */
#define NV_USERD_SLOT_SIZE	0x200
#define NV_USERD_GP_GET		0x88
#define NV_USERD_GP_PUT		0x8c

/* === USERMODE PRI regs (tu102/dev_vm.h:228-229, vfn/tu102.c:102) ===
 * BAR0 base of TURING_USERMODE_A = VFN_PRIV_BASE(0xb80000) + USER_OFF(0x30000)
 *                                  = 0xbb0000. */
#define NV_USERMODE_BASE	0xbb0000u
#define NV_USERMODE_TIME_LO	(NV_USERMODE_BASE + 0x80)
#define NV_USERMODE_TIME_HI	(NV_USERMODE_BASE + 0x84)
#define NV_USERMODE_DOORBELL	(NV_USERMODE_BASE + 0x90)

#define SUBMIT_POLL_MS		1000
#define SUBMIT_POLL_STEP_MS	10


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
	/* USERD page: 4 KiB / 8 slots * 0x200. GSP indexes within using
	CHANNEL_USERD_INDEX_VALUE=chid%8. Match nouveau B.2 walkthrough. */
	chan->userd_vram = nvkm_gsp_vram_alloc(sc, 0x1000U, 0x1000);
	device_printf(sc->dev,
	    "gsp_rm: chan->inst_vram=0x%llx chan->userd_vram=0x%llx (alloc\'d)\n",
	    (unsigned long long)chan->inst_vram,
	    (unsigned long long)chan->userd_vram);

	/* Zero inst block via PRAMIN so we know any later non-zero bytes
	 * are written by GSP, not stale data from prior allocator state. */
	if (chan->inst_vram != 0) {
		lwkt_gettoken(&sc->gsp_tok);
		uint32_t saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
		nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(chan->inst_vram >> 16));
		for (uint32_t off = 0; off < 0x1000; off += 4)
			nvkm_wr32(sc, NV_PRAMIN + (uint32_t)((chan->inst_vram + off) & 0xffffu), 0);
		(void)nvkm_rd32(sc, NV_PRAMIN);
		nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
		lwkt_reltoken(&sc->gsp_tok);
		device_printf(sc->dev, "gsp_rm: chan inst block zeroed via PRAMIN\n");
	}
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

	/* === Pre-allocate submit BOs (VRAM PT + VRAM PD0/SPT,
	 * sysmem data) and write host PT.
	 * Mirrors nouveau r535/vmm.c:125 aperture=1 + VRAM PT. */
	{
		/* All PT pages + data BOs in VRAM, host-accessed via BAR1.
		 * Matches nouveau (everything in VRAM, L2-coherent both
		 * sides). PDE/PTE aperture = VIDMEM. */
		if ((err = nvkm_gsp_bar1_alloc_page(sc, &chan->submit_pd0)) ||
		    (err = nvkm_gsp_bar1_alloc_page(sc, &chan->submit_spt)) ||
		    (err = nvkm_gsp_bar1_alloc_page(sc, &chan->submit_push)) ||
		    (err = nvkm_gsp_bar1_alloc_page(sc, &chan->submit_gpf)) ||
		    (err = nvkm_gsp_bar1_alloc_page(sc, &chan->submit_sema))) {
			device_printf(sc->dev,
			    "gsp_rm: chan submit page alloc failed err=%d\n", err);
			contigfree(chan->mthdbuf_kva, chan->mthdbuf_size,
			    M_NVKM_MTHDBUF);
			chan->mthdbuf_kva = NULL;
			return (err);
		}

		const uint32_t pd1_idx = (SUBMIT_GVA_PUSHBUF >> NVKM_GMMU_PD1_SHIFT)
		    & (NVKM_GMMU_PD1_ENTRIES - 1);
		const uint32_t pd0_idx = (SUBMIT_GVA_PUSHBUF >> NVKM_GMMU_PD0_SHIFT)
		    & (NVKM_GMMU_PD0_ENTRIES - 1);
		const uint32_t spt_idx = (SUBMIT_GVA_PUSHBUF >> NVKM_GMMU_SPT_SHIFT)
		    & (NVKM_GMMU_SPT_ENTRIES - 1);

		/* PD1[k] -> PD0 (VRAM); PD0[k].small -> SPT (VRAM); SPT entries
		 * for push/gpf/sema (all VRAM). All writes via BAR1. */
		nvkm_gsp_bar1_wr64(sc,
		    vmm->pt[2].page.bar1_gva + pd1_idx * 8,
		    nvkm_pde_to_vram(chan->submit_pd0.vram_paddr));
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_pd0.bar1_gva + (pd0_idx * 2 + 0) * 8,
		    nvkm_pde_to_vram(chan->submit_spt.vram_paddr));
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_pd0.bar1_gva + (pd0_idx * 2 + 1) * 8, 0);
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_spt.bar1_gva + (spt_idx + 0) * 8,
		    nvkm_pte_to_vram(chan->submit_push.vram_paddr));
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_spt.bar1_gva + (spt_idx + 1) * 8,
		    nvkm_pte_to_vram(chan->submit_gpf.vram_paddr));
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_spt.bar1_gva + (spt_idx + 2) * 8,
		    nvkm_pte_to_vram(chan->submit_sema.vram_paddr));

		/* Read back EVERY level of the PT chain via BAR1 to verify
		 * that COPY_SERVER_RESERVED_PDES didn't clobber our PD3/PD2/PD1
		 * chain and that our PD0/SPT writes landed. */
		uint64_t rb_pd3 = nvkm_gsp_bar1_rd64(sc, vmm->pt[0].page.bar1_gva + 0);
		uint64_t rb_pd2 = nvkm_gsp_bar1_rd64(sc, vmm->pt[1].page.bar1_gva + 0);
		uint64_t rb_pd1 = nvkm_gsp_bar1_rd64(sc, vmm->pt[2].page.bar1_gva + pd1_idx * 8);
		uint64_t rb_pd0_big = nvkm_gsp_bar1_rd64(sc, chan->submit_pd0.bar1_gva + (pd0_idx * 2 + 0) * 8);
		uint64_t rb_pd0_small = nvkm_gsp_bar1_rd64(sc, chan->submit_pd0.bar1_gva + (pd0_idx * 2 + 1) * 8);
		uint64_t rb_spt0 = nvkm_gsp_bar1_rd64(sc, chan->submit_spt.bar1_gva + spt_idx * 8);
		device_printf(sc->dev,
		    "gsp_rm: PT readback: PD3[0]=0x%016llx PD2[0]=0x%016llx PD1[%u]=0x%016llx\n",
		    (unsigned long long)rb_pd3, (unsigned long long)rb_pd2,
		    pd1_idx, (unsigned long long)rb_pd1);
		device_printf(sc->dev,
		    "gsp_rm: PT readback: PD0[%u].BIG=0x%016llx .SMALL=0x%016llx SPT[%u]=0x%016llx\n",
		    pd0_idx, (unsigned long long)rb_pd0_big,
		    (unsigned long long)rb_pd0_small,
		    spt_idx, (unsigned long long)rb_spt0);
		/* Read channel inst[0x200] via BAR1 (L2-coherent). PRAMIN bypasses L2
		 * so we couldn\'t see GSP\'s writes through it. Map chan inst into BAR1
		 * temporarily. Also read PDB via PRAMIN for comparison. */
		{
			uint64_t pramin_pdb = 0, bar1_pdb = 0;
			(void)nvkm_gsp_pramin_rd64(sc, chan->inst_vram + 0x200, &pramin_pdb);
			/* alloc one BAR1 GVA for chan inst */
			uint64_t inst_bar1_gva = sc->bar1.next_gva;
			sc->bar1.next_gva += 0x1000;
			device_printf(sc->dev,
			    "gsp_rm: DIAG inst_bar1_gva=0x%llx mapping to vram=0x%llx (bar1 SPT=0x%llx idx=%llu)\n",
			    (unsigned long long)inst_bar1_gva,
			    (unsigned long long)chan->inst_vram,
			    (unsigned long long)sc->bar1.spt_paddr,
			    (unsigned long long)(inst_bar1_gva >> 12));

			(void)nvkm_gsp_bar1_map_vram(sc, inst_bar1_gva, chan->inst_vram);
			nvkm_gsp_bar1_flush(sc);
			/* Full PDB invalidate on BAR1\'s PDB (sc->gsp_bar1_pdb) so walker
			 * picks up the new SPT entry. */
			uint64_t bar1_inv = (sc->gsp_bar1_pdb >> 12) << 4;
			for (int spin = 0; spin < 200; spin++) {
				if (nvkm_rd32(sc, 0x100c80) & 0x00ff0000u) break;
				DELAY(10);
			}
			nvkm_wr32(sc, 0x100cb8, (uint32_t)bar1_inv);
			nvkm_wr32(sc, 0x100cbc, 0x80000000u);
			for (int spin = 0; spin < 200; spin++) {
				if (!(nvkm_rd32(sc, 0x100cbc) & 0x80000000u)) break;
				DELAY(10);
			}

			/* AFTER map_vram + TLB invalidate: dump BAR1 SPT, should see
			 * SPT[9] = our inst PDE if writes actually landed. */
			for (uint64_t si = 7; si < 12; si++) {
				uint64_t spte = 0;
				(void)nvkm_gsp_pramin_rd64(sc, sc->bar1.spt_paddr + si*8, &spte);
				device_printf(sc->dev,
				    "gsp_rm: POST-MAP BAR1_SPT[%llu] = 0x%016llx\n",
				    (unsigned long long)si, (unsigned long long)spte);
			}
			bar1_pdb = nvkm_gsp_bar1_rd64(sc, inst_bar1_gva + 0x200);
			device_printf(sc->dev,
			    "gsp_rm: chan inst[0x200] PRAMIN=0x%016llx BAR1=0x%016llx (our PD3 paddr=0x%llx)\n",
			    (unsigned long long)pramin_pdb,
			    (unsigned long long)bar1_pdb,
			    (unsigned long long)vmm->pt[0].page.vram_paddr);
			/* Dump first 64 bytes of inst block via BAR1 */
			device_printf(sc->dev,
			    "gsp_rm: chan inst[0..0x40] via BAR1: %08x %08x %08x %08x   %08x %08x %08x %08x\n",
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva +  0),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva +  4),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva +  8),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 12),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 16),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 20),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 24),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 28));
			device_printf(sc->dev,
			    "gsp_rm: chan inst[0x200..0x220] via BAR1: %08x %08x %08x %08x   %08x %08x %08x %08x\n",
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x200),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x204),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x208),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x20c),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x210),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x214),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x218),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x21c));
			/* Volta+ channel RAMIN: PDB at NV_RAMIN_SC_PAGE_DIR_BASE_LO/HI(0)
			 * = byte offset 0x2a0/0x2a4 (subcontext 0). bits: target[1:0],
			 * vol[2], fault_replay_tex[4], fault_replay_gcc[5], lo[31:12]
			 * in dword 0x2a0; hi[31:0] in dword 0x2a4. */
			device_printf(sc->dev,
			    "gsp_rm: chan inst[0x2a0..0x2c0] via BAR1: %08x %08x %08x %08x   %08x %08x %08x %08x\n",
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2a0),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2a4),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2a8),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2ac),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2b0),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2b4),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2b8),
			    nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2bc));
			uint32_t sc0_lo = nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2a0);
			uint32_t sc0_hi = nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x2a4);
			uint64_t sc0_pdb = ((uint64_t)sc0_hi << 32) | (sc0_lo & ~0xfffu);
			device_printf(sc->dev,
			    "gsp_rm: SC0 PDB target=%u vol=%u pdb_paddr=0x%llx (our PD3=0x%llx)\n",
			    sc0_lo & 3u, (sc0_lo >> 2) & 1u,
			    (unsigned long long)sc0_pdb,
			    (unsigned long long)vmm->pt[0].page.vram_paddr);

			/* Sanity: write marker via PRAMIN to chan->inst_vram[0x100],
			 * read via BAR1, vice versa. If both read the marker, mapping
			 * is consistent. */
			lwkt_gettoken(&sc->gsp_tok);
			uint32_t s2 = nvkm_rd32(sc, NV_PBUS_PRAMIN);
			nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(chan->inst_vram >> 16));
			nvkm_wr32(sc, NV_PRAMIN + (uint32_t)((chan->inst_vram + 0x100) & 0xffffu), 0x11223344u);
			(void)nvkm_rd32(sc, NV_PRAMIN);
			nvkm_wr32(sc, NV_PBUS_PRAMIN, s2);
			lwkt_reltoken(&sc->gsp_tok);
			nvkm_gsp_bar1_flush(sc);
			uint32_t bar1_at_100 = nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + 0x100);
			nvkm_gsp_bar1_wr32(sc, inst_bar1_gva + 0x180, 0x55667788u);
			nvkm_gsp_bar1_flush(sc);
			uint64_t pramin_at_180 = 0;
			(void)nvkm_gsp_pramin_rd64(sc, chan->inst_vram + 0x180, &pramin_at_180);
			device_printf(sc->dev,
			    "gsp_rm: SANITY PRAMIN-wrote 0x11223344 @inst+0x100, BAR1 reads 0x%08x | BAR1-wrote 0x55667788 @inst+0x180, PRAMIN reads 0x%08x\n",
			    bar1_at_100, (uint32_t)(pramin_at_180 & 0xffffffffu));
		}

		device_printf(sc->dev,
		    "gsp_rm: expected: PD3[0]=0x%llx (PD2 paddr) PD2[0]=0x%llx (PD1 paddr) PD1[%u]=0x%llx (PD0 paddr) PD0.SMALL=0x%llx (SPT paddr) SPT[0]=0x%llx (push paddr)\n",
		    (unsigned long long)nvkm_pde_to_vram(vmm->pt[1].page.vram_paddr),
		    (unsigned long long)nvkm_pde_to_vram(vmm->pt[2].page.vram_paddr),
		    pd1_idx, (unsigned long long)nvkm_pde_to_vram(chan->submit_pd0.vram_paddr),
		    (unsigned long long)nvkm_pde_to_vram(chan->submit_spt.vram_paddr),
		    (unsigned long long)nvkm_pte_to_vram(chan->submit_push.vram_paddr));

		device_printf(sc->dev,
		    "gsp_rm: submit PT (VRAM via BAR1): PD0 vram=0x%llx bar1=0x%llx "
		    "SPT vram=0x%llx bar1=0x%llx\n",
		    (unsigned long long)chan->submit_pd0.vram_paddr,
		    (unsigned long long)chan->submit_pd0.bar1_gva,
		    (unsigned long long)chan->submit_spt.vram_paddr,
		    (unsigned long long)chan->submit_spt.bar1_gva);
		device_printf(sc->dev,
		    "gsp_rm: BOs push vram=0x%llx bar1=0x%llx gpf vram=0x%llx bar1=0x%llx sema vram=0x%llx bar1=0x%llx\n",
		    (unsigned long long)chan->submit_push.vram_paddr,
		    (unsigned long long)chan->submit_push.bar1_gva,
		    (unsigned long long)chan->submit_gpf.vram_paddr,
		    (unsigned long long)chan->submit_gpf.bar1_gva,
		    (unsigned long long)chan->submit_sema.vram_paddr,
		    (unsigned long long)chan->submit_sema.bar1_gva);
	}

	/* Allocate chid FIRST -- nouveau encodes it in the channel handle:
	 * NVKM_RM_CHAN(chid) = 0xf1f00000 | chid. GSP uses handle\'s chid to
	 * route doorbells; mismatched handle silently drops doorbell signals. */
	chan->chid = nvkm_chid_alloc(sc);
	if (chan->chid < 0) {
		contigfree(chan->mthdbuf_kva, chan->mthdbuf_size, M_NVKM_MTHDBUF);
		chan->mthdbuf_kva = NULL;
		return (ENOMEM);
	}
	args = nvkm_gsp_rm_alloc_get(&device->object,
	    NVKM_RM_CHANNEL | (uint32_t)chan->chid,
	    TURING_CHANNEL_GPFIFO_A, sizeof(*args), &chan->object);
	if (args == NULL) {
		contigfree(chan->mthdbuf_kva, chan->mthdbuf_size,
		    M_NVKM_MTHDBUF);
		chan->mthdbuf_kva = NULL;
		return (ENOMEM);
	}

	/* gpFifoOffset/Entries point at our pre-mapped sysmem ring. */
	args->gpFifoOffset = SUBMIT_GVA_GPFIFO;
	args->gpFifoEntries = 512;   /* 4 KiB / 8 byte entry */
	/* chid allocated from host pool (rsvd_chids=1 ->
	 * nvkm_chid_alloc starts at 1). Encode into
	 * USERD_INDEX_VALUE/PAGE_VALUE per r570/fifo.c:r570_chan_alloc. */
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
	/* Match nouveau: single-slot size = gv100_chan_userd.size = 0x200
	 * (fifo/gv100.c:73). Even though our VRAM allocation is a full
	 * 4 KiB page, GSP expects size = per-channel slot, not the page. */
	args->userdMem.size = NV_CHANNEL_USERD_SIZE;
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

	/* nouveau r535_chan_ramfc_write fifo.c:188-216: bind engine + enable
	 * GPFIFO scheduling. Both are RM_CONTROL on the channel object. */
	{
		struct {
			uint32_t engineType;
		} *bind;
		bind = nvkm_gsp_rm_ctrl_get(&chan->object,
		    /* NVA06F_CTRL_CMD_BIND */ 0xa06f0104u,
		    sizeof(*bind));
		if (bind == NULL) {
			device_printf(sc->dev,
			    "gsp_rm: BIND ctrl_get failed\n");
			return (ENOMEM);
		}
		bind->engineType = engine_type;
		err = nvkm_gsp_rm_ctrl_wr(&chan->object, bind);
		if (err != 0) {
			device_printf(sc->dev,
			    "gsp_rm: NVA06F_BIND engine=0x%x failed err=%d\n",
			    engine_type, err);
			return (err);
		}
	}

	{
		/* NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS as nouveau vendors it:
		 * nouveau/nvkm/subdev/gsp/rm/r535/nvrm/fifo.h:310 -- only 2 bytes
		 * (bEnable + bSkipSubmit). Open-rm 570 header added a 3rd field
		 * (bSkipEnable) but GSP r570 still accepts the 2-byte form;
		 * passing 3 bytes triggers FINN strict-size validation and GSP
		 * returns NV_ERR_INVALID_ARGUMENT (0x1f). Stay at 2 bytes. */
		struct {
			uint8_t bEnable;
			uint8_t bSkipSubmit;
		} *sched;
		sched = nvkm_gsp_rm_ctrl_get(&chan->object,
		    /* NVA06F_CTRL_CMD_GPFIFO_SCHEDULE */ 0xa06f0103u,
		    sizeof(*sched));
		if (sched == NULL) {
			device_printf(sc->dev,
			    "gsp_rm: SCHEDULE ctrl_get failed\n");
			return (ENOMEM);
		}
		sched->bEnable = 1;
		sched->bSkipSubmit = 0;
		err = nvkm_gsp_rm_ctrl_wr(&chan->object, sched);
		if (err != 0) {
			device_printf(sc->dev,
			    "gsp_rm: NVA06F_GPFIFO_SCHEDULE failed err=%d\n",
			    err);
			return (err);
		}
	}

	/* TURING_DMA_COPY_A engine object under the channel.
	 * nouveau r535/ce.c:28-44: parent=chan, params version=1,
	 * engineType=NV2080_ENGINE_TYPE_COPY0+inst. */
	{
		struct {
			uint32_t version;
			uint32_t engineType;
		} *args;
		args = nvkm_gsp_rm_alloc_get(&chan->object,
		    /* handle */ 0xc5b50000u,
		    /* TURING_DMA_COPY_A */ 0x0000c5b5u,
		    sizeof(*args), &chan->ce_obj);
		if (args == NULL) {
			device_printf(sc->dev,
			    "gsp_rm: CE alloc_get failed\n");
			return (ENOMEM);
		}
		args->version = 1;
		args->engineType = engine_type;
		err = nvkm_gsp_rm_alloc_wr(&chan->ce_obj, args);
		if (err != 0) {
			device_printf(sc->dev,
			    "gsp_rm: TURING_DMA_COPY_A alloc failed err=%d\n",
			    err);
			memset(&chan->ce_obj, 0, sizeof(chan->ce_obj));
			return (err);
		}
		device_printf(sc->dev,
		    "gsp_rm: TURING_DMA_COPY_A handle=0x%x on channel=0x%x ok\n",
		    chan->ce_obj.handle, chan->object.handle);
	}

	{
		uint32_t runl = 0;
		int qerr = nvkm_gsp_query_ce0_runlist(sc, &runl);
		if (qerr == 0) {
			uint32_t token = (runl << 16) | (uint32_t)chan->chid;
			device_printf(sc->dev,
			    "gsp_rm: CE0 runlist=%u chid=%d doorbell_token=0x%08x\n",
			    runl, chan->chid, token);
		} else {
			device_printf(sc->dev,
			    "gsp_rm: runlist query failed err=%d\n", qerr);
		}
	}

	/* Ask GSP for the work-submit token. RPC fails with INVALID_STATE
	 * if channel isn\'t on runlist - definitive proof SCHEDULE worked. */
	{
		struct { uint32_t workSubmitToken; } *t;
		t = nvkm_gsp_rm_ctrl_get(&chan->object,
		    /* NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN */ 0xc36f0108u,
		    sizeof(*t));
		if (t == NULL) {
			device_printf(sc->dev,
			    "gsp_rm: GET_WORK_SUBMIT_TOKEN ctrl_get failed\n");
		} else {
			t->workSubmitToken = 0xdeadbeef;
			int werr = nvkm_gsp_rm_ctrl_rd(&chan->object, (void**)&t,
			    sizeof(*t));
			if (werr != 0) {
				device_printf(sc->dev,
				    "gsp_rm: GET_WORK_SUBMIT_TOKEN err=%d "
				    "(channel not on runlist?)\n", werr);
			} else {
				chan->gsp_token = t->workSubmitToken;
				device_printf(sc->dev,
				    "gsp_rm: GSP workSubmitToken=0x%08x\n",
				    chan->gsp_token);
			}
		}
	}

	device_printf(sc->dev,
	    "gsp_rm: TURING_CHANNEL_GPFIFO_A handle=0x%x engine=0x%x bound+scheduled+CE\n",
	    chan->object.handle, engine_type);

	/* Map USERD VRAM page at a BAR1 GVA so host writes go through
	 * BAR1 walker -> L2-coherent VRAM (path nouveau uses). */
	chan->userd_bar2_gva = sc->bar1.next_gva;
	sc->bar1.next_gva += 0x1000;
	(void)nvkm_gsp_bar1_map_vram(sc, chan->userd_bar2_gva,
	    chan->userd_vram);
	nvkm_gsp_bar1_flush(sc);

	/* Diag: write+readback marker at the actual USERD GVA we just mapped. */
	{
		nvkm_gsp_bar1_wr32(sc, chan->userd_bar2_gva + 0x10, 0xCAFEBABEu);
		nvkm_gsp_bar1_flush(sc);
		uint32_t rb = nvkm_gsp_bar1_rd32(sc, chan->userd_bar2_gva + 0x10);
		device_printf(sc->dev,
		    "bar1_diag: USERD page via BAR1 GVA 0x%llx+0x10 readback = 0x%08x (expect cafebabe)\n",
		    (unsigned long long)chan->userd_bar2_gva, rb);
		nvkm_gsp_bar1_wr32(sc, chan->userd_bar2_gva + 0x10, 0); /* clean for USERD */
		nvkm_gsp_bar1_flush(sc);
	}

	/* TURING_USERMODE_A is allocated once per device in vmm_ctor
	 * (nouveau does this at drm init, before any channel). */

	/* Pre-publish sc->gsp_chan so submit_test can see it. */
	sc->gsp_chan = chan;
	(void)nvkm_gsp_submit_test(sc);
	return (0);
}



int
nvkm_gsp_submit_test(struct nvkm_softc *sc)
{
	struct nvkm_gsp_chan *chan = sc->gsp_chan;
	uint32_t saved_pramin;
	int ms;

	if (chan == NULL || chan->submit_push.bar1_gva == 0)
		return (ENXIO);

	/* All BOs are VRAM, accessed via BAR1. */
	const uint64_t push_bar1 = chan->submit_push.bar1_gva;
	const uint64_t gpf_bar1  = chan->submit_gpf.bar1_gva;
	const uint64_t sema_bar1 = chan->submit_sema.bar1_gva;

	/* Pushbuf -- NVC36F SEM_ADDR_LO/HI/PAYLOAD_LO + SEM_EXECUTE.
	 * Refs: clc36f.h:95-128, push906f.h:23-49, chanc36f.c:26-49. */
	nvkm_gsp_bar1_wr32(sc, push_bar1 +  0, NVC36F_PUSH_HDR_SEM_ADDR_TRIPLET);
	nvkm_gsp_bar1_wr32(sc, push_bar1 +  4, (uint32_t)(SUBMIT_GVA_SEMA & 0xffffffffu));
	nvkm_gsp_bar1_wr32(sc, push_bar1 +  8, (uint32_t)((SUBMIT_GVA_SEMA >> 32) & 0xffu));
	nvkm_gsp_bar1_wr32(sc, push_bar1 + 12, SEM_PAYLOAD);
	nvkm_gsp_bar1_wr32(sc, push_bar1 + 16, NVC36F_PUSH_HDR_SEM_EXECUTE);
	nvkm_gsp_bar1_wr32(sc, push_bar1 + 20, NVC36F_SEM_EXECUTE_RELEASE);

	/* GPFIFO entry[0] -- nvif/chan506f.c:nvif_chan506f_gpfifo_push.
	 * dw0 = lower_32(push_gva); dw1 = upper_8(push_gva) | (dwords<<10). */
	nvkm_gsp_bar1_wr32(sc, gpf_bar1 + 0, (uint32_t)(SUBMIT_GVA_PUSHBUF & 0xffffffffu));
	nvkm_gsp_bar1_wr32(sc, gpf_bar1 + 4,
	    (uint32_t)((SUBMIT_GVA_PUSHBUF >> 32) & 0xffu)
	    | (SUBMIT_PUSH_DWORDS << NVC06F_GP_ENTRY1_LENGTH_SHIFT));

	nvkm_gsp_bar1_wr32(sc, sema_bar1 + 0, 0);

	device_printf(sc->dev,
	    "gsp_submit: push[0..5]= %08x %08x %08x %08x %08x %08x\n",
	    nvkm_gsp_bar1_rd32(sc, push_bar1 +  0),
	    nvkm_gsp_bar1_rd32(sc, push_bar1 +  4),
	    nvkm_gsp_bar1_rd32(sc, push_bar1 +  8),
	    nvkm_gsp_bar1_rd32(sc, push_bar1 + 12),
	    nvkm_gsp_bar1_rd32(sc, push_bar1 + 16),
	    nvkm_gsp_bar1_rd32(sc, push_bar1 + 20));
	device_printf(sc->dev,
	    "gsp_submit: gpf[0..1]= %08x %08x  sema=%08x\n",
	    nvkm_gsp_bar1_rd32(sc, gpf_bar1 + 0),
	    nvkm_gsp_bar1_rd32(sc, gpf_bar1 + 4),
	    nvkm_gsp_bar1_rd32(sc, sema_bar1 + 0));

	/* USERD slot housekeeping + GP_PUT=1, via BAR1 (L2-coherent).
	 * USERD is in VRAM at chan->userd_vram + chid * USERD_SLOT_SIZE.
	 * BAR1 maps chan->userd_vram -> BAR1_GVA_USERD (4 KiB page),
	 * so the chid slot is at BAR1_GVA_USERD + chid * USERD_SLOT_SIZE.
	 * Mirrors gf100_chan_userd_clear (fifo/gf100.c:118-132). */
	uint64_t slot_bar1 = chan->userd_bar2_gva
	    + (uint64_t)chan->chid * NV_USERD_SLOT_SIZE;
	static const uint32_t userd_clear_offs[] = {
		0x40, 0x44, 0x48, 0x4c, 0x50, 0x58, 0x5c, 0x60, 0x88
	};
	for (unsigned k = 0; k < sizeof(userd_clear_offs)/sizeof(userd_clear_offs[0]); k++)
		nvkm_gsp_bar1_wr32(sc, slot_bar1 + userd_clear_offs[k], 0);
	nvkm_gsp_bar1_wr32(sc, slot_bar1 + NV_USERD_GP_PUT, 1);
	/* Match nouveau nvif_chanc36f_gpfifo_kick (chanc36f.c:12-22):
	 *   wmb(); read USERD offset 0 to flush BAR1 posted writes to vidmem,
	 *   THEN doorbell. We additionally print readback for diagnostic. */
	cpu_sfence();
	uint32_t flush_rb = nvkm_gsp_bar1_rd32(sc, slot_bar1 + 0);
	uint32_t put_rb   = nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_PUT);
	uint32_t get_rb   = nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_GET);
	device_printf(sc->dev,
	    "gsp_submit: BAR1 USERD readback flush[0]=0x%08x GP_PUT=0x%08x GP_GET=0x%08x\n",
	    flush_rb, put_rb, get_rb);
	(void)saved_pramin;

	/* USERMODE TIME tick test (PRI / BAR0 reachability). */
	uint32_t t0_lo = nvkm_rd32(sc, NV_USERMODE_TIME_LO);
	uint32_t t0_hi = nvkm_rd32(sc, NV_USERMODE_TIME_HI);
	DELAY(100);
	uint32_t t1_lo = nvkm_rd32(sc, NV_USERMODE_TIME_LO);
	uint32_t t1_hi = nvkm_rd32(sc, NV_USERMODE_TIME_HI);
	device_printf(sc->dev,
	    "gsp_submit: USERMODE TIME %08x:%08x -> %08x:%08x\n",
	    t0_hi, t0_lo, t1_hi, t1_lo);

	/* Snapshot GSP LOGRM put just before the doorbell so we know
	 * exactly which log bytes (if any) are GSP's response. */
	uint64_t logrm_put_pre = (sc->gsp_logrm.kva != NULL)
	    ? *(volatile uint64_t *)sc->gsp_logrm.kva : 0;
	device_printf(sc->dev,
	    "gsp_submit: pre-doorbell LOGRM put=0x%llx\n",
	    (unsigned long long)logrm_put_pre);

	/* Doorbell at BAR0+USERMODE_DOORBELL = (runlist<<16) | chid.
	 * Try multiple writes with different tokens as a diagnostic.
	 * If any one triggers PBDMA, we'll see GP_GET advance. */
	device_printf(sc->dev, "gsp_submit: doorbell spray begin\n");
	for (int rep = 0; rep < 10; rep++) {
		nvkm_wr32(sc, NV_USERMODE_DOORBELL, (uint32_t)chan->chid);
		DELAY(1000);
	}
	/* Also try token formats: alternative runlist encoding, raw chid, etc. */
	nvkm_wr32(sc, NV_USERMODE_DOORBELL, 0x00000001u);
	DELAY(1000);
	nvkm_wr32(sc, NV_USERMODE_DOORBELL, 0x00010001u); /* runlist=1? */
	DELAY(1000);
	nvkm_wr32(sc, NV_USERMODE_DOORBELL, 0x00000000u); /* zero token */
	DELAY(1000);
	nvkm_wr32(sc, NV_USERMODE_DOORBELL, 0xffffffffu); /* all-ones */
	DELAY(1000);
	cpu_sfence();
	device_printf(sc->dev, "gsp_submit: doorbell spray end\n");

	device_printf(sc->dev,
	    "gsp_submit: kicked GP_PUT=1 doorbell=0x%08x, polling sema...\n",
	    (uint32_t)chan->chid);

	/* Give GSP its own RISC-V time to react before we start polling.
	 * Any PBDMA fault / RC trigger from this submit lands in logrm
	 * within milliseconds. We snapshot here so we know whether
	 * doorbell ever produced any observable side-effect. */
	DELAY(100000);
	uint64_t logrm_put_post = (sc->gsp_logrm.kva != NULL)
	    ? *(volatile uint64_t *)sc->gsp_logrm.kva : 0;
	device_printf(sc->dev,
	    "gsp_submit: post-doorbell 100ms snapshot: GP_GET=0x%08x "
	    "GP_PUT=0x%08x sema=0x%08x LOGRM put=0x%llx (delta=%lld)\n",
	    nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_GET),
	    nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_PUT),
	    nvkm_gsp_bar1_rd32(sc, sema_bar1 + 0),
	    (unsigned long long)logrm_put_post,
	    (long long)(logrm_put_post - logrm_put_pre));

	/* Hardware-side post-doorbell state dump. If the doorbell at BAR0+
	 * 0xbb0090 reached USERMODE -> PFIFO, PFIFO_INTR_0 should show
	 * NOTIFY_CHANNEL_PENDING set; PBDMA_STATUS should advance.
	 * Registers per Pascal/Volta/Turing PFIFO/PBDMA priv layout. */
	{
		uint32_t pfifo_intr_0      = nvkm_rd32(sc, 0x00040108u);
		uint32_t pfifo_intr_en_0   = nvkm_rd32(sc, 0x00040140u);
		uint32_t pfifo_intr_1      = nvkm_rd32(sc, 0x0004010cu);
		uint32_t pbdma0_intr_0     = nvkm_rd32(sc, 0x00040808u);
		uint32_t pbdma0_intr_1     = nvkm_rd32(sc, 0x00040884u);
		uint32_t pbdma0_status     = nvkm_rd32(sc, 0x0004080cu);
		uint32_t pbdma0_runlist    = nvkm_rd32(sc, 0x00040990u);
		uint32_t pmc_intr_en_0     = nvkm_rd32(sc, 0x00000140u);
		uint32_t pmc_intr_en_1     = nvkm_rd32(sc, 0x00000144u);
		uint32_t pmc_enable        = nvkm_rd32(sc, 0x00000200u);
		device_printf(sc->dev,
		    "gsp_submit: HW: PFIFO INTR_0=%08x EN_0=%08x INTR_1=%08x | "
		    "PBDMA0 INTR_0=%08x INTR_1=%08x STATUS=%08x RUNLIST=%08x | "
		    "PMC INTR_EN_0=%08x EN_1=%08x ENABLE=%08x\n",
		    pfifo_intr_0, pfifo_intr_en_0, pfifo_intr_1,
		    pbdma0_intr_0, pbdma0_intr_1, pbdma0_status, pbdma0_runlist,
		    pmc_intr_en_0, pmc_intr_en_1, pmc_enable);
	}

	uint32_t last_get = 0xffffffffu;
	for (ms = 0; ms < SUBMIT_POLL_MS; ms += SUBMIT_POLL_STEP_MS) {
		cpu_lfence();
		uint32_t sema_val = nvkm_gsp_bar1_rd32(sc, sema_bar1 + 0); if (sema_val == SEM_PAYLOAD) {
			device_printf(sc->dev,
			    "gsp_submit: SEM release OK after %d ms (sema=0x%08x)\n",
			    ms, sema_val);
			return (0);
		}
		/* Sample GP_GET every step; log whenever it changes. */
		uint32_t cur_get = nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_GET);
		if (cur_get != last_get) {
			device_printf(sc->dev,
			    "gsp_submit: t=%dms GP_GET=0x%08x (sema=0x%08x)\n",
			    ms, cur_get, nvkm_gsp_bar1_rd32(sc, sema_bar1 + 0));
			last_get = cur_get;
		}
		DELAY(SUBMIT_POLL_STEP_MS * 1000);
	}
	device_printf(sc->dev,
	    "gsp_submit: SEM TIMEOUT %d ms, sema=0x%08x, last GP_GET=0x%08x\n",
	    SUBMIT_POLL_MS, nvkm_gsp_bar1_rd32(sc, sema_bar1 + 0), last_get);
	return (0);
}


int
nvkm_gsp_query_ce0_runlist(struct nvkm_softc *sc, uint32_t *runl_out)
{
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS_r570 *p;
	void *q;
	int err;
	uint32_t i, found = 0;

	if (sc->gsp_internal_subdevice == 0)
		return (ENXIO);

	memset(&tmp_client, 0, sizeof(tmp_client));
	tmp_client.sc = sc;
	tmp_client.object.client = &tmp_client;
	tmp_client.object.handle = sc->gsp_internal_client;
	tmp_subdev.client = &tmp_client;
	tmp_subdev.parent = NULL;
	tmp_subdev.handle = sc->gsp_internal_subdevice;

	p = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE, sizeof(*p));
	if (p == NULL)
		return (ENOMEM);
	q = p;
	err = nvkm_gsp_rm_ctrl_rd(&tmp_subdev, &q, sizeof(*p));
	if (err != 0 || q == NULL) {
		device_printf(sc->dev,
		    "gsp_rm: FIFO_GET_DEVICE_INFO_TABLE failed err=%d\n",
		    err);
		return (err ? err : EIO);
	}
	p = q;

	for (i = 0; i < p->numEntries && i < NV2080_FIFO_DEV_INFO_MAX_ENTRIES; i++) {
		uint32_t rmtype = p->entries[i].engineData[ENGINE_INFO_TYPE_RM_ENGINE_TYPE];
		uint32_t runl   = p->entries[i].engineData[ENGINE_INFO_TYPE_RUNLIST];
		device_printf(sc->dev,
		    "gsp_rm: fifo entry[%u] name=%.16s rm_type=%u runlist=%u\n",
		    i, p->entries[i].engineName, rmtype, runl);
		if (rmtype == RM_ENGINE_TYPE_COPY0 && !found) {
			*runl_out = runl;
			found = 1;
		}
	}
	nvkm_gsp_rm_ctrl_done(&tmp_subdev, q);

	if (!found) {
		device_printf(sc->dev, "gsp_rm: COPY0 not in fifo info table\n");
		return (ENOENT);
	}
	return (0);
}

int
nvkm_gsp_chan_dtor(struct nvkm_gsp_chan *chan)
{
	struct nvkm_softc *sc = chan->object.client ?
	    chan->object.client->sc : NULL;
	if (chan->ce_obj.handle != 0)
		(void)nvkm_gsp_rm_free(&chan->ce_obj);
	int err = nvkm_gsp_rm_free(&chan->object);
	if (sc != NULL && chan->chid > 0)
		nvkm_chid_free(sc, chan->chid);
	/* All submit_* pages are VRAM bump allocations -- no host free. */
	nvkm_gsp_bar1_free_page(sc, &chan->submit_pd0);
	nvkm_gsp_bar1_free_page(sc, &chan->submit_spt);
	nvkm_gsp_bar1_free_page(sc, &chan->submit_push);
	nvkm_gsp_bar1_free_page(sc, &chan->submit_gpf);
	nvkm_gsp_bar1_free_page(sc, &chan->submit_sema);
	if (chan->mthdbuf_kva != NULL) {
		contigfree(chan->mthdbuf_kva, chan->mthdbuf_size,
		    M_NVKM_MTHDBUF);
		chan->mthdbuf_kva = NULL;
	}
	return (err);
}
