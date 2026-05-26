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

#define NVKM_GSP_DEBUG_RM_ALLOC		0

#define NVKM_ALIGN_UP(v, a)	(((v) + (a) - 1) & ~((a) - 1))
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
	uint32_t h_client = rpc->hClient;
	uint32_t h_parent = rpc->hParent;
	uint32_t h_object = rpc->hObject;
	uint32_t h_class = rpc->hClass;
	uint32_t params_size = rpc->paramsSize;
	uint32_t flags = rpc->flags;

#if NVKM_GSP_DEBUG_RM_ALLOC
	if (h_class == 0x0000c597U) {
		device_printf(sc->dev,
		    "gsp_rm: RM_ALLOC envelope cls=0x%x client=0x%x "
		    "parent=0x%x object=0x%x paramsSize=%u flags=0x%x "
		    "expected_repc=%u\n",
		    h_class, h_client, h_parent, h_object, params_size,
		    flags, expected_repc);
	}
#else
	(void)h_client;
	(void)params_size;
	(void)flags;
#endif

	rep = nvkm_gsp_rpc_push(sc, rpc, NVKM_GSP_RPC_REPLY_RECV,
	    expected_repc);
	if (rep == NULL)
		return (EIO);

	if (rep->status != 0) {
		device_printf(sc->dev,
		    "gsp_rm: ALLOC cls=0x%x obj=0x%x parent=0x%x failed status=0x%x\n",
		    h_class, h_object, h_parent, rep->status);
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

/* === GR context promotion (nouveau r535/r570 gr.c) === */
#define NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO 0x20800a32U
#define NV2080_CTRL_INTERNAL_GR_MAX_ENGINES 8U
#define NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT 0x1aU

struct NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_BUFFER_INFO_dfly {
	uint32_t size;
	uint32_t alignment;
};

struct NV2080_CTRL_INTERNAL_STATIC_GR_CONTEXT_BUFFERS_INFO_dfly {
	struct NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_BUFFER_INFO_dfly
	    engine[NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT];
};

struct NV2080_CTRL_INTERNAL_STATIC_GR_GET_CONTEXT_BUFFERS_INFO_PARAMS_dfly {
	struct NV2080_CTRL_INTERNAL_STATIC_GR_CONTEXT_BUFFERS_INFO_dfly
	    engineContextBuffersInfo[NV2080_CTRL_INTERNAL_GR_MAX_ENGINES];
};

#define NV0080_ENGINE_ID_GRAPHICS		0x00U
#define NV0080_ENGINE_ID_GRAPHICS_PATCH		0x10U
#define NV0080_ENGINE_ID_GRAPHICS_BUNDLE_CB	0x11U
#define NV0080_ENGINE_ID_GRAPHICS_PAGEPOOL_GLOBAL 0x12U
#define NV0080_ENGINE_ID_GRAPHICS_ATTRIBUTE_CB	0x13U
#define NV0080_ENGINE_ID_GRAPHICS_RTV_CB_GLOBAL	0x14U
#define NV0080_ENGINE_ID_GRAPHICS_FECS_EVENT	0x17U
#define NV0080_ENGINE_ID_GRAPHICS_PRIV_ACCESS_MAP 0x18U

#define NV2080_CTXBUF_ID_MAIN			0U
#define NV2080_CTXBUF_ID_PATCH			2U
#define NV2080_CTXBUF_ID_BUFFER_BUNDLE_CB	3U
#define NV2080_CTXBUF_ID_PAGEPOOL		4U
#define NV2080_CTXBUF_ID_ATTRIBUTE_CB		5U
#define NV2080_CTXBUF_ID_RTV_CB_GLOBAL		6U
#define NV2080_CTXBUF_ID_FECS_EVENT		9U
#define NV2080_CTXBUF_ID_PRIV_ACCESS_MAP	10U
#define NV2080_CTXBUF_ID_UNRESTRICTED_PRIV_ACCESS_MAP 11U

#define NV2080_CTRL_GPU_PROMOTE_CONTEXT_MAX_ENTRIES 16U
#define NV2080_CTRL_CMD_GPU_PROMOTE_CTX		0x2080012bU
#define NV2080_CTRL_CMD_GR_GET_ZCULL_INFO	0x20801206U

struct NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ENTRY_dfly {
	uint64_t gpuPhysAddr;
	uint64_t gpuVirtAddr;
	uint64_t size;
	uint32_t physAttr;
	uint16_t bufferId;
	uint8_t  bInitialize;
	uint8_t  bNonmapped;
};

struct NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS_dfly {
	uint32_t engineType;
	uint32_t hClient;
	uint32_t ChID;
	uint32_t hChanClient;
	uint32_t hObject;
	uint32_t hVirtMemory;
	uint64_t virtAddress;
	uint64_t size;
	uint32_t entryCount;
	uint8_t  _pad[4];
	struct NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ENTRY_dfly
	    promoteEntry[NV2080_CTRL_GPU_PROMOTE_CONTEXT_MAX_ENTRIES];
};

struct NV2080_CTRL_GR_GET_ZCULL_INFO_PARAMS_dfly {
	uint32_t widthAlignPixels;
	uint32_t heightAlignPixels;
	uint32_t pixelSquaresByAliquots;
	uint32_t aliquotTotal;
	uint32_t zcullRegionByteMultiplier;
	uint32_t zcullRegionHeaderSize;
	uint32_t zcullSubregionHeaderSize;
	uint32_t subregionCount;
	uint32_t subregionWidthAlignPixels;
	uint32_t subregionHeightAlignPixels;
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
		uint32_t leaf_mask[8] = {
			0x00031c80u, 0, 0, 0, 0x0c000000u, 0, 0, 0,
		};

		device_printf(sc->dev,
		    "gsp_rm: INTR_GET_KERNEL_TABLE tableLen=%u\n",
		    r->tableLen);
		for (uint32_t i = 0; i < r->tableLen &&
		    i < NV2080_CTRL_INTERNAL_INTR_MAX_TABLE_SIZE; i++) {
			uint32_t vectors[2] = {
				r->table[i].vectorStall,
				r->table[i].vectorNonStall,
			};

			device_printf(sc->dev,
			    "  [%u] engineIdx=%3u mask=0x%08x stall=%u nonStall=%u\n",
			    i, r->table[i].engineIdx, r->table[i].pmcIntrMask,
			    r->table[i].vectorStall, r->table[i].vectorNonStall);
			for (uint32_t j = 0; j < 2; j++) {
				uint32_t vector = vectors[j];
				uint32_t leaf = vector / 32u;
				uint32_t bit = vector % 32u;

				if (vector == 0xffffffffu || leaf >= 8u)
					continue;
				leaf_mask[leaf] |= (1u << bit);
			}
		}
		/*
		 * Nouveau stores this table in gsp->intr[] and lets the nvkm
		 * interrupt framework allow the vectors requested by each GSP
		 * backed subdev/engine.  We do not have that framework yet, so
		 * allow the finite vectors from the RM table directly, keeping
		 * the Fedora-observed bits as the base mask.
		 */
		for (uint32_t leaf = 0; leaf < 8u; leaf++) {
			if (leaf_mask[leaf] != 0)
				nvkm_wr32(sc, 0xb81200u + leaf * 4u,
				    leaf_mask[leaf]);
		}
		device_printf(sc->dev,
		    "gsp_rm: intr_allow table leaf[0]=0x%08x leaf[1]=0x%08x "
		    "leaf[2]=0x%08x leaf[3]=0x%08x leaf[4]=0x%08x\n",
		    leaf_mask[0], leaf_mask[1], leaf_mask[2], leaf_mask[3],
		    leaf_mask[4]);

		/* Enable INTR_TOP_EN_SET[0] = 0xf to enable subtree-0 intrs to fire.
		 * Fedora has this set; we missed it. Without TOP enable, LEAF intrs
		 * pend but never propagate to CPU/PBDMA scheduler ack path. */
		nvkm_wr32(sc, 0xb81608u, 0x0000000fu);
		device_printf(sc->dev,
		    "gsp_rm: INTR_TOP_EN_SET[0] = 0xf (was 0)\n");
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
 * 2048-entry bitmap. chid 0 is not allocatable, and chid 1 is kept free for
 * the GR golden channel path that nouveau allocates with rsvd_chids.
 * Alloc: find first 0-bit, set, return id. Free: clear bit. */
void
nvkm_chid_init(struct nvkm_softc *sc)
{
	lwkt_token_init(&sc->chid_tok, "nvkm-chid");
	memset(sc->chid_used, 0, sizeof(sc->chid_used));
	sc->chid_used[0] |= 0x3ULL;  /* chid 0 reserved, chid 1 for GR golden */
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
#ifdef NVKM_DEBUG_VRAM_ALLOC
	device_printf(sc->dev,
	    "gsp_rm: VRAM alloc 0x%llx (size 0x%llx)\n",
	    (unsigned long long)off, (unsigned long long)size);
#endif
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

/* GSP-RM owns runlist programming. Keep the old manual RUNLIST_NUM poke
 * behind a debug switch so diagnostics observe the scheduler's state. */
#define NVKM_GSP_DEBUG_HAIL_MARY_RUNLIST	0


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
#define SUBMIT_GVA_STRIDE	0x10000ULL
static uint32_t nvkm_gsp_submit_gva_slot;

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

static void
nvkm_gsp_sched_trace(struct nvkm_softc *sc, const char *tag,
    const struct nvkm_gsp_chan *chan, uint32_t engine_type)
{
#ifndef NVKM_DEBUG_SCHED_TRACE
	(void)sc;
	(void)tag;
	(void)chan;
	(void)engine_type;
	return;
#else
	uint32_t runl_id;
	uint32_t chid = (uint32_t)chan->chid;
	uint32_t pccsr_inst, pccsr_chan;
	uint32_t rl_base_lo, rl_base_hi, rl_num, rl_status;
	switch (engine_type) {
	case NV2080_ENGINE_TYPE_COPY2:
		runl_id = 8;
		break;
	case NV2080_ENGINE_TYPE_COPY0:
	case NV2080_ENGINE_TYPE_COPY1:
	default:
		runl_id = 0;
		break;
	}
	pccsr_inst = nvkm_rd32(sc, 0x00800000 + chid * 8);
	pccsr_chan = nvkm_rd32(sc, 0x00800004 + chid * 8);
	rl_base_lo = nvkm_rd32(sc, 0x002b00 + runl_id * 0x10);
	rl_base_hi = nvkm_rd32(sc, 0x002b04 + runl_id * 0x10);
	rl_num = nvkm_rd32(sc, 0x002b08 + runl_id * 0x10);
	rl_status = nvkm_rd32(sc, 0x002b0c + runl_id * 0x10);

	device_printf(sc->dev,
	    "gsp_rm: SCHED_TRACE %-18s chid=%u runlist=%u "
	    "PCCSR_INST=0x%08x(bind=%u ptr=0x%x target=%u) "
	    "PCCSR_CHANNEL=0x%08x(enable=%u busy=%u) "
	    "RUNLIST=%08x:%08x num=0x%08x status=0x%08x\n",
	    tag, chid, runl_id, pccsr_inst, !!(pccsr_inst & 0x80000000u),
	    pccsr_inst & 0x0fffffffu, (pccsr_inst >> 28) & 0x3u,
	    pccsr_chan, pccsr_chan & 0x1u,
	    (pccsr_chan >> 28) & 0x1u, rl_base_hi, rl_base_lo,
	    rl_num, rl_status);
#endif
}

static void
nvkm_gsp_userd_clear(struct nvkm_softc *sc, const struct nvkm_gsp_chan *chan)
{
	static const uint32_t userd_clear_offs[] = {
		0x040, 0x044, 0x048, 0x04c, 0x050,
		0x058, 0x05c, 0x060, NV_USERD_GP_GET, NV_USERD_GP_PUT,
	};
	uint64_t slot_bar1 = chan->userd_bar2_gva +
	    (uint64_t)chan->chid * NV_USERD_SLOT_SIZE;
	unsigned int i;

	for (i = 0; i < sizeof(userd_clear_offs) /
	    sizeof(userd_clear_offs[0]); i++)
		nvkm_gsp_bar1_wr32(sc, slot_bar1 + userd_clear_offs[i], 0);
	nvkm_gsp_bar1_flush(sc);

#ifdef NVKM_DEBUG_USERD_CLEAR
	device_printf(sc->dev,
	    "gsp_rm: USERD clear before schedule slot=0x%llx "
	    "GP_GET=0x%08x GP_PUT=0x%08x\n",
	    (unsigned long long)slot_bar1,
	    nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_GET),
	    nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_PUT));
#endif
}

static void nvkm_gsp_zero_vram(struct nvkm_softc *sc, uint64_t paddr,
    uint64_t size);

static uint32_t
nvkm_order_base_2_u64(uint64_t value)
{
	uint32_t shift;
	uint64_t n;

	if (value <= 1)
		return (0);

	shift = 0;
	n = 1;
	while (n < value) {
		n <<= 1;
		shift++;
	}
	return (shift);
}

static int
nvkm_gsp_chan_rm_alloc(struct nvkm_gsp_vmm *vmm, struct nvkm_gsp_chan *chan,
    uint32_t handle, uint32_t engine_type, uint8_t priv, uint64_t inst_addr,
    uint64_t userd_addr, uint64_t mthdbuf_addr, uint32_t mthdbuf_size,
    uint64_t gpfifo_offset, uint32_t gpfifo_length)
{
	struct nvkm_gsp_device *device = &vmm->device;
	struct nvkm_softc *sc = vmm->sc;
	struct NV_CHANNEL_ALLOC_PARAMS_r570 *args;
	uint32_t userd_p, userd_i;
	int err;

	args = nvkm_gsp_rm_alloc_get(&device->object, handle,
	    TURING_CHANNEL_GPFIFO_A, sizeof(*args), &chan->object);
	if (args == NULL)
		return (ENOMEM);

	args->gpFifoOffset = gpfifo_offset;
	args->gpFifoEntries = gpfifo_length / 8;

	userd_p = (uint32_t)chan->chid / 8u;
	userd_i = (uint32_t)chan->chid % 8u;
	args->flags =
	    ((userd_i & 7u) << 8) |
	    ((userd_p & 0x1ffu) << 12) |
	    (1U << 21) /* USERD_INDEX_PAGE_FIXED */;
	if (priv)
		args->flags |= (1U << 5) /* PRIVILEGED_CHANNEL_TRUE */;

	args->hVASpace = vmm->vaspace.handle;
	args->engineType = engine_type;

	args->instanceMem.base = inst_addr;
	args->instanceMem.size = NV_CHANNEL_INST_SIZE;
	args->instanceMem.addressSpace = NV_MEMORY_DESC_ADDRSPACE_VIDMEM;
	args->instanceMem.cacheAttrib = 1;

	args->userdMem.base = userd_addr;
	args->userdMem.size = NV_CHANNEL_USERD_SIZE;
	args->userdMem.addressSpace = NV_MEMORY_DESC_ADDRSPACE_VIDMEM;
	args->userdMem.cacheAttrib = 1;

	args->ramfcMem.base = inst_addr;
	args->ramfcMem.size = NV_CHANNEL_RAMFC_SIZE;
	args->ramfcMem.addressSpace = NV_MEMORY_DESC_ADDRSPACE_VIDMEM;
	args->ramfcMem.cacheAttrib = 1;

	args->mthdbufMem.base = mthdbuf_addr;
	args->mthdbufMem.size = mthdbuf_size;
	args->mthdbufMem.addressSpace = NV_MEMORY_DESC_ADDRSPACE_SYSMEM_NONCOH;
	args->mthdbufMem.cacheAttrib = 0;

	args->internalFlags = NV_KERNELCHANNEL_INTERNALFLAGS_ERRNOT_NONE |
	    NV_KERNELCHANNEL_INTERNALFLAGS_ECCNOT_NONE;
	if (priv)
		args->internalFlags |= NV_KERNELCHANNEL_INTERNALFLAGS_PRIV_ADMIN;
	else
		args->internalFlags |= NV_KERNELCHANNEL_INTERNALFLAGS_PRIV_USER;

	err = nvkm_gsp_rm_alloc_wr(&chan->object, args);
	if (err != 0) {
		device_printf(sc->dev,
		    "gsp_rm: TURING_CHANNEL_GPFIFO_A alloc failed err=%d "
		    "handle=0x%x chid=%d inst=0x%llx userd=0x%llx "
		    "mthdbuf=0x%llx gpfifo=0x%llx/0x%x\n",
		    err, handle, chan->chid, (unsigned long long)inst_addr,
		    (unsigned long long)userd_addr,
		    (unsigned long long)mthdbuf_addr,
		    (unsigned long long)gpfifo_offset, gpfifo_length);
		return (err);
	}

	nvkm_gsp_sched_trace(sc, "after-chan-alloc", chan, engine_type);
	return (0);
}

static int
nvkm_gsp_golden_chan_ctor(struct nvkm_gsp_vmm *vmm,
    struct nvkm_gsp_chan *chan)
{
	struct nvkm_softc *sc = vmm->sc;
	uint32_t mthdbuf_size;
	int err;

	memset(chan, 0, sizeof(*chan));

	chan->chid = 1;
	chan->inst_vram = nvkm_gsp_vram_alloc(sc, 0x12000, 0x1000);
	if (chan->inst_vram == 0)
		return (ENOMEM);
	chan->userd_vram = chan->inst_vram + 0x1000;
	chan->mthdbuf_paddr = chan->inst_vram + 0x2000;
	mthdbuf_size = sc->mthdbuf_size ? sc->mthdbuf_size : 0x4000U;
	chan->mthdbuf_size = mthdbuf_size;
	nvkm_gsp_zero_vram(sc, chan->inst_vram, 0x12000);

	device_printf(sc->dev,
	    "gsp_rm: GR oneinit golden inst=0x%llx userd=0x%llx "
	    "mthdbuf=0x%llx size=0x%x handle=0x%x chid=%d\n",
	    (unsigned long long)chan->inst_vram,
	    (unsigned long long)chan->userd_vram,
	    (unsigned long long)chan->mthdbuf_paddr, mthdbuf_size,
	    NVKM_RM_CHANNEL, chan->chid);

	err = nvkm_gsp_chan_rm_alloc(vmm, chan, NVKM_RM_CHANNEL,
	    NV2080_ENGINE_TYPE_GRAPHICS, 1, chan->inst_vram,
	    chan->userd_vram, chan->mthdbuf_paddr, mthdbuf_size, 0, 0x1000);
	if (err != 0)
		return (err);

	return (0);
}

int
nvkm_gsp_chan_ctor(struct nvkm_gsp_vmm *vmm,
    uint32_t engine_type, struct nvkm_gsp_chan *chan)
{
	struct nvkm_softc *sc = vmm->sc;
	uint32_t mthdbuf_sz;
	int err;

	memset(chan, 0, sizeof(*chan));
	chan->submit_gva_push = NVKM_VMM_CLIENT_BASE +
	    (uint64_t)nvkm_gsp_submit_gva_slot * SUBMIT_GVA_STRIDE;
	chan->submit_gva_gpf = chan->submit_gva_push + 0x1000ULL;
	chan->submit_gva_sema = chan->submit_gva_push + 0x2000ULL;
	nvkm_gsp_submit_gva_slot++;

	/* VRAM: inst block + USERD (separate pages). */
	chan->inst_vram  = nvkm_gsp_vram_alloc(sc, NV_CHANNEL_INST_SIZE, 0x1000);
	/* USERD page: 4 KiB / 8 slots * 0x200. GSP indexes within using
	CHANNEL_USERD_INDEX_VALUE=chid%8. Match nouveau B.2 walkthrough. */
	chan->userd_vram = nvkm_gsp_vram_alloc(sc, 0x1000U, 0x1000);
	device_printf(sc->dev,
	    "gsp_rm: chan->inst_vram=0x%llx chan->userd_vram=0x%llx (alloc\'d)\n",
	    (unsigned long long)chan->inst_vram,
	    (unsigned long long)chan->userd_vram);

	/* Map inst block to BAR1 immediately for L2-coherent writes.
	 * nouveau writes inst via BAR1/instmem path (also L2-coherent);
	 * we previously used PRAMIN which bypasses L2 so GSP's later reads
	 * (through its own BAR1 view) could miss our writes. */
	if (chan->inst_vram != 0) {
		chan->inst_bar1_gva = sc->bar1.next_gva;
		sc->bar1.next_gva += 0x1000;
		(void)nvkm_gsp_bar1_map_vram(sc, chan->inst_bar1_gva, chan->inst_vram);
		nvkm_gsp_bar1_flush(sc);
		nvkm_gsp_bar1_invalidate(sc);
		device_printf(sc->dev,
		    "gsp_rm: inst mapped to BAR1 GVA 0x%llx (paddr 0x%llx)\n",
		    (unsigned long long)chan->inst_bar1_gva,
		    (unsigned long long)chan->inst_vram);

		/* Zero inst block (4 KiB) via BAR1 wr32. */
		for (uint32_t off = 0; off < 0x1000; off += 4)
			nvkm_gsp_bar1_wr32(sc, chan->inst_bar1_gva + off, 0);
		nvkm_gsp_bar1_flush(sc);
		device_printf(sc->dev, "gsp_rm: chan inst block zeroed via BAR1\n");

		/* PRE-FILL inst[0x200/0x204] with PD3 PDB in NV_RAMIN format. */
		uint64_t pdb_paddr = vmm->pt[0].page.vram_paddr;
		uint32_t pdb_lo = (uint32_t)((pdb_paddr >> 12) << 12)
		    | (1u << 10) /* USE_NEW_PT_FORMAT (VER2) */
		    | (1u << 11) /* BIG_PAGE_SIZE_64KB */
		    /* target VID_MEM = bits[1:0] = 0, vol = bit 2 = 0 */;
		uint32_t pdb_hi = (uint32_t)(pdb_paddr >> 32);
		nvkm_gsp_bar1_wr32(sc, chan->inst_bar1_gva + 0x200, pdb_lo);
		nvkm_gsp_bar1_wr32(sc, chan->inst_bar1_gva + 0x204, pdb_hi);
		nvkm_gsp_bar1_flush(sc);
		device_printf(sc->dev,
		    "gsp_rm: PRE-FILL chan inst[0x200]=0x%08x [0x204]=0x%08x via BAR1 (PDB=0x%llx)\n",
		    pdb_lo, pdb_hi, (unsigned long long)pdb_paddr);
	}
	if (chan->inst_vram == 0 || chan->userd_vram == 0) {
		device_printf(sc->dev,
		    "gsp_rm: channel VRAM alloc failed\n");
		return (ENOMEM);
	}

	/* sysmem: CE method buffer. Size queried at attach. */
	mthdbuf_sz = sc->mthdbuf_size ? sc->mthdbuf_size : 0x4000U;
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
		struct nvkm_bar1_page submit_lpt = {0};
		if ((err = nvkm_gsp_bar1_alloc_page(sc, &chan->submit_pd0)) ||
		    (err = nvkm_gsp_bar1_alloc_page(sc, &chan->submit_spt)) ||
		    (err = nvkm_gsp_bar1_alloc_page(sc, &submit_lpt))) {
			device_printf(sc->dev,
			    "gsp_rm: chan submit page alloc failed err=%d\n", err);
			contigfree(chan->mthdbuf_kva, chan->mthdbuf_size,
			    M_NVKM_MTHDBUF);
			chan->mthdbuf_kva = NULL;
			return (err);
		}
		/* push/gpf/sema in sysmem (matches nouveau NVIF_MEM_COHERENT GART). */
		chan->submit_push.kva = contigmalloc(0x1000, M_NVKM_MTHDBUF,
		    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, 0x1000, 0);
		chan->submit_gpf.kva  = contigmalloc(0x1000, M_NVKM_MTHDBUF,
		    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, 0x1000, 0);
		chan->submit_sema.kva = contigmalloc(0x1000, M_NVKM_MTHDBUF,
		    M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, 0x1000, 0);
		if (chan->submit_push.kva == NULL || chan->submit_gpf.kva == NULL ||
		    chan->submit_sema.kva == NULL) {
			device_printf(sc->dev,
			    "gsp_rm: sysmem submit BO alloc failed\n");
			return (ENOMEM);
		}
		chan->submit_push.paddr = vtophys(chan->submit_push.kva);
		chan->submit_gpf.paddr  = vtophys(chan->submit_gpf.kva);
		chan->submit_sema.paddr = vtophys(chan->submit_sema.kva);
		device_printf(sc->dev,
		    "gsp_rm: sysmem BOs push=0x%llx gpf=0x%llx sema=0x%llx\n",
		    (unsigned long long)chan->submit_push.paddr,
		    (unsigned long long)chan->submit_gpf.paddr,
		    (unsigned long long)chan->submit_sema.paddr);

		const uint32_t pd1_idx = (chan->submit_gva_push >> NVKM_GMMU_PD1_SHIFT)
		    & (NVKM_GMMU_PD1_ENTRIES - 1);
		const uint32_t pd0_idx = (chan->submit_gva_push >> NVKM_GMMU_PD0_SHIFT)
		    & (NVKM_GMMU_PD0_ENTRIES - 1);
		const uint32_t spt_idx = (chan->submit_gva_push >> NVKM_GMMU_SPT_SHIFT)
		    & (NVKM_GMMU_SPT_ENTRIES - 1);

		/* PD1[k] -> PD0 (VRAM); PD0[k].small -> SPT (VRAM); SPT entries
		 * for push/gpf/sema (all VRAM). All writes via BAR1. */
		nvkm_gsp_bar1_wr64(sc,
		    vmm->pt[2].page.bar1_gva + pd1_idx * 8,
		    nvkm_pde_to_vram(chan->submit_pd0.vram_paddr));
		/* PD0 dual entry: BIG = empty LPT (all-zero so 64 KiB walks invalid,
		 * walker falls back to SMALL); SMALL = SPT (our 4 KiB pages). */
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_pd0.bar1_gva + (pd0_idx * 2 + 0) * 8,
		    nvkm_pde_to_vram(submit_lpt.vram_paddr));
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_pd0.bar1_gva + (pd0_idx * 2 + 1) * 8,
		    nvkm_pde_to_vram(chan->submit_spt.vram_paddr));
		/* SPT entries: 3 sysmem data pages, aperture SYS_COH. */
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_spt.bar1_gva + (spt_idx + 0) * 8,
		    nvkm_pte_to_sysmem(chan->submit_push.paddr));
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_spt.bar1_gva + (spt_idx + 1) * 8,
		    nvkm_pte_to_sysmem(chan->submit_gpf.paddr));
		nvkm_gsp_bar1_wr64(sc,
		    chan->submit_spt.bar1_gva + (spt_idx + 2) * 8,
		    nvkm_pte_to_sysmem(chan->submit_sema.paddr));

#ifdef NVKM_DEBUG_SUBMIT_PT_DUMP
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
			/* inst already mapped to BAR1 in chan_ctor early; reuse */
			uint64_t inst_bar1_gva = chan->inst_bar1_gva;
			device_printf(sc->dev,
			    "gsp_rm: DIAG inst_bar1_gva=0x%llx mapping to vram=0x%llx (bar1 SPT=0x%llx idx=%llu)\n",
			    (unsigned long long)inst_bar1_gva,
			    (unsigned long long)chan->inst_vram,
			    (unsigned long long)sc->bar1.spt_paddr,
			    (unsigned long long)(inst_bar1_gva >> 12));

			/* (no need to re-map or invalidate — done in chan_ctor early) */

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
			/* Scan whole 4 KiB inst block for any non-zero dword. */
			{
				uint32_t nz_count = 0;
				for (uint32_t off = 0; off < 0x1000; off += 4) {
					uint32_t v = nvkm_gsp_bar1_rd32(sc, inst_bar1_gva + off);
					if (v != 0) {
						device_printf(sc->dev,
						    "gsp_rm: inst[0x%03x] = 0x%08x\n", off, v);
						nz_count++;
						if (nz_count > 20) break;
					}
				}
				device_printf(sc->dev, "gsp_rm: total non-zero inst dwords: %u\n", nz_count);
			}
						device_printf(sc->dev,
			    "gsp_rm: SC0 PDB target=%u vol=%u pdb_paddr=0x%llx (our PD3=0x%llx)\n",
			    sc0_lo & 3u, (sc0_lo >> 2) & 1u,
			    (unsigned long long)sc0_pdb,
			    (unsigned long long)vmm->pt[0].page.vram_paddr);

		}

		device_printf(sc->dev,
		    "gsp_rm: expected: PD3[0]=0x%llx (PD2 paddr) PD2[0]=0x%llx (PD1 paddr) PD1[%u]=0x%llx (PD0 paddr) PD0.SMALL=0x%llx (SPT paddr) SPT[0]=0x%llx (push paddr)\n",
		    (unsigned long long)nvkm_pde_to_vram(vmm->pt[1].page.vram_paddr),
		    (unsigned long long)nvkm_pde_to_vram(vmm->pt[2].page.vram_paddr),
		    pd1_idx, (unsigned long long)nvkm_pde_to_vram(chan->submit_pd0.vram_paddr),
		    (unsigned long long)nvkm_pde_to_vram(chan->submit_spt.vram_paddr),
		    (unsigned long long)nvkm_pte_to_sysmem(chan->submit_push.paddr));

		device_printf(sc->dev,
		    "gsp_rm: submit PT (VRAM via BAR1): PD0 vram=0x%llx bar1=0x%llx "
		    "SPT vram=0x%llx bar1=0x%llx\n",
		    (unsigned long long)chan->submit_pd0.vram_paddr,
		    (unsigned long long)chan->submit_pd0.bar1_gva,
		    (unsigned long long)chan->submit_spt.vram_paddr,
		    (unsigned long long)chan->submit_spt.bar1_gva);
		device_printf(sc->dev,
		    "gsp_rm: BOs push vram=0x%llx bar1=0x%llx gpf vram=0x%llx bar1=0x%llx sema vram=0x%llx bar1=0x%llx\n",
		    (unsigned long long)chan->submit_push.paddr,
		    (unsigned long long)(uintptr_t)chan->submit_push.kva,
		    (unsigned long long)chan->submit_gpf.paddr,
		    (unsigned long long)(uintptr_t)chan->submit_gpf.kva,
		    (unsigned long long)chan->submit_sema.paddr,
		    (unsigned long long)(uintptr_t)chan->submit_sema.kva);
#endif
		err = nvkm_gsp_vmm_map_sysmem(vmm, chan->submit_gva_push,
		    chan->submit_push.paddr, 0x1000);
		if (err != 0)
			return (err);
		err = nvkm_gsp_vmm_map_sysmem(vmm, chan->submit_gva_gpf,
		    chan->submit_gpf.paddr, 0x1000);
		if (err != 0)
			return (err);
		err = nvkm_gsp_vmm_map_sysmem(vmm, chan->submit_gva_sema,
		    chan->submit_sema.paddr, 0x1000);
		if (err != 0)
			return (err);
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

	/* nouveau clears USERD before RAMFC/channel programming. Do the BAR1
	 * mapping and clear before RM schedules the channel, otherwise HOST can
	 * see stale GP_GET/GP_PUT immediately after SCHEDULE. */
	chan->userd_bar2_gva = sc->bar1.next_gva;
	sc->bar1.next_gva += 0x1000;
	(void)nvkm_gsp_bar1_map_vram(sc, chan->userd_bar2_gva,
	    chan->userd_vram);
	nvkm_gsp_bar1_flush(sc);
	nvkm_gsp_bar1_invalidate(sc);
	nvkm_gsp_userd_clear(sc, chan);

#ifdef NVKM_DEBUG_USERD_CLEAR
	/* Diag: write+readback marker at the actual USERD GVA we just mapped. */
	{
		nvkm_gsp_bar1_wr32(sc, chan->userd_bar2_gva + 0x10, 0xCAFEBABEu);
		nvkm_gsp_bar1_flush(sc);
		uint32_t rb = nvkm_gsp_bar1_rd32(sc, chan->userd_bar2_gva + 0x10);
		device_printf(sc->dev,
		    "bar1_diag: USERD page via BAR1 GVA 0x%llx+0x10 readback = 0x%08x (expect cafebabe)\n",
		    (unsigned long long)chan->userd_bar2_gva, rb);
		nvkm_gsp_bar1_wr32(sc, chan->userd_bar2_gva + 0x10, 0);
		nvkm_gsp_bar1_flush(sc);
	}
#endif

	/* gpFifoOffset/Entries point at our pre-mapped sysmem ring.  Per
	 * nouveau r570_chan_alloc caller (r535/fifo.c:185), userd_addr is
	 * the per-chid slot address, not just the USERD page base. */
	err = nvkm_gsp_chan_rm_alloc(vmm, chan,
	    NVKM_RM_CHANNEL | (uint32_t)chan->chid, engine_type, 1,
	    chan->inst_vram,
	    chan->userd_vram + (uint64_t)chan->chid * NV_USERD_SLOT_SIZE,
	    chan->mthdbuf_paddr, mthdbuf_sz, chan->submit_gva_gpf, 0x1000);
	if (err != 0) {
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
		nvkm_gsp_sched_trace(sc, "after-bind", chan, engine_type);

#ifdef NVKM_DEBUG_SCHED_TRACE
		/* Query workSubmitToken BEFORE SCHEDULE to show the transition. */
	{
		struct { uint32_t workSubmitToken; } *t;
		t = nvkm_gsp_rm_ctrl_get(&chan->object, 0xc36f0108u, sizeof(*t));
		if (t == NULL) {
			device_printf(sc->dev,
			    "gsp_rm: TRACE pre-SCHEDULE token: ctrl_get failed\n");
		} else {
			t->workSubmitToken = 0xdeadbeef;
			int perr = nvkm_gsp_rm_ctrl_rd(&chan->object, (void**)&t,
			    sizeof(*t));
			device_printf(sc->dev,
		    "gsp_rm: TRACE pre-SCHEDULE token: err=%d val=0x%08x\n",
			    perr, t->workSubmitToken);
		}
	}
#endif

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
		nvkm_gsp_sched_trace(sc, "after-schedule", chan, engine_type);

		/* Engine class object: TURING_DMA_COPY_A for CE engines.
	 * GRAPHICS would need TURING_A (0xc597) + ctx buffers; we skip for
	 * simple scheduling test - NVC36F SEM_RELEASE is channel-level and
	 * does not require an engine class object. */
	if (engine_type >= NV2080_ENGINE_TYPE_COPY0 &&
	    engine_type <= NV2080_ENGINE_TYPE_COPY2) {
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
	} else {
			device_printf(sc->dev,
			    "gsp_rm: SKIP engine class alloc (engine_type=0x%x is not CE)\n",
			    engine_type);
		}
		nvkm_gsp_sched_trace(sc, "after-ce-alloc", chan, engine_type);

#ifdef NVKM_DEBUG_SCHED_TRACE
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
#endif

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
				    "gsp_rm: TRACE post-SCHEDULE token: err=%d "
				    "(channel not on runlist?)\n", werr);
			} else {
				chan->gsp_token = t->workSubmitToken;
				device_printf(sc->dev,
				    "gsp_rm: workSubmitToken=0x%08x\n",
				    chan->gsp_token);
				}
			}
		}
		nvkm_gsp_sched_trace(sc, "after-token", chan, engine_type);

		device_printf(sc->dev,
	    "gsp_rm: TURING_CHANNEL_GPFIFO_A handle=0x%x engine=0x%x bound+scheduled+CE\n",
	    chan->object.handle, engine_type);

	/* HAIL MARY: manually write NV_RUNLIST_NUM register to commit runlist
	 * to PBDMA. nouveau non-GSP path (tu102_runl_commit) writes this
	 * as the commit trigger. In GSP mode nouveau doesn't, expecting
	 * GSP to do it — but our PBDMA never picks up channels. Try writing
	 * NUM ourselves and see if PBDMA wakes.
	 * Runlist VRAM contains our cgrp+chan entries (2 cgrp headers + 2
	 * channels = 4 entries based on dump). Write count=4. */
#if NVKM_GSP_DEBUG_HAIL_MARY_RUNLIST
	{
		uint32_t runl_id = 0;
		switch (engine_type) {
		case NV2080_ENGINE_TYPE_COPY0:
		case NV2080_ENGINE_TYPE_COPY1:
			runl_id = 0;
			break;
		case NV2080_ENGINE_TYPE_COPY2:
			runl_id = 8;
			break;
		default:
			runl_id = 0;
		}
		uint32_t b_lo = nvkm_rd32(sc, 0x002b00 + runl_id * 0x10);
		uint32_t b_hi = nvkm_rd32(sc, 0x002b04 + runl_id * 0x10);
		uint32_t num_pre  = nvkm_rd32(sc, 0x002b08 + runl_id * 0x10);
		uint32_t stat_pre = nvkm_rd32(sc, 0x002b0c + runl_id * 0x10);
		device_printf(sc->dev,
		    "gsp_rm: HAIL MARY pre-write RUNLIST[%u]: BASE=%08x:%08x NUM=0x%08x STATUS=0x%08x\n",
		    runl_id, b_hi, b_lo, num_pre, stat_pre);

		/* Write count=4 to NUM to trigger commit. Try various counts. */
		if (b_lo != 0xbadf5040u && b_lo != 0) {
			nvkm_wr32(sc, 0x002b08 + runl_id * 0x10, 4);
			DELAY(10000);
			uint32_t num_post  = nvkm_rd32(sc, 0x002b08 + runl_id * 0x10);
			uint32_t stat_post = nvkm_rd32(sc, 0x002b0c + runl_id * 0x10);
			device_printf(sc->dev,
			    "gsp_rm: HAIL MARY wrote NUM=4 → post NUM=0x%08x STATUS=0x%08x\n",
			    num_post, stat_post);
		}
	}
#endif

	/* TURING_USERMODE_A is allocated once per device in vmm_ctor
	 * (nouveau does this at drm init, before any channel). */

	/* The first ctor call builds the bootstrap submit_test channel.  DRM
	 * user channels are tracked by nvkm_drm.c and must not replace it. */
	if (sc->gsp_chan == NULL)
		sc->gsp_chan = chan;
	return (0);
}



int
nvkm_gsp_submit_test(struct nvkm_softc *sc)
{
	struct nvkm_gsp_chan *chan = sc->gsp_chan;
	uint32_t saved_pramin;
	int ms;

	if (chan == NULL || chan->submit_push.kva == NULL)
		return (ENXIO);

	/* push/gpf/sema in sysmem, CPU direct write (cache-coherent on x86). */
	uint32_t *const push_w = (uint32_t *)chan->submit_push.kva;
	uint32_t *const gpf_w  = (uint32_t *)chan->submit_gpf.kva;
	uint32_t *const sema_w = (uint32_t *)chan->submit_sema.kva;

	/* Pushbuf -- NVC36F SEM_ADDR_LO/HI/PAYLOAD_LO + SEM_EXECUTE.
	 * Refs: clc36f.h:95-128, push906f.h:23-49, chanc36f.c:26-49. */
	push_w[0] = NVC36F_PUSH_HDR_SEM_ADDR_TRIPLET;
	push_w[1] = (uint32_t)(chan->submit_gva_sema & 0xffffffffu);
	push_w[2] = (uint32_t)((chan->submit_gva_sema >> 32) & 0xffu);
	push_w[3] = SEM_PAYLOAD;
	push_w[4] = NVC36F_PUSH_HDR_SEM_EXECUTE;
	push_w[5] = NVC36F_SEM_EXECUTE_RELEASE;

	/* GPFIFO entry[0] -- nvif/chan506f.c:nvif_chan506f_gpfifo_push.
	 * dw0 = lower_32(push_gva); dw1 = upper_8(push_gva) | (dwords<<10). */
	gpf_w[0] = (uint32_t)(chan->submit_gva_push & 0xffffffffu);
	gpf_w[1] = (uint32_t)((chan->submit_gva_push >> 32) & 0xffu)
	    | (SUBMIT_PUSH_DWORDS << NVC06F_GP_ENTRY1_LENGTH_SHIFT);
	cpu_sfence();  /* ensure push/gpf stores reach sysmem before USERD GP_PUT */

	sema_w[0] = 0;

	device_printf(sc->dev,
	    "gsp_submit: push[0..5]= %08x %08x %08x %08x %08x %08x\n",
	    push_w[0], push_w[1], push_w[2], push_w[3], push_w[4], push_w[5]);
	device_printf(sc->dev,
	    "gsp_submit: gpf[0..1]= %08x %08x  sema=%08x\n",
	    gpf_w[0], gpf_w[1], sema_w[0]);

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
	/* Sleep 200ms after SCHEDULE for GSP scheduler to load runlist into
	 * PBDMA. SCHEDULE returns when GSP-side scheduling decision is made
	 * but actual PBDMA RUNLIST_BASE write may be deferred. */
	device_printf(sc->dev, "gsp_submit: 200ms wait for GSP scheduler...\n");
	DELAY(200000);

#ifdef NVKM_DEBUG_SUBMIT_HW
	{
		uint32_t um_t0_lo = nvkm_rd32(sc, NV_USERMODE_TIME_LO);
		uint32_t um_t0_hi = nvkm_rd32(sc, NV_USERMODE_TIME_HI);
		uint32_t db_rb    = nvkm_rd32(sc, NV_USERMODE_DOORBELL);
		device_printf(sc->dev,
		    "gsp_submit: DIAG pre-doorbell USERMODE_TIME=%08x:%08x DOORBELL_RB=0x%08x\n",
		    um_t0_hi, um_t0_lo, db_rb);

		/* PCCSR_CHANNEL read for our chid. Per TU104 dev_fifo.ref.txt:
		 *   NV_PCCSR_CHANNEL_INST(i)    = 0x00800000 + i*8  (bit 31 = BIND)
		 *   NV_PCCSR_CHANNEL(i)         = 0x00800004 + i*8  (bit 0 = ENABLE)
		 * PBDMA gates scheduling on ENABLE=IN_USE; if BIND/SCHEDULE
		 * didn't set these, PBDMA ignores doorbells. */
		{
			uint32_t pccsr_inst = nvkm_rd32(sc, 0x00800000 + chan->chid * 8);
			uint32_t pccsr_chan = nvkm_rd32(sc, 0x00800004 + chan->chid * 8);
			/* Dump RAMFC at the paddr PCCSR points at — may differ from
			 * chan->inst_vram if GSP allocated its own inst block. */
			{
				uint64_t real_inst = ((uint64_t)pccsr_inst & 0x0fffffffull) << 12;
				device_printf(sc->dev,
				    "gsp_submit: DIAG real inst (per PCCSR) = 0x%llx, chan->inst_vram = 0x%llx, %s\n",
				    (unsigned long long)real_inst,
				    (unsigned long long)chan->inst_vram,
				    real_inst == chan->inst_vram ? "MATCH" : "*** DIFFER ***");
				if (real_inst != 0 && real_inst != chan->inst_vram) {
					lwkt_gettoken(&sc->gsp_tok);
					uint32_t saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
					nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(real_inst >> 16));
					device_printf(sc->dev,
					    "gsp_submit: DIAG real-inst[0x000..0x060]: %08x %08x %08x %08x %08x %08x %08x %08x  %08x %08x %08x %08x %08x %08x %08x %08x  %08x %08x %08x %08x %08x %08x %08x %08x\n",
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x00) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x04) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x08) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x0c) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x10) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x14) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x18) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x1c) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x20) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x24) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x28) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x2c) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x30) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x34) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x38) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x3c) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x40) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x44) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x48) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x4c) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x50) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x54) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x58) & 0xffffu)),
					    nvkm_rd32(sc, NV_PRAMIN + (uint32_t)((real_inst + 0x5c) & 0xffffu)));
					nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
					lwkt_reltoken(&sc->gsp_tok);
				}
			}
			device_printf(sc->dev,
			    "gsp_submit: DIAG PCCSR[chid=%d]: INST=0x%08x (BIND=%u, INST_PTR>>12=0x%x, TARGET=%u) CHANNEL=0x%08x (ENABLE=%u, BUSY=%u)\n",
			    chan->chid, pccsr_inst,
			    (pccsr_inst >> 31) & 1u,
			    pccsr_inst & 0x0fffffffu,
			    (pccsr_inst >> 28) & 3u,
			    pccsr_chan,
			    pccsr_chan & 1u,
			    (pccsr_chan >> 28) & 1u);
			/* Also read PCCSR for chid=0 (GSP helper) for comparison. */
			uint32_t h_inst = nvkm_rd32(sc, 0x00800000 + 0 * 8);
			uint32_t h_chan = nvkm_rd32(sc, 0x00800004 + 0 * 8);
			device_printf(sc->dev,
			    "gsp_submit: DIAG PCCSR[chid=0 GSP-helper]: INST=0x%08x CHANNEL=0x%08x\n",
			    h_inst, h_chan);
		}

#ifdef NVKM_DEBUG_SUBMIT_RUNLIST_DUMP
		/* Per nouveau tu102_runl_commit (fifo/tu102.c:71):
		 *   NV_RUNLIST_BASE_LO = 0x002b00 + (runl_id * 0x10)
		 *   NV_RUNLIST_BASE_HI = 0x002b04 + (runl_id * 0x10)
		 *   NV_RUNLIST_NUM     = 0x002b08 + (runl_id * 0x10)
		 *   NV_RUNLIST_STATUS  = 0x002b0c + (runl_id * 0x10)  bit15 = PENDING
		 * Read runlist 0 (GRAPHICS+CE0+CE1) and runlist 8 (CE2 = ours).
		 * If GSP populated them, RUNLIST_BASE is a real VRAM paddr; if
		 * unconfigured, reads PRI_BAD (0xbadf5040). */
		for (uint32_t rl = 0; rl <= 8; rl += 8) {
			uint32_t base_lo = nvkm_rd32(sc, 0x002b00 + rl * 0x10);
			uint32_t base_hi = nvkm_rd32(sc, 0x002b04 + rl * 0x10);
			uint32_t num     = nvkm_rd32(sc, 0x002b08 + rl * 0x10);
			uint32_t status  = nvkm_rd32(sc, 0x002b0c + rl * 0x10);
			device_printf(sc->dev,
			    "gsp_submit: DIAG RUNLIST[%u]: BASE=%08x:%08x NUM=0x%08x STATUS=0x%08x\n",
			    rl, base_hi, base_lo, num, status);
			/* If BASE looks like a real VRAM paddr (not PRI_BAD, not zero),
			 * dump first 256 bytes of runlist VRAM. */
			if (base_lo != 0xbadf5040u && base_lo != 0 && (base_hi & 0xffffff00) == 0) {
				uint64_t rl_paddr = ((uint64_t)base_hi << 32) | base_lo;
				/* nouveau encodes BASE as (target<<28) | (addr>>12) on pre-Turing
				 * but Turing splits lo/hi differently. Try both interpretations. */
				uint64_t rl_paddr_alt = ((uint64_t)base_hi << 32) | ((uint64_t)base_lo << 12);
				device_printf(sc->dev,
				    "gsp_submit:   raw paddr=0x%llx  shifted-lo=0x%llx\n",
				    (unsigned long long)rl_paddr,
				    (unsigned long long)rl_paddr_alt);
				/* Try raw paddr interpretation - dump up to 256B via PRAMIN. */
				lwkt_gettoken(&sc->gsp_tok);
				uint32_t saved_p = nvkm_rd32(sc, NV_PBUS_PRAMIN);
				nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(rl_paddr >> 16));
				for (uint32_t off = 0; off < 0x100; off += 0x10) {
					uint32_t w[4];
					for (int k = 0; k < 4; k++)
						w[k] = nvkm_rd32(sc, NV_PRAMIN +
						    (uint32_t)((rl_paddr + off + k * 4) & 0xffffu));
					device_printf(sc->dev,
					    "gsp_submit:   runlist[%u]+0x%02x: %08x %08x %08x %08x\n",
					    rl, off, w[0], w[1], w[2], w[3]);
				}
				nvkm_wr32(sc, NV_PBUS_PRAMIN, saved_p);
				lwkt_reltoken(&sc->gsp_tok);
			}
		}
#endif
	}
#endif
	device_printf(sc->dev,
	    "gsp_submit: doorbell token=0x%08x\n", chan->gsp_token);
	/* Use the workSubmitToken GSP gave us. It encodes the runlist/chid
	 * tuple; raw chid or guessed token writes are diagnostic hail-marys.
	 */
	nvkm_wr32(sc, NV_USERMODE_DOORBELL, chan->gsp_token);
	cpu_sfence();

#ifdef NVKM_DEBUG_SUBMIT_HW
	/* Re-read USERMODE TIME and DOORBELL after the write. */
	{
		uint32_t um_t1_lo = nvkm_rd32(sc, NV_USERMODE_TIME_LO);
		uint32_t um_t1_hi = nvkm_rd32(sc, NV_USERMODE_TIME_HI);
		uint32_t db_rb    = nvkm_rd32(sc, NV_USERMODE_DOORBELL);
		device_printf(sc->dev,
		    "gsp_submit: DIAG post-doorbell USERMODE_TIME=%08x:%08x DOORBELL_RB=0x%08x\n",
		    um_t1_hi, um_t1_lo, db_rb);
	}
#endif

#ifdef NVKM_DEBUG_SUBMIT_INST_DUMP
	/* DIAG (b): full channel inst block (RAMFC area) 0..0x400 via PRAMIN.
	 * Look for GSP-written USERD_PTR (probably ~0x100-0x108 or 0x4-0xc),
	 * GP_BASE/GP_PUT/GP_GET fields, ENG_CTX_PTR, etc. */
	{
		lwkt_gettoken(&sc->gsp_tok);
		uint32_t saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
		nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(chan->inst_vram >> 16));
		for (uint32_t off = 0; off < 0x400; off += 0x20) {
			uint32_t w[8];
			for (int k = 0; k < 8; k++)
				w[k] = nvkm_rd32(sc, NV_PRAMIN
				    + (uint32_t)((chan->inst_vram + off + k * 4) & 0xffffu));
			device_printf(sc->dev,
			    "gsp_submit: DIAG inst[0x%03x]: %08x %08x %08x %08x  %08x %08x %08x %08x\n",
			    off, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
		}
		nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
		lwkt_reltoken(&sc->gsp_tok);
	}
#endif

	device_printf(sc->dev,
	    "gsp_submit: kicked GP_PUT=1 doorbell token=0x%08x, polling sema...\n",
	    chan->gsp_token);

	/* Keep the same scheduler settle window, but avoid tick spam. */
	{
		uint64_t lr_t0 = (sc->gsp_logrm.kva != NULL)
		    ? *(volatile uint64_t *)sc->gsp_logrm.kva : 0;
		uint64_t lr = lr_t0;
		for (int t = 1; t <= 10; t++) {
			DELAY(100000);
			lr = (sc->gsp_logrm.kva != NULL)
			    ? *(volatile uint64_t *)sc->gsp_logrm.kva : 0;
		}
		device_printf(sc->dev,
		    "gsp_submit: LOGRM wait 1000ms put=0x%llx delta=%lld\n",
		    (unsigned long long)lr, (long long)(lr - lr_t0));
	}

		/* Give GSP its own RISC-V time to react before we start polling.
	 * Any PBDMA fault / RC trigger from this submit lands in logrm
	 * within milliseconds. We snapshot here so we know whether
	 * doorbell ever produced any observable side-effect. */
	DELAY(100000);
	uint64_t logrm_put_post = (sc->gsp_logrm.kva != NULL)
	    ? *(volatile uint64_t *)sc->gsp_logrm.kva : 0;
#ifdef NVKM_DEBUG_SUBMIT_VRAM_SCAN
	/* Non-nouveau diagnostic: scans a large protected/high VRAM range and
	 * can pollute LOGRM with host BAR2 region faults. Keep it off unless
	 * specifically chasing RAMFC placement.
	 */
	{
		uint64_t needle = chan->userd_vram;
		uint64_t needle_pte = nvkm_pte_to_vram(needle);
		device_printf(sc->dev,
		    "gsp_rm: SCAN looking for userd_vram=0x%llx (or PTE 0x%llx) in VRAM\n",
		    (unsigned long long)needle, (unsigned long long)needle_pte);
		uint64_t scan_start = 0x2b4070000ULL;
		uint64_t scan_end   = 0x2c0000000ULL;
		uint64_t needle2 = chan->inst_vram;
		uint64_t needle2_pte = nvkm_pte_to_vram(needle2);
		device_printf(sc->dev,
		    "gsp_rm: SCAN also looking for inst_vram=0x%llx (PTE 0x%llx) in 0x%llx..0x%llx\n",
		    (unsigned long long)needle2, (unsigned long long)needle2_pte,
		    (unsigned long long)scan_start, (unsigned long long)scan_end);
		int found = 0;
		lwkt_gettoken(&sc->gsp_tok);
		uint32_t saved = nvkm_rd32(sc, NV_PBUS_PRAMIN);
		for (uint64_t p = scan_start; p < scan_end && found < 10; p += 0x10000) {
			
			nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(p >> 16));
			for (uint32_t off = 0; off < 0x10000 && found < 10; off += 8) {
				uint32_t lo = nvkm_rd32(sc, NV_PRAMIN + off);
				uint32_t hi = nvkm_rd32(sc, NV_PRAMIN + off + 4);
				uint64_t val = ((uint64_t)hi << 32) | lo;
				if (val == needle || val == needle_pte || val == needle2 || val == needle2_pte) {
					device_printf(sc->dev,
					    "gsp_rm: SCAN FOUND at 0x%llx (val=0x%016llx)\n",
					    (unsigned long long)(p + off),
					    (unsigned long long)val);
					found++;
				}
			}
		}
		nvkm_wr32(sc, NV_PBUS_PRAMIN, saved);
		lwkt_reltoken(&sc->gsp_tok);
		device_printf(sc->dev, "gsp_rm: SCAN total occurrences: %d\n", found);

		/* Dump 256 bytes around where we found userd_vram. */
		{
			uint64_t dump_base = 0x2b6b90000ULL;
			lwkt_gettoken(&sc->gsp_tok);
			uint32_t s2 = nvkm_rd32(sc, NV_PBUS_PRAMIN);
			nvkm_wr32(sc, NV_PBUS_PRAMIN, (uint32_t)(dump_base >> 16));
			for (uint32_t off = 0; off < 0x100; off += 16) {
				uint32_t w0 = nvkm_rd32(sc, NV_PRAMIN + off + 0);
				uint32_t w1 = nvkm_rd32(sc, NV_PRAMIN + off + 4);
				uint32_t w2 = nvkm_rd32(sc, NV_PRAMIN + off + 8);
				uint32_t w3 = nvkm_rd32(sc, NV_PRAMIN + off + 12);
				device_printf(sc->dev,
				    "gsp_rm: DUMP@0x%llx: %08x %08x %08x %08x\n",
				    (unsigned long long)(dump_base + off), w0, w1, w2, w3);
			}
			nvkm_wr32(sc, NV_PBUS_PRAMIN, s2);
			lwkt_reltoken(&sc->gsp_tok);
		}
	}
#endif

		/* PRAMIN-read USERD slot to verify GP_PUT actually landed in
	 * chan->userd_vram (independent of BAR1 path). */
	{
		uint64_t userd_slot_paddr = chan->userd_vram + (uint64_t)chan->chid * 0x200;
		uint64_t pramin_gp_put = 0, pramin_gp_get = 0;
		(void)nvkm_gsp_pramin_rd64(sc, userd_slot_paddr + 0x88, &pramin_gp_get);
		(void)nvkm_gsp_pramin_rd64(sc, userd_slot_paddr + 0x8c, &pramin_gp_put);
		device_printf(sc->dev,
		    "gsp_submit: PRAMIN USERD@0x%llx (chid %d slot): GP_GET=0x%08x GP_PUT=0x%08x\n",
		    (unsigned long long)userd_slot_paddr, chan->chid,
		    (uint32_t)(pramin_gp_get & 0xffffffffu),
		    (uint32_t)(pramin_gp_put & 0xffffffffu));
	}

		device_printf(sc->dev,
	    "gsp_submit: post-doorbell 100ms snapshot: GP_GET=0x%08x "
	    "GP_PUT=0x%08x sema=0x%08x LOGRM put=0x%llx (delta=%lld)\n",
	    nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_GET),
	    nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_PUT),
	    sema_w[0],
	    (unsigned long long)logrm_put_post,
	    (long long)(logrm_put_post - logrm_put_pre));

#ifdef NVKM_DEBUG_SUBMIT_HW
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
#endif

	uint32_t last_get = 0xffffffffu;
	for (ms = 0; ms < SUBMIT_POLL_MS; ms += SUBMIT_POLL_STEP_MS) {
		(void)nvkm_gsp_msg_dispatch_all(sc);
		cpu_lfence();
		uint32_t sema_val = sema_w[0]; if (sema_val == SEM_PAYLOAD) {
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
			    ms, cur_get, sema_w[0]);
			last_get = cur_get;
		}
		DELAY(SUBMIT_POLL_STEP_MS * 1000);
	}
	device_printf(sc->dev,
	    "gsp_submit: SEM TIMEOUT %d ms, sema=0x%08x, last GP_GET=0x%08x\n",
	    SUBMIT_POLL_MS, sema_w[0], last_get);

	/* Query GSP for channel state via NV20_SUBDEVICE_DIAG class. */
	{
		struct nvkm_gsp_object diag = {0};
		void *sargs;
		sargs = nvkm_gsp_rm_alloc_get(&sc->gsp_vmm->device.subdevice, 0xd1a00000u,
		    0x0000208fu /* NV20_SUBDEVICE_DIAG */, 0, &diag);
		if (false) {
			device_printf(sc->dev, "DIAG: alloc_get failed\n");
		} else {
			int derr = nvkm_gsp_rm_alloc_wr(&diag, sargs);
			device_printf(sc->dev, "DIAG: alloc err=%d handle=0x%x\n", derr, diag.handle);
			if (derr == 0) {
				/* NV208F_CTRL_CMD_FIFO_GET_CHANNEL_STATE = 0x208f0403 */
				struct {
					uint32_t hChannel;
					uint32_t hClient;
					uint8_t  bBound;
					uint8_t  bEnabled;
					uint8_t  bScheduled;
					uint8_t  bCpuMap;
					uint8_t  bContention;
					uint8_t  bRunlistSet;
					uint8_t  bDeferRC;
				} *cs;
				cs = nvkm_gsp_rm_ctrl_get(&diag, 0x208f0403u, sizeof(*cs));
				if (cs != NULL) {
					cs->hChannel = chan->object.handle;
					cs->hClient = sc->gsp_vmm->client.object.handle;
					int cerr = nvkm_gsp_rm_ctrl_rd(&diag, (void**)&cs, sizeof(*cs));
					if (cerr == 0) {
						device_printf(sc->dev,
						    "DIAG CHANNEL_STATE chid=%d: bBound=%d bEnabled=%d bScheduled=%d "
						    "bCpuMap=%d bContention=%d bRunlistSet=%d bDeferRC=%d\n",
						    chan->chid, cs->bBound, cs->bEnabled, cs->bScheduled,
						    cs->bCpuMap, cs->bContention, cs->bRunlistSet, cs->bDeferRC);
					} else {
						device_printf(sc->dev, "DIAG GET_CHANNEL_STATE err=%d\n", cerr);
					}
				}
				nvkm_gsp_rm_free(&diag);
			}
		}
	}

	/* Probe channel scheduling state by issuing STOP_CHANNEL.
	 * If channel was scheduled and running, STOP preempts it (visible).
	 * If never scheduled, STOP returns NV_ERR_INVALID_STATE. */
	{
		struct { uint8_t bImmediate; } *sp;
		sp = nvkm_gsp_rm_ctrl_get(&chan->object,
		    /* NVA06F_CTRL_CMD_STOP_CHANNEL */ 0xa06f0112u,
		    sizeof(*sp));
		if (sp != NULL) {
			sp->bImmediate = 1;
			int sperr = nvkm_gsp_rm_ctrl_wr(&chan->object, sp);
			device_printf(sc->dev, "STOP_CHANNEL bImmediate=1 err=%d\n", sperr);
		} else {
			device_printf(sc->dev, "STOP_CHANNEL ctrl_get failed\n");
		}
	}

	/* After STOP, re-SCHEDULE the channel + bump GP_PUT to 2 + doorbell again. */
	{
		struct {
			uint8_t bEnable;
			uint8_t bSkipSubmit;
		} *sched;
		sched = nvkm_gsp_rm_ctrl_get(&chan->object, 0xa06f0103u, sizeof(*sched));
		if (sched) {
			sched->bEnable = 1;
			sched->bSkipSubmit = 0;
			int serr = nvkm_gsp_rm_ctrl_wr(&chan->object, sched);
			device_printf(sc->dev, "RE-SCHEDULE err=%d\n", serr);
			if (serr == 0) {
				/* Bump GP_PUT to 2 and write doorbell again */
				nvkm_gsp_bar1_wr32(sc, slot_bar1 + NV_USERD_GP_PUT, 2);
				nvkm_gsp_bar1_flush(sc);
				uint64_t lr0 = (sc->gsp_logrm.kva) ? *(volatile uint64_t*)sc->gsp_logrm.kva : 0;
				nvkm_wr32(sc, NV_USERMODE_DOORBELL, chan->gsp_token);  /* use GSP-provided token */
				DELAY(200000);
				uint64_t lr1 = (sc->gsp_logrm.kva) ? *(volatile uint64_t*)sc->gsp_logrm.kva : 0;
				uint32_t gp_get = nvkm_gsp_bar1_rd32(sc, slot_bar1 + NV_USERD_GP_GET);
				uint32_t sema_val = sema_w[0];
				device_printf(sc->dev,
				    "RE-DOORBELL after STOP+SCHED+GP_PUT=2: GP_GET=0x%08x sema=0x%08x LOGRM delta=%lld\n",
				    gp_get, sema_val, (long long)(lr1 - lr0));
			}
		}
	}

	/* Dump LOGRM + LOGINIT bytes to see GSP-side activity. */
	for (int which = 0; which < 2; which++) {
		void *kva = which ? (void *)sc->gsp_loginit.kva : (void *)sc->gsp_logrm.kva;
		const char *name = which ? "LOGINIT" : "LOGRM";
		if (kva == NULL) continue;
		uint8_t *lr = (uint8_t *)kva;
		uint64_t lr_put = *(volatile uint64_t *)lr;
		device_printf(sc->dev, "GSP %s put=0x%llx dump 0..0x180:\n",
		    name, (unsigned long long)lr_put);
		for (uint32_t o = 0; o < 0x180; o += 16) {
			device_printf(sc->dev,
			    "  %s[0x%03x]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    name, o,
			    lr[o+0], lr[o+1], lr[o+2], lr[o+3], lr[o+4], lr[o+5], lr[o+6], lr[o+7],
			    lr[o+8], lr[o+9], lr[o+10], lr[o+11], lr[o+12], lr[o+13], lr[o+14], lr[o+15]);
		}
	}
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
#ifdef NVKM_DEBUG_FIFO_TABLE
		device_printf(sc->dev,
		    "gsp_rm: fifo entry[%u] name=%.16s rm_type=%u runlist=%u\n",
		    i, p->entries[i].engineName, rmtype, runl);
#endif
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
	/* PT pages are VRAM bump allocations -- no host free; sysmem data pages contigfree. */
	nvkm_gsp_bar1_free_page(sc, &chan->submit_pd0);
	nvkm_gsp_bar1_free_page(sc, &chan->submit_spt);
	if (chan->submit_push.kva != NULL)
		contigfree(chan->submit_push.kva, 0x1000, M_NVKM_MTHDBUF);
	if (chan->submit_gpf.kva != NULL)
		contigfree(chan->submit_gpf.kva, 0x1000, M_NVKM_MTHDBUF);
	if (chan->submit_sema.kva != NULL)
		contigfree(chan->submit_sema.kva, 0x1000, M_NVKM_MTHDBUF);
	for (uint32_t i = 0; i < chan->gr_ctxbuf_nr; i++) {
		if (chan->gr_ctxbuf[i].kva != NULL)
			contigfree(chan->gr_ctxbuf[i].kva,
			    chan->gr_ctxbuf[i].size, M_NVKM_MTHDBUF);
	}
	if (chan->mthdbuf_kva != NULL) {
		contigfree(chan->mthdbuf_kva, chan->mthdbuf_size,
		    M_NVKM_MTHDBUF);
		chan->mthdbuf_kva = NULL;
	}
	return (err);
}

static int
nvkm_gsp_gr_ctxbuf_map(uint32_t engine_id, uint32_t *buffer_id,
    uint8_t *global, uint8_t *init, uint8_t *ro, uint8_t *nonmapped)
{
	*global = 1;
	*init = 0;
	*ro = 0;
	*nonmapped = 0;

	switch (engine_id) {
	case NV0080_ENGINE_ID_GRAPHICS:
		*buffer_id = NV2080_CTXBUF_ID_MAIN;
		*global = 0;
		*init = 1;
		return (0);
	case NV0080_ENGINE_ID_GRAPHICS_PATCH:
		*buffer_id = NV2080_CTXBUF_ID_PATCH;
		*global = 0;
		*init = 1;
		return (0);
	case NV0080_ENGINE_ID_GRAPHICS_BUNDLE_CB:
		*buffer_id = NV2080_CTXBUF_ID_BUFFER_BUNDLE_CB;
		return (0);
	case NV0080_ENGINE_ID_GRAPHICS_PAGEPOOL_GLOBAL:
		*buffer_id = NV2080_CTXBUF_ID_PAGEPOOL;
		return (0);
	case NV0080_ENGINE_ID_GRAPHICS_ATTRIBUTE_CB:
		*buffer_id = NV2080_CTXBUF_ID_ATTRIBUTE_CB;
		return (0);
	case NV0080_ENGINE_ID_GRAPHICS_RTV_CB_GLOBAL:
		*buffer_id = NV2080_CTXBUF_ID_RTV_CB_GLOBAL;
		return (0);
	case NV0080_ENGINE_ID_GRAPHICS_FECS_EVENT:
		*buffer_id = NV2080_CTXBUF_ID_FECS_EVENT;
		*init = 1;
		return (0);
	case NV0080_ENGINE_ID_GRAPHICS_PRIV_ACCESS_MAP:
		*buffer_id = NV2080_CTXBUF_ID_PRIV_ACCESS_MAP;
		*init = 1;
		*ro = 1;
		*nonmapped = 1;
		return (0);
	default:
		return (ENOENT);
	}
}

static void
nvkm_gsp_zero_vram(struct nvkm_softc *sc, uint64_t paddr, uint64_t size)
{
	static uint64_t scratch_gva;

	if (scratch_gva == 0) {
		scratch_gva = sc->bar1.next_gva;
		sc->bar1.next_gva += NVKM_GMMU_PT_PAGE_SIZE;
	}

	for (uint64_t off = 0; off < size; off += NVKM_GMMU_PT_PAGE_SIZE) {
		(void)nvkm_gsp_bar1_map_vram(sc, scratch_gva, paddr + off);
		for (uint32_t i = 0; i < NVKM_GMMU_PT_PAGE_SIZE; i += 4)
			nvkm_gsp_bar1_wr32(sc, scratch_gva + i, 0);
	}
	nvkm_gsp_bar1_flush(sc);
	nvkm_gsp_bar1_invalidate(sc);
}

static struct nvkm_gsp_gr_ctxbuf *
nvkm_gsp_gr_global_ctxbuf(struct nvkm_softc *sc, uint32_t buffer_id)
{
	for (uint32_t i = 0; i < sc->gr_ctxbuf_nr; i++) {
		if (sc->gr_ctxbuf_mem[i].buffer_id == buffer_id)
			return (&sc->gr_ctxbuf_mem[i]);
	}
	return (NULL);
}

static int
nvkm_gsp_gr_save_global_ctxbuf(struct nvkm_softc *sc,
    const struct nvkm_gsp_gr_ctxbuf *buf)
{
	if (sc->gr_ctxbuf_mem == NULL) {
		sc->gr_ctxbuf_mem = kzalloc(sizeof(*sc->gr_ctxbuf_mem) *
		    NVKM_GSP_GR_MAX_CTXBUFS, GFP_KERNEL);
		if (sc->gr_ctxbuf_mem == NULL)
			return (ENOMEM);
	}
	if (sc->gr_ctxbuf_nr >= NVKM_GSP_GR_MAX_CTXBUFS)
		return (ENOSPC);

	sc->gr_ctxbuf_mem[sc->gr_ctxbuf_nr++] = *buf;
	return (0);
}

int
nvkm_gsp_chan_promote_gr_ctx(struct nvkm_gsp_vmm *vmm,
    struct nvkm_gsp_chan *chan, uint8_t golden)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_client tmp_client;
	struct nvkm_gsp_object tmp_subdev;
	struct NV2080_CTRL_INTERNAL_STATIC_GR_GET_CONTEXT_BUFFERS_INFO_PARAMS_dfly *info;
	struct NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS_dfly *ctrl;
	void *q;
	uint64_t next_gva;
	int err;

	if (chan->gr_ctx_promoted)
		return (0);

	memset(&tmp_client, 0, sizeof(tmp_client));
	tmp_client.sc = sc;
	tmp_client.object.client = &tmp_client;
	tmp_client.object.handle = sc->gsp_internal_client;
	tmp_subdev.client = &tmp_client;
	tmp_subdev.parent = NULL;
	tmp_subdev.handle = sc->gsp_internal_subdevice;

	info = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
	    NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO,
	    sizeof(*info));
	if (info == NULL)
		return (ENOMEM);
	q = info;
	err = nvkm_gsp_rm_ctrl_rd(&tmp_subdev, &q, sizeof(*info));
	if (err != 0 || q == NULL)
		return (err ? err : EIO);
	info = q;

	{
		struct NV2080_CTRL_GR_GET_ZCULL_INFO_PARAMS_dfly *zcull;
		zcull = nvkm_gsp_rm_ctrl_get(&tmp_subdev,
		    NV2080_CTRL_CMD_GR_GET_ZCULL_INFO, sizeof(*zcull));
		if (zcull == NULL) {
			nvkm_gsp_rm_ctrl_done(&tmp_subdev, info);
			return (ENOMEM);
		}
		q = zcull;
		err = nvkm_gsp_rm_ctrl_rd(&tmp_subdev, &q, sizeof(*zcull));
		if (err != 0 || q == NULL) {
			nvkm_gsp_rm_ctrl_done(&tmp_subdev, info);
			return (err ? err : EIO);
		}
		zcull = q;
		device_printf(sc->dev,
		    "gsp_rm: GR zcull widthAlign=%u heightAlign=%u "
		    "subregions=%u err=0\n",
		    zcull->widthAlignPixels, zcull->heightAlignPixels,
		    zcull->subregionCount);
		nvkm_gsp_rm_ctrl_done(&tmp_subdev, zcull);
	}

	/*
	 * Nouveau creates the GR golden VMM with start=0x1000 and no fixed
	 * upper limit, then lets nvkm_vmm_get_locked() choose the first aligned
	 * hole for each ctxbuf.  Keep the same low-VA allocation pattern here;
	 * these addresses live in the GR golden VMM, not in the submit-test
	 * client VA window.
	 */
	next_gva = 0x1000ULL;

	ctrl = nvkm_gsp_rm_ctrl_get(&vmm->device.subdevice,
	   NV2080_CTRL_CMD_GPU_PROMOTE_CTX, sizeof(*ctrl));
	if (ctrl == NULL) {
		nvkm_gsp_rm_ctrl_done(&tmp_subdev, info);
		return (ENOMEM);
	}
	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->engineType = NV2080_ENGINE_TYPE_GRAPHICS;
	ctrl->hChanClient = vmm->client.object.handle;
	ctrl->hObject = chan->object.handle;
	if (golden && sc->gr_ctxbuf_mem != NULL)
		sc->gr_ctxbuf_nr = 0;

	for (uint32_t i = 0;
	    i < NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_COUNT;
	    i++) {
		struct NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_BUFFER_INFO_dfly *bi =
		    &info->engineContextBuffersInfo[0].engine[i];
		struct NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ENTRY_dfly *e;
		struct nvkm_gsp_gr_ctxbuf *buf;
		uint32_t buffer_id;
		uint64_t size, alloc_size, entry_size, mem_align, gva_align;
		uint32_t page_shift, gva_align_shift;
		uint8_t global, init, ro, nonmapped, target;
		uint8_t alloc;
		uint8_t entry_nonmapped;

		if (bi->size == 0)
			continue;
		if (nvkm_gsp_gr_ctxbuf_map(i, &buffer_id, &global, &init,
		    &ro, &nonmapped) != 0)
			continue;
		if (ctrl->entryCount >= NV2080_CTRL_GPU_PROMOTE_CONTEXT_MAX_ENTRIES ||
		    chan->gr_ctxbuf_nr >= NVKM_GSP_GR_MAX_CTXBUFS) {
			err = ENOSPC;
			goto out_done;
		}

		size = bi->size;
		if (buffer_id == NV2080_CTXBUF_ID_MAIN)
			size = NVKM_ALIGN_UP(size, 0x1000) + 64 * 0x1000;
		entry_size = size;

		if (size >= (1ULL << 21))
			page_shift = 21;
		else if (size >= (1ULL << 16))
			page_shift = 16;
		else
			page_shift = 12;

		if (buffer_id == NV2080_CTXBUF_ID_ATTRIBUTE_CB)
			gva_align_shift = nvkm_order_base_2_u64(size);
		else
			gva_align_shift = page_shift;

		mem_align = 1ULL << page_shift;
		gva_align = 1ULL << gva_align_shift;
		alloc_size = NVKM_ALIGN_UP(size, mem_align);
		next_gva = NVKM_ALIGN_UP(next_gva, gva_align);
		target = init ? NVKM_GSP_GR_CTXBUF_TARGET_INST :
		    NVKM_GSP_GR_CTXBUF_TARGET_INST_SR_LOST;
		alloc = golden || !global;
		entry_nonmapped = nonmapped && alloc;

		if (!alloc &&
		    buffer_id == NV2080_CTXBUF_ID_UNRESTRICTED_PRIV_ACCESS_MAP)
			continue;

		buf = &chan->gr_ctxbuf[chan->gr_ctxbuf_nr];
		if (alloc) {
			buf->paddr = nvkm_gsp_vram_alloc(sc, alloc_size,
			    mem_align);
			if (buf->paddr == 0) {
				err = ENOMEM;
				goto out_done;
			}
		} else {
			struct nvkm_gsp_gr_ctxbuf *global_buf;

			global_buf = nvkm_gsp_gr_global_ctxbuf(sc, buffer_id);
			if (global_buf == NULL) {
				device_printf(sc->dev,
				    "gsp_rm: missing global ctxbuf id=%u\n",
				    buffer_id);
				err = ENOENT;
				goto out_done;
			}
			buf->paddr = global_buf->paddr;
			alloc_size = global_buf->size;
		}
		buf->size = alloc_size;
		buf->gva = next_gva;
		buf->buffer_id = buffer_id;
		buf->target = target;
		buf->init = init;
		buf->ro = ro;
		buf->nonmapped = entry_nonmapped;
		chan->gr_ctxbuf_nr++;

		if (init && alloc)
			nvkm_gsp_zero_vram(sc, buf->paddr, buf->size);
		if (!entry_nonmapped) {
			err = nvkm_gsp_vmm_map_vram_flags(vmm, buf->gva,
			    buf->paddr, buf->size, 1, ro);
			if (err != 0)
				goto out_done;
		}
		if (golden && global) {
			err = nvkm_gsp_gr_save_global_ctxbuf(sc, buf);
			if (err != 0)
				goto out_done;
		}

		e = &ctrl->promoteEntry[ctrl->entryCount++];
		e->gpuVirtAddr = entry_nonmapped ? 0 : buf->gva;
		e->bufferId = (uint16_t)buffer_id;
		e->bInitialize = init && alloc;
		e->bNonmapped = entry_nonmapped;
		if (e->bInitialize) {
			e->gpuPhysAddr = buf->paddr;
			e->size = entry_size;
			e->physAttr = 4;
		}
		device_printf(sc->dev,
		    "gsp_rm: gr promote ctxbuf id=%u eng=%u entry=0x%llx "
		    "alloc=0x%llx pa=0x%llx va=0x%llx global=%u init=%u "
		    "ro=%u target=%u nm=%u\n",
		    buffer_id, i, (unsigned long long)e->size,
		    (unsigned long long)buf->size,
		    (unsigned long long)e->gpuPhysAddr,
		    (unsigned long long)e->gpuVirtAddr, global, init, ro,
		    target, entry_nonmapped);
		next_gva += buf->size;

		/* nouveau r535_gr_get_ctxbuf_info() duplicates PRIV_ACCESS_MAP
		 * as UNRESTRICTED_PRIV_ACCESS_MAP.  The first one is nonmapped;
		 * the unrestricted copy is separately allocated and mapped. */
		if (buffer_id == NV2080_CTXBUF_ID_PRIV_ACCESS_MAP) {
			if (ctrl->entryCount >=
			    NV2080_CTRL_GPU_PROMOTE_CONTEXT_MAX_ENTRIES ||
			    chan->gr_ctxbuf_nr >= NVKM_GSP_GR_MAX_CTXBUFS) {
				err = ENOSPC;
				goto out_done;
			}

			next_gva = NVKM_ALIGN_UP(next_gva, gva_align);
			buf = &chan->gr_ctxbuf[chan->gr_ctxbuf_nr];
			if (alloc) {
				buf->paddr = nvkm_gsp_vram_alloc(sc, alloc_size,
				    mem_align);
				if (buf->paddr == 0) {
					err = ENOMEM;
					goto out_done;
				}
			} else {
				struct nvkm_gsp_gr_ctxbuf *global_buf;

				global_buf = nvkm_gsp_gr_global_ctxbuf(sc,
				    NV2080_CTXBUF_ID_UNRESTRICTED_PRIV_ACCESS_MAP);
				if (global_buf == NULL) {
					device_printf(sc->dev,
					    "gsp_rm: missing global ctxbuf id=%u\n",
					    NV2080_CTXBUF_ID_UNRESTRICTED_PRIV_ACCESS_MAP);
					err = ENOENT;
					goto out_done;
				}
				buf->paddr = global_buf->paddr;
				alloc_size = global_buf->size;
			}
			buf->size = alloc_size;
			buf->gva = next_gva;
			buf->buffer_id =
			    NV2080_CTXBUF_ID_UNRESTRICTED_PRIV_ACCESS_MAP;
			buf->target = target;
			buf->init = init;
			buf->ro = ro;
			buf->nonmapped = 0;
			chan->gr_ctxbuf_nr++;

			if (init && alloc)
				nvkm_gsp_zero_vram(sc, buf->paddr, buf->size);
			err = nvkm_gsp_vmm_map_vram_flags(vmm, buf->gva,
			    buf->paddr, buf->size, 1, ro);
			if (err != 0)
				goto out_done;
			if (golden && global) {
				err = nvkm_gsp_gr_save_global_ctxbuf(sc, buf);
				if (err != 0)
					goto out_done;
			}

			e = &ctrl->promoteEntry[ctrl->entryCount++];
			e->gpuVirtAddr = buf->gva;
			e->bufferId =
			    NV2080_CTXBUF_ID_UNRESTRICTED_PRIV_ACCESS_MAP;
			e->bInitialize = init && alloc;
			e->bNonmapped = 0;
			if (e->bInitialize) {
				e->gpuPhysAddr = buf->paddr;
				e->size = entry_size;
				e->physAttr = 4;
			}
			device_printf(sc->dev,
			    "gsp_rm: gr promote ctxbuf id=%u eng=%u entry=0x%llx "
			    "alloc=0x%llx pa=0x%llx va=0x%llx global=%u "
			    "init=%u ro=%u target=%u nm=%u\n",
			    e->bufferId, i, (unsigned long long)e->size,
			    (unsigned long long)buf->size,
			    (unsigned long long)e->gpuPhysAddr,
			    (unsigned long long)e->gpuVirtAddr, global, init,
			    ro, target, 0);
			next_gva += buf->size;
		}
	}

	uint32_t entry_count = ctrl->entryCount;
	err = nvkm_gsp_rm_ctrl_wr(&vmm->device.subdevice, ctrl);
	device_printf(sc->dev,
	    "gsp_rm: GPU_PROMOTE_CTX chan=0x%x chid=%d golden=%u entries=%u err=%d\n",
	    chan->object.handle, chan->chid, golden, entry_count, err);
	if (err == 0)
		chan->gr_ctx_promoted = 1;
out_done:
	nvkm_gsp_rm_ctrl_done(&tmp_subdev, info);
	return (err);
}

int
nvkm_gsp_gr_oneinit(struct nvkm_gsp_vmm *vmm)
{
	struct nvkm_softc *sc = vmm->sc;
	struct nvkm_gsp_vmm *golden_vmm;
	struct nvkm_gsp_chan *golden;
	struct nvkm_gsp_object threed;
	int err;
	static int done;

	if (done)
		return (0);

	golden_vmm = kzalloc(sizeof(*golden_vmm), GFP_KERNEL);
	if (golden_vmm == NULL)
		return (ENOMEM);

	golden = kzalloc(sizeof(*golden), GFP_KERNEL);
	if (golden == NULL) {
		kfree(golden_vmm);
		return (ENOMEM);
	}

	device_printf(sc->dev,
	    "gsp_rm: GR oneinit: golden channel begin\n");

	err = nvkm_gsp_vmm_ctor(sc, 0xc1d00002, golden_vmm);
	if (err != 0)
		goto out_free;

	err = nvkm_gsp_golden_chan_ctor(golden_vmm, golden);
	if (err != 0)
		goto out_vmm;

	err = nvkm_gsp_chan_promote_gr_ctx(golden_vmm, golden, 1);
	if (err != 0)
		goto out_chan;

	memset(&threed, 0, sizeof(threed));
	err = nvkm_gsp_chan_alloc_obj(golden, 0x97000000u, 0x0000c597u,
	    &threed);
	if (err != 0)
		goto out_chan;

	(void)nvkm_gsp_rm_free(&threed);
	done = 1;
	device_printf(sc->dev,
	    "gsp_rm: GR oneinit: golden channel complete\n");

out_chan:
	(void)nvkm_gsp_chan_dtor(golden);
out_vmm:
	nvkm_gsp_vmm_dtor(golden_vmm);
out_free:
	kfree(golden);
	kfree(golden_vmm);
	return (err);
}

int
nvkm_gsp_chan_alloc_obj(struct nvkm_gsp_chan *chan, uint32_t handle,
    uint32_t oclass, struct nvkm_gsp_object *obj)
{
	struct nvkm_softc *sc = chan->object.client->sc;
	int err;

	memset(obj, 0, sizeof(*obj));

	switch (oclass) {
	case 0x0000c5b5: { /* TURING_DMA_COPY_A */
		struct {
			uint32_t version;
			uint32_t engineType;
		} *args;

		args = nvkm_gsp_rm_alloc_get(&chan->object, handle, oclass,
		    sizeof(*args), obj);
		if (args == NULL)
			return (ENOMEM);
		args->version = 1;
		args->engineType = NV2080_ENGINE_TYPE_COPY0;
		err = nvkm_gsp_rm_alloc_wr(obj, args);
		break;
	}
	case 0x0000c597: /* TURING_A */
	case 0x0000c5c0: /* TURING_COMPUTE_A */
	case 0x0000902d: /* FERMI_TWOD_A */
	case 0x0000a140: /* KEPLER_INLINE_TO_MEMORY_B */
	{
		void *args;

		args = nvkm_gsp_rm_alloc_get(&chan->object, handle, oclass, 0,
		    obj);
		if (args == NULL)
			return (ENOMEM);
		err = nvkm_gsp_rm_alloc_wr(obj, args);
		break;
	}
	default:
		return (EINVAL);
	}

	device_printf(sc->dev,
	    "gsp_rm: channel obj alloc cls=0x%x handle=0x%x chan=0x%x err=%d\n",
	    oclass, handle, chan->object.handle, err);
	if (err != 0)
		memset(obj, 0, sizeof(*obj));
	return (err);
}
