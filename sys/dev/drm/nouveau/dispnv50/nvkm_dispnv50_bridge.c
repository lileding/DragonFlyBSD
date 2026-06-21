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
#include "nvkm_bo.h"
#include "core.h"
#include "curs.h"
#include "head.h"
#include "wndw.h"

#include <machine/framebuffer.h>
#include <sys/callout.h>
#include <sys/sbuf.h>

#include <core/gpuobj.h>
#include <core/memory.h>
#include <core/object.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fourcc.h>
#include <engine/disp/conn.h>
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
#define NVKM_DISPNV50_BLOCKLINEAR_KIND	0x06U
#define NVKM_DISPNV50_DMAOBJ_VRAM_RW_SP	0x00000045U
#define NVKM_DISPNV50_DMAOBJ_VRAM_RW_LP	0x00000005U
#define NVKM_DISPNV50_DMAOBJ_VRAM_RW_LP_KIND(kind) \
	(NVKM_DISPNV50_DMAOBJ_VRAM_RW_LP | ((uint32_t)(kind) << 20))
#define NVKM_DISPNV50_STATUS_POLL_COUNT	50U
#define NVKM_DISPNV50_WIND_POLL_COUNT	2000U	/* 2s, matches nouveau */
#define NVKM_DISPNV50_STATUS_POLL_US	1000U
#define NVKM_DISPNV50_ILUT_ENTRIES	1024U
#define NVKM_DISPNV50_ILUT_VSS_ENTRIES	4U
#define NVKM_DISPNV50_ILUT_TOTAL_ENTRIES \
	(NVKM_DISPNV50_ILUT_VSS_ENTRIES + NVKM_DISPNV50_ILUT_ENTRIES + 1U)
#define NVKM_DISPNV50_ILUT_BYTES \
	(NVKM_DISPNV50_ILUT_TOTAL_ENTRIES * 8U)
#define NVKM_DISPNV50_CONSOLE_FLUSH_HZ	25U

enum nvkm_dispnv50_audit_op {
	NVKM_DISPNV50_AUDIT_NONE = 0,
	NVKM_DISPNV50_AUDIT_ATOMIC_ENABLE,
	NVKM_DISPNV50_AUDIT_PLANE_UPDATE,
	NVKM_DISPNV50_AUDIT_PLANE_DISABLE,
	NVKM_DISPNV50_AUDIT_CURSOR_UPDATE,
	NVKM_DISPNV50_AUDIT_CURSOR_DISABLE,
};

/*
 * Snapshot of the bridge state visible to dev.drm.0.state.
 *
 * Ownership:
 *   The parent nvkm_dispnv50_state owns these records.  They borrow no BO,
 *   CRTC, connector, or channel references; all pointers are converted to
 *   scalar state before publication.
 *
 * Lifetime:
 *   current is the last notifier-confirmed modeset/update or disable known to
 *   this bridge.  pending is the last async page-flip kicked to hardware; the
 *   final latch is still proven by vblank/flip events until D7 wires display
 *   fences as a first-class audit source.
 *
 * Threading:
 *   Updated from KMS commit context and read locklessly from sysctl.  Values
 *   are diagnostic breadcrumbs only and must not drive synchronization.
 */
struct nvkm_dispnv50_audit_snapshot {
	bool valid;
	bool async;
	bool armed;
	bool disabled;
	enum nvkm_dispnv50_audit_op op;
	u64 seq;
	u32 head;
	u32 win;
	u32 display_id;
	u64 scanout_addr;
	u64 scanout_offset;
	u32 width;
	u32 height;
	u32 pitch;
	u32 format;
	u64 modifier;
	u8 kind;
	bool user;
};

struct nvkm_dispnv50_cursor_audit {
	bool valid;
	bool enabled;
	bool async;
	u64 seq;
	u32 head;
	int32_t x;
	int32_t y;
	u64 offset;
	u32 width;
	u32 height;
	u32 pitch;
	u32 format;
	u64 modifier;
	u32 bo_scanout_pin_count;
};

struct nvkm_dispnv50_state {
	struct nv50_disp disp;
	struct nvif_disp ifdisp;
	struct nouveau_bo sync_bo;
	struct nvkm_memory *sync_mem;
	struct nv50_head head[4];
	struct nv50_wndw *wndw[8];
	struct nv50_wndw *curs[4];
	struct nvkm_memory *ilut;
	u64 ilut_offset;
	struct nvkm_memory *olut;
	u64 olut_offset;
	struct nvkm_memory *scanout;
	u64 scanout_offset;
	u32 scanout_width;
	u32 scanout_height;
	u32 scanout_pitch;
	u32 scanout_format;
	u64 scanout_modifier;
	u8 scanout_kind;
	struct fb_info console_fb;
	void *console_shadow;
	void *console_snapshot;
	u64 console_shadow_size;
	uint64_t *console_shadow_bar1_gva;
	uint64_t console_direct_bar1_gva;
	u64 console_direct_bar1_size;
	u64 console_scanout_addr;
	u32 console_shadow_bar1_pages;
	struct callout console_flush_callout;
	bool console_callout_ready;
	bool console_flush_active;
	bool scanout_user;
	bool console_fb_registered;
	bool console_direct_map;
	u64 console_flush_count;
	u64 console_flush_error_count;
	bool core_ready;
	u64 audit_seqno;
	struct nvkm_dispnv50_audit_snapshot audit_current;
	struct nvkm_dispnv50_audit_snapshot audit_pending;
	struct nvkm_dispnv50_cursor_audit audit_cursor[4];
};

static void
nvkm_dispnv50_debug_dmac_sbuf(struct nvkm_softc *sc, struct sbuf *sb,
    const char *name, struct nv50_dmac *dmac)
{
	u32 user_put;
	u32 user_get;
	u32 ctrl = 0;
	u32 stat = 0;
	u32 channel;
	bool valid = false;
	bool idle = false;

	/*
	 * Ownership: this diagnostic borrows the display channel and never owns
	 * USERD, push memory, or hardware register state.
	 * Lifetime: every value is a racing snapshot; commits and IRQ handlers may
	 * update PUT/GET/status while the sysctl string is being built.
	 * Threading: callers hold no display locks.  Reads are side-effect free and
	 * must not be used for synchronization or forward progress decisions.
	 */
	if (sc == NULL || sb == NULL || name == NULL || dmac == NULL ||
	    dmac->dfly_user == 0) {
		sbuf_printf(sb, "%s_dmac = unavailable\n", name);
		return;
	}

	user_put = nvkm_rd32(sc, dmac->dfly_user + 0x00);
	user_get = nvkm_rd32(sc, dmac->dfly_user + 0x04);

	switch (dmac->dfly_oclass & 0xff) {
	case 0x7d:
		ctrl = nvkm_rd32(sc, 0x6104e0);
		stat = nvkm_rd32(sc, 0x610630);
		idle = ((stat & 0x001f0000) == 0x000b0000);
		valid = true;
		break;
	case 0x7e:
		channel = 1 + dmac->dfly_inst;
		ctrl = nvkm_rd32(sc, 0x6104e0 + channel * 4);
		stat = nvkm_rd32(sc, 0x610664 + (channel - 1) * 4);
		idle = ((stat & 0x000f0000) == 0x00040000);
		valid = true;
		break;
	default:
		break;
	}

	sbuf_printf(sb, "%s_dmac = %p\n", name, dmac);
	sbuf_printf(sb, "%s_dmac_class = 0x%08x\n", name,
	    (u32)dmac->dfly_oclass);
	sbuf_printf(sb, "%s_dmac_inst = %d\n", name, dmac->dfly_inst);
	sbuf_printf(sb, "%s_dmac_user = 0x%08x\n", name, dmac->dfly_user);
	sbuf_printf(sb, "%s_dmac_sw_put = %u\n", name, dmac->put);
	sbuf_printf(sb, "%s_dmac_sw_cur = %u\n", name, dmac->cur);
	sbuf_printf(sb, "%s_dmac_hw_put = 0x%08x\n", name, user_put);
	sbuf_printf(sb, "%s_dmac_hw_get = 0x%08x\n", name, user_get);
	sbuf_printf(sb, "%s_dmac_hw_get_dwords = %u\n", name, user_get >> 2);
	sbuf_printf(sb, "%s_dmac_status_valid = %d\n", name, valid);
	sbuf_printf(sb, "%s_dmac_ctrl = 0x%08x\n", name, ctrl);
	sbuf_printf(sb, "%s_dmac_stat = 0x%08x\n", name, stat);
	sbuf_printf(sb, "%s_dmac_idle = %d\n", name, idle);
	sbuf_printf(sb, "%s_dmac_last_idle = %d\n", name,
	    dmac->dfly_last_idle);
	sbuf_printf(sb, "%s_dmac_last_stat = 0x%08x\n", name,
	    dmac->dfly_last_stat);
}

static u32
nvkm_dispnv50_align_u32(u32 value, u32 align)
{
	return ((value + align - 1) & ~(align - 1));
}

static bool
nvkm_dispnv50_modifier_is_linear(u64 modifier)
{
	return (modifier == DRM_FORMAT_MOD_LINEAR ||
	    modifier == DRM_FORMAT_MOD_INVALID);
}

static bool
nvkm_dispnv50_modifier_is_blocklinear(u64 modifier)
{
	return (!nvkm_dispnv50_modifier_is_linear(modifier));
}

static u8
nvkm_dispnv50_modifier_kind(u64 modifier)
{
	return ((modifier >> 12) & 0xff);
}

static u8
nvkm_dispnv50_modifier_blockh(u64 modifier)
{
	return (modifier & 0x0f);
}

static int
nvkm_dispnv50_neg_errno(int err)
{
	if (err > 0)
		return -err;
	return err;
}

static const char *
nvkm_dispnv50_audit_op_name(enum nvkm_dispnv50_audit_op op)
{
	switch (op) {
	case NVKM_DISPNV50_AUDIT_NONE:
		return "none";
	case NVKM_DISPNV50_AUDIT_ATOMIC_ENABLE:
		return "atomic_enable";
	case NVKM_DISPNV50_AUDIT_PLANE_UPDATE:
		return "plane_update";
	case NVKM_DISPNV50_AUDIT_PLANE_DISABLE:
		return "plane_disable";
	case NVKM_DISPNV50_AUDIT_CURSOR_UPDATE:
		return "cursor_update";
	case NVKM_DISPNV50_AUDIT_CURSOR_DISABLE:
		return "cursor_disable";
	default:
		return "unknown";
	}
}

static void
nvkm_dispnv50_cursor_audit_capture(struct nvkm_dispnv50_state *state,
    struct drm_plane_state *plane_state, struct nvkm_bo *bo, u32 head,
    bool enabled, bool async)
{
	struct nvkm_dispnv50_cursor_audit *audit;
	struct drm_framebuffer *fb;

	if (state == NULL || head >= nitems(state->audit_cursor))
		return;

	audit = &state->audit_cursor[head];
	memset(audit, 0, sizeof(*audit));
	audit->valid = true;
	audit->enabled = enabled;
	audit->async = async;
	audit->seq = ++state->audit_seqno;
	audit->head = head;
	if (plane_state == NULL || !enabled)
		return;

	fb = plane_state->fb;
	audit->x = plane_state->crtc_x;
	audit->y = plane_state->crtc_y;
	if (fb != NULL) {
		audit->width = fb->width;
		audit->height = fb->height;
		audit->pitch = fb->pitches[0];
		audit->format = fb->format != NULL ? fb->format->format : 0;
		audit->modifier = fb->modifier;
		if (bo != NULL)
			audit->offset = bo->paddr + fb->offsets[0];
	}
	if (bo != NULL)
		audit->bo_scanout_pin_count = bo->scanout_pin_count;
}

static u64
nvkm_dispnv50_scanout_addr(struct nvkm_dispnv50_state *state)
{
	if (state == NULL)
		return 0;
	if (state->scanout_user || state->scanout == NULL)
		return state->scanout_offset;
	return nvkm_memory_addr(state->scanout);
}

static void
nvkm_dispnv50_audit_capture(struct nvkm_dispnv50_state *state,
    struct nvkm_dispnv50_audit_snapshot *snap,
    enum nvkm_dispnv50_audit_op op, u32 head, u32 win, u32 display_id,
    bool async, bool armed, bool disabled)
{
	if (state == NULL || snap == NULL)
		return;

	memset(snap, 0, sizeof(*snap));
	snap->valid = true;
	snap->async = async;
	snap->armed = armed;
	snap->disabled = disabled;
	snap->op = op;
	snap->seq = ++state->audit_seqno;
	snap->head = head;
	snap->win = win;
	snap->display_id = display_id;
	snap->scanout_addr = nvkm_dispnv50_scanout_addr(state);
	snap->scanout_offset = state->scanout_offset;
	snap->width = state->scanout_width;
	snap->height = state->scanout_height;
	snap->pitch = state->scanout_pitch;
	snap->format = state->scanout_format;
	snap->modifier = state->scanout_modifier;
	snap->kind = state->scanout_kind;
	snap->user = state->scanout_user;
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
nvkm_dispnv50_read_paddr_pixel(struct nvkm_softc *sc, u64 paddr,
    u32 pitch, u32 x, u32 y, u32 *pixel)
{
	u64 byte = (u64)y * pitch + (u64)x * NVKM_DISPNV50_SCANOUT_BPP;
	u64 addr = paddr + byte;
	u64 page = addr & ~(u64)(PAGE_SIZE - 1);
	u64 page_off = addr - page;
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

static void
nvkm_dispnv50_debug_audit_snapshot_sbuf(struct sbuf *sb, const char *name,
    const struct nvkm_dispnv50_audit_snapshot *snap)
{
	if (snap == NULL || !snap->valid) {
		sbuf_printf(sb, "%s_valid = 0\n", name);
		return;
	}

	sbuf_printf(sb, "%s_valid = 1\n", name);
	sbuf_printf(sb, "%s_seq = %llu\n", name,
	    (unsigned long long)snap->seq);
	sbuf_printf(sb, "%s_op = %s\n", name,
	    nvkm_dispnv50_audit_op_name(snap->op));
	sbuf_printf(sb, "%s_head = %u\n", name, snap->head);
	sbuf_printf(sb, "%s_win = %u\n", name, snap->win);
	sbuf_printf(sb, "%s_display_id = 0x%08x\n", name, snap->display_id);
	sbuf_printf(sb, "%s_async = %d\n", name, snap->async);
	sbuf_printf(sb, "%s_armed = %d\n", name, snap->armed);
	sbuf_printf(sb, "%s_disabled = %d\n", name, snap->disabled);
	sbuf_printf(sb, "%s_scanout_addr = 0x%016llx\n", name,
	    (unsigned long long)snap->scanout_addr);
	sbuf_printf(sb, "%s_scanout_offset = 0x%016llx\n", name,
	    (unsigned long long)snap->scanout_offset);
	sbuf_printf(sb, "%s_scanout_size = %ux%u\n", name, snap->width,
	    snap->height);
	sbuf_printf(sb, "%s_scanout_pitch = %u\n", name, snap->pitch);
	sbuf_printf(sb, "%s_scanout_format = 0x%08x\n", name, snap->format);
	sbuf_printf(sb, "%s_scanout_modifier = 0x%016llx\n", name,
	    (unsigned long long)snap->modifier);
	sbuf_printf(sb, "%s_scanout_kind = 0x%02x\n", name, snap->kind);
	sbuf_printf(sb, "%s_scanout_user = %d\n", name, snap->user);
}

static void
nvkm_dispnv50_debug_head_sbuf(struct nvkm_softc *sc, struct sbuf *sb,
    struct nvkm_dispnv50_state *state, u32 head)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_plane *primary;
	struct drm_plane *cursor;
	struct drm_plane_state *plane_state;
	struct drm_plane_state *cursor_state;
	struct drm_framebuffer *fb;
	struct drm_framebuffer *cursor_fb;
	struct drm_gem_object *obj;
	struct drm_gem_object *cursor_obj;
	struct nvkm_bo *bo;
	struct nvkm_bo *cursor_bo;
	struct nv50_head *nvhead;
	struct nv50_wndw *curs;
	struct nvkm_dispnv50_cursor_audit *cursor_audit;
	bool head_ready;
	bool cursor_hooks;

	nvhead = head < nitems(state->head) ? &state->head[head] : NULL;
	head_ready = nvhead != NULL && nvhead->func != NULL;
	cursor_hooks = head_ready && nvhead->func->curs_set != NULL &&
	    nvhead->func->curs_clr != NULL &&
	    nvhead->func->curs_layout != NULL &&
	    nvhead->func->curs_format != NULL;
	crtc = (sc != NULL && head < nitems(sc->kms_crtc)) ?
	    sc->kms_crtc[head] : NULL;
	crtc_state = crtc != NULL ? crtc->state : NULL;
	primary = crtc != NULL ? crtc->primary : NULL;
	cursor = crtc != NULL ? crtc->cursor : NULL;
	plane_state = primary != NULL ? primary->state : NULL;
	cursor_state = cursor != NULL ? cursor->state : NULL;
	fb = plane_state != NULL ? plane_state->fb : NULL;
	cursor_fb = cursor_state != NULL ? cursor_state->fb : NULL;
	obj = fb != NULL ? fb->obj[0] : NULL;
	cursor_obj = cursor_fb != NULL ? cursor_fb->obj[0] : NULL;
	bo = obj != NULL ? to_nvkm_bo(obj) : NULL;
	cursor_bo = cursor_obj != NULL ? to_nvkm_bo(cursor_obj) : NULL;
	curs = head < nitems(state->curs) ? state->curs[head] : NULL;
	cursor_audit = head < nitems(state->audit_cursor) ?
	    &state->audit_cursor[head] : NULL;

	sbuf_printf(sb, "head[%u]_ready = %d\n", head, head_ready);
	sbuf_printf(sb, "head[%u]_crtc = %p\n", head, crtc);
	sbuf_printf(sb, "head[%u]_crtc_active = %d\n", head,
	    crtc_state != NULL && crtc_state->active);
	sbuf_printf(sb, "head[%u]_crtc_enable = %d\n", head,
	    crtc_state != NULL && crtc_state->enable);
	sbuf_printf(sb, "head[%u]_vblank_masked = %d\n", head,
	    sc != NULL && (sc->gsp_disp_vblank_mask & BIT(head)) != 0);
	sbuf_printf(sb, "head[%u]_disp_status = 0x%08x\n", head,
	    sc != NULL && head < nitems(sc->gsp_disp_head_status) ?
	    sc->gsp_disp_head_status[head] : 0);
	sbuf_printf(sb, "head[%u]_cursor_hooks = %d\n", head, cursor_hooks);
	sbuf_printf(sb, "head[%u]_cursor_plane_present = %d\n", head,
	    cursor != NULL);
	sbuf_printf(sb, "head[%u]_cursor_channel_present = %d\n", head,
	    curs != NULL);
	sbuf_printf(sb, "head[%u]_cursor_enabled = %d\n", head,
	    cursor_state != NULL && cursor_state->visible);
	sbuf_printf(sb, "head[%u]_cursor_async_last = %d\n", head,
	    cursor_audit != NULL && cursor_audit->valid && cursor_audit->async);
	sbuf_printf(sb, "head[%u]_cursor_last_seq = %llu\n", head,
	    cursor_audit != NULL && cursor_audit->valid ?
	    (unsigned long long)cursor_audit->seq : 0ULL);
	sbuf_printf(sb, "head[%u]_cursor_last_x = %d\n", head,
	    cursor_audit != NULL && cursor_audit->valid ? cursor_audit->x : 0);
	sbuf_printf(sb, "head[%u]_cursor_last_y = %d\n", head,
	    cursor_audit != NULL && cursor_audit->valid ? cursor_audit->y : 0);
	sbuf_printf(sb, "head[%u]_cursor_fb = %p\n", head, cursor_fb);
	if (cursor_fb != NULL) {
		sbuf_printf(sb, "head[%u]_cursor_fb_size = %ux%u\n",
		    head, cursor_fb->width, cursor_fb->height);
		sbuf_printf(sb, "head[%u]_cursor_fb_pitch = %u\n",
		    head, cursor_fb->pitches[0]);
		sbuf_printf(sb, "head[%u]_cursor_fb_format = 0x%08x\n",
		    head, cursor_fb->format != NULL ?
		    cursor_fb->format->format : 0);
		sbuf_printf(sb, "head[%u]_cursor_fb_modifier = 0x%016llx\n",
		    head, (unsigned long long)cursor_fb->modifier);
	}
	sbuf_printf(sb, "head[%u]_cursor_bo = %p\n", head, cursor_bo);
	if (cursor_bo != NULL) {
		sbuf_printf(sb, "head[%u]_cursor_bo_paddr = 0x%016llx\n",
		    head, (unsigned long long)cursor_bo->paddr);
		sbuf_printf(sb, "head[%u]_cursor_bo_scanout_pin_count = %u\n",
		    head, cursor_bo->scanout_pin_count);
		sbuf_printf(sb, "head[%u]_cursor_bo_ttm_pin_count = %u\n",
		    head, cursor_bo->ttm_pin_count);
	}
	sbuf_printf(sb, "head[%u]_primary_plane = %p\n", head, primary);
	sbuf_printf(sb, "head[%u]_scanout_fb = %p\n", head, fb);
	if (fb != NULL) {
		sbuf_printf(sb, "head[%u]_scanout_fb_size = %ux%u\n",
		    head, fb->width, fb->height);
		sbuf_printf(sb, "head[%u]_scanout_fb_pitch = %u\n",
		    head, fb->pitches[0]);
		sbuf_printf(sb, "head[%u]_scanout_fb_format = 0x%08x\n",
		    head, fb->format != NULL ? fb->format->format : 0);
		sbuf_printf(sb, "head[%u]_scanout_fb_modifier = 0x%016llx\n",
		    head, (unsigned long long)fb->modifier);
	}
	sbuf_printf(sb, "head[%u]_scanout_bo = %p\n", head, bo);
	if (bo != NULL) {
		sbuf_printf(sb, "head[%u]_scanout_bo_size = %llu\n", head,
		    (unsigned long long)obj->size);
		sbuf_printf(sb, "head[%u]_scanout_bo_domain = 0x%08x\n",
		    head, bo->domain);
		sbuf_printf(sb, "head[%u]_scanout_bo_paddr = 0x%016llx\n",
		    head, (unsigned long long)bo->paddr);
		sbuf_printf(sb, "head[%u]_scanout_bo_ttm_pin_count = %u\n",
		    head, bo->ttm_pin_count);
		sbuf_printf(sb, "head[%u]_scanout_bo_scanout_pin_count = %u\n",
		    head, bo->scanout_pin_count);
		sbuf_printf(sb,
		    "head[%u]_scanout_bo_scanout_no_evict_pin_count = %u\n",
		    head, bo->scanout_no_evict_pin_count);
		sbuf_printf(sb, "head[%u]_scanout_bo_vm_bind_pin_count = %u\n",
		    head, bo->vm_bind_pin_count);
		sbuf_printf(sb, "head[%u]_scanout_bo_resv = %p\n",
		    head, bo->vm_resv != NULL ? bo->vm_resv : &bo->resv);
	}
}

static void
nvkm_dispnv50_debug_windows_sbuf(struct nvkm_softc *sc, struct sbuf *sb,
    struct nvkm_dispnv50_state *state)
{
	struct nv50_wndw *wndw;
	char name[16];

	for (u32 i = 0; i < nitems(state->wndw); i++) {
		wndw = state->wndw[i];
		sbuf_printf(sb, "wndw[%u]_present = %d\n", i,
		    wndw != NULL);
		if (wndw == NULL)
			continue;
		sbuf_printf(sb, "wndw[%u]_id = %d\n", i, wndw->id);
		sbuf_printf(sb, "wndw[%u]_interlock = 0x%08x\n",
		    i, wndw->interlock.data);
		sbuf_printf(sb, "wndw[%u]_interlock_wimm = 0x%08x\n",
		    i, wndw->interlock.wimm);
		sbuf_printf(sb, "wndw[%u]_ntfy = 0x%04x\n", i, wndw->ntfy);
		sbuf_printf(sb, "wndw[%u]_sema = 0x%04x\n", i, wndw->sema);
		snprintf(name, sizeof(name), "wndw%u", i);
		nvkm_dispnv50_debug_dmac_sbuf(sc, sb, name, &wndw->wndw);
		snprintf(name, sizeof(name), "wimm%u", i);
		nvkm_dispnv50_debug_dmac_sbuf(sc, sb, name, &wndw->wimm);
	}
}

static void
nvkm_dispnv50_debug_outps_sbuf(struct nvkm_softc *sc, struct sbuf *sb)
{
	struct nvkm_outp *outp;
	u32 index = 0;

	if (sc == NULL || sc->disp == NULL) {
		sbuf_cat(sb, "outp_count = 0\n");
		return;
	}

	list_for_each_entry(outp, &sc->disp->outps, head) {
		struct nvkm_conn *conn = outp->conn;
		struct nvkm_ior *ior = outp->ior;

		sbuf_printf(sb, "outp[%u]_index = %d\n", index, outp->index);
		sbuf_printf(sb, "outp[%u]_type = 0x%02x\n",
		    index, outp->info.type);
		sbuf_printf(sb, "outp[%u]_heads = 0x%02x\n",
		    index, outp->info.heads);
		sbuf_printf(sb, "outp[%u]_connector = 0x%02x\n",
		    index, outp->info.connector);
		sbuf_printf(sb, "outp[%u]_location = 0x%02x\n",
		    index, outp->info.location);
		sbuf_printf(sb, "outp[%u]_or = 0x%02x\n", index,
		    outp->info.or);
		sbuf_printf(sb, "outp[%u]_link = 0x%02x\n", index,
		    outp->info.link);
		sbuf_printf(sb, "outp[%u]_acquired = %u\n",
		    index, outp->acquired);
		sbuf_printf(sb, "outp[%u]_conn = %p\n", index, conn);
		if (conn != NULL) {
			sbuf_printf(sb, "outp[%u]_conn_index = %d\n",
			    index, conn->index);
			sbuf_printf(sb, "outp[%u]_conn_type = 0x%02x\n",
			    index, conn->info.type);
			sbuf_printf(sb, "outp[%u]_conn_location = 0x%02x\n",
			    index, conn->info.location);
		}
		sbuf_printf(sb, "outp[%u]_ior = %p\n", index, ior);
		if (ior != NULL) {
			sbuf_printf(sb, "outp[%u]_ior_type = %u\n",
			    index, ior->type);
			sbuf_printf(sb, "outp[%u]_ior_id = %d\n",
			    index, ior->id);
			sbuf_printf(sb, "outp[%u]_ior_name = %s\n",
			    index, ior->name);
			sbuf_printf(sb, "outp[%u]_ior_asy_head = 0x%02x\n",
			    index, ior->asy.head);
			sbuf_printf(sb, "outp[%u]_ior_asy_proto = %u\n",
			    index, ior->asy.proto);
			sbuf_printf(sb, "outp[%u]_ior_asy_link = %u\n",
			    index, ior->asy.link);
			sbuf_printf(sb, "outp[%u]_ior_arm_head = 0x%02x\n",
			    index, ior->arm.head);
			sbuf_printf(sb, "outp[%u]_ior_arm_proto = %u\n",
			    index, ior->arm.proto);
			sbuf_printf(sb, "outp[%u]_ior_arm_link = %u\n",
			    index, ior->arm.link);
		}
		sbuf_printf(sb, "outp[%u]_dp_enabled = %d\n",
		    index, outp->info.type == DCB_OUTPUT_DP && outp->dp.enabled);
		sbuf_printf(sb, "outp[%u]_dp_mst = %d\n",
		    index, outp->info.type == DCB_OUTPUT_DP && outp->dp.mst);
		sbuf_printf(sb, "outp[%u]_dp_rates = %d\n",
		    index, outp->info.type == DCB_OUTPUT_DP ? outp->dp.rates : 0);
		index++;
	}
	sbuf_printf(sb, "outp_count = %u\n", index);
}

void
nvkm_dispnv50_debug_sbuf(struct nvkm_softc *sc, struct sbuf *sb)
{
	struct nvkm_dispnv50_state *state;
	u64 scanout_addr;
	u32 xs[4];
	u32 ys[2];
	u32 pixel;
	int err;

	/*
	 * Ownership: this diagnostic borrows sc->dispnv50 and never owns the
	 * scanout BO, framebuffer, or BAR1 mappings beyond each readback call.
	 * Lifetime: the sampled values are a best-effort snapshot; KMS commits may
	 * replace the state immediately after a field is read.
	 * Threading: callers do not hold the atomic commit locks.  Reads must stay
	 * side-effect free and tolerate racing with modesets, VT switches, and BO
	 * cleanup.  This is for postmortem observability, not synchronization.
	 */
	state = sc != NULL ? sc->dispnv50 : NULL;
	if (state == NULL) {
		sbuf_cat(sb, "dispnv50 = NULL\n");
		return;
	}

	scanout_addr = nvkm_dispnv50_scanout_addr(state);
	sbuf_printf(sb, "dispnv50 = %p\n", state);
	sbuf_printf(sb, "core_ready = %d\n", state->core_ready);
	sbuf_printf(sb, "display_audit_version = 1\n");
	sbuf_printf(sb, "display_audit_seqno = %llu\n",
	    (unsigned long long)state->audit_seqno);
	sbuf_printf(sb, "display_audit_scanout_pin_balance = %lld\n",
	    (long long)sc->kms_scanout_pin_count -
	    (long long)sc->kms_scanout_unpin_count);
	sbuf_printf(sb, "scanout_user = %d\n", state->scanout_user);
	sbuf_printf(sb, "scanout_memory = %p\n", state->scanout);
	sbuf_printf(sb, "scanout_addr = 0x%016llx\n",
	    (unsigned long long)scanout_addr);
	sbuf_printf(sb, "scanout_offset = 0x%016llx\n",
	    (unsigned long long)state->scanout_offset);
	sbuf_printf(sb, "scanout_width = %u\n", state->scanout_width);
	sbuf_printf(sb, "scanout_height = %u\n", state->scanout_height);
	sbuf_printf(sb, "scanout_pitch = %u\n", state->scanout_pitch);
	sbuf_printf(sb, "scanout_format = 0x%08x\n", state->scanout_format);
	sbuf_printf(sb, "scanout_modifier = 0x%016llx\n",
	    (unsigned long long)state->scanout_modifier);
	sbuf_printf(sb, "scanout_kind = 0x%02x\n", state->scanout_kind);
	sbuf_printf(sb, "scanout_blocklinear = %d\n",
	    nvkm_dispnv50_modifier_is_blocklinear(state->scanout_modifier));
	sbuf_printf(sb, "console_registered = %d\n", state->console_fb_registered);
	sbuf_printf(sb, "console_direct_map = %d\n", state->console_direct_map);
	sbuf_printf(sb, "console_flush_active = %d\n", state->console_flush_active);
	sbuf_printf(sb, "console_scanout_addr = 0x%016llx\n",
	    (unsigned long long)state->console_scanout_addr);
	sbuf_printf(sb, "console_flush_count = %llu\n",
	    (unsigned long long)state->console_flush_count);
	sbuf_printf(sb, "console_flush_error_count = %llu\n",
	    (unsigned long long)state->console_flush_error_count);
	nvkm_dispnv50_debug_audit_snapshot_sbuf(sb,
	    "display_audit_current", &state->audit_current);
	nvkm_dispnv50_debug_audit_snapshot_sbuf(sb,
	    "display_audit_pending", &state->audit_pending);
	for (u32 head = 0; head < nitems(state->head); head++)
		nvkm_dispnv50_debug_head_sbuf(sc, sb, state, head);
	nvkm_dispnv50_debug_windows_sbuf(sc, sb, state);
	nvkm_dispnv50_debug_outps_sbuf(sc, sb);
	if (state->disp.core != NULL)
		nvkm_dispnv50_debug_dmac_sbuf(sc, sb, "core",
		    &state->disp.core->chan);
	else
		sbuf_cat(sb, "core_dmac = unavailable\n");

	if (scanout_addr == 0 || state->scanout_width == 0 ||
	    state->scanout_height == 0 || state->scanout_pitch == 0) {
		sbuf_cat(sb, "scanout_samples = unavailable\n");
		return;
	}

	xs[0] = 0;
	xs[1] = state->scanout_width / 4;
	xs[2] = state->scanout_width / 2;
	xs[3] = (state->scanout_width * 3) / 4;
	ys[0] = 0;
	ys[1] = state->scanout_height / 2;
	for (u32 yi = 0; yi < nitems(ys); yi++) {
		for (u32 xi = 0; xi < nitems(xs); xi++) {
			pixel = 0;
			err = nvkm_dispnv50_read_paddr_pixel(sc, scanout_addr,
			    state->scanout_pitch, xs[xi], ys[yi], &pixel);
			sbuf_printf(sb,
			    "scanout_sample[%u,%u] err=%d pixel=0x%08x\n",
			    xs[xi], ys[yi], err, pixel);
		}
	}
}

static int
nvkm_dispnv50_console_map_direct(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, u64 size)
{
	void *bar1_base;
	u64 scanout_addr;
	u64 map_addr;
	u64 map_offset;
	u64 map_size;
	uint64_t bar1_gva;
	int err;

	if (sc == NULL || state == NULL || state->scanout == NULL ||
	    sc->bar_res[1] == NULL || size == 0)
		return -ENODEV;

	bar1_base = rman_get_virtual(sc->bar_res[1]);
	if (bar1_base == NULL)
		return -ENODEV;

	scanout_addr = nvkm_memory_addr(state->scanout);
	map_addr = scanout_addr & ~(u64)(PAGE_SIZE - 1);
	map_offset = scanout_addr - map_addr;
	if (size > (u64)-1 - map_offset)
		return -EINVAL;
	map_size = size + map_offset;

	err = nvkm_gsp_bar1_map_existing_range(sc, map_addr, map_size,
	    &bar1_gva);
	if (err != 0)
		return nvkm_dispnv50_neg_errno(err);
	if (bar1_gva > rman_get_size(sc->bar_res[1]) ||
	    map_size > rman_get_size(sc->bar_res[1]) - bar1_gva) {
		nvkm_gsp_bar1_unmap_existing_range(sc, bar1_gva, map_size);
		return -ENOSPC;
	}

	state->console_direct_bar1_gva = bar1_gva;
	state->console_direct_bar1_size = map_size;
	state->console_scanout_addr = scanout_addr;
	state->console_direct_map = true;
	state->console_fb.vaddr = (vm_offset_t)((uintptr_t)bar1_base +
	    bar1_gva + map_offset);
	state->console_fb.paddr = (vm_paddr_t)(rman_get_start(sc->bar_res[1]) +
	    bar1_gva + map_offset);
	return 0;
}

static int
nvkm_dispnv50_console_map_shadow_scanout(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, u64 size)
{
	u64 base;
	u32 pages;
	int err;

	if (sc == NULL || state == NULL || state->scanout == NULL || size == 0)
		return -ENODEV;

	pages = (u32)((size + PAGE_SIZE - 1) / PAGE_SIZE);
	state->console_shadow_bar1_gva = kzalloc((u64)pages * sizeof(u64),
	    GFP_KERNEL);
	if (state->console_shadow_bar1_gva == NULL)
		return -ENOMEM;
	state->console_shadow_bar1_pages = pages;

	base = nvkm_memory_addr(state->scanout);
	state->console_scanout_addr = base;
	for (u32 i = 0; i < pages; i++) {
		err = nvkm_gsp_bar1_map_existing(sc, base + (u64)i * PAGE_SIZE,
		    &state->console_shadow_bar1_gva[i]);
		if (err != 0) {
			for (u32 j = 0; j < i; j++)
				nvkm_gsp_bar1_unmap_existing(sc,
				    state->console_shadow_bar1_gva[j]);
			kfree(state->console_shadow_bar1_gva);
			state->console_shadow_bar1_gva = NULL;
			state->console_scanout_addr = 0;
			state->console_shadow_bar1_pages = 0;
			return nvkm_dispnv50_neg_errno(err);
		}
	}

	return 0;
}

static void
nvkm_dispnv50_console_unmap_scanout(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state)
{
	if (state == NULL)
		return;

	if (state->console_direct_map && sc != NULL)
		nvkm_gsp_bar1_unmap_existing_range(sc,
		    state->console_direct_bar1_gva,
		    state->console_direct_bar1_size);
	state->console_direct_map = false;
	state->console_direct_bar1_gva = 0;
	state->console_direct_bar1_size = 0;

	if (state->console_shadow_bar1_gva != NULL && sc != NULL) {
		for (u32 i = 0; i < state->console_shadow_bar1_pages; i++)
			nvkm_gsp_bar1_unmap_existing(sc,
			    state->console_shadow_bar1_gva[i]);
	}
	kfree(state->console_shadow_bar1_gva);
	state->console_shadow_bar1_gva = NULL;
	state->console_scanout_addr = 0;
	state->console_shadow_bar1_pages = 0;
}

static int
nvkm_dispnv50_copy_buffer_range_to_scanout(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, const u8 *src, u64 start, u64 size)
{
	u64 done = 0;
	u64 total;

	if (sc == NULL || state == NULL || state->scanout == NULL ||
	    state->console_shadow_bar1_gva == NULL || src == NULL)
		return -ENODEV;

	total = (u64)state->scanout_pitch * state->scanout_height;
	if (start > total || size > total - start)
		return -ENODEV;

	while (done < size) {
		u64 byte = start + done;
		u32 page = (u32)(byte / PAGE_SIZE);
		u32 page_off = (u32)(byte & (PAGE_SIZE - 1));
		u32 chunk;

		if (page >= state->console_shadow_bar1_pages ||
		    state->console_shadow_bar1_gva[page] == 0)
			return -ENODEV;

		chunk = PAGE_SIZE - page_off;
		if ((u64)chunk > size - done)
			chunk = (u32)(size - done);
		for (u32 off = 0; off < chunk; off += 4) {
			u32 pixel;

			memcpy(&pixel, src + start + done + off, sizeof(pixel));
			nvkm_gsp_bar1_wr32(sc,
			    state->console_shadow_bar1_gva[page] + page_off +
			    off, pixel);
		}

		done += chunk;
	}

	nvkm_gsp_bar1_flush(sc);
	return 0;
}

static int
nvkm_dispnv50_copy_shadow_to_scanout(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state)
{
	u64 size;

	if (state == NULL || state->console_shadow == NULL ||
	    state->console_snapshot == NULL)
		return -ENODEV;

	size = (u64)state->scanout_pitch * state->scanout_height;
	if (state->console_shadow_size < size)
		return -ENODEV;

	memcpy(state->console_snapshot, state->console_shadow, size);
	return nvkm_dispnv50_copy_buffer_range_to_scanout(sc, state,
	    state->console_snapshot, 0, size);
}

static int
nvkm_dispnv50_console_flush_rows(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, u32 first, u32 last)
{
	u64 start;
	u64 size;

	if (last <= first)
		return 0;

	start = (u64)first * state->scanout_pitch;
	size = (u64)(last - first) * state->scanout_pitch;

	memcpy((u8 *)state->console_snapshot + start,
	    (u8 *)state->console_shadow + start, size);
	return nvkm_dispnv50_copy_buffer_range_to_scanout(sc, state,
	    state->console_snapshot, start, size);
}

static int
nvkm_dispnv50_copy_dirty_shadow_to_scanout(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, u32 *dirty_rows)
{
	const u32 height = state->scanout_height;
	const u32 pitch = state->scanout_pitch;
	u32 run_first = 0;
	bool in_run = false;
	int ret;

	*dirty_rows = 0;
	for (u32 y = 0; y < height; y++) {
		u64 off = (u64)y * pitch;

		if (memcmp((u8 *)state->console_shadow + off,
		    (u8 *)state->console_snapshot + off, pitch) != 0) {
			if (!in_run) {
				run_first = y;
				in_run = true;
			}
			(*dirty_rows)++;
			continue;
		}

		if (!in_run)
			continue;

		ret = nvkm_dispnv50_console_flush_rows(sc, state, run_first, y);
		if (ret != 0)
			return ret;
		in_run = false;
	}

	if (in_run) {
		ret = nvkm_dispnv50_console_flush_rows(sc, state, run_first,
		    height);
		if (ret != 0)
			return ret;
	}

	return 0;
}

static int
nvkm_dispnv50_console_flush_ticks(void)
{
	int ticks = hz / NVKM_DISPNV50_CONSOLE_FLUSH_HZ;

	return ticks < 1 ? 1 : ticks;
}

static void
nvkm_dispnv50_console_flush(void *arg)
{
	struct nvkm_dispnv50_state *state = arg;
	struct nvkm_softc *sc = state != NULL ? state->disp.dfly_sc : NULL;
	u32 dirty_rows = 0;
	int ret;

	if (state == NULL || !state->console_flush_active)
		return;

	ret = nvkm_dispnv50_copy_dirty_shadow_to_scanout(sc, state,
	    &dirty_rows);
	state->console_flush_count++;
	if (ret != 0) {
		state->console_flush_error_count++;
		if (sc != NULL && state->console_flush_error_count <= 4)
			nvkm_infof(sc->dev,
			    "drm: dispnv50 console flush failed err=%d "
			    "count=%llu\n", ret,
			    (unsigned long long)state->console_flush_error_count);
	}

	callout_reset(&state->console_flush_callout,
	    nvkm_dispnv50_console_flush_ticks(),
	    nvkm_dispnv50_console_flush, state);
}

static int
nvkm_dispnv50_console_start_flush(struct nvkm_dispnv50_state *state)
{
	if (!state->console_callout_ready) {
		callout_init_mp(&state->console_flush_callout);
		state->console_callout_ready = true;
	}

	if (state->console_flush_active)
		return 0;

	state->console_flush_active = true;
	callout_reset(&state->console_flush_callout,
	    nvkm_dispnv50_console_flush_ticks(),
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
	struct nvkm_softc *sc;

	if (state == NULL)
		return;

	sc = state->disp.dfly_sc;
	nvkm_dispnv50_console_stop_flush(state);
	if (state->console_fb_registered) {
		unregister_framebuffer(&state->console_fb);
		state->console_fb_registered = false;
	}
	nvkm_dispnv50_console_unmap_scanout(sc, state);
	if (state->console_snapshot != NULL) {
		kfree(state->console_snapshot);
		state->console_snapshot = NULL;
	}
	if (state->console_shadow != NULL) {
		kfree(state->console_shadow);
		state->console_shadow = NULL;
	}
	state->console_shadow_size = 0;
	memset(&state->console_fb, 0, sizeof(state->console_fb));
}

/*
 * syscons fb_set_par hook.  do_switch_scr() enqueues this on taskqueue
 * thread when a VT switch leaves a graphics VT; register_framebuffer()
 * also calls it synchronously once.  Restoring is only valid when the
 * screen still points at a userspace framebuffer and no DRM master is
 * left to fight with, so both other cases bail out.
 */
static int
nvkm_dispnv50_console_fb_set_par(struct fb_info *info)
{
	struct nvkm_dispnv50_state *state;
	struct nvkm_softc *sc;

	state = info != NULL ? info->par : NULL;
	if (state == NULL)
		return (0);
	sc = state->disp.dfly_sc;
	if (sc == NULL || sc->drm_dev == NULL)
		return (0);
	if (sc->drm_dev->master != NULL)
		return (0);
	if (!state->scanout_user)
		return (0);

	(void)nvkm_drm_kms_schedule(sc, "fb_set_par");
	return (0);
}

static int
nvkm_dispnv50_console_register(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state)
{
	u64 size;
	int ret;
	int direct_ret;

	if (state == NULL || state->scanout == NULL ||
	    state->scanout_width == 0 || state->scanout_height == 0 ||
	    state->scanout_pitch == 0)
		return -ENODEV;

	size = (u64)state->scanout_pitch * state->scanout_height;
	if (state->console_fb_registered &&
	    state->console_fb.width == state->scanout_width &&
	    state->console_fb.height == state->scanout_height &&
	    state->console_fb.stride == state->scanout_pitch &&
	    state->console_scanout_addr == nvkm_memory_addr(state->scanout)) {
		if (state->console_direct_map)
			return 0;
		if (state->console_shadow_size >= size &&
		    state->console_snapshot != NULL)
			return nvkm_dispnv50_console_start_flush(state);
	}

	nvkm_dispnv50_console_unregister(state);

	memset(&state->console_fb, 0, sizeof(state->console_fb));
	state->console_fb.width = state->scanout_width;
	state->console_fb.height = state->scanout_height;
	state->console_fb.stride = state->scanout_pitch;
	state->console_fb.depth = 32;
	state->console_fb.is_vga_boot_display = 0;
	state->console_fb.par = state;
	state->console_fb.device = sc->dev;
	state->console_fb.fbops.fb_set_par = nvkm_dispnv50_console_fb_set_par;

	direct_ret = nvkm_dispnv50_console_map_direct(sc, state, size);
	if (direct_ret == 0) {
		ret = register_framebuffer(&state->console_fb);
		if (ret != 0) {
			nvkm_infof(sc->dev,
			    "drm: dispnv50 direct console fb register failed "
			    "err=%d\n", ret);
			nvkm_dispnv50_console_unregister(state);
			return ret;
		}
		state->console_fb_registered = true;
		nvkm_infof(sc->dev,
		    "drm: dispnv50 console fb registered %ux%u pitch=%u "
		    "direct_bar1=0x%llx size=0x%llx vaddr=%p paddr=0x%llx\n",
		    state->scanout_width, state->scanout_height,
		    state->scanout_pitch,
		    (unsigned long long)state->console_direct_bar1_gva,
		    (unsigned long long)state->console_direct_bar1_size,
		    (void *)state->console_fb.vaddr,
		    (unsigned long long)state->console_fb.paddr);
		return 0;
	}
	nvkm_infof(sc->dev,
	    "drm: dispnv50 direct console BAR1 map failed err=%d; "
	    "falling back to shadow flush\n", direct_ret);

	state->console_shadow = kzalloc(size, GFP_KERNEL);
	if (state->console_shadow == NULL)
		return -ENOMEM;
	state->console_snapshot = kzalloc(size, GFP_KERNEL);
	if (state->console_snapshot == NULL) {
		nvkm_dispnv50_console_unregister(state);
		return -ENOMEM;
	}
	state->console_shadow_size = size;

	ret = nvkm_dispnv50_console_map_shadow_scanout(sc, state, size);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 shadow console BAR1 map failed err=%d\n",
		    ret);
		nvkm_dispnv50_console_unregister(state);
		return ret;
	}

	state->console_fb.vaddr = (vm_offset_t)state->console_shadow;
	state->console_fb.paddr = vtophys(state->console_shadow);

	ret = register_framebuffer(&state->console_fb);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 shadow console fb register failed err=%d\n",
		    ret);
		nvkm_dispnv50_console_unregister(state);
		return ret;
	}
	state->console_fb_registered = true;

	/*
	 * Fallback only: syscons renders into this CPU shadow buffer. Mirror
	 * changed rows to the pitch-linear VRAM scanout BO with cached BAR1
	 * page mappings.
	 */
	ret = nvkm_dispnv50_copy_shadow_to_scanout(sc, state);
	if (ret != 0)
		nvkm_infof(sc->dev,
		    "drm: dispnv50 console initial flush failed err=%d\n", ret);
	(void)nvkm_dispnv50_console_start_flush(state);

	nvkm_infof(sc->dev,
	    "drm: dispnv50 console fb registered %ux%u pitch=%u "
	    "shadow=%p size=0x%llx bar1_pages=%u flush_hz=%u\n",
	    state->scanout_width, state->scanout_height, state->scanout_pitch,
	    state->console_shadow, (unsigned long long)state->console_shadow_size,
	    state->console_shadow_bar1_pages, NVKM_DISPNV50_CONSOLE_FLUSH_HZ);
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

static u32
nvkm_dispnv50_dmac_get_dword(struct nv50_dmac *dmac)
{
	return nvkm_rd32(dmac->dfly_sc, dmac->dfly_user + 0x04) >> 2;
}

/* Dwords writable at cur before colliding with the HW fetch pointer; EVO
 * fetch must stay 5 dwords short of GET (NVIDIA/nouveau behaviour). */
static int
nvkm_dispnv50_dmac_free_dwords(struct nv50_dmac *dmac)
{
	u32 get = nvkm_dispnv50_dmac_get_dword(dmac);

	if (get > dmac->cur)
		return (int)(get - dmac->cur - 5);
	return (int)(dmac->max - dmac->cur);
}

/*
 * Wrap the push buffer.  EVO is a PUT/GET ring: PUT may only rewind to 0
 * via an explicit JUMP method, and only once GET has left offset 0
 * (PUT == GET would be ignored by HW).  Unlike Linux nouveau we stage
 * methods in a CPU shadow, so the staged dwords plus the JUMP must be
 * copied into the real push buffer here -- the follow-up kick() rewinds
 * PUT to 0 and its put..cur copy window no longer covers them.
 */
static int
nvkm_dispnv50_dmac_wind(struct nv50_dmac *dmac)
{
	struct nvkm_softc *sc = dmac->dfly_sc;
	u32 get = nvkm_dispnv50_dmac_get_dword(dmac);
	u32 cur = (u32)(dmac->push.cur - dmac->dfly_shadow);
	u32 i;

	if (get == 0) {
		if (dmac->put == 0 && cur != 0)
			dmac->push.kick(&dmac->push);
		for (i = 0; i < NVKM_DISPNV50_WIND_POLL_COUNT; i++) {
			get = nvkm_dispnv50_dmac_get_dword(dmac);
			if (get > 0)
				break;
			DELAY(NVKM_DISPNV50_STATUS_POLL_US);
		}
		if (get == 0)
			return -ETIMEDOUT;
		cur = (u32)(dmac->push.cur - dmac->dfly_shadow);
	}

	dmac->dfly_shadow[cur] = 0x20000000;	/* EVO JUMP to offset 0 */
	for (i = dmac->put; i <= cur; i++)
		nvkm_wo32(dmac->dfly_push_mem, i * 4, dmac->dfly_shadow[i]);
	nvkm_gsp_bar1_flush(sc);
	dmac->cur = 0;
	return 0;
}

static int
nvkm_dispnv50_dmac_wait(struct nvif_push *push, u32 size)
{
	struct nv50_dmac *dmac = container_of(push, struct nv50_dmac, push);
	int free;
	u32 i;
	int ret;

	if (dmac->dfly_shadow == NULL || dmac->dfly_push_mem == NULL)
		return -ENODEV;
	if (size > dmac->max)
		return -EINVAL;

	dmac->cur = (u32)(push->cur - dmac->dfly_shadow);
	if (dmac->cur + size >= dmac->max) {
		ret = nvkm_dispnv50_dmac_wind(dmac);
		if (ret != 0)
			return ret;
		push->cur = dmac->dfly_shadow + dmac->cur;	/* offset 0 */
		push->kick(push);	/* publishes the wrap: PUT = 0 */
	}

	free = nvkm_dispnv50_dmac_free_dwords(dmac);
	for (i = 0; free < (int)size && i < NVKM_DISPNV50_WIND_POLL_COUNT; i++) {
		DELAY(NVKM_DISPNV50_STATUS_POLL_US);
		free = nvkm_dispnv50_dmac_free_dwords(dmac);
	}
	if (free < (int)size)
		return -ETIMEDOUT;

	push->bgn = dmac->dfly_shadow + dmac->cur;
	push->cur = push->bgn;
	push->end = push->bgn + free;
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
	if (!sc->kms_push_trace)
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

	if (sc->kms_push_trace || !idle) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 %s status cur=%u user_put=0x%08x "
		    "ctrl=0x%08x stat=0x%08x idle=%u attempts=%u\n",
		    label, cur, user_put, ctrl, stat, idle, attempts);
	}

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
	void *bar0;
	u32 user;
	u64 user_size;
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
	bar0 = sc->bar_res[0] != NULL ? rman_get_virtual(sc->bar_res[0]) : NULL;
	if (bar0 == NULL) {
		ret = -ENODEV;
		goto fail;
	}
	user_size = (oclass[0] & 0xff) == 0x7d ? 0x10000ULL : 0x1000ULL;
	dmac->base.user.map.ptr = (void __iomem *)((uint8_t *)bar0 + user);
	dmac->base.user.map.size = user_size;
	dmac->base.user.oclass = oclass[0];
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
			u32 fb_handle = NV50_DISP_HANDLE_WNDW_CTX(
			    NVKM_DISPNV50_SCANOUT_KIND);
			u32 fb_blocklinear_handle = NV50_DISP_HANDLE_WNDW_CTX(
			    NVKM_DISPNV50_BLOCKLINEAR_KIND);

			/*
			 * Linux creates this object in nv50_wndw_prepare_fb().
			 * Bind both pitch and TU102 blocklinear kind objects so
			 * the image handle can match the framebuffer modifier.
			 */
			ret = nvkm_dispnv50_ctxdma_new(dmac, oclass[0],
			    inst, "kmsWndwFbCtxDma", fb_handle, 0, vram_limit,
			    NVKM_DISPNV50_DMAOBJ_VRAM_RW_LP_KIND(
				NVKM_DISPNV50_SCANOUT_KIND),
			    &dmac->dfly_fb, &dmac->dfly_fb_object);
			if (ret) {
				nvkm_infof(sc->dev,
				    "drm: dispnv50 fb ctxdma failed "
				    "class=0x%x inst=%d handle=0x%x "
				    "limit=0x%llx err=%d\n",
				    oclass[0], inst, fb_handle,
				    (unsigned long long)vram_limit, ret);
				goto fail;
			}
			ret = nvkm_dispnv50_ctxdma_new(dmac, oclass[0],
			    inst, "kmsWndwFbBlocklinearCtxDma",
			    fb_blocklinear_handle, 0, vram_limit,
			    NVKM_DISPNV50_DMAOBJ_VRAM_RW_LP_KIND(
				NVKM_DISPNV50_BLOCKLINEAR_KIND),
			    &dmac->dfly_fb_blocklinear,
			    &dmac->dfly_fb_blocklinear_object);
			if (ret) {
				nvkm_infof(sc->dev,
				    "drm: dispnv50 blocklinear fb ctxdma "
				    "failed class=0x%x inst=%d handle=0x%x "
				    "limit=0x%llx err=%d\n",
				    oclass[0], inst, fb_blocklinear_handle,
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
	nvkm_dispnv50_ctxdma_drop(&dmac->dfly_fb_blocklinear_object);
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

static const struct nv50_wndw_func nvkm_dispnv50_cursor_wndw_func = {
};

int
curs507a_new_(const struct nv50_wimm_func *func, struct nouveau_drm *drm,
    int head, s32 oclass, u32 interlock_data, struct nv50_wndw **pwndw)
{
	struct nvif_disp_chan_v0 args;
	struct nv50_wndw *wndw = NULL;
	int ret;

	if (func == NULL || drm == NULL || pwndw == NULL || head < 0)
		return -EINVAL;
	*pwndw = NULL;

	ret = nv50_wndw_new_(&nvkm_dispnv50_cursor_wndw_func, drm->dev,
	    DRM_PLANE_TYPE_CURSOR, "curs", head, NULL, BIT(head),
	    NV50_DISP_INTERLOCK_CURS, interlock_data, &wndw);
	if (ret != 0)
		return ret;

	memset(&args, 0, sizeof(args));
	args.id = head;
	ret = nv50_dmac_create(drm, &oclass, head, &args, sizeof(args), -1,
	    &wndw->wimm);
	if (ret != 0) {
		kfree(wndw);
		return ret;
	}

	wndw->immd = func;
	wndw->interlock.wimm = interlock_data;
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

static int
nvkm_dispnv50_cursor_init(struct nvkm_softc *sc, uint32_t head)
{
	struct nvkm_dispnv50_state *state;
	struct nouveau_drm drm;
	int ret;

	if (head >= nitems(((struct nvkm_dispnv50_state *)0)->curs))
		return -EINVAL;
	if (nvkm_dispnv50_core_init(sc) != 0)
		return -ENODEV;

	state = sc->dispnv50;
	if (state->curs[head] != NULL)
		return 0;

	drm.dev = sc->drm_dev;
	ret = cursc37a_new(&drm, (int)head, TU102_DISP_CURSOR,
	    &state->curs[head]);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 cursor channel alloc failed head=%u err=%d\n",
		    head, ret);
		return ret;
	}

	nvkm_infof(sc->dev,
	    "drm: dispnv50 cursor channel staged head=%u\n", head);
	return 0;
}

static int
nvkm_dispnv50_cursor_atom_fill(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct drm_crtc *crtc,
    struct nv50_core *core, struct nv50_head *head,
    struct nv50_wndw_atom *asyw, struct nv50_head_atom *asyh,
    struct nvkm_bo **pbo)
{
	struct drm_plane_state *plane_state;
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;
	u64 min_size;
	u64 offset;
	int ret;

	(void)sc;
	(void)state;
	if (crtc == NULL || crtc->cursor == NULL ||
	    crtc->cursor->state == NULL || core == NULL || head == NULL ||
	    asyw == NULL || asyh == NULL || pbo == NULL)
		return -EINVAL;

	plane_state = crtc->cursor->state;
	fb = plane_state->fb;
	if (!plane_state->visible || fb == NULL || fb->obj[0] == NULL ||
	    fb->format == NULL)
		return -ENOENT;
	if (fb->format->format != DRM_FORMAT_ARGB8888)
		return -EINVAL;

	obj = fb->obj[0];
	bo = to_nvkm_bo(obj);
	if (!(bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) || bo->paddr == 0)
		return -EINVAL;

	min_size = (u64)(fb->height - 1) * fb->pitches[0] +
	    (u64)fb->width * fb->format->cpp[0] + fb->offsets[0];
	if (obj->size < min_size)
		return -EINVAL;

	offset = bo->paddr + fb->offsets[0];
	memset(asyw, 0, sizeof(*asyw));
	memset(asyh, 0, sizeof(*asyh));
	asyw->state = *plane_state;
	asyw->image.w = fb->width;
	asyw->image.h = fb->height;
	asyw->image.pitch[0] = fb->pitches[0];
	asyw->image.format = NVC37D_HEAD_SET_CONTROL_CURSOR_FORMAT_A8R8G8B8;
	asyw->image.handle[0] = core->chan.vram.handle;
	asyw->image.offset[0] = offset;
	asyw->point.x = (u16)plane_state->crtc_x;
	asyw->point.y = (u16)plane_state->crtc_y;

	ret = head->func->curs_layout(head, asyw, asyh);
	if (ret != 0)
		return ret;
	ret = head->func->curs_format(head, asyw, asyh);
	if (ret != 0)
		return ret;

	asyh->curs.visible = true;
	asyh->curs.handle = core->chan.vram.handle;
	asyh->curs.offset = offset;
	*pbo = bo;
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
	    state->scanout_pitch == pitch) {
		state->scanout_offset = nvkm_memory_addr(state->scanout);
		state->scanout_format = DRM_FORMAT_XRGB8888;
		state->scanout_modifier = DRM_FORMAT_MOD_LINEAR;
		state->scanout_kind = NVKM_DISPNV50_SCANOUT_KIND;
		state->scanout_user = false;
		return 0;
	}

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
	state->scanout_format = DRM_FORMAT_XRGB8888;
	state->scanout_modifier = DRM_FORMAT_MOD_LINEAR;
	state->scanout_kind = NVKM_DISPNV50_SCANOUT_KIND;
	state->scanout_user = false;
	nvkm_infof(sc->dev,
	    "drm: dispnv50 scanout staged %ux%u pitch=%u vram=0x%llx "
	    "offset=0x%llx relative=0x%llx\n",
	    width, height, pitch,
	    (unsigned long long)nvkm_memory_addr(state->scanout),
	    (unsigned long long)state->scanout_offset,
	    (unsigned long long)relative);
	return 0;
}

static int
nvkm_dispnv50_scanout_from_fb(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct drm_crtc *crtc)
{
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;
	u64 min_size;
	u32 cpp;

	(void)sc;
	if (crtc->primary == NULL || crtc->primary->state == NULL)
		return (ENOENT);
	fb = crtc->primary->state->fb;
	if (fb == NULL || fb->obj[0] == NULL || fb->format == NULL)
		return (ENOENT);

	obj = fb->obj[0];
	bo = to_nvkm_bo(obj);
	if (!(bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) || bo->paddr == 0)
		return (EINVAL);
	if (fb->width == 0 || fb->height == 0)
		return (EINVAL);

	cpp = fb->format->cpp[0];
	min_size = (u64)(fb->height - 1) * fb->pitches[0] +
	    (u64)fb->width * cpp + fb->offsets[0];
	if (obj->size < min_size)
		return (EINVAL);

	/*
	 * Keep the console fb registered while userspace scans out: syscons
	 * only fires the fb_set_par hook on VT switch-away if sc->fbi still
	 * carries it.  Console text keeps rendering into the (invisible)
	 * console BO, same as Linux fbcon under X.
	 */

	state->scanout_offset = bo->paddr + fb->offsets[0];
	state->scanout_width = fb->width;
	state->scanout_height = fb->height;
	state->scanout_pitch = fb->pitches[0];
	state->scanout_format = fb->format->format;
	state->scanout_modifier = fb->modifier;
	state->scanout_kind =
	    nvkm_dispnv50_modifier_is_blocklinear(fb->modifier) ?
	    nvkm_dispnv50_modifier_kind(fb->modifier) :
	    NVKM_DISPNV50_SCANOUT_KIND;
	state->scanout_user = true;
	nvkm_infof(sc->dev,
	    "drm: dispnv50 user scanout %ux%u pitch=%u format=0x%08x "
	    "modifier=0x%016llx kind=0x%02x vram=0x%llx bo=%p\n",
	    state->scanout_width, state->scanout_height, state->scanout_pitch,
	    state->scanout_format,
	    (unsigned long long)state->scanout_modifier, state->scanout_kind,
	    (unsigned long long)state->scanout_offset,
	    bo);
	return (0);
}

static u32
nvkm_dispnv50_wndw_format(u32 format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
		return (NVC57E_SET_PARAMS_FORMAT_X8R8G8B8);
	case DRM_FORMAT_ARGB8888:
		return (NVC57E_SET_PARAMS_FORMAT_A8R8G8B8);
	case DRM_FORMAT_XBGR8888:
		return (NVC57E_SET_PARAMS_FORMAT_X8B8G8R8);
	case DRM_FORMAT_ABGR8888:
		return (NVC57E_SET_PARAMS_FORMAT_A8B8G8R8);
	case DRM_FORMAT_RGB565:
		return (NVC57E_SET_PARAMS_FORMAT_R5G6B5);
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555:
		return (NVC57E_SET_PARAMS_FORMAT_A1R5G5B5);
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		return (NVC57E_SET_PARAMS_FORMAT_A2R10G10B10);
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return (NVC57E_SET_PARAMS_FORMAT_A2B10G10R10);
	default:
		return (0);
	}
}

static int
nvkm_dispnv50_select_scanout(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct drm_crtc *crtc,
    bool allow_internal)
{
	struct drm_display_mode *mode;
	int ret;

	ret = nvkm_dispnv50_scanout_from_fb(sc, state, crtc);
	if (ret == ENOENT && allow_internal) {
		if (crtc == NULL || crtc->state == NULL)
			return (EINVAL);
		mode = &crtc->state->adjusted_mode;
		if (mode->hdisplay == 0 || mode->vdisplay == 0)
			return (EINVAL);
		ret = nvkm_dispnv50_scanout_ensure(sc, state,
		    mode->hdisplay, mode->vdisplay);
	}
	if (ret != 0)
		return ret;
	if (nvkm_dispnv50_wndw_format(state->scanout_format) == 0)
		return (EINVAL);
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
	asyw->image.format = nvkm_dispnv50_wndw_format(state->scanout_format);
	if (nvkm_dispnv50_modifier_is_blocklinear(state->scanout_modifier)) {
		asyw->image.blockh =
		    nvkm_dispnv50_modifier_blockh(state->scanout_modifier);
		asyw->image.layout =
		    NVC57E_SET_STORAGE_MEMORY_LAYOUT_BLOCKLINEAR;
		asyw->image.blocks[0] = state->scanout_pitch >> 6;
		asyw->image.pitch[0] = 0;
	} else {
		asyw->image.blockh =
		    NVC57E_SET_STORAGE_BLOCK_HEIGHT_NVD_BLOCK_HEIGHT_ONE_GOB;
		asyw->image.layout = NVC57E_SET_STORAGE_MEMORY_LAYOUT_PITCH;
		asyw->image.blocks[0] = 0;
		asyw->image.pitch[0] = state->scanout_pitch;
	}
	asyw->image.handle[0] =
	    NV50_DISP_HANDLE_WNDW_CTX(state->scanout_kind);
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

static int
nvkm_dispnv50_window_program(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct drm_crtc *crtc,
    struct nv50_core *core, struct nv50_wndw *wndw, u32 *interlock,
    bool sanitize, bool async, enum nvkm_dispnv50_audit_op op, u32 head,
    u32 display_id, const char *reason)
{
	struct nv50_wndw_atom asyw;
	int ret;
	bool commit_core;

	if (interlock == NULL)
		return -EINVAL;

	nvkm_dispnv50_wndw_atom_fill(&asyw, crtc, state);
	if (sanitize) {
		ret = nvkm_dispnv50_wndw_sanitize(wndw);
		if (ret != 0)
			goto fail;
	}

	/* A page-flip only changes the scanout buffer (the image); notifier,
	 * ILUT and blend were programmed at modeset and are unchanged, so the
	 * async path pushes image_set + UPDATE only (mirrors nouveau
	 * nv50_wndw_flush_set gating each emitter on a dirty bit). */
	if (!async) {
		ret = nvkm_dispnv50_wndw_ntfy_enable(sc, state, wndw, &asyw);
		if (ret != 0)
			goto fail;
	}
	ret = wndw->func->image_set(wndw, &asyw);
	if (ret != 0)
		goto fail;
	if (!async) {
		ret = nvkm_dispnv50_wndw_ilut_set(sc, state, wndw, &asyw);
		if (ret != 0)
			goto fail;
		ret = wndw->func->blend_set(wndw, &asyw);
		if (ret != 0)
			goto fail;
	}

	commit_core = interlock[NV50_DISP_INTERLOCK_CORE] != 0;
	interlock[NV50_DISP_INTERLOCK_WNDW] |= wndw->interlock.data;
	ret = wndw->func->update(wndw, interlock);
	if (ret != 0)
		goto fail;
	if (commit_core) {
		ret = nvkm_dispnv50_core_commit_notify(sc, state, core,
		    interlock);
		if (ret != 0)
			goto fail;
	}

	/* Async (page-flip): the UPDATE is kicked; the HW latches the new
	 * scanout at the next vblank and the DRM flip event completes there
	 * (real vblank). Do not block the commit thread on the notifier or
	 * read back channel status. DRM serialises flips via the event. */
	if (async) {
		nvkm_dispnv50_audit_capture(state, &state->audit_pending, op,
		    head, (u32)wndw->id, display_id, true, false, false);
		return 0;
	}

	nvkm_dispnv50_dmac_trace_status(sc, &wndw->wndw, wndw->wndw.cur);
	if (!wndw->wndw.dfly_last_idle) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 window channel not idle after %s "
		    "win=%d stat=0x%08x\n", reason, wndw->id,
		    wndw->wndw.dfly_last_stat);
	}

	ret = nvkm_dispnv50_wndw_wait_armed(sc, state, wndw, &asyw);
	if (ret != 0)
		goto fail;

	nvkm_dispnv50_audit_capture(state, &state->audit_current, op, head,
	    (u32)wndw->id, display_id, false, true, false);
	memset(&state->audit_pending, 0, sizeof(state->audit_pending));

	nvkm_infof(sc->dev,
	    "drm: dispnv50 %s armed win=%d scanout=0x%llx offset=0x%llx "
	    "user=%d\n", reason, wndw->id,
	    (unsigned long long)(state->scanout_user ? state->scanout_offset :
	    nvkm_memory_addr(state->scanout)),
	    (unsigned long long)state->scanout_offset, state->scanout_user);
	if (!state->scanout_user) {
		ret = nvkm_dispnv50_console_register(sc, state);
		if (ret != 0)
			nvkm_infof(sc->dev,
			    "drm: dispnv50 console fb deferred err=%d\n", ret);
	}
	return 0;

fail:
	nvkm_infof(sc->dev,
	    "drm: dispnv50 %s failed win=%d err=%d\n",
	    reason, wndw != NULL ? wndw->id : -1, ret);
	if (ret == 0)
		return -ENODEV;
	return ret;
}

static int
nvkm_dispnv50_window_disable(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_core *core,
    struct nv50_wndw *wndw)
{
	struct nv50_wndw_atom asyw;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	u32 head = (u32)-1;
	u32 display_id = 0;
	int ret;

	(void)core;
	if (wndw->func->image_clr == NULL)
		return 0;

	memset(&asyw, 0, sizeof(asyw));
	ret = nvkm_dispnv50_wndw_ntfy_enable(sc, state, wndw, &asyw);
	if (ret != 0)
		goto fail;
	ret = wndw->func->image_clr(wndw);
	if (ret != 0)
		goto fail;

	interlock[NV50_DISP_INTERLOCK_WNDW] |= wndw->interlock.data;
	ret = wndw->func->update(wndw, interlock);
	if (ret != 0)
		goto fail;
	ret = nvkm_dispnv50_wndw_wait_armed(sc, state, wndw, &asyw);
	if (ret != 0)
		goto fail;

	if (state->audit_current.valid &&
	    state->audit_current.win == (u32)wndw->id) {
		head = state->audit_current.head;
		display_id = state->audit_current.display_id;
	}
	nvkm_dispnv50_audit_capture(state, &state->audit_current,
	    NVKM_DISPNV50_AUDIT_PLANE_DISABLE, head, (u32)wndw->id, display_id,
	    false, true, true);
	memset(&state->audit_pending, 0, sizeof(state->audit_pending));

	nvkm_infof(sc->dev, "drm: dispnv50 window disabled win=%d\n",
	    wndw->id);
	return 0;

fail:
	nvkm_infof(sc->dev,
	    "drm: dispnv50 window disable failed win=%d err=%d\n",
	    wndw != NULL ? wndw->id : -1, ret);
	if (ret == 0)
		return -ENODEV;
	return ret;
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
nvkm_dispnv50_cursor_update(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_wndw_atom asyw;
	struct nv50_head_atom asyh;
	struct nv50_wndw *curs;
	struct nv50_head *nvhead;
	struct nv50_core *core;
	struct nvkm_bo *bo = NULL;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	int ret;

	if (sc == NULL || crtc == NULL || sc->disp == NULL)
		return -ENODEV;

	ret = nvkm_dispnv50_head_init(sc, head);
	if (ret != 0)
		return ret;
	ret = nvkm_dispnv50_cursor_init(sc, head);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	if (state == NULL || state->disp.core == NULL ||
	    head >= nitems(state->head) || head >= nitems(state->curs))
		return -ENODEV;

	core = state->disp.core;
	nvhead = &state->head[head];
	curs = state->curs[head];
	if (nvhead->func == NULL || nvhead->func->curs_set == NULL ||
	    nvhead->func->curs_layout == NULL ||
	    nvhead->func->curs_format == NULL || curs == NULL ||
	    curs->immd == NULL || curs->immd->point == NULL ||
	    curs->immd->update == NULL)
		return -ENODEV;

	ret = nvkm_dispnv50_cursor_atom_fill(sc, state, crtc, core, nvhead,
	    &asyw, &asyh, &bo);
	if (ret != 0)
		return ret;

	ret = nvhead->func->curs_set(nvhead, &asyh);
	if (ret != 0)
		return ret;
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	interlock[NV50_DISP_INTERLOCK_CURS] |= curs->interlock.data;
	ret = nvkm_dispnv50_core_commit_notify(sc, state, core, interlock);
	if (ret != 0)
		return ret;

	ret = curs->immd->point(curs, &asyw);
	if (ret != 0)
		return ret;
	ret = curs->immd->update(curs, interlock);
	if (ret != 0)
		return ret;

	nvkm_dispnv50_cursor_audit_capture(state, crtc->cursor->state, bo,
	    head, true, false);
	nvkm_infof(sc->dev,
	    "drm: dispnv50 cursor update head=%u pos=%d,%d size=%ux%u "
	    "offset=0x%llx bo=%p\n",
	    head, asyw.state.crtc_x, asyw.state.crtc_y,
	    asyw.state.fb->width, asyw.state.fb->height,
	    (unsigned long long)asyh.curs.offset, bo);
	return 0;
}

int
nvkm_dispnv50_cursor_async_update(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, int32_t x, int32_t y)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_wndw_atom asyw;
	struct nv50_wndw *curs;
	struct nvkm_bo *bo = NULL;
	struct drm_plane_state *plane_state;
	struct drm_framebuffer *fb;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	int ret;

	if (sc == NULL || crtc == NULL || crtc->cursor == NULL ||
	    crtc->cursor->state == NULL || sc->disp == NULL)
		return -ENODEV;

	ret = nvkm_dispnv50_cursor_init(sc, head);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	if (state == NULL || head >= nitems(state->curs) ||
	    state->curs[head] == NULL)
		return -ENODEV;

	curs = state->curs[head];
	if (curs->immd == NULL || curs->immd->point == NULL ||
	    curs->immd->update == NULL)
		return -ENODEV;

	plane_state = crtc->cursor->state;
	fb = plane_state->fb;
	if (fb != NULL && fb->obj[0] != NULL)
		bo = to_nvkm_bo(fb->obj[0]);

	memset(&asyw, 0, sizeof(asyw));
	asyw.state = *plane_state;
	asyw.state.crtc_x = x;
	asyw.state.crtc_y = y;
	asyw.point.x = (u16)x;
	asyw.point.y = (u16)y;

	ret = curs->immd->point(curs, &asyw);
	if (ret != 0)
		return ret;
	ret = curs->immd->update(curs, interlock);
	if (ret != 0)
		return ret;

	nvkm_dispnv50_cursor_audit_capture(state, plane_state, bo, head, true,
	    true);
	state->audit_cursor[head].x = x;
	state->audit_cursor[head].y = y;
	return 0;
}

int
nvkm_dispnv50_cursor_disable(struct nvkm_softc *sc, uint32_t head)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_head *nvhead;
	struct nv50_core *core;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	int ret;

	if (sc == NULL || sc->disp == NULL)
		return -ENODEV;

	ret = nvkm_dispnv50_head_init(sc, head);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	if (state == NULL || state->disp.core == NULL ||
	    head >= nitems(state->head))
		return -ENODEV;

	core = state->disp.core;
	nvhead = &state->head[head];
	if (nvhead->func == NULL || nvhead->func->curs_clr == NULL)
		return -ENODEV;

	ret = nvhead->func->curs_clr(nvhead);
	if (ret != 0)
		return ret;
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	interlock[NV50_DISP_INTERLOCK_CURS] |=
	    (head < nitems(state->curs) && state->curs[head] != NULL) ?
	    state->curs[head]->interlock.data : BIT(head);
	ret = nvkm_dispnv50_core_commit_notify(sc, state, core, interlock);
	if (ret != 0)
		return ret;

	nvkm_dispnv50_cursor_audit_capture(state, NULL, NULL, head, false,
	    false);
	nvkm_infof(sc->dev, "drm: dispnv50 cursor disabled head=%u\n", head);
	return 0;
}

int
nvkm_dispnv50_plane_update(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t win)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_wndw *wndw;
	struct nv50_core *core;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	int ret;

	if (sc == NULL || crtc == NULL || crtc->state == NULL || sc->disp == NULL)
		return -ENODEV;

	ret = nvkm_dispnv50_wndw_init(sc, win);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	if (state == NULL || state->disp.core == NULL ||
	    win >= nitems(state->wndw) || state->wndw[win] == NULL)
		return -ENODEV;

	core = state->disp.core;
	wndw = state->wndw[win];
	ret = nvkm_dispnv50_select_scanout(sc, state, crtc, true);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 plane scanout select failed win=%u "
		    "format=0x%08x err=%d\n", win, state->scanout_format, ret);
		return ret;
	}

	return nvkm_dispnv50_window_program(sc, state, crtc, core, wndw,
	    interlock, false, true, NVKM_DISPNV50_AUDIT_PLANE_UPDATE,
	    (u32)drm_crtc_index(crtc), 0, "plane update");
}

int
nvkm_dispnv50_plane_disable(struct nvkm_softc *sc, uint32_t win)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_wndw *wndw;
	struct nv50_core *core;
	int ret;

	if (sc == NULL || sc->disp == NULL)
		return -ENODEV;

	ret = nvkm_dispnv50_wndw_init(sc, win);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	if (state == NULL || state->disp.core == NULL ||
	    win >= nitems(state->wndw) || state->wndw[win] == NULL)
		return -ENODEV;

	core = state->disp.core;
	wndw = state->wndw[win];
	return nvkm_dispnv50_window_disable(sc, state, core, wndw);
}

int
nvkm_dispnv50_atomic_enable(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, uint32_t win, uint32_t display_id,
    const struct nvkm_dispnv50_hdmi_info *hdmi)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_head_atom asyh;
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

	ret = nvkm_dispnv50_select_scanout(sc, state, crtc, true);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 scanout select failed head=%u win=%u "
		    "format=0x%08x err=%d\n",
		    head, win, state->scanout_format, ret);
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

	ret = nvkm_dispnv50_window_program(sc, state, crtc, core, wndw,
	    interlock, true, false, NVKM_DISPNV50_AUDIT_ATOMIC_ENABLE,
	    head, display_id, "bridge");
	if (ret != 0)
		goto fail;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 bridge armed head=%u win=%u display=0x%x "
	    "scanout=0x%llx offset=0x%llx\n",
	    head, win, display_id,
	    (unsigned long long)(state->scanout_user ? state->scanout_offset :
	    nvkm_memory_addr(state->scanout)),
	    (unsigned long long)state->scanout_offset);
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
