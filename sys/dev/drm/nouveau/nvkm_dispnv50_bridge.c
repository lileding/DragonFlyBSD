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

#include <machine/framebuffer.h>
#include <sys/callout.h>

#include <core/gpuobj.h>
#include <core/memory.h>
#include <core/object.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <engine/disp/outp.h>
#include <engine/disp/ior.h>
#include <nouveau_bo.h>
#include <nvif/cl0002.h>
#include <nvif/class.h>
#include <nvif/if0014.h>
#include <nvhw/class/clc37d.h>
#include <nvhw/class/clc37e.h>
#include <nvhw/class/clc57e.h>
#include <subdev/bios/dcb.h>

#ifdef nvkm_rd32
#undef nvkm_rd32
#endif
#ifdef nvkm_wr32
#undef nvkm_wr32
#endif

#define NV50_DISP_HANDLE_SYNCBUF	0xf0000000U
#define NV50_DISP_HANDLE_VRAM		0xf0000001U
#define NV50_DISP_HANDLE_WNDW_CTX(kind)	(0xfb000000U | (kind))
#define NVKM_DISPNV50_PUSH_DWORDS	(0x1000U / 4U)
#define NVKM_DISPNV50_SCANOUT_BPP	4U
#define NVKM_DISPNV50_SCANOUT_KIND	0U
#define NVKM_DISPNV50_DMAOBJ_VRAM_RW_SP	0x00000045U
#define NVKM_DISPNV50_DMAOBJ_VRAM_RW_LP	0x00000005U
#define NVKM_DISPNV50_STATUS_POLL_COUNT	50U
#define NVKM_DISPNV50_STATUS_POLL_US	1000U
#define NVKM_DISPNV50_ILUT_ENTRIES	1024U
#define NVKM_DISPNV50_ILUT_VSS_ENTRIES	4U
#define NVKM_DISPNV50_ILUT_TOTAL_ENTRIES \
	(NVKM_DISPNV50_ILUT_VSS_ENTRIES + NVKM_DISPNV50_ILUT_ENTRIES + 1U)
#define NVKM_DISPNV50_ILUT_BYTES \
	(NVKM_DISPNV50_ILUT_TOTAL_ENTRIES * 8U)
#define NVKM_DISPNV50_CONSOLE_FLUSH_DIV	4U

struct nvkm_dispnv50_state {
	struct nv50_disp disp;
	struct nvif_disp ifdisp;
	struct nouveau_bo sync_bo;
	struct nvkm_memory *sync_mem;
	struct nv50_head head[4];
	struct nv50_wndw *wndw[8];
	struct nvkm_memory *ilut;
	u64 ilut_offset;
	struct nvkm_memory *olut;
	u64 olut_offset;
	struct nvkm_memory *scanout;
	u64 scanout_offset;
	u32 scanout_width;
	u32 scanout_height;
	u32 scanout_pitch;
	struct fb_info console_fb;
	void *console_shadow;
	u64 console_shadow_size;
	struct callout console_flush_callout;
	bool console_callout_ready;
	bool console_flush_active;
	bool console_fb_registered;
	u64 console_flush_count;
	u64 console_flush_error_count;
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

static int
nvkm_dispnv50_vram_offset(struct nvkm_softc *sc, u64 paddr, u64 size,
    u64 *offset)
{
	u64 base;
	u64 off;

	if (sc == NULL || offset == NULL || size == 0)
		return -EINVAL;
	if (sc->fb_usable_size == 0 ||
	    sc->fb_usable_base + sc->fb_usable_size <= sc->fb_usable_base)
		return -ENODEV;

	base = sc->fb_usable_base;
	if (paddr < base)
		return -EINVAL;

	off = paddr - base;
	if (off >= sc->fb_usable_size || size > sc->fb_usable_size - off)
		return -EINVAL;

	*offset = off;
	return 0;
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
nvkm_dispnv50_read_scanout_pixel(struct nvkm_softc *sc,
    struct nvkm_memory *memory, u32 pitch, u32 x, u32 y, u32 *pixel)
{
	u64 byte = (u64)y * pitch + (u64)x * NVKM_DISPNV50_SCANOUT_BPP;
	u64 page = (nvkm_memory_addr(memory) + byte) & ~(u64)(PAGE_SIZE - 1);
	u64 page_off = (nvkm_memory_addr(memory) + byte) - page;
	uint64_t gva;
	int err;

	err = nvkm_gsp_bar1_map_existing(sc, page, &gva);
	if (err != 0)
		return nvkm_dispnv50_neg_errno(err);

	*pixel = nvkm_gsp_bar1_rd32(sc, gva + page_off);
	nvkm_gsp_bar1_unmap_existing(sc, gva);
	return 0;
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
	if (width != 0 && height != 0) {
		u32 p0 = 0, p1 = 0, p2 = 0, p3 = 0;
		int e0, e1, e2, e3;

		e0 = nvkm_dispnv50_read_scanout_pixel(sc, memory, pitch, 0, 0, &p0);
		e1 = nvkm_dispnv50_read_scanout_pixel(sc, memory, pitch, width / 4,
		    height / 2, &p1);
		e2 = nvkm_dispnv50_read_scanout_pixel(sc, memory, pitch, width / 2,
		    height / 2, &p2);
		e3 = nvkm_dispnv50_read_scanout_pixel(sc, memory, pitch,
		    (width * 3) / 4, height / 2, &p3);
		nvkm_infof(sc->dev,
		    "drm: dispnv50 scanout pattern readback err=%d/%d/%d/%d "
		    "p00=0x%08x p25=0x%08x p50=0x%08x p75=0x%08x\n",
		    e0, e1, e2, e3, p0, p1, p2, p3);
	}
	return 0;
}

static int
nvkm_dispnv50_copy_shadow_to_scanout(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state)
{
	const u8 *src;
	u64 base;
	u64 size;
	u64 done = 0;

	if (sc == NULL || state == NULL || state->scanout == NULL ||
	    state->console_shadow == NULL)
		return -ENODEV;

	base = nvkm_memory_addr(state->scanout);
	size = (u64)state->scanout_pitch * state->scanout_height;
	if (state->console_shadow_size < size)
		return -ENODEV;

	src = state->console_shadow;
	while (done < size) {
		uint64_t gva;
		u32 chunk;
		int err;

		err = nvkm_gsp_bar1_map_existing(sc, base + done, &gva);
		if (err != 0)
			return nvkm_dispnv50_neg_errno(err);

		chunk = (size - done) > PAGE_SIZE ? PAGE_SIZE :
		    (u32)(size - done);
		for (u32 off = 0; off < chunk; off += 4) {
			u32 pixel;

			memcpy(&pixel, src + done + off, sizeof(pixel));
			nvkm_gsp_bar1_wr32(sc, gva + off, pixel);
		}

		nvkm_gsp_bar1_unmap_existing(sc, gva);
		done += chunk;
	}

	nvkm_gsp_bar1_flush(sc);
	return 0;
}

static void
nvkm_dispnv50_console_flush(void *arg)
{
	struct nvkm_dispnv50_state *state = arg;
	struct nvkm_softc *sc = state != NULL ? state->disp.dfly_sc : NULL;
	int ret;
	int ticks;

	if (state == NULL || !state->console_flush_active)
		return;

	ret = nvkm_dispnv50_copy_shadow_to_scanout(sc, state);
	state->console_flush_count++;
	if (ret != 0) {
		state->console_flush_error_count++;
		if (sc != NULL && state->console_flush_error_count <= 4)
			nvkm_infof(sc->dev,
			    "drm: dispnv50 console flush failed err=%d "
			    "count=%llu\n", ret,
			    (unsigned long long)state->console_flush_error_count);
	}

	ticks = hz / NVKM_DISPNV50_CONSOLE_FLUSH_DIV;
	if (ticks < 1)
		ticks = 1;
	callout_reset(&state->console_flush_callout, ticks,
	    nvkm_dispnv50_console_flush, state);
}

static int
nvkm_dispnv50_console_start_flush(struct nvkm_dispnv50_state *state)
{
	int ticks;

	if (!state->console_callout_ready) {
		callout_init_mp(&state->console_flush_callout);
		state->console_callout_ready = true;
	}

	if (state->console_flush_active)
		return 0;

	ticks = hz / NVKM_DISPNV50_CONSOLE_FLUSH_DIV;
	if (ticks < 1)
		ticks = 1;
	state->console_flush_active = true;
	callout_reset(&state->console_flush_callout, ticks,
	    nvkm_dispnv50_console_flush, state);
	return 0;
}

static void
nvkm_dispnv50_console_stop_flush(struct nvkm_dispnv50_state *state)
{
	if (state == NULL || !state->console_callout_ready)
		return;

	state->console_flush_active = false;
	callout_drain(&state->console_flush_callout);
}

static void
nvkm_dispnv50_console_unregister(struct nvkm_dispnv50_state *state)
{
	if (state == NULL)
		return;

	nvkm_dispnv50_console_stop_flush(state);
	if (state->console_fb_registered) {
		unregister_framebuffer(&state->console_fb);
		state->console_fb_registered = false;
	}
	if (state->console_shadow != NULL) {
		kfree(state->console_shadow);
		state->console_shadow = NULL;
	}
	state->console_shadow_size = 0;
	memset(&state->console_fb, 0, sizeof(state->console_fb));
}

static int
nvkm_dispnv50_console_register(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state)
{
	u64 size;
	int ret;

	if (state == NULL || state->scanout == NULL ||
	    state->scanout_width == 0 || state->scanout_height == 0 ||
	    state->scanout_pitch == 0)
		return -ENODEV;

	size = (u64)state->scanout_pitch * state->scanout_height;
	if (state->console_fb_registered &&
	    state->console_fb.width == state->scanout_width &&
	    state->console_fb.height == state->scanout_height &&
	    state->console_fb.stride == state->scanout_pitch &&
	    state->console_shadow_size >= size)
		return nvkm_dispnv50_console_start_flush(state);

	nvkm_dispnv50_console_unregister(state);

	state->console_shadow = kzalloc(size, GFP_KERNEL);
	if (state->console_shadow == NULL)
		return -ENOMEM;
	state->console_shadow_size = size;

	memset(&state->console_fb, 0, sizeof(state->console_fb));
	state->console_fb.vaddr = (vm_offset_t)state->console_shadow;
	state->console_fb.paddr = vtophys(state->console_shadow);
	state->console_fb.width = state->scanout_width;
	state->console_fb.height = state->scanout_height;
	state->console_fb.stride = state->scanout_pitch;
	state->console_fb.depth = 32;
	state->console_fb.is_vga_boot_display = 0;
	state->console_fb.par = state;
	state->console_fb.device = sc->dev;

	ret = register_framebuffer(&state->console_fb);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 console fb register failed err=%d\n", ret);
		nvkm_dispnv50_console_unregister(state);
		return ret;
	}
	state->console_fb_registered = true;

	/*
	 * syscons renders into this CPU shadow buffer.  Until we grow dirty
	 * tracking or a stable linear BAR1 mapping, periodically mirror it to
	 * the pitch-linear VRAM scanout BO programmed above.
	 */
	ret = nvkm_dispnv50_copy_shadow_to_scanout(sc, state);
	if (ret != 0)
		nvkm_infof(sc->dev,
		    "drm: dispnv50 console initial flush failed err=%d\n", ret);
	(void)nvkm_dispnv50_console_start_flush(state);

	nvkm_infof(sc->dev,
	    "drm: dispnv50 console fb registered %ux%u pitch=%u "
	    "shadow=%p size=0x%llx\n",
	    state->scanout_width, state->scanout_height, state->scanout_pitch,
	    state->console_shadow, (unsigned long long)state->console_shadow_size);
	return 0;
}

struct nvkm_dispnv50_dmaobj {
	struct nvkm_object object;
	struct nvkm_softc *sc;
	u64 start;
	u64 limit;
	u32 flags0;
	int ramht_cookie;
};

static int
nvkm_dispnv50_dmaobj_bind(struct nvkm_object *object,
    struct nvkm_gpuobj *parent, int align, struct nvkm_gpuobj **pgpuobj)
{
	struct nvkm_dispnv50_dmaobj *dmaobj =
	    container_of(object, struct nvkm_dispnv50_dmaobj, object);
	u64 start = dmaobj->start >> 8;
	u64 limit = dmaobj->limit >> 8;
	int ret;

	ret = nvkm_gpuobj_new(NULL, 24, align, false, parent, pgpuobj);
	if (ret != 0)
		return ret;

	nvkm_kmap(*pgpuobj);
	nvkm_wo32(*pgpuobj, 0x00, dmaobj->flags0);
	nvkm_wo32(*pgpuobj, 0x04, (u32)start);
	nvkm_wo32(*pgpuobj, 0x08, (u32)(start >> 32));
	nvkm_wo32(*pgpuobj, 0x0c, (u32)limit);
	nvkm_wo32(*pgpuobj, 0x10, (u32)(limit >> 32));
	nvkm_done(*pgpuobj);
	return 0;
}

static const struct nvkm_object_func nvkm_dispnv50_dmaobj_func = {
	.bind = nvkm_dispnv50_dmaobj_bind,
};

static void
nvkm_dispnv50_ctxdma_drop(struct nvkm_dispnv50_dmaobj **pobject)
{
	if (pobject == NULL || *pobject == NULL)
		return;

	if ((*pobject)->ramht_cookie > 0)
		nvkm_gsp_disp_dmac_unbind((*pobject)->sc,
		    (*pobject)->ramht_cookie);
	kfree(*pobject);
	*pobject = NULL;
}

static int
nvkm_dispnv50_ctxdma_new(struct nv50_dmac *dmac, s32 oclass, int inst,
    const char *name, u32 handle, u64 start, u64 limit,
    u32 flags0, struct nvif_object *object,
    struct nvkm_dispnv50_dmaobj **pobject)
{
	struct nvkm_dispnv50_dmaobj *dmaobj;
	int ret;

	if (dmac == NULL || object == NULL || pobject == NULL ||
	    limit < start)
		return -EINVAL;

	dmaobj = kzalloc(sizeof(*dmaobj), GFP_KERNEL);
	if (dmaobj == NULL)
		return -ENOMEM;

	dmaobj->object.func = &nvkm_dispnv50_dmaobj_func;
	dmaobj->sc = dmac->dfly_sc;
	dmaobj->start = start;
	dmaobj->limit = limit;
	dmaobj->flags0 = flags0;

	ret = nvkm_gsp_disp_dmac_bind(dmac->dfly_sc, oclass, inst,
	    &dmaobj->object, handle);
	if (ret < 0) {
		kfree(dmaobj);
		return ret;
	}
	dmaobj->ramht_cookie = ret;

	object->name = name;
	object->handle = handle;
	object->oclass = NV_DMA_IN_MEMORY;
	object->priv = dmaobj;
	*pobject = dmaobj;
	return 0;
}

static int
nvkm_dispnv50_sync_ensure(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state)
{
	u64 relative;
	int ret;

	state->sync_bo.sc = sc;
	if (state->sync_mem != NULL)
		return 0;

	ret = nvkm_memory_new(sc->core_device, NVKM_MEM_TARGET_VRAM, 0x1000,
	    0x1000, false, &state->sync_mem);
	if (ret != 0)
		return ret;

	ret = nvkm_dispnv50_vram_offset(sc, nvkm_memory_addr(state->sync_mem),
	    nvkm_memory_size(state->sync_mem), &relative);
	if (ret != 0) {
		nvkm_memory_unref(&state->sync_mem);
		return ret;
	}

	state->sync_bo.offset = nvkm_memory_addr(state->sync_mem);
	nvkm_infof(sc->dev,
	    "drm: dispnv50 sync buffer staged vram=0x%llx offset=0x%llx "
	    "relative=0x%llx\n",
	    (unsigned long long)nvkm_memory_addr(state->sync_mem),
	    (unsigned long long)state->sync_bo.offset,
	    (unsigned long long)relative);
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

static const char *
nvkm_dispnv50_dmac_label(struct nv50_dmac *dmac)
{
	switch (dmac->dfly_oclass & 0xff) {
	case 0x7d:
		return "core";
	case 0x7e:
		return "wndw";
	default:
		return "chan";
	}
}

static void
nvkm_dispnv50_dmac_trace_push(struct nvkm_softc *sc, struct nv50_dmac *dmac,
    u32 put, u32 cur)
{
	const char *label = nvkm_dispnv50_dmac_label(dmac);
	u32 remaining = 0;
	u32 method = 0;

	if ((dmac->dfly_oclass & 0xff) != 0x7d &&
	    (dmac->dfly_oclass & 0xff) != 0x7e)
		return;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 %s push class=0x%x inst=%d put=%u cur=%u\n",
	    label, dmac->dfly_oclass, dmac->dfly_inst, put, cur);

	for (u32 i = put; i < cur; i++) {
		u32 data = dmac->dfly_shadow[i];

		if (remaining != 0) {
			nvkm_infof(sc->dev,
			    "drm: dispnv50 %s push[%03u] method=0x%04x data=0x%08x\n",
			    label, i, method, data);
			method += 4;
			remaining--;
			continue;
		}

		if ((data >> 29) == 0 && ((data >> 18) & 0x3ff) != 0) {
			method = data & 0x3ffc;
			remaining = (data >> 18) & 0x3ff;
			nvkm_infof(sc->dev,
			    "drm: dispnv50 %s push[%03u] hdr method=0x%04x count=%u raw=0x%08x\n",
			    label, i, method, remaining, data);
		} else {
			nvkm_infof(sc->dev,
			    "drm: dispnv50 %s push[%03u] raw=0x%08x\n",
			    label, i, data);
		}
	}
}

static bool
nvkm_dispnv50_dmac_read_status(struct nvkm_softc *sc, struct nv50_dmac *dmac,
    u32 *user_put, u32 *ctrl, u32 *stat)
{
	switch (dmac->dfly_oclass & 0xff) {
	case 0x7d:
		*user_put = nvkm_rd32(sc, dmac->dfly_user + 0x00);
		*ctrl = nvkm_rd32(sc, 0x6104e0);
		*stat = nvkm_rd32(sc, 0x610630);
		return true;
	case 0x7e: {
		u32 channel = 1 + dmac->dfly_inst;

		*user_put = nvkm_rd32(sc, dmac->dfly_user + 0x00);
		*ctrl = nvkm_rd32(sc, 0x6104e0 + channel * 4);
		*stat = nvkm_rd32(sc, 0x610664 + (channel - 1) * 4);
		return true;
	}
	default:
		return false;
	}
}

static bool
nvkm_dispnv50_dmac_status_idle(struct nv50_dmac *dmac, u32 stat)
{
	switch (dmac->dfly_oclass & 0xff) {
	case 0x7d:
		return ((stat & 0x001f0000) == 0x000b0000);
	case 0x7e:
		return ((stat & 0x000f0000) == 0x00040000);
	default:
		return false;
	}
}

static void
nvkm_dispnv50_dmac_trace_status(struct nvkm_softc *sc, struct nv50_dmac *dmac,
    u32 cur)
{
	const char *label = nvkm_dispnv50_dmac_label(dmac);
	u32 user_put = 0;
	u32 ctrl = 0;
	u32 stat = 0;
	u32 attempt;
	u32 attempts;
	bool idle = false;

	for (attempt = 0; attempt < NVKM_DISPNV50_STATUS_POLL_COUNT; attempt++) {
		if (!nvkm_dispnv50_dmac_read_status(sc, dmac, &user_put, &ctrl,
		    &stat)) {
			dmac->dfly_last_idle = false;
			dmac->dfly_last_stat = 0;
			return;
		}
		idle = nvkm_dispnv50_dmac_status_idle(dmac, stat);
		if (idle)
			break;
		DELAY(NVKM_DISPNV50_STATUS_POLL_US);
	}
	attempts = attempt < NVKM_DISPNV50_STATUS_POLL_COUNT ?
	    attempt + 1 : NVKM_DISPNV50_STATUS_POLL_COUNT;
	dmac->dfly_last_idle = idle;
	dmac->dfly_last_stat = stat;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 %s status cur=%u user_put=0x%08x ctrl=0x%08x "
	    "stat=0x%08x idle=%u attempts=%u\n",
	    label, cur, user_put, ctrl, stat, idle, attempts);

	if (!idle && (dmac->dfly_oclass & 0xff) == 0x7e) {
		u32 chid = 1 + dmac->dfly_inst;
		u32 err = 0x6101f0 + chid * 12;
		u32 err_stat = nvkm_rd32(sc, err + 0x00);
		u32 err_data = nvkm_rd32(sc, err + 0x04);
		u32 err_code = nvkm_rd32(sc, err + 0x08);

		nvkm_infof(sc->dev,
		    "drm: dispnv50 %s error chid=%u stat=0x%08x data=0x%08x "
		    "code=0x%08x\n",
		    label, chid, err_stat, err_data, err_code);
	}
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
	nvkm_dispnv50_dmac_trace_push(sc, dmac, dmac->put, cur);

	for (u32 i = dmac->put; i < cur; i++)
		nvkm_wo32(dmac->dfly_push_mem, i * 4, dmac->dfly_shadow[i]);

	nvkm_gsp_bar1_flush(sc);
	nvkm_wr32(sc, dmac->dfly_user + 0x00, cur << 2);
	(void)nvkm_rd32(sc, dmac->dfly_user + 0x00);
	if ((dmac->dfly_oclass & 0xff) == 0x7e) {
		dmac->dfly_last_idle = false;
		dmac->dfly_last_stat = 0;
	} else {
		nvkm_dispnv50_dmac_trace_status(sc, dmac, cur);
	}

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
	dmac->dfly_oclass = oclass[0];
	dmac->dfly_inst = inst;
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
	if (ret) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 set_pushbuf failed class=0x%x inst=%d "
		    "push=0x%llx err=%d\n",
		    oclass[0], inst,
		    (unsigned long long)nvkm_memory_addr(dmac->dfly_push_mem),
		    ret);
		goto fail;
	}

	ret = nvkm_gsp_disp_dmac_alloc(sc, oclass[0], inst, 0,
	    dmac->dfly_object);
	if (ret) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 dmac alloc failed class=0x%x inst=%d "
		    "err=%d\n",
		    oclass[0], inst, ret);
		goto fail;
	}

	if (syncbuf >= 0) {
		u64 vram_limit;

		ret = nvkm_dispnv50_ctxdma_new(dmac, oclass[0], inst,
		    "kmsSyncCtxDma", NV50_DISP_HANDLE_SYNCBUF, (u64)syncbuf,
		    (u64)syncbuf + 0x0fff, NVKM_DISPNV50_DMAOBJ_VRAM_RW_SP,
		    &dmac->sync, &dmac->dfly_sync_object);
		if (ret) {
			nvkm_infof(sc->dev,
			    "drm: dispnv50 sync ctxdma failed class=0x%x "
			    "inst=%d start=0x%llx limit=0x%llx err=%d\n",
			    oclass[0], inst, (unsigned long long)syncbuf,
			    (unsigned long long)syncbuf + 0x0fff, ret);
			goto fail;
		}

		if (sc->fb_usable_size == 0 ||
		    sc->fb_usable_base + sc->fb_usable_size <= sc->fb_usable_base) {
			nvkm_infof(sc->dev,
			    "drm: dispnv50 invalid vram range base=0x%llx "
			    "size=0x%llx\n",
			    (unsigned long long)sc->fb_usable_base,
			    (unsigned long long)sc->fb_usable_size);
			ret = -ENODEV;
			goto fail;
		}

		vram_limit = sc->fb_usable_base + sc->fb_usable_size - 1;
		ret = nvkm_dispnv50_ctxdma_new(dmac, oclass[0], inst,
		    "kmsVramCtxDma", NV50_DISP_HANDLE_VRAM, 0, vram_limit,
		    NVKM_DISPNV50_DMAOBJ_VRAM_RW_SP, &dmac->vram,
		    &dmac->dfly_vram_object);
		if (ret) {
			nvkm_infof(sc->dev,
			    "drm: dispnv50 vram ctxdma failed class=0x%x "
			    "inst=%d limit=0x%llx err=%d\n",
			    oclass[0], inst, (unsigned long long)vram_limit,
			    ret);
			goto fail;
		}

		if ((oclass[0] & 0xff) == 0x7e) {
			u32 fb_handle =
			    NV50_DISP_HANDLE_WNDW_CTX(
				NVKM_DISPNV50_SCANOUT_KIND);

			/*
			 * Linux creates this object in nv50_wndw_prepare_fb().
			 * The first-light path has one pitch-linear scanout, so
			 * bind the matching window framebuffer ctxdma here.
			 */
			ret = nvkm_dispnv50_ctxdma_new(dmac, oclass[0],
			    inst, "kmsWndwFbCtxDma", fb_handle, 0, vram_limit,
			    NVKM_DISPNV50_DMAOBJ_VRAM_RW_LP, &dmac->dfly_fb,
			    &dmac->dfly_fb_object);
			if (ret) {
				nvkm_infof(sc->dev,
				    "drm: dispnv50 fb ctxdma failed "
				    "class=0x%x inst=%d handle=0x%x "
				    "limit=0x%llx err=%d\n",
				    oclass[0], inst, fb_handle,
				    (unsigned long long)vram_limit, ret);
				goto fail;
			}
		}

		nvkm_infof(sc->dev,
		    "drm: dispnv50 ctxdma staged class=0x%x inst=%d "
		    "sync=0x%x vram=0x%x fb=0x%x syncbuf=0x%llx "
		    "vram_limit=0x%llx\n",
		    oclass[0], inst, dmac->sync.handle,
		    dmac->vram.handle, dmac->dfly_fb.handle,
		    (unsigned long long)syncbuf,
		    (unsigned long long)vram_limit);
	}

	dmac->push.wait = nvkm_dispnv50_dmac_wait;
	dmac->push.kick = nvkm_dispnv50_dmac_kick;
	dmac->push.mem.object.map.ptr = dmac->dfly_shadow;
	dmac->push.mem.addr = nvkm_memory_addr(dmac->dfly_push_mem);
	dmac->push.mem.size = nvkm_memory_size(dmac->dfly_push_mem);
	dmac->push.bgn = dmac->dfly_shadow;
	dmac->push.cur = dmac->push.bgn;
	dmac->push.end = dmac->push.bgn;
	dmac->max = NVKM_DISPNV50_PUSH_DWORDS - 1;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 dmac class=0x%x inst=%d push=0x%llx user=0x%x "
	    "sync=0x%x vram=0x%x\n",
	    oclass[0], inst,
	    (unsigned long long)nvkm_memory_addr(dmac->dfly_push_mem), user,
	    dmac->sync.handle, dmac->vram.handle);
	return 0;

fail:
	nvkm_dispnv50_ctxdma_drop(&dmac->dfly_fb_object);
	nvkm_dispnv50_ctxdma_drop(&dmac->dfly_vram_object);
	nvkm_dispnv50_ctxdma_drop(&dmac->dfly_sync_object);
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
	s64 syncbuf;
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

	syncbuf = disp->sync != NULL ? (s64)disp->sync->offset : -1;
	ret = nv50_dmac_create(drm, &oclass, 0, NULL, 0, syncbuf,
	    &core->chan);
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

	ret = nvkm_dispnv50_sync_ensure(sc, state);
	if (ret)
		return ret;

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
	u64 relative;
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

	ret = nvkm_dispnv50_vram_offset(sc, nvkm_memory_addr(state->scanout),
	    nvkm_memory_size(state->scanout), &relative);
	if (ret != 0) {
		nvkm_memory_unref(&state->scanout);
		return ret;
	}

	state->scanout_offset = nvkm_memory_addr(state->scanout);
	state->scanout_width = width;
	state->scanout_height = height;
	state->scanout_pitch = pitch;
	nvkm_infof(sc->dev,
	    "drm: dispnv50 scanout staged %ux%u pitch=%u vram=0x%llx "
	    "offset=0x%llx relative=0x%llx\n",
	    width, height, pitch,
	    (unsigned long long)nvkm_memory_addr(state->scanout),
	    (unsigned long long)state->scanout_offset,
	    (unsigned long long)relative);
	return 0;
}

static u16
nvkm_dispnv50_fixed_u0_16_fp16(u16 fixed)
{
	int exp = 0;
	int mantissa = 0;

	if (fixed != 0) {
		while (--exp != 0 && !(fixed & 0x8000))
			fixed <<= 1;
		mantissa = ((fixed << 1) & 0xffc0) >> 6;
		exp += 15;
	}
	return (u16)((exp << 10) | mantissa);
}

static void
nvkm_dispnv50_ilut_write_entry(struct nvkm_memory *memory, u64 offset,
    u16 value)
{
	nvkm_wo32(memory, offset + 0, (u32)value | ((u32)value << 16));
	nvkm_wo32(memory, offset + 4, (u32)value);
}

static int
nvkm_dispnv50_ilut_ensure(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state)
{
	u64 relative = 0;
	u64 offset;
	u32 i;
	int ret;

	if (state->ilut != NULL)
		return 0;

	ret = nvkm_memory_new(sc->core_device, NVKM_MEM_TARGET_VRAM,
	    NVKM_DISPNV50_ILUT_BYTES, 0x1000, true, &state->ilut);
	if (ret != 0)
		return ret;

	for (i = 0; i < NVKM_DISPNV50_ILUT_ENTRIES; i++) {
		u16 fixed = (u16)((i << 16) >> 10);
		u16 value = nvkm_dispnv50_fixed_u0_16_fp16(fixed);

		offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES + i) * 8ULL;
		nvkm_dispnv50_ilut_write_entry(state->ilut, offset, value);
	}

	offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES +
	    NVKM_DISPNV50_ILUT_ENTRIES) * 8ULL;
	nvkm_dispnv50_ilut_write_entry(state->ilut, offset,
	    nvkm_dispnv50_fixed_u0_16_fp16(
		(u16)(((NVKM_DISPNV50_ILUT_ENTRIES - 1U) << 16) >> 10)));
	nvkm_gsp_bar1_flush(sc);

	state->ilut_offset = nvkm_memory_addr(state->ilut);
	(void)nvkm_dispnv50_vram_offset(sc, state->ilut_offset,
	    nvkm_memory_size(state->ilut), &relative);
	nvkm_infof(sc->dev,
	    "drm: dispnv50 ilut staged entries=%u vram=0x%llx "
	    "offset=0x%llx relative=0x%llx\n",
	    NVKM_DISPNV50_ILUT_TOTAL_ENTRIES,
	    (unsigned long long)nvkm_memory_addr(state->ilut),
	    (unsigned long long)state->ilut_offset,
	    (unsigned long long)relative);
	return 0;
}

static void
nvkm_dispnv50_olut_write_entry(struct nvkm_memory *memory, u64 offset,
    u16 value)
{
	nvkm_wo32(memory, offset + 0, (u32)value | ((u32)value << 16));
	nvkm_wo32(memory, offset + 4, (u32)value);
}

static int
nvkm_dispnv50_olut_ensure(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state)
{
	u64 relative = 0;
	u64 offset;
	u32 i;
	int ret;

	if (state->olut != NULL)
		return 0;

	ret = nvkm_memory_new(sc->core_device, NVKM_MEM_TARGET_VRAM,
	    NVKM_DISPNV50_ILUT_BYTES, 0x1000, true, &state->olut);
	if (ret != 0)
		return ret;

	for (i = 0; i < NVKM_DISPNV50_ILUT_ENTRIES; i++) {
		u16 value = (u16)((i << 16) >> 10);

		offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES + i) * 8ULL;
		nvkm_dispnv50_olut_write_entry(state->olut, offset, value);
	}

	offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES +
	    NVKM_DISPNV50_ILUT_ENTRIES) * 8ULL;
	nvkm_dispnv50_olut_write_entry(state->olut, offset,
	    (u16)(((NVKM_DISPNV50_ILUT_ENTRIES - 1U) << 16) >> 10));
	nvkm_gsp_bar1_flush(sc);

	state->olut_offset = nvkm_memory_addr(state->olut);
	(void)nvkm_dispnv50_vram_offset(sc, state->olut_offset,
	    nvkm_memory_size(state->olut), &relative);
	nvkm_infof(sc->dev,
	    "drm: dispnv50 olut staged entries=%u vram=0x%llx "
	    "offset=0x%llx relative=0x%llx\n",
	    NVKM_DISPNV50_ILUT_TOTAL_ENTRIES,
	    (unsigned long long)nvkm_memory_addr(state->olut),
	    (unsigned long long)state->olut_offset,
	    (unsigned long long)relative);
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
	asyw->image.handle[0] =
	    NV50_DISP_HANDLE_WNDW_CTX(NVKM_DISPNV50_SCANOUT_KIND);
	asyw->image.offset[0] = state->scanout_offset;

	asyw->blend.depth = 255;
	asyw->blend.k1 = 255;
	asyw->blend.src_color =
	    NVC37E_SET_COMPOSITION_FACTOR_SELECT_SRC_COLOR_FACTOR_MATCH_SELECT_K1;
	asyw->blend.dst_color =
	    NVC37E_SET_COMPOSITION_FACTOR_SELECT_DST_COLOR_FACTOR_MATCH_SELECT_NEG_K1;
}

static int
nvkm_dispnv50_wndw_ilut_set(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_wndw *wndw,
    struct nv50_wndw_atom *asyw)
{
	int ret;

	if (wndw->func->ilut == NULL || wndw->func->xlut_set == NULL ||
	    !wndw->func->ilut_identity)
		return 0;

	ret = nvkm_dispnv50_ilut_ensure(sc, state);
	if (ret != 0)
		return ret;

	memset(&asyw->xlut, 0, sizeof(asyw->xlut));
	wndw->func->ilut(wndw, asyw, 0);
	asyw->xlut.handle = wndw->wndw.vram.handle;
	asyw->xlut.i.offset = state->ilut_offset;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 window ilut handle=0x%x offset=0x%llx "
	    "size=%u mode=%u output=%u\n",
	    asyw->xlut.handle, (unsigned long long)asyw->xlut.i.offset,
	    asyw->xlut.i.size, asyw->xlut.i.mode,
	    asyw->xlut.i.output_mode);
	return wndw->func->xlut_set(wndw, asyw);
}

static int
nvkm_dispnv50_head_olut_set(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_core *core,
    struct nv50_head *head, struct nv50_head_atom *asyh)
{
	int ret;

	if (head->func->olut == NULL || head->func->olut_set == NULL ||
	    !head->func->olut_identity)
		return 0;

	ret = nvkm_dispnv50_olut_ensure(sc, state);
	if (ret != 0)
		return ret;
	if (!head->func->olut(head, asyh, 0))
		return -EINVAL;

	asyh->olut.handle = core->chan.vram.handle;
	asyh->olut.offset = state->olut_offset;
	nvkm_infof(sc->dev,
	    "drm: dispnv50 head olut handle=0x%x offset=0x%llx "
	    "size=%u mode=%u output=%u\n",
	    asyh->olut.handle, (unsigned long long)asyh->olut.offset,
	    asyh->olut.size, asyh->olut.mode, asyh->olut.output_mode);
	return head->func->olut_set(head, asyh);
}

static int
nvkm_dispnv50_wndw_sanitize(struct nv50_wndw *wndw)
{
	int ret;

	if (wndw->func->ntfy_clr != NULL) {
		ret = wndw->func->ntfy_clr(wndw);
		if (ret != 0)
			return ret;
	}
	if (wndw->func->sema_clr != NULL) {
		ret = wndw->func->sema_clr(wndw);
		if (ret != 0)
			return ret;
	}
	if (wndw->func->xlut_clr != NULL) {
		ret = wndw->func->xlut_clr(wndw);
		if (ret != 0)
			return ret;
	}
	if (wndw->func->csc_clr != NULL) {
		ret = wndw->func->csc_clr(wndw);
		if (ret != 0)
			return ret;
	}
	return 0;
}

static int
nvkm_dispnv50_wndw_ntfy_enable(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_wndw *wndw,
    struct nv50_wndw_atom *asyw)
{
	if (state->disp.sync == NULL || wndw->func->ntfy_reset == NULL ||
	    wndw->func->ntfy_set == NULL)
		return -ENODEV;

	asyw->ntfy.handle = wndw->wndw.sync.handle;
	asyw->ntfy.offset = wndw->ntfy;
	asyw->ntfy.awaken = false;
	wndw->func->ntfy_reset(state->disp.sync, wndw->ntfy);
	wndw->ntfy ^= 0x10;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 window notifier armed win=%d handle=0x%x "
	    "offset=0x%llx next=0x%x\n",
	    wndw->id, asyw->ntfy.handle,
	    (unsigned long long)asyw->ntfy.offset, wndw->ntfy);
	return wndw->func->ntfy_set(wndw, asyw);
}

static int
nvkm_dispnv50_core_commit_notify(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_core *core, u32 *interlock)
{
	u32 status;
	int ret;

	if (state->disp.sync == NULL || core->func->ntfy_init == NULL ||
	    core->func->ntfy_wait_done == NULL)
		return -ENODEV;

	core->func->ntfy_init(state->disp.sync, NV50_DISP_CORE_NTFY);
	ret = core->func->update(core, interlock, true);
	if (ret != 0)
		return ret;

	ret = core->func->ntfy_wait_done(state->disp.sync, NV50_DISP_CORE_NTFY,
	    core->chan.base.device);
	status = nouveau_bo_rd32(state->disp.sync, NV50_DISP_CORE_NTFY / 4);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 core notifier timeout status=0x%08x "
		    "err=%d\n", status, ret);
		return ret;
	}

	nvkm_infof(sc->dev,
	    "drm: dispnv50 core notifier done status=0x%08x\n", status);
	return 0;
}

static int
nvkm_dispnv50_wndw_wait_armed(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_wndw *wndw,
    struct nv50_wndw_atom *asyw)
{
	u32 status;
	int ret;

	if (state->disp.sync == NULL || wndw->func->ntfy_wait_begun == NULL)
		return -ENODEV;

	ret = wndw->func->ntfy_wait_begun(state->disp.sync, asyw->ntfy.offset,
	    wndw->wndw.base.device);
	status = nouveau_bo_rd32(state->disp.sync, asyw->ntfy.offset / 4);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 window notifier timeout win=%d "
		    "offset=0x%llx status=0x%08x err=%d\n",
		    wndw->id, (unsigned long long)asyw->ntfy.offset,
		    status, ret);
		return ret;
	}

	nvkm_infof(sc->dev,
	    "drm: dispnv50 window notifier begun win=%d offset=0x%llx "
	    "status=0x%08x\n",
	    wndw->id, (unsigned long long)asyw->ntfy.offset, status);
	return 0;
}

static u8
nvkm_dispnv50_hdmi_max_ac_packet(struct drm_display_mode *mode)
{
	const u32 rekey = 56;
	u32 blank;
	u32 packet;

	if (mode == NULL || mode->htotal <= mode->hdisplay)
		return 0;

	blank = mode->htotal - mode->hdisplay;
	if (blank <= rekey + 18)
		return 0;

	packet = (blank - rekey - 18) / 32;
	if (packet > 0x1f)
		return 0x1f;
	return (u8)packet;
}

static u32
nvkm_dispnv50_hdmi_clock_khz(struct drm_display_mode *mode)
{
	if (mode == NULL)
		return 0;
	if (mode->clock > 0)
		return (u32)mode->clock;
	if (mode->crtc_clock > 0)
		return (u32)mode->crtc_clock;
	return 0;
}

static int
nvkm_dispnv50_hdmi_enable(struct nvkm_softc *sc, struct nvkm_outp *outp,
    struct drm_display_mode *mode, uint32_t head,
    const struct nvkm_dispnv50_hdmi_info *hdmi)
{
	const u8 rekey = 56;
	struct nvkm_ior *ior;
	u32 clock_khz;
	u8 max_ac_packet;
	bool has_infoframe = false;
	bool scdc_supported = false;
	bool scdc_scrambling = false;
	bool scdc_low_rates = false;

	if (outp == NULL || outp->ior == NULL)
		return -ENODEV;

	ior = outp->ior;
	if (ior->func == NULL || ior->func->hdmi == NULL ||
	    ior->func->hdmi->ctrl == NULL)
		return -ENODEV;

	max_ac_packet = nvkm_dispnv50_hdmi_max_ac_packet(mode);
	clock_khz = nvkm_dispnv50_hdmi_clock_khz(mode);
	if (hdmi != NULL) {
		has_infoframe = hdmi->has_infoframe;
		scdc_supported = hdmi->scdc_supported;
		scdc_scrambling = hdmi->scdc_scrambling;
		scdc_low_rates = hdmi->scdc_low_rates;
	}
	ior->func->hdmi->ctrl(ior, (int)head, true, max_ac_packet, rekey);
	if (ior->func->hdmi->scdc != NULL) {
		ior->func->hdmi->scdc(ior, clock_khz, scdc_supported,
		    scdc_scrambling, scdc_low_rates);
	}

	nvkm_infof(sc->dev,
	    "drm: dispnv50 hdmi enabled outp=%02x sor=%d head=%u "
	    "max_ac_packet=%u rekey=%u clock=%u infoframe=%d scdc=%d "
	    "scrambling=%d low_rates=%d\n",
	    outp->index, ior->id, head, max_ac_packet, rekey, clock_khz,
	    has_infoframe, scdc_supported, scdc_scrambling, scdc_low_rates);
	return 0;
}

static struct nvkm_outp *
nvkm_dispnv50_find_outp(struct nvkm_softc *sc, uint32_t display_id)
{
	struct nvkm_outp *outp;
	int id;

	if (sc == NULL || sc->disp == NULL || display_id == 0)
		return NULL;

	id = ffs(display_id) - 1;
	list_for_each_entry(outp, &sc->disp->outps, head) {
		if (outp->index == id)
			return outp;
	}

	return NULL;
}

static int
nvkm_dispnv50_route_tmds(struct nvkm_softc *sc, struct nv50_core *core,
    struct nv50_head_atom *asyh, struct drm_display_mode *mode, uint32_t head,
    uint32_t display_id, const struct nvkm_dispnv50_hdmi_info *hdmi)
{
	struct nvkm_outp *outp;
	u32 proto;
	u32 ctrl;
	int ret;

	outp = nvkm_dispnv50_find_outp(sc, display_id);
	if (outp == NULL)
		return -ENODEV;
	if (outp->info.type != DCB_OUTPUT_TMDS) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 route deferred: display=0x%x outp=%02x type=%02x\n",
		    display_id, outp->index, outp->info.type);
		return -ENOSYS;
	}

	if (outp->ior == NULL) {
		if (outp->func == NULL || outp->func->acquire == NULL)
			return -ENODEV;
		ret = outp->func->acquire(outp, false);
		if (ret != 0)
			return ret;
	}
	if (outp->ior == NULL)
		return -ENODEV;
	if (core->func->sor == NULL || core->func->sor->ctrl == NULL)
		return -ENODEV;

	ret = nvkm_dispnv50_hdmi_enable(sc, outp, mode, head, hdmi);
	if (ret != 0)
		return ret;

	proto = (outp->ior->asy.link & 1) ?
	    NVC37D_SOR_SET_CONTROL_PROTOCOL_SINGLE_TMDS_A :
	    NVC37D_SOR_SET_CONTROL_PROTOCOL_SINGLE_TMDS_B;
	ctrl = NVVAL(NVC37D, SOR_SET_CONTROL, PROTOCOL, proto) | BIT(head);

	ret = core->func->sor->ctrl(core, outp->ior->id, ctrl, asyh);
	if (ret == 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 route display=0x%x outp=%02x sor=%d link=%u proto=%u head=%u\n",
		    display_id, outp->index, outp->ior->id, outp->ior->asy.link,
		    proto, head);
	}
	return ret;
}

int
nvkm_dispnv50_atomic_enable(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, uint32_t win, uint32_t display_id,
    const struct nvkm_dispnv50_hdmi_info *hdmi)
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
	if (nvhead->func->static_wndw_map != NULL)
		nvhead->func->static_wndw_map(nvhead, &asyh);
	asyh.wndw.mask |= BIT(win);

	if (nvhead->func->display_id != NULL) {
		ret = nvhead->func->display_id(nvhead, display_id);
		if (ret != 0)
			goto fail;
	}
	if (nvhead->func->view != NULL) {
		ret = nvhead->func->view(nvhead, &asyh);
		if (ret != 0)
			goto fail;
	}
	if (nvhead->func->mode != NULL) {
		ret = nvhead->func->mode(nvhead, &asyh);
		if (ret != 0)
			goto fail;
	}
	if (nvhead->func->dither != NULL) {
		ret = nvhead->func->dither(nvhead, &asyh);
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
	ret = nvkm_dispnv50_route_tmds(sc, core, &asyh, mode, head, display_id,
	    hdmi);
	if (ret != 0)
		goto fail;
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 head state head=%u view=%ux%u->%ux%u "
	    "wndw_mask=0x%x wndw_owned=0x%x\n",
	    head, asyh.view.iW, asyh.view.iH, asyh.view.oW, asyh.view.oH,
	    asyh.wndw.mask, asyh.wndw.owned);

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

	ret = nvkm_dispnv50_head_olut_set(sc, state, core, nvhead, &asyh);
	if (ret != 0)
		goto fail;
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;

	nvkm_dispnv50_wndw_atom_fill(&asyw, crtc, state);
	ret = nvkm_dispnv50_wndw_sanitize(wndw);
	if (ret != 0)
		goto fail;
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	ret = nvkm_dispnv50_wndw_ntfy_enable(sc, state, wndw, &asyw);
	if (ret != 0)
		goto fail;
	ret = wndw->func->image_set(wndw, &asyw);
	if (ret != 0)
		goto fail;
	ret = nvkm_dispnv50_wndw_ilut_set(sc, state, wndw, &asyw);
	if (ret != 0)
		goto fail;
	ret = wndw->func->blend_set(wndw, &asyw);
	if (ret != 0)
		goto fail;
	interlock[NV50_DISP_INTERLOCK_WNDW] |= wndw->interlock.data;
	ret = wndw->func->update(wndw, interlock);
	if (ret != 0)
		goto fail;
	ret = nvkm_dispnv50_core_commit_notify(sc, state, core, interlock);
	if (ret != 0)
		goto fail;
	nvkm_dispnv50_dmac_trace_status(sc, &wndw->wndw, wndw->wndw.cur);
	if (!wndw->wndw.dfly_last_idle) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 window channel not idle after core notifier "
		    "win=%u stat=0x%08x\n", win, wndw->wndw.dfly_last_stat);
	}
	ret = nvkm_dispnv50_wndw_wait_armed(sc, state, wndw, &asyw);
	if (ret != 0)
		goto fail;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 bridge armed head=%u win=%u display=0x%x "
	    "scanout=0x%llx offset=0x%llx\n",
	    head, win, display_id,
	    (unsigned long long)nvkm_memory_addr(state->scanout),
	    (unsigned long long)state->scanout_offset);
	ret = nvkm_dispnv50_console_register(sc, state);
	if (ret != 0)
		nvkm_infof(sc->dev,
		    "drm: dispnv50 console fb deferred err=%d\n", ret);
	return 0;

fail:
	nvkm_infof(sc->dev,
	    "drm: dispnv50 bridge failed head=%u win=%u display=0x%x err=%d\n",
	    head, win, display_id, ret);
	if (ret == 0)
		return (-ENODEV);
	return ret;
}

void
nvkm_dispnv50_fini(struct nvkm_softc *sc)
{
	if (sc == NULL || sc->dispnv50 == NULL)
		return;

	nvkm_dispnv50_console_unregister(sc->dispnv50);
}
