/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM resource manager RPC helpers.
 *
 * Wraps NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC (103) and GSP_RM_CONTROL (76)
 * around the existing nvkm_gsp_rpc_* primitives. Envelope structs are
 * the v03_00 layout (stable from r535 through r570 — verified against
 * Linux nouveau r535/r570 sources and open-rm 570.144 headers).
 *
 * Per-class params structs MUST be sourced from r570/nvrm/ headers,
 * not r535 — several classes added fields between versions
 * (e.g. NV0000_ALLOC_PARAMETERS gained pOsPidInfo in r570).
 */

#ifndef _NVKM_GSP_RM_H_
#define _NVKM_GSP_RM_H_

#include "nvkm_priv.h"

/* GSP-RM RPC function numbers — Linux nouveau r570/nvrm/rpcfn.h. */
#define NV_VGPU_MSG_FUNCTION_FREE		10
#define NV_VGPU_MSG_FUNCTION_DUP_OBJECT		21
#define NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL	76
#define NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC	103

/* Stable envelope (v03_00) — same in r535 and r570. */
struct rpc_gsp_rm_alloc_v03_00 {
	uint32_t hClient;
	uint32_t hParent;
	uint32_t hObject;
	uint32_t hClass;
	uint32_t status;	/* OUT */
	uint32_t paramsSize;
	uint32_t flags;
	uint8_t  reserved[4];
	uint8_t  params[];
};

struct rpc_gsp_rm_control_v03_00 {
	uint32_t hClient;
	uint32_t hObject;
	uint32_t cmd;
	uint32_t status;	/* OUT */
	uint32_t paramsSize;
	uint32_t flags;
	uint8_t  params[];
};

struct NVOS00_PARAMETERS_v03_00 {
	uint32_t hRoot;
	uint32_t hObjectParent;
	uint32_t hObjectOld;
	int32_t  status;
};

struct rpc_free_v03_00 {
	struct NVOS00_PARAMETERS_v03_00 params;
};

/* Resource handle tracking. Mirrors nouveau's nvkm_gsp_client +
 * nvkm_gsp_object. A client is itself an object whose parent is itself
 * (the RM_ALLOC for NV01_ROOT uses the same handle for client/parent/obj). */
struct nvkm_gsp_object {
	struct nvkm_gsp_client	*client;
	struct nvkm_gsp_object	*parent;	/* NULL for client root */
	uint32_t		 handle;
};

struct nvkm_gsp_client {
	struct nvkm_gsp_object	 object;	/* embedded; .client = self */
	struct nvkm_softc	*sc;
};

/* === RM_ALLOC ===
 * Allocate a new RM resource under `parent`. `new_obj` is the caller's
 * uninitialised nvkm_gsp_object; we fill it in. Returned pointer is the
 * class-specific params region — caller fills it then calls _wr. */
void	*nvkm_gsp_rm_alloc_get(struct nvkm_gsp_object *parent,
	    uint32_t handle, uint32_t oclass, uint32_t params_size,
	    struct nvkm_gsp_object *new_obj);

/* Submit the prepared alloc params. Returns 0 on GSP-side success. */
int	 nvkm_gsp_rm_alloc_wr(struct nvkm_gsp_object *obj, void *params);

/* Discard the prepared buffer without sending (e.g. error path). */
void	 nvkm_gsp_rm_alloc_done(struct nvkm_gsp_object *obj, void *params);

/* Free an allocated RM resource. */
int	 nvkm_gsp_rm_free(struct nvkm_gsp_object *obj);

/* === RM_CONTROL ===
 * Prepare a control request against an existing object. */
void	*nvkm_gsp_rm_ctrl_get(struct nvkm_gsp_object *obj, uint32_t cmd,
	    uint32_t params_size);

/* Send + wait + return params with OUT fields filled (caller frees via _done). */
int	 nvkm_gsp_rm_ctrl_rd(struct nvkm_gsp_object *obj, void **params,
	    uint32_t repc);

/* Send + ignore reply (still RECV but discard). */
int	 nvkm_gsp_rm_ctrl_wr(struct nvkm_gsp_object *obj, void *params);

void	 nvkm_gsp_rm_ctrl_done(struct nvkm_gsp_object *obj, void *params);

/* === Convenience === */
int	 nvkm_gsp_client_ctor(struct nvkm_softc *sc, uint32_t handle,
	    struct nvkm_gsp_client *client);
int	 nvkm_gsp_client_dtor(struct nvkm_gsp_client *client);

/* Allocated device + subdevice tree under a client. */
struct nvkm_gsp_device {
	struct nvkm_gsp_object	object;		/* NV01_DEVICE_0 */
	struct nvkm_gsp_object	subdevice;	/* NV20_SUBDEVICE_0 */
};

int	 nvkm_gsp_device_ctor(struct nvkm_gsp_client *client,
	    struct nvkm_gsp_device *device);
int	 nvkm_gsp_device_dtor(struct nvkm_gsp_device *device);

/* Allocated VA space (FERMI_VASPACE_A) — server-managed PDE flavour
 * (external = false). Caller still has to call COPY_SERVER_RESERVED_PDES
 * to obtain the page-table copies before issuing map ops. */
struct nvkm_gsp_vaspace {
	struct nvkm_gsp_object	object;
};

int	 nvkm_gsp_vaspace_ctor(struct nvkm_gsp_device *device,
	    struct nvkm_gsp_vaspace *vas);
int	 nvkm_gsp_vaspace_dtor(struct nvkm_gsp_vaspace *vas);

/* VRAM bump allocator. The window is set in nvkm_gsp_vram_init using
 * a fixed safe carveout (TODO: parse GSP fbRegionInfoParams). All
 * allocations are PAGE_SIZE-aligned; alloc returns the physical VRAM
 * offset to hand to GSP-RM as NV_MEMORY_DESC_PARAMS.base. */
int	  nvkm_gsp_vram_init(struct nvkm_softc *sc);
uint64_t nvkm_gsp_vram_alloc(struct nvkm_softc *sc, uint64_t size,
	    uint64_t align);

/* RM_ENGINE_TYPE values used as channel/cgrp engineType field. */
#define NV2080_ENGINE_TYPE_COPY0	0x00000009U

/* Channel group (KEPLER_CHANNEL_GROUP_A / TSG). */
struct nvkm_gsp_chgrp {
	struct nvkm_gsp_object	object;
};

int	 nvkm_gsp_chgrp_ctor(struct nvkm_gsp_device *device,
	    struct nvkm_gsp_vaspace *vas, uint32_t engine_type,
	    struct nvkm_gsp_chgrp *grp);
int	 nvkm_gsp_chgrp_dtor(struct nvkm_gsp_chgrp *grp);

/* GPFIFO channel (TURING_CHANNEL_GPFIFO_A for our Turing target).
 * Owns the VRAM backing for inst/USERD and the sysmem mthdbuf. */
struct nvkm_gsp_chan {
	struct nvkm_gsp_object	object;
	uint64_t		inst_vram;	/* VRAM physical base */
	uint64_t		userd_vram;	/* VRAM physical base */
	void			*mthdbuf_kva;	/* sysmem kva (contigmalloc) */
	uint64_t		mthdbuf_paddr;	/* sysmem physical base */
};

int	 nvkm_gsp_chan_ctor(struct nvkm_gsp_device *device,
	    struct nvkm_gsp_vaspace *vas, struct nvkm_gsp_chgrp *chgrp,
	    uint32_t engine_type, struct nvkm_gsp_chan *chan);
int	 nvkm_gsp_chan_dtor(struct nvkm_gsp_chan *chan);

/* NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO — same struct as in static_info,
 * but cleaner to query via RM_CONTROL on the subdevice. */
#define NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO	0x20801320U
#define NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_MAX_ENTRIES	16U
#define NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_MEM_TYPES	17U

struct nvkm_fb_region_info {
	uint64_t base;
	uint64_t limit;
	uint64_t reserved;
	uint32_t performance;
	uint8_t  supportCompressed;
	uint8_t  supportISO;
	uint8_t  bProtected;
	uint8_t  blackList[17];
};

struct NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_PARAMS_r570 {
	uint32_t numFBRegions;
	uint8_t  _pad[4];
	struct nvkm_fb_region_info fbRegion[16];
};

#endif /* _NVKM_GSP_RM_H_ */
