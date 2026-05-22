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

#define FERMI_VASPACE_A			0x000090f1U
#define NVKM_RM_VASPACE			0x90f10000u
#define NV_VASPACE_ALLOCATION_INDEX_GPU_NEW	0x00U

struct NV_VASPACE_ALLOCATION_PARAMETERS_r535 {
	uint32_t index;
	int32_t  flags;
	uint64_t vaSize;
	uint64_t vaStartInternal;
	uint64_t vaLimitInternal;
	uint32_t bigPageSize;
	uint8_t  _pad[4];
	uint64_t vaBase;
};

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
	/* flags = 0 -> server-managed (kernel-owned) PDEs; nouveau's
	 * non-external path. We don't yet do COPY_SERVER_RESERVED_PDES
	 * — that goes in when we need to install our own mappings. */
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
	sc->vram_bump_base  = NVKM_VRAM_BUMP_BASE;
	sc->vram_bump_next  = NVKM_VRAM_BUMP_BASE;
	sc->vram_bump_limit = NVKM_VRAM_BUMP_BASE + NVKM_VRAM_BUMP_SIZE;
	device_printf(sc->dev,
	    "gsp_rm: VRAM bump alloc window 0x%llx..0x%llx\n",
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

	off = (sc->vram_bump_next + align - 1) & ~(align - 1);
	if (off + size > sc->vram_bump_limit) {
		device_printf(sc->dev,
		    "gsp_rm: VRAM bump alloc exhausted (need 0x%llx)\n",
		    (unsigned long long)size);
		return (0);
	}
	sc->vram_bump_next = off + size;
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
