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
#include "head.h"
#include "wndw.h"

#include <core/memory.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <nouveau_bo.h>
#include <nvif/class.h>
#include <nvif/if0014.h>
#include <nvhw/class/clc37e.h>
#include <nvhw/class/clc57e.h>

#define NV50_DISP_HANDLE_SYNCBUF	0xf0000000U
#define NV50_DISP_HANDLE_VRAM		0xf0000001U
#define NVKM_DISPNV50_PUSH_DWORDS	(0x1000U / 4U)
#define NVKM_DISPNV50_SCANOUT_BPP	4U

struct nvkm_dispnv50_state {
	struct nv50_disp disp;
	struct nvif_disp ifdisp;
	struct nouveau_bo sync_bo;
	struct nv50_head head[4];
	struct nv50_wndw *wndw[8];
	struct nvkm_memory *scanout;
	u32 scanout_width;
	u32 scanout_height;
	u32 scanout_pitch;
	bool core_ready;
};

static u32
nvkm_dispnv50_align_u32(u32 value, u32 align)
{
	return ((value + align - 1) & ~(align - 1));
}

static int
nvkm_dispnv50_neg_errno(int err)
{
	if (err > 0)
		return -err;
	return err;
}

static u32
nvkm_dispnv50_pattern_pixel(u32 x, u32 y, u32 width, u32 height)
{
	u32 bar = width >= 4 ? width / 4 : 1;

	if (width > 64 && height > 64 &&
	    (y < 32 || x < 32 || x >= width - 32 || y >= height - 32))
		return 0x00ffffff;
	if (x < bar)
		return 0x00ff3030;
	if (x < bar * 2)
		return 0x0030ff30;
	if (x < bar * 3)
		return 0x003030ff;
	return ((x ^ y) & 0x20) ? 0x00d0d0d0 : 0x00404040;
}

static int
nvkm_dispnv50_fill_scanout(struct nvkm_softc *sc, struct nvkm_memory *memory,
    u32 width, u32 height, u32 pitch)
{
	const u64 base = nvkm_memory_addr(memory);
	const u64 size = (u64)pitch * height;
	u64 done = 0;

	while (done < size) {
		uint64_t gva;
		u32 chunk;
		int err;

		err = nvkm_gsp_bar1_map_existing(sc, base + done, &gva);
		if (err != 0)
			return nvkm_dispnv50_neg_errno(err);

		chunk = (size - done) > PAGE_SIZE ? PAGE_SIZE : (u32)(size - done);
		for (u32 off = 0; off < chunk; off += 4) {
			u64 pos = done + off;
			u32 line = (u32)(pos / pitch);
			u32 line_off = (u32)(pos - (u64)line * pitch);
			u32 pixel = 0;

			if (line < height && line_off < width * NVKM_DISPNV50_SCANOUT_BPP)
				pixel = nvkm_dispnv50_pattern_pixel(line_off / 4,
				    line, width, height);
			nvkm_gsp_bar1_wr32(sc, gva + off, pixel);
		}

		nvkm_gsp_bar1_unmap_existing(sc, gva);
		done += chunk;
	}

	nvkm_gsp_bar1_flush(sc);
	return 0;
}

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
	int inst = head;
	u32 user;
	int ret;

	(void)syncbuf;

	if (drm == NULL || drm->dev == NULL || oclass == NULL || dmac == NULL)
		return -EINVAL;

	disp = nv50_disp(drm->dev);
	if (disp == NULL || disp->dfly_sc == NULL)
		return -ENODEV;
	sc = disp->dfly_sc;

	if (args != NULL && argc == sizeof(struct nvif_disp_chan_v0)) {
		struct nvif_disp_chan_v0 *chan_args = args;

		if (chan_args->version != 0)
			return -ENOSYS;
		inst = chan_args->id;
	}

	ret = nvkm_dispnv50_user_offset(oclass[0], inst, &user);
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

	ret = nvkm_gsp_disp_channel_pushbuf(sc, oclass[0], inst,
	    dmac->dfly_push_mem);
	if (ret)
		goto fail;

	ret = nvkm_gsp_disp_dmac_alloc(sc, oclass[0], inst, 0,
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
	    "drm: dispnv50 dmac class=0x%x inst=%d push=0x%llx user=0x%x\n",
	    oclass[0], inst,
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
nv50_wndw_new_(const struct nv50_wndw_func *func, struct drm_device *dev,
    enum drm_plane_type type, const char *name, int index,
    const u32 *format, u32 heads, enum nv50_disp_interlock_type interlock_type,
    u32 interlock_data, struct nv50_wndw **pwndw)
{
	struct nv50_wndw *wndw;

	(void)dev;
	(void)type;
	(void)name;
	(void)format;
	(void)heads;
	(void)interlock_type;

	if (func == NULL || pwndw == NULL || index < 0)
		return -EINVAL;

	wndw = kzalloc(sizeof(*wndw), GFP_KERNEL);
	if (wndw == NULL)
		return -ENOMEM;

	wndw->func = func;
	wndw->id = index;
	wndw->interlock.data = interlock_data;
	*pwndw = wndw;
	return 0;
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

static int
nvkm_dispnv50_wndw_init(struct nvkm_softc *sc, uint32_t win)
{
	struct nvkm_dispnv50_state *state;
	struct nouveau_drm drm;
	int ret;

	if (win >= nitems(((struct nvkm_dispnv50_state *)0)->wndw))
		return -EINVAL;
	if (nvkm_dispnv50_core_init(sc) != 0)
		return -ENODEV;

	state = sc->dispnv50;
	if (state->wndw[win] != NULL)
		return 0;

	drm.dev = sc->drm_dev;
	ret = wndwc57e_new(&drm, DRM_PLANE_TYPE_PRIMARY, (int)win,
	    TU102_DISP_WINDOW_CHANNEL_DMA, &state->wndw[win]);
	if (ret) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 window channel alloc failed win=%u err=%d\n",
		    win, ret);
		return ret;
	}

	nvkm_infof(sc->dev, "drm: dispnv50 window channel staged win=%u\n",
	    win);
	return 0;
}

static int
nvkm_dispnv50_head_init(struct nvkm_softc *sc, uint32_t head)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_head *nvhead;

	if (head >= nitems(((struct nvkm_dispnv50_state *)0)->head))
		return -EINVAL;
	if (nvkm_dispnv50_core_init(sc) != 0)
		return -ENODEV;

	state = sc->dispnv50;
	nvhead = &state->head[head];
	if (nvhead->func != NULL)
		return 0;
	if (state->disp.core->func->head == NULL)
		return -ENODEV;

	nvhead->func = state->disp.core->func->head;
	nvhead->disp = &state->disp;
	nvhead->base.base.dev = sc->drm_dev;
	nvhead->base.index = (int)head;

	nvkm_infof(sc->dev, "drm: dispnv50 head staged head=%u\n", head);
	return 0;
}

static void
nvkm_dispnv50_head_atom_mode(struct nv50_head_atom *asyh,
    struct drm_display_mode *mode)
{
	struct nv50_head_mode *m = &asyh->mode;
	u32 blankus;

	drm_mode_set_crtcinfo(mode,
	    CRTC_INTERLACE_HALVE_V | CRTC_STEREO_DOUBLE);

	m->h.active = mode->crtc_htotal;
	m->h.synce = mode->crtc_hsync_end - mode->crtc_hsync_start - 1;
	m->h.blanke = mode->crtc_hblank_end - mode->crtc_hsync_start - 1;
	m->h.blanks = m->h.blanke + mode->crtc_hdisplay;

	m->v.active = mode->crtc_vtotal;
	m->v.synce = mode->crtc_vsync_end - mode->crtc_vsync_start - 1;
	m->v.blanke = mode->crtc_vblank_end - mode->crtc_vsync_start - 1;
	m->v.blanks = m->v.blanke + mode->crtc_vdisplay;

	blankus = (m->v.active - mode->crtc_vdisplay - 2) * m->h.active;
	blankus *= 1000;
	blankus /= mode->crtc_clock;
	m->v.blankus = blankus;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
		m->v.blank2e = m->v.active + m->v.blanke;
		m->v.blank2s = m->v.blank2e + mode->crtc_vdisplay;
		m->v.active = (m->v.active * 2) + 1;
		m->interlace = true;
	} else {
		m->v.blank2e = 0;
		m->v.blank2s = 1;
		m->interlace = false;
	}
	m->clock = mode->crtc_clock;

	asyh->or.depth = 0;
	asyh->or.crc_raster = 0;
	asyh->or.nhsync = !!(mode->flags & DRM_MODE_FLAG_NHSYNC);
	asyh->or.nvsync = !!(mode->flags & DRM_MODE_FLAG_NVSYNC);
	asyh->or.bpc = 8;
}

static void
nvkm_dispnv50_head_atom_fill(struct nv50_head_atom *asyh,
    struct drm_crtc_state *state)
{
	memset(asyh, 0, sizeof(*asyh));
	asyh->state.mode = state->mode;
	asyh->state.adjusted_mode = state->adjusted_mode;
	asyh->view.iW = state->mode.hdisplay;
	asyh->view.iH = state->mode.vdisplay;
	asyh->view.oW = state->adjusted_mode.hdisplay;
	asyh->view.oH = state->adjusted_mode.vdisplay;
	nvkm_dispnv50_head_atom_mode(asyh, &asyh->state.adjusted_mode);
}

static int
nvkm_dispnv50_scanout_ensure(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, u32 width, u32 height)
{
	u32 pitch = nvkm_dispnv50_align_u32(width * NVKM_DISPNV50_SCANOUT_BPP,
	    256);
	u64 size = (u64)pitch * height;
	int ret;

	if (state->scanout != NULL &&
	    state->scanout_width == width &&
	    state->scanout_height == height &&
	    state->scanout_pitch == pitch)
		return 0;

	nvkm_memory_unref(&state->scanout);
	ret = nvkm_memory_new(sc->core_device, NVKM_MEM_TARGET_VRAM, size,
	    0x1000, false, &state->scanout);
	if (ret != 0)
		return ret;

	ret = nvkm_dispnv50_fill_scanout(sc, state->scanout, width, height,
	    pitch);
	if (ret != 0) {
		nvkm_memory_unref(&state->scanout);
		return ret;
	}

	state->scanout_width = width;
	state->scanout_height = height;
	state->scanout_pitch = pitch;
	nvkm_infof(sc->dev,
	    "drm: dispnv50 scanout staged %ux%u pitch=%u vram=0x%llx\n",
	    width, height, pitch,
	    (unsigned long long)nvkm_memory_addr(state->scanout));
	return 0;
}

static void
nvkm_dispnv50_wndw_atom_fill(struct nv50_wndw_atom *asyw,
    struct drm_crtc *crtc, struct nvkm_dispnv50_state *state)
{
	u32 width = state->scanout_width;
	u32 height = state->scanout_height;

	memset(asyw, 0, sizeof(*asyw));
	asyw->state.crtc = crtc;
	asyw->state.crtc_x = 0;
	asyw->state.crtc_y = 0;
	asyw->state.crtc_w = width;
	asyw->state.crtc_h = height;
	asyw->state.src_x = 0;
	asyw->state.src_y = 0;
	asyw->state.src_w = width << 16;
	asyw->state.src_h = height << 16;

	asyw->image.interval = 1;
	asyw->image.mode = NVC57E_SET_PRESENT_CONTROL_BEGIN_MODE_NON_TEARING;
	asyw->image.w = width;
	asyw->image.h = height;
	asyw->image.blockh =
	    NVC57E_SET_STORAGE_BLOCK_HEIGHT_NVD_BLOCK_HEIGHT_ONE_GOB;
	asyw->image.layout = NVC57E_SET_STORAGE_MEMORY_LAYOUT_PITCH;
	asyw->image.format = NVC57E_SET_PARAMS_FORMAT_A8R8G8B8;
	asyw->image.blocks[0] = 0;
	asyw->image.pitch[0] = state->scanout_pitch;
	asyw->image.handle[0] = NV50_DISP_HANDLE_VRAM;
	asyw->image.offset[0] = nvkm_memory_addr(state->scanout);

	asyw->blend.depth = 255;
	asyw->blend.k1 = 255;
	asyw->blend.src_color =
	    NVC37E_SET_COMPOSITION_FACTOR_SELECT_SRC_COLOR_FACTOR_MATCH_SELECT_K1;
	asyw->blend.dst_color =
	    NVC37E_SET_COMPOSITION_FACTOR_SELECT_DST_COLOR_FACTOR_MATCH_SELECT_NEG_K1;
}

int
nvkm_dispnv50_atomic_enable(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, uint32_t win, uint32_t display_id)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_head_atom asyh;
	struct nv50_wndw_atom asyw;
	struct nv50_head *nvhead;
	struct nv50_wndw *wndw;
	struct nv50_core *core;
	struct drm_display_mode *mode;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	int ret;

	if (sc == NULL || crtc == NULL || crtc->state == NULL || sc->disp == NULL)
		return (-ENODEV);

	ret = nvkm_dispnv50_wndw_init(sc, win);
	if (ret != 0)
		return ret;
	ret = nvkm_dispnv50_head_init(sc, head);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	core = state->disp.core;
	nvhead = &state->head[head];
	wndw = state->wndw[win];
	mode = &crtc->state->adjusted_mode;

	ret = nvkm_dispnv50_scanout_ensure(sc, state, mode->hdisplay,
	    mode->vdisplay);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 scanout alloc failed head=%u win=%u err=%d\n",
		    head, win, ret);
		return ret;
	}

	nvkm_dispnv50_head_atom_fill(&asyh, crtc->state);
	if (nvhead->func->display_id != NULL) {
		ret = nvhead->func->display_id(nvhead, display_id);
		if (ret != 0)
			goto fail;
	}
	if (nvhead->func->mode != NULL) {
		ret = nvhead->func->mode(nvhead, &asyh);
		if (ret != 0)
			goto fail;
	}
	if (nvhead->func->procamp != NULL) {
		ret = nvhead->func->procamp(nvhead, &asyh);
		if (ret != 0)
			goto fail;
	}
	if (nvhead->func->or != NULL) {
		ret = nvhead->func->or(nvhead, &asyh);
		if (ret != 0)
			goto fail;
	}
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;

	if (core->assign_windows) {
		ret = core->func->wndw.owner(core);
		if (ret != 0)
			goto fail;
		ret = core->func->update(core, interlock, false);
		if (ret != 0)
			goto fail;
		core->assign_windows = false;
		memset(interlock, 0, sizeof(interlock));
	}

	nvkm_dispnv50_wndw_atom_fill(&asyw, crtc, state);
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	ret = wndw->func->image_set(wndw, &asyw);
	if (ret != 0)
		goto fail;
	ret = wndw->func->blend_set(wndw, &asyw);
	if (ret != 0)
		goto fail;
	interlock[NV50_DISP_INTERLOCK_WNDW] |= wndw->interlock.data;
	ret = wndw->func->update(wndw, interlock);
	if (ret != 0)
		goto fail;
	ret = core->func->update(core, interlock, false);
	if (ret != 0)
		goto fail;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 bridge armed head=%u win=%u display=0x%x "
	    "scanout=0x%llx\n",
	    head, win, display_id,
	    (unsigned long long)nvkm_memory_addr(state->scanout));
	return 0;

fail:
	nvkm_infof(sc->dev,
	    "drm: dispnv50 bridge failed head=%u win=%u display=0x%x err=%d\n",
	    head, win, display_id, ret);
	if (ret == 0)
		return (-ENODEV);
	return ret;
}
