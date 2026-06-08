/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local boundary for wiring DRM/KMS commits into imported nouveau
 * dispnv50 emitters.
 *
 * The replaced display path carried its own GSP display object, RAMIN,
 * core/window pushbuffers, and notifier ctxdma state.  That state is gone.
 * The active display owner is now the imported nouveau display engine at
 * sc->disp; the dispnv50 channel ABI still needs to be staged before this
 * bridge can program scanout.
 */
#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include "core.h"

#include <core/memory.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <nouveau_bo.h>
#include <nvif/class.h>

#define NV50_DISP_HANDLE_SYNCBUF	0xf0000000U
#define NV50_DISP_HANDLE_VRAM		0xf0000001U
#define NVKM_DISPNV50_PUSH_DWORDS	(0x1000U / 4U)

struct nvkm_dispnv50_state {
	struct nv50_disp disp;
	struct nvif_disp ifdisp;
	struct nouveau_bo sync_bo;
	bool core_ready;
};

static int
nvkm_dispnv50_user_offset(s32 oclass, int head, u32 *offset)
{
	switch (oclass & 0xff) {
	case 0x7d:
		*offset = 0x680000;
		return 0;
	case 0x7e:
		*offset = 0x690000 + head * 0x1000;
		return 0;
	case 0x7b:
		*offset = 0x6b0000 + head * 0x1000;
		return 0;
	case 0x7a:
		*offset = 0x6d8000 + head * 0x1000;
		return 0;
	default:
		return -EINVAL;
	}
}

static int
nvkm_dispnv50_dmac_wait(struct nvif_push *push, u32 size)
{
	struct nv50_dmac *dmac = container_of(push, struct nv50_dmac, push);
	u32 cur;

	if (dmac->dfly_shadow == NULL || dmac->dfly_push_mem == NULL)
		return -ENODEV;
	if (size > dmac->max)
		return -EINVAL;

	cur = (u32)(push->cur - dmac->dfly_shadow);
	if (cur + size >= dmac->max) {
		if (cur != dmac->put)
			push->kick(push);
		cur = 0;
		dmac->put = 0;
	}

	dmac->cur = cur;
	push->bgn = dmac->dfly_shadow + cur;
	push->cur = push->bgn;
	push->end = dmac->dfly_shadow + dmac->max;
	return 0;
}

static void
nvkm_dispnv50_dmac_kick(struct nvif_push *push)
{
	struct nv50_dmac *dmac = container_of(push, struct nv50_dmac, push);
	struct nvkm_softc *sc = dmac->dfly_sc;
	u32 cur;

	if (sc == NULL || dmac->dfly_push_mem == NULL ||
	    dmac->dfly_shadow == NULL)
		return;

	cur = (u32)(push->cur - dmac->dfly_shadow);
	for (u32 i = dmac->put; i < cur; i++)
		nvkm_wo32(dmac->dfly_push_mem, i * 4, dmac->dfly_shadow[i]);

	nvkm_gsp_bar1_flush(sc);
	nvkm_wr32(sc, dmac->dfly_user + 0x00, cur << 2);
	(void)nvkm_rd32(sc, dmac->dfly_user + 0x00);

	dmac->put = cur;
	dmac->cur = cur;
}

struct nv50_disp *
nv50_disp(struct drm_device *dev)
{
	struct nvkm_softc *sc;

	if (dev == NULL)
		return (NULL);
	sc = dev->dev_private;
	if (sc == NULL || sc->dispnv50 == NULL)
		return (NULL);
	return (&sc->dispnv50->disp);
}

int
nv50_dmac_create(struct nouveau_drm *drm, s32 *oclass, int head,
    void *args, u32 argc, s64 syncbuf, struct nv50_dmac *dmac)
{
	struct nv50_disp *disp;
	struct nvkm_softc *sc;
	u32 user;
	int ret;

	(void)args;
	(void)argc;
	(void)syncbuf;

	if (drm == NULL || drm->dev == NULL || oclass == NULL || dmac == NULL)
		return -EINVAL;

	disp = nv50_disp(drm->dev);
	if (disp == NULL || disp->dfly_sc == NULL)
		return -ENODEV;
	sc = disp->dfly_sc;

	ret = nvkm_dispnv50_user_offset(oclass[0], head, &user);
	if (ret)
		return ret;

	memset(dmac, 0, sizeof(*dmac));
	dmac->dfly_sc = sc;
	dmac->dfly_user = user;
	dmac->dfly_shadow = kzalloc(0x1000, GFP_KERNEL);
	dmac->dfly_object = kzalloc(sizeof(*dmac->dfly_object), GFP_KERNEL);
	if (dmac->dfly_shadow == NULL || dmac->dfly_object == NULL) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = nvkm_memory_new(sc->core_device, NVKM_MEM_TARGET_HOST, 0x1000,
	    0x1000, true, &dmac->dfly_push_mem);
	if (ret)
		goto fail;

	ret = nvkm_gsp_disp_channel_pushbuf(sc, oclass[0], head,
	    dmac->dfly_push_mem);
	if (ret)
		goto fail;

	ret = nvkm_gsp_disp_dmac_alloc(sc, oclass[0], head, 0,
	    dmac->dfly_object);
	if (ret)
		goto fail;

	dmac->push.wait = nvkm_dispnv50_dmac_wait;
	dmac->push.kick = nvkm_dispnv50_dmac_kick;
	dmac->push.mem.object.map.ptr = dmac->dfly_shadow;
	dmac->push.mem.addr = nvkm_memory_addr(dmac->dfly_push_mem);
	dmac->push.mem.size = nvkm_memory_size(dmac->dfly_push_mem);
	dmac->push.bgn = dmac->dfly_shadow;
	dmac->push.cur = dmac->push.bgn;
	dmac->push.end = dmac->push.bgn;
	dmac->max = NVKM_DISPNV50_PUSH_DWORDS - 1;
	dmac->sync.handle = 0;
	dmac->vram.handle = NV50_DISP_HANDLE_VRAM;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 dmac class=0x%x head=%d push=0x%llx user=0x%x\n",
	    oclass[0], head,
	    (unsigned long long)nvkm_memory_addr(dmac->dfly_push_mem), user);
	return 0;

fail:
	nvkm_memory_unref(&dmac->dfly_push_mem);
	kfree(dmac->dfly_object);
	dmac->dfly_object = NULL;
	kfree(dmac->dfly_shadow);
	dmac->dfly_shadow = NULL;
	return ret;
}

int
core507d_new_(const struct nv50_core_func *func, struct nouveau_drm *drm,
    s32 oclass, struct nv50_core **pcore)
{
	struct nv50_disp *disp;
	struct nv50_core *core;
	int ret;

	if (func == NULL || drm == NULL || pcore == NULL)
		return -EINVAL;

	disp = nv50_disp(drm->dev);
	if (disp == NULL)
		return -ENODEV;

	core = kzalloc(sizeof(*core), GFP_KERNEL);
	if (core == NULL)
		return -ENOMEM;

	core->func = func;
	core->disp = disp;

	ret = nv50_dmac_create(drm, &oclass, 0, NULL, 0, -1, &core->chan);
	if (ret) {
		kfree(core);
		*pcore = NULL;
		return ret;
	}

	*pcore = core;
	return 0;
}

static int
nvkm_dispnv50_core_init(struct nvkm_softc *sc)
{
	struct nvkm_dispnv50_state *state;
	struct nouveau_drm drm;
	int ret;

	if (sc->dispnv50 != NULL && sc->dispnv50->core_ready)
		return 0;

	if (sc->drm_dev == NULL || sc->disp == NULL || sc->core_device == NULL)
		return -ENODEV;

	state = sc->dispnv50;
	if (state == NULL) {
		state = kzalloc(sizeof(*state), GFP_KERNEL);
		if (state == NULL)
			return -ENOMEM;
		state->disp.dfly_sc = sc;
		state->disp.disp = &state->ifdisp;
		state->disp.sync = &state->sync_bo;
		sc->dispnv50 = state;
	}

	drm.dev = sc->drm_dev;
	ret = corec57d_new(&drm, TU102_DISP_CORE_CHANNEL_DMA,
	    &state->disp.core);
	if (ret) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 core channel alloc failed %d\n", ret);
		return ret;
	}

	ret = state->disp.core->func->init(state->disp.core);
	if (ret) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 core init failed %d\n", ret);
		return ret;
	}

	state->core_ready = true;
	nvkm_infof(sc->dev, "drm: dispnv50 core channel staged\n");
	return 0;
}

int
nvkm_dispnv50_atomic_enable(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, uint32_t win, uint32_t display_id)
{
	if (sc == NULL || crtc == NULL || crtc->state == NULL || sc->disp == NULL)
		return (-ENODEV);

	if (nvkm_dispnv50_core_init(sc) != 0)
		return (-ENODEV);

	nvkm_infof(sc->dev,
	    "drm: dispnv50 bridge deferred: window/head ABI not staged "
	    "head=%u win=%u display=0x%x\n",
	    head, win, display_id);
	return (-ENODEV);
}
