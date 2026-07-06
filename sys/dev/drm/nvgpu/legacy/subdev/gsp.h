/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Local DragonFly shim for linux-v7.0 nouveau MIT display code.
 * Do not copy Linux include/nvkm/subdev/gsp.h here; it has no explicit
 * per-file license marker in the v7.0 tree.
 */
#ifndef _DFLY_NVKM_SUBDEV_GSP_H_
#define _DFLY_NVKM_SUBDEV_GSP_H_

#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"

#include <core/subdev.h>
#include <linux/err.h>

struct nvkm_rm;
struct nvkm_gsp_mem;
struct nvkm_gsp_radix3;
struct nvkm_gsp_event;

struct nvkm_gsp {
	struct nvkm_subdev subdev;
	struct nvkm_softc *sc;
	struct nvkm_rm *rm;
	struct {
		struct nvkm_gsp_device device;
	} internal;
};

static __inline bool
nvkm_gsp_rm(struct nvkm_gsp *gsp)
{
	return (gsp != NULL);
}

typedef void (*nvkm_gsp_event_func)(struct nvkm_gsp_event *, void *repv,
    u32 repc);

static __inline int
nvkm_gsp_dfly_linux_errno(int ret)
{
	return (ret > 0 ? -ret : ret);
}

static __inline void *
nvkm_gsp_dfly_err_ptr(int ret)
{
	if (ret == 0)
		ret = EIO;
	return (ERR_PTR(nvkm_gsp_dfly_linux_errno(ret)));
}

struct nvkm_gsp_event {
	struct nvkm_gsp_device *device;
	u32 id;
	nvkm_gsp_event_func func;
	struct nvkm_gsp_object object;
	struct list_head head;
};

static __inline int
nvkm_gsp_device_event_ctor(struct nvkm_gsp_device *device, u32 handle, u32 id,
    nvkm_gsp_event_func func, struct nvkm_gsp_event *event)
{
	memset(event, 0, sizeof(*event));
	event->device = device;
	event->id = id;
	event->func = func;
	event->object.client = device->object.client;
	event->object.parent = &device->subdevice;
	event->object.handle = handle;
	INIT_LIST_HEAD(&event->head);
	return (0);
}

static __inline void
nvkm_gsp_event_dtor(struct nvkm_gsp_event *event)
{
	memset(event, 0, sizeof(*event));
}

static __inline int
nvkm_gsp_client_device_ctor(struct nvkm_gsp *gsp, struct nvkm_gsp_client *client,
    struct nvkm_gsp_device *device)
{
	int ret;

	if (gsp == NULL || gsp->sc == NULL) {
		memset(client, 0, sizeof(*client));
		memset(device, 0, sizeof(*device));
		client->gsp = gsp;
		client->object.client = client;
		device->object.client = client;
		device->subdevice.client = client;
		return (0);
	}

	ret = nvkm_gsp_client_ctor(gsp->sc, 0xc1d00073u, client);
	if (ret != 0)
		return (nvkm_gsp_dfly_linux_errno(ret));
	client->gsp = gsp;
	ret = nvkm_gsp_device_ctor(client, device);
	if (ret != 0)
		(void)nvkm_gsp_client_dtor(client);
	return (nvkm_gsp_dfly_linux_errno(ret));
}

static __inline int
nvkm_gsp_intr_stall(struct nvkm_gsp *gsp, enum nvkm_subdev_type type, int inst)
{
	(void)gsp;
	(void)type;
	(void)inst;
	return (0);
}

static __inline int
nvkm_gsp_rm_alloc_wr_import(struct nvkm_gsp_object *object, void *params)
{
	return (nvkm_gsp_dfly_linux_errno(nvkm_gsp_rm_alloc_wr(object, params)));
}

static __inline void *
nvkm_gsp_rm_alloc_get_import(struct nvkm_gsp_object *parent, u32 handle,
    u32 oclass, u32 argc, struct nvkm_gsp_object *object)
{
	void *params;

	params = nvkm_gsp_rm_alloc_get(parent, handle, oclass, argc, object);
	if (params == NULL)
		return (ERR_PTR(-ENOMEM));
	return (params);
}

static __inline int
nvkm_gsp_rm_ctrl_wr_import(struct nvkm_gsp_object *object, void *params)
{
	return (nvkm_gsp_dfly_linux_errno(nvkm_gsp_rm_ctrl_wr(object, params)));
}

static __inline void *
nvkm_gsp_rm_ctrl_get_import(struct nvkm_gsp_object *object, u32 cmd, u32 argc)
{
	void *params;

	params = nvkm_gsp_rm_ctrl_get(object, cmd, argc);
	if (params == NULL)
		return (ERR_PTR(-ENOMEM));
	return (params);
}

/*
 * Imported nouveau GSP display code expects Linux helper semantics:
 * pointer helpers return ERR_PTR() on failure, and integer helpers return
 * negative errno.  The native DragonFly RM helpers keep their local
 * NULL/positive-errno contract, so this shim translates at the import edge.
 */
static __inline int
nvkm_gsp_rm_alloc(struct nvkm_gsp_object *parent, u32 handle, u32 oclass,
    u32 argc, struct nvkm_gsp_object *object)
{
	void *params;

	params = nvkm_gsp_rm_alloc_get_import(parent, handle, oclass, argc,
	    object);
	if (IS_ERR(params))
		return (PTR_ERR(params));
	return (nvkm_gsp_rm_alloc_wr_import(object, params));
}

static __inline int
nvkm_gsp_rm_ctrl_push_ptr(struct nvkm_gsp_object *object, void **params, u32 repc)
{
	return (nvkm_gsp_dfly_linux_errno(nvkm_gsp_rm_ctrl_rd(object, params,
	    repc)));
}

static __inline void *
nvkm_gsp_rm_ctrl_rd_ptr(struct nvkm_gsp_object *object, u32 cmd, u32 repc)
{
	void *params;
	int ret;

	params = nvkm_gsp_rm_ctrl_get_import(object, cmd, repc);
	if (IS_ERR(params))
		return (params);

	ret = nvkm_gsp_rm_ctrl_push_ptr(object, &params, repc);
	if (ret != 0)
		return (ERR_PTR(ret));
	if (params == NULL)
		return (nvkm_gsp_dfly_err_ptr(EIO));
	return (params);
}

#define nvkm_gsp_rm_alloc_get(parent, handle, oclass, argc, object) \
	nvkm_gsp_rm_alloc_get_import((parent), (handle), (oclass), (argc), \
	    (object))
#define nvkm_gsp_rm_alloc_wr(object, params) \
	nvkm_gsp_rm_alloc_wr_import((object), (params))
#define nvkm_gsp_rm_ctrl_get(object, cmd, argc) \
	nvkm_gsp_rm_ctrl_get_import((object), (cmd), (argc))
#define nvkm_gsp_rm_ctrl_wr(object, params) \
	nvkm_gsp_rm_ctrl_wr_import((object), (params))
#define nvkm_gsp_rm_ctrl_push(object, params, repc) \
	nvkm_gsp_rm_ctrl_push_ptr((object), (void **)(params), (repc))
#define nvkm_gsp_rm_ctrl_rd(object, cmd, repc) \
	nvkm_gsp_rm_ctrl_rd_ptr((object), (cmd), (repc))

#endif
