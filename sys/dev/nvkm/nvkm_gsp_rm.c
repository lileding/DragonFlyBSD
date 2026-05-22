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
