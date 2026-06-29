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
#include <linux/math64.h>
#include <nouveau_bo.h>
#include <nvif/cl0002.h>
#include <nvif/class.h>
#include <nvif/if0014.h>
#include <nvhw/class/clc37d.h>
#include <nvhw/class/clc37e.h>
#include <nvhw/class/clc57e.h>
#include <subdev/bios/dcb.h>

/* drm_dp_helper.h conflicts with DragonFly's imported display/drm_dp.h here. */
bool drm_dp_channel_eq_ok(const u8 link_status[DP_LINK_STATUS_SIZE],
    int lane_count);
int drm_dp_downstream_max_clock(const u8 dpcd[DP_RECEIVER_CAP_SIZE],
    const u8 port_cap[4]);

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
#define NVKM_DISPNV50_DP_TU_SIZE	64U
#define NVKM_DISPNV50_DP_WM_ADJUST	2U
#define NVKM_DISPNV50_DP_WM_LIMIT	20U
#define NVKM_DISPNV50_DP_WM_ADJUST_INC	8U
#define NVKM_DISPNV50_DP_WM_LIMIT_INC	22U
#define NVKM_DISPNV50_DP_PRECISION	100000U

enum nvkm_dispnv50_audit_op {
	NVKM_DISPNV50_AUDIT_NONE = 0,
	NVKM_DISPNV50_AUDIT_ATOMIC_ENABLE,
	NVKM_DISPNV50_AUDIT_CRTC_DISABLE,
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
	int32_t crtc_x;
	int32_t crtc_y;
	u32 crtc_w;
	u32 crtc_h;
	u32 src_x;
	u32 src_y;
	u32 src_w;
	u32 src_h;
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

/*
 * Window resources that have been accepted by a window UPDATE.
 *
 * Ownership:
 *   The parent nvkm_dispnv50_state owns one record per window id.  The record
 *   stores only scalar resource state; it owns no notifier buffer, LUT memory,
 *   fb, BO, channel, or KMS object reference.
 *
 * Lifetime:
 *   A record becomes valid after nvkm submits a window transaction.  It tracks
 *   the last hardware-visible resource set/clear that nvkm emitted, not any
 *   firmware/GOP state imported before the first sanitize transaction.
 *
 * Threading:
 *   Updated from serialized KMS commit paths and read locklessly by sysctl
 *   diagnostics.  It is a local mirror of nouveau's armed window atom and must
 *   not be used as a synchronization primitive.
 */
struct nvkm_dispnv50_wndw_armed {
	bool valid;
	bool ntfy;
	bool sema;
	bool xlut;
	bool csc;
	bool image;
};

/*
 * Head resources that have been accepted by a core UPDATE.
 *
 * Ownership:
 *   The parent nvkm_dispnv50_state owns one record per HEAD id.  The record
 *   stores only scalar resource state; it owns no output, IOR, LUT memory,
 *   cursor BO, channel, or KMS object reference.
 *
 * Lifetime:
 *   A record becomes valid after nvkm submits a HEAD transaction.  It tracks
 *   the last hardware-visible resource set/clear that nvkm emitted, mirroring
 *   nouveau's armed head atom for clear-mask decisions.
 *
 * Threading:
 *   Updated from serialized KMS commit paths and read locklessly by sysctl
 *   diagnostics.  It is diagnostic and method-selection state, not a
 *   synchronization primitive.
 */
struct nvkm_dispnv50_head_armed {
	bool valid;
	bool display;
	bool output;
	bool olut;
	bool cursor;
};

struct nvkm_dispnv50_state {
	struct nv50_disp disp;
	struct nvif_disp ifdisp;
	struct nouveau_bo sync_bo;
	struct nvkm_memory *sync_mem;
	struct nv50_head head[NVKM_DISPLAY_MAX_HEADS];
	struct nv50_wndw *wndw[NVKM_DISPLAY_MAX_WINDOWS];
	struct nv50_wndw *curs[NVKM_DISPLAY_MAX_CURSORS];
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
	int32_t window_crtc_x;
	int32_t window_crtc_y;
	u32 window_crtc_w;
	u32 window_crtc_h;
	u32 window_src_x;
	u32 window_src_y;
	u32 window_src_w;
	u32 window_src_h;
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
	struct nvkm_dispnv50_cursor_audit audit_cursor[NVKM_DISPLAY_MAX_CURSORS];
	struct nvkm_dispnv50_head_armed head_armed[NVKM_DISPLAY_MAX_HEADS];
	struct nvkm_dispnv50_wndw_armed wndw_armed[NVKM_DISPLAY_MAX_WINDOWS];
};

static uint32_t
nvkm_dispnv50_cap_min(uint32_t value, uint32_t cap)
{
	if (cap != 0 && value > cap)
		return (cap);
	return (value);
}

static uint32_t
nvkm_dispnv50_head_capacity(struct nvkm_softc *sc)
{
	uint32_t heads = NVKM_DISPLAY_MAX_HEADS;

	if (sc != NULL && sc->chip != NULL)
		heads = nvkm_dispnv50_cap_min(heads, sc->chip->display_heads);
	return (heads);
}

static uint32_t
nvkm_dispnv50_window_capacity(struct nvkm_softc *sc)
{
	uint32_t windows = NVKM_DISPLAY_MAX_WINDOWS;

	if (sc != NULL && sc->chip != NULL)
		windows = nvkm_dispnv50_cap_min(windows, sc->chip->display_windows);
	return (windows);
}

static uint32_t
nvkm_dispnv50_cursor_capacity(struct nvkm_softc *sc)
{
	uint32_t cursors = NVKM_DISPLAY_MAX_CURSORS;

	if (sc != NULL && sc->chip != NULL)
		cursors = nvkm_dispnv50_cap_min(cursors, sc->chip->display_cursors);
	return (cursors);
}

static uint32_t
nvkm_dispnv50_sor_capacity(struct nvkm_softc *sc)
{
	uint32_t sors = NVKM_DISPLAY_MAX_SORS;

	if (sc != NULL && sc->chip != NULL)
		sors = nvkm_dispnv50_cap_min(sors, sc->chip->display_sors);
	return (sors);
}

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
		idle = user_put == user_get &&
		    (((stat & 0x001f0000) == 0x000b0000) ||
		    ((stat & 0x001f0000) == 0x000c0000));
		valid = true;
		break;
	case 0x7e:
		channel = 1 + dmac->dfly_inst;
		ctrl = nvkm_rd32(sc, 0x6104e0 + channel * 4);
		stat = nvkm_rd32(sc, 0x610664 + (channel - 1) * 4);
		idle = user_put == user_get &&
		    ((stat & 0x000f0000) == 0x00040000);
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
	case NVKM_DISPNV50_AUDIT_CRTC_DISABLE:
		return "crtc_disable";
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
	snap->crtc_x = state->window_crtc_x;
	snap->crtc_y = state->window_crtc_y;
	snap->crtc_w = state->window_crtc_w;
	snap->crtc_h = state->window_crtc_h;
	snap->src_x = state->window_src_x;
	snap->src_y = state->window_src_y;
	snap->src_w = state->window_src_w;
	snap->src_h = state->window_src_h;
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
	sbuf_printf(sb, "%s_crtc_rect = %d,%d %ux%u\n", name,
	    snap->crtc_x, snap->crtc_y, snap->crtc_w, snap->crtc_h);
	sbuf_printf(sb, "%s_source_rect = %u,%u %ux%u\n", name,
	    snap->src_x >> 16, snap->src_y >> 16, snap->src_w >> 16,
	    snap->src_h >> 16);
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
	bool cursor_enabled;

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
	cursor_enabled = cursor_state != NULL && cursor_state->visible &&
	    cursor_fb != NULL;

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
	    cursor_enabled);
	sbuf_printf(sb, "head[%u]_armed_valid = %d\n", head,
	    head < nitems(state->head_armed) ? state->head_armed[head].valid :
	    false);
	sbuf_printf(sb, "head[%u]_armed_display = %d\n", head,
	    head < nitems(state->head_armed) ? state->head_armed[head].display :
	    false);
	sbuf_printf(sb, "head[%u]_armed_output = %d\n", head,
	    head < nitems(state->head_armed) ? state->head_armed[head].output :
	    false);
	sbuf_printf(sb, "head[%u]_armed_olut = %d\n", head,
	    head < nitems(state->head_armed) ? state->head_armed[head].olut :
	    false);
	sbuf_printf(sb, "head[%u]_armed_cursor = %d\n", head,
	    head < nitems(state->head_armed) ? state->head_armed[head].cursor :
	    false);
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
		sbuf_printf(sb, "wndw[%u]_armed_valid = %d\n", i,
		    state->wndw_armed[i].valid);
		sbuf_printf(sb, "wndw[%u]_armed_ntfy = %d\n", i,
		    state->wndw_armed[i].ntfy);
		sbuf_printf(sb, "wndw[%u]_armed_sema = %d\n", i,
		    state->wndw_armed[i].sema);
		sbuf_printf(sb, "wndw[%u]_armed_xlut = %d\n", i,
		    state->wndw_armed[i].xlut);
		sbuf_printf(sb, "wndw[%u]_armed_csc = %d\n", i,
		    state->wndw_armed[i].csc);
		sbuf_printf(sb, "wndw[%u]_armed_image = %d\n", i,
		    state->wndw_armed[i].image);
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
	sbuf_printf(sb, "display_head_capacity = %u\n",
	    nvkm_dispnv50_head_capacity(sc));
	sbuf_printf(sb, "display_window_capacity = %u\n",
	    nvkm_dispnv50_window_capacity(sc));
	sbuf_printf(sb, "display_cursor_capacity = %u\n",
	    nvkm_dispnv50_cursor_capacity(sc));
	sbuf_printf(sb, "display_sor_capacity = %u\n",
	    nvkm_dispnv50_sor_capacity(sc));
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
	sbuf_printf(sb, "scanout_source_rect = %u,%u %ux%u\n",
	    state->window_src_x >> 16, state->window_src_y >> 16,
	    state->window_src_w >> 16, state->window_src_h >> 16);
	sbuf_printf(sb, "scanout_crtc_rect = %d,%d %ux%u\n",
	    state->window_crtc_x, state->window_crtc_y,
	    state->window_crtc_w, state->window_crtc_h);
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

static void
nvkm_dispnv50_dmac_destroy(struct nv50_dmac *dmac)
{
	struct nvkm_softc *sc;
	int ret;

	if (dmac == NULL)
		return;

	sc = dmac->dfly_sc;
	nvkm_dispnv50_ctxdma_drop(&dmac->dfly_fb_object);
	nvkm_dispnv50_ctxdma_drop(&dmac->dfly_fb_blocklinear_object);
	nvkm_dispnv50_ctxdma_drop(&dmac->dfly_vram_object);
	nvkm_dispnv50_ctxdma_drop(&dmac->dfly_sync_object);

	if (dmac->dfly_object != NULL) {
		if (dmac->dfly_object->client != NULL &&
		    dmac->dfly_object->handle != 0) {
			ret = nvkm_gsp_rm_free(dmac->dfly_object);
			if (ret != 0 && sc != NULL)
				nvkm_infof(sc->dev,
				    "drm: dispnv50 dmac free failed "
				    "class=0x%x inst=%d handle=0x%x err=%d\n",
				    dmac->dfly_oclass, dmac->dfly_inst,
				    dmac->dfly_object->handle, ret);
			dmac->dfly_object->handle = 0;
		}
		kfree(dmac->dfly_object);
		dmac->dfly_object = NULL;
	}

	nvkm_memory_unref(&dmac->dfly_push_mem);
	kfree(dmac->dfly_shadow);
	dmac->dfly_shadow = NULL;
	memset(dmac, 0, sizeof(*dmac));
}

static void
nvkm_dispnv50_wndw_destroy(struct nv50_wndw **pwndw)
{
	struct nv50_wndw *wndw;

	if (pwndw == NULL || *pwndw == NULL)
		return;

	wndw = *pwndw;
	nvkm_dispnv50_dmac_destroy(&wndw->wimm);
	nvkm_dispnv50_dmac_destroy(&wndw->wndw);
	kfree(wndw);
	*pwndw = NULL;
}

static void
nvkm_dispnv50_core_destroy(struct nv50_core **pcore)
{
	struct nv50_core *core;

	if (pcore == NULL || *pcore == NULL)
		return;

	core = *pcore;
	nvkm_dispnv50_dmac_destroy(&core->chan);
	kfree(core);
	*pcore = NULL;
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

static void
nvkm_dispnv50_dmac_dump_fetch_window(struct nvkm_softc *sc,
    struct nv50_dmac *dmac, u32 user_get, u32 user_put)
{
	const char *label = nvkm_dispnv50_dmac_label(dmac);
	u32 get = user_get >> 2;
	u32 put = user_put >> 2;
	u32 start;
	u32 end;
	u32 i;

	if (dmac->dfly_shadow == NULL || dmac->dfly_push_mem == NULL ||
	    dmac->max == 0)
		return;
	if (get >= dmac->max)
		return;

	start = get > 4 ? get - 4 : 0;
	end = get + 8;
	if (end > dmac->max)
		end = dmac->max;

	nvkm_infof(sc->dev,
	    "drm: dispnv50 %s fetch-window get=0x%08x(%u) "
	    "put=0x%08x(%u)\n", label, user_get, get, user_put, put);
	for (i = start; i < end; i++) {
		u32 shadow = dmac->dfly_shadow[i];
		u32 vram = nvkm_ro32(dmac->dfly_push_mem, i * 4);

		nvkm_infof(sc->dev,
		    "drm: dispnv50 %s fetch[%03u] shadow=0x%08x "
		    "vram=0x%08x%s%s\n", label, i, shadow, vram,
		    i == get ? " <GET>" : "", i == put ? " <PUT>" : "");
	}
}

static bool
nvkm_dispnv50_dmac_read_status(struct nvkm_softc *sc, struct nv50_dmac *dmac,
    u32 *user_put, u32 *user_get, u32 *ctrl, u32 *stat)
{
	switch (dmac->dfly_oclass & 0xff) {
	case 0x7d:
		*user_put = nvkm_rd32(sc, dmac->dfly_user + 0x00);
		*user_get = nvkm_rd32(sc, dmac->dfly_user + 0x04);
		*ctrl = nvkm_rd32(sc, 0x6104e0);
		*stat = nvkm_rd32(sc, 0x610630);
		return true;
	case 0x7e: {
		u32 channel = 1 + dmac->dfly_inst;

		*user_put = nvkm_rd32(sc, dmac->dfly_user + 0x00);
		*user_get = nvkm_rd32(sc, dmac->dfly_user + 0x04);
		*ctrl = nvkm_rd32(sc, 0x6104e0 + channel * 4);
		*stat = nvkm_rd32(sc, 0x610664 + (channel - 1) * 4);
		return true;
	}
	default:
		return false;
	}
}

static bool
nvkm_dispnv50_dmac_status_idle(struct nv50_dmac *dmac, u32 user_put,
    u32 user_get, u32 stat)
{
	if (user_put != user_get)
		return false;
	switch (dmac->dfly_oclass & 0xff) {
	case 0x7d:
		return (((stat & 0x001f0000) == 0x000b0000) ||
		    ((stat & 0x001f0000) == 0x000c0000));
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
	u32 user_get = 0;
	u32 ctrl = 0;
	u32 stat = 0;
	u32 attempt;
	u32 attempts;
	bool idle = false;

	for (attempt = 0; attempt < NVKM_DISPNV50_STATUS_POLL_COUNT; attempt++) {
		if (!nvkm_dispnv50_dmac_read_status(sc, dmac, &user_put,
		    &user_get, &ctrl, &stat)) {
			dmac->dfly_last_idle = false;
			dmac->dfly_last_stat = 0;
			return;
		}
		idle = nvkm_dispnv50_dmac_status_idle(dmac, user_put, user_get,
		    stat);
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
		    "user_get=0x%08x ctrl=0x%08x stat=0x%08x idle=%u "
		    "attempts=%u\n",
		    label, cur, user_put, user_get, ctrl, stat, idle, attempts);
	}
	if (!idle)
		nvkm_dispnv50_dmac_dump_fetch_window(sc, dmac, user_get,
		    user_put);

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

	/*
	 * The EVO fetcher observes the push buffer through display DMA, not
	 * through the CPU shadow.  Order all shadow-to-pushbuf stores before
	 * publishing PUT, matching nouveau's DMA kick contract.  The BAR1 read
	 * keeps the existing VRAM/BAR1 flush behaviour for VRAM-backed memory.
	 */
	cpu_sfence();
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
	push->bgn = push->cur;
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

	if ((oclass[0] & 0xff) == 0x7a) {
		/*
		 * Cursor channels are PIO immediate channels in nouveau/r535:
		 * RM receives a NULL pushbuf and allocates a
		 * CHANNELPIO_ALLOCATION object.  The imported cursc37a emitter
		 * writes the BAR user aperture directly with NVIF_WR32(), so
		 * a shadow push buffer would be both unused and semantically
		 * wrong.
		 */
		dmac->dfly_object = kzalloc(sizeof(*dmac->dfly_object),
		    GFP_KERNEL);
		if (dmac->dfly_object == NULL) {
			ret = -ENOMEM;
			goto fail;
		}

		ret = nvkm_gsp_disp_pio_alloc(sc, oclass[0], inst,
		    dmac->dfly_object);
		if (ret) {
			nvkm_infof(sc->dev,
			    "drm: dispnv50 pio alloc failed class=0x%x "
			    "inst=%d err=%d\n", oclass[0], inst, ret);
			goto fail;
		}

		nvkm_infof(sc->dev,
		    "drm: dispnv50 pio class=0x%x inst=%d user=0x%x\n",
		    oclass[0], inst, user);
		return 0;
	}

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
#ifdef NVKM_DFLY_GSP_DISPLAY_ONLY
	core->dfly_window_count = nvkm_dispnv50_window_capacity(disp->dfly_sc);
#endif

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
		nvkm_dispnv50_core_destroy(&state->disp.core);
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

	if (win >= nvkm_dispnv50_window_capacity(sc))
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
		nvkm_dispnv50_wndw_destroy(&state->wndw[win]);
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

	if (head >= nvkm_dispnv50_head_capacity(sc))
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

	if (head >= nvkm_dispnv50_cursor_capacity(sc))
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
		nvkm_dispnv50_wndw_destroy(&state->curs[head]);
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

static uint8_t
nvkm_dispnv50_crtc_scanout_depth(struct drm_crtc *crtc)
{
	struct drm_plane_state *plane_state;
	struct drm_framebuffer *fb;

	if (crtc == NULL || crtc->primary == NULL)
		return (24);
	plane_state = crtc->primary->state;
	if (plane_state == NULL)
		return (24);
	fb = plane_state->fb;
	if (fb == NULL || fb->format == NULL || fb->format->depth == 0)
		return (24);
	return (fb->format->depth);
}

static uint32_t
nvkm_dispnv50_u32_min(uint32_t a, uint32_t b)
{
	return (a < b ? a : b);
}

static uint32_t
nvkm_dispnv50_view_subtract_border(uint32_t value, uint32_t border)
{
	uint32_t delta;

	if (border > UINT32_MAX / 2U)
		return (1);
	delta = border * 2U;
	if (value <= delta)
		return (1);
	return (value - delta);
}

static uint32_t
nvkm_dispnv50_view_rescale(uint32_t value, uint32_t numerator,
    uint32_t denominator)
{
	uint64_t ratio;

	if (denominator == 0)
		return (value);
	ratio = ((uint64_t)numerator << 19) / denominator;
	return ((uint32_t)((value * ratio + ratio / 2U) >> 19));
}

/*
 * Apply connector scaler and underscan state to the head view rectangle.
 *
 * Ownership:
 *   Borrows the atomic head atom and scalar head config for this commit only.
 *   The bridge never stores connector or DRM state pointers.
 *
 * Lifetime:
 *   Must run after nvkm_dispnv50_head_atom_fill() populated mode snapshots and
 *   before the head view method is emitted.
 *
 * Threading:
 *   Commit-tail local. The caller holds the modeset serialization required for
 *   programming a display transaction.
 */
static void
nvkm_dispnv50_head_apply_view(struct nv50_head_atom *asyh,
    const struct nvkm_dispnv50_head_config *config)
{
	const struct drm_display_mode *user = &asyh->state.mode;
	const struct drm_display_mode *output = &asyh->state.adjusted_mode;
	int output_width;
	int output_height;
	uint32_t user_height;
	uint32_t scaling_mode;
	bool underscan;

	user_height = user->vdisplay;
	if ((user->flags & DRM_MODE_FLAG_3D_MASK) ==
	    DRM_MODE_FLAG_3D_FRAME_PACKING)
		user_height += user->vtotal;

	asyh->view.iW = user->hdisplay;
	asyh->view.iH = user_height;
	drm_mode_get_hv_timing(output, &output_width, &output_height);
	asyh->view.oW = output_width > 0 ? (uint32_t)output_width : 0;
	asyh->view.oH = output_height > 0 ? (uint32_t)output_height : 0;

	if (config == NULL)
		return;

	underscan = config->underscan_mode == NVKM_DISPNV50_UNDERSCAN_ON ||
	    (config->underscan_mode == NVKM_DISPNV50_UNDERSCAN_AUTO &&
	    config->underscan_auto_is_hdmi);
	if (underscan && asyh->view.oW != 0 && asyh->view.oH != 0) {
		uint32_t border_x = config->underscan_hborder;
		uint32_t border_y = config->underscan_vborder;
		uint32_t original_width = asyh->view.oW;
		uint32_t original_height = asyh->view.oH;

		if (border_x != 0)
			asyh->view.oW =
			    nvkm_dispnv50_view_subtract_border(asyh->view.oW,
			    border_x);
		else
			asyh->view.oW =
			    nvkm_dispnv50_view_subtract_border(asyh->view.oW,
			    (asyh->view.oW >> 4) + 32U);

		if (border_y != 0)
			asyh->view.oH =
			    nvkm_dispnv50_view_subtract_border(asyh->view.oH,
			    border_y);
		else
			asyh->view.oH = nvkm_dispnv50_view_rescale(
			    asyh->view.oW, original_height, original_width);
	}

	scaling_mode = config->scaling_mode;
	switch (scaling_mode) {
	case DRM_MODE_SCALE_CENTER:
		asyh->view.oW =
		    nvkm_dispnv50_u32_min(asyh->view.iW, asyh->view.oW);
		asyh->view.oH =
		    nvkm_dispnv50_u32_min(asyh->view.iH, asyh->view.oH);
		break;
	case DRM_MODE_SCALE_ASPECT:
		if (asyh->view.iW == 0 || asyh->view.iH == 0 ||
		    asyh->view.oW == 0 || asyh->view.oH == 0)
			break;
		if ((uint64_t)asyh->view.oW * asyh->view.iH >
		    (uint64_t)asyh->view.iW * asyh->view.oH) {
			asyh->view.oW = nvkm_dispnv50_view_rescale(
			    asyh->view.oH, asyh->view.iW, asyh->view.iH);
		} else {
			asyh->view.oH = nvkm_dispnv50_view_rescale(
			    asyh->view.oW, asyh->view.iH, asyh->view.iW);
		}
		break;
	default:
		break;
	}
}

static void
nvkm_dispnv50_head_apply_config(struct nv50_head_atom *asyh,
    struct drm_crtc *crtc, const struct nvkm_dispnv50_head_config *config)
{
	uint32_t mode;

	if (config == NULL) {
		asyh->or.bpc = 8;
		return;
	}

	asyh->or.bpc = config->bpc != 0 ? config->bpc : 8;
	mode = config->dither_mode;
	if (mode == NVKM_DISPNV50_DITHER_MODE_AUTO) {
		if (nvkm_dispnv50_crtc_scanout_depth(crtc) > asyh->or.bpc * 3U)
			mode = NVKM_DISPNV50_DITHER_MODE_DYNAMIC2X2;
		else
			mode = NVKM_DISPNV50_DITHER_MODE_OFF;
	}

	if (mode == NVKM_DISPNV50_DITHER_MODE_OFF) {
		asyh->dither.enable =
		    NVC37D_HEAD_SET_DITHER_CONTROL_ENABLE_DISABLE;
		asyh->dither.bits = 0;
		asyh->dither.mode = 0;
		return;
	}

	asyh->dither.enable = NVC37D_HEAD_SET_DITHER_CONTROL_ENABLE_ENABLE;
	switch (mode) {
	case NVKM_DISPNV50_DITHER_MODE_STATIC2X2:
		asyh->dither.mode =
		    NVC37D_HEAD_SET_DITHER_CONTROL_MODE_STATIC_2X2;
		break;
	case NVKM_DISPNV50_DITHER_MODE_TEMPORAL:
		asyh->dither.mode =
		    NVC37D_HEAD_SET_DITHER_CONTROL_MODE_TEMPORAL;
		break;
	case NVKM_DISPNV50_DITHER_MODE_ON:
	case NVKM_DISPNV50_DITHER_MODE_DYNAMIC2X2:
	default:
		asyh->dither.mode =
		    NVC37D_HEAD_SET_DITHER_CONTROL_MODE_DYNAMIC_2X2;
		break;
	}

	if (config->dither_depth == NVKM_DISPNV50_DITHER_DEPTH_6BPC)
		asyh->dither.bits =
		    NVC37D_HEAD_SET_DITHER_CONTROL_BITS_TO_6_BITS;
	else if (config->dither_depth == NVKM_DISPNV50_DITHER_DEPTH_8BPC ||
	    asyh->or.bpc >= 8)
		asyh->dither.bits =
		    NVC37D_HEAD_SET_DITHER_CONTROL_BITS_TO_8_BITS;
	else
		asyh->dither.bits =
		    NVC37D_HEAD_SET_DITHER_CONTROL_BITS_TO_6_BITS;
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
		state->window_crtc_x = 0;
		state->window_crtc_y = 0;
		state->window_crtc_w = width;
		state->window_crtc_h = height;
		state->window_src_x = 0;
		state->window_src_y = 0;
		state->window_src_w = width << 16;
		state->window_src_h = height << 16;
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
	state->window_crtc_x = 0;
	state->window_crtc_y = 0;
	state->window_crtc_w = width;
	state->window_crtc_h = height;
	state->window_src_x = 0;
	state->window_src_y = 0;
	state->window_src_w = width << 16;
	state->window_src_h = height << 16;
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
	struct drm_plane_state *pstate;
	struct nvkm_bo *bo;
	u64 min_size;
	u32 cpp;

	(void)sc;
	if (crtc->primary == NULL || crtc->primary->state == NULL)
		return (ENOENT);
	pstate = crtc->primary->state;
	fb = pstate->fb;
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
	state->window_crtc_x = pstate->crtc_x;
	state->window_crtc_y = pstate->crtc_y;
	state->window_crtc_w = pstate->crtc_w;
	state->window_crtc_h = pstate->crtc_h;
	state->window_src_x = pstate->src_x;
	state->window_src_y = pstate->src_y;
	state->window_src_w = pstate->src_w;
	state->window_src_h = pstate->src_h;
	state->scanout_user = true;
	nvkm_infof(sc->dev,
	    "drm: dispnv50 user scanout %ux%u src=%u,%u %ux%u "
	    "dst=%d,%d %ux%u pitch=%u format=0x%08x modifier=0x%016llx "
	    "kind=0x%02x vram=0x%llx bo=%p\n",
	    state->scanout_width, state->scanout_height,
	    state->window_src_x >> 16, state->window_src_y >> 16,
	    state->window_src_w >> 16, state->window_src_h >> 16,
	    state->window_crtc_x, state->window_crtc_y,
	    state->window_crtc_w, state->window_crtc_h,
	    state->scanout_pitch, state->scanout_format,
	    (unsigned long long)state->scanout_modifier, state->scanout_kind,
	    (unsigned long long)state->scanout_offset,
	    bo);
	return (0);
}

/*
 * Return whether the currently armed scanout belongs to userspace.
 *
 * Ownership:
 *   Borrows sc and the dispnv50 bridge state for one scalar read.  No display
 *   object, framebuffer, BO, or memory reference is retained.
 *
 * Lifetime:
 *   The return value is a momentary ownership snapshot.  Callers must still
 *   hold or acquire normal KMS modeset locks before changing scanout state.
 *
 * Threading:
 *   Intended for process-context KMS restore decisions.  The field is written
 *   by serialized KMS commit paths; readers use it only to choose a
 *   conservative full-modeset restore, never as a synchronization primitive.
 */
bool
nvkm_dispnv50_scanout_is_user(struct nvkm_softc *sc)
{
	struct nvkm_dispnv50_state *state;

	if (sc == NULL)
		return (false);
	state = sc->dispnv50;
	if (state == NULL)
		return (false);
	return (state->scanout_user);
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

/*
 * Ownership:
 *   Borrows the nvkm-owned scalar scanout/window snapshot and the committed
 *   CRTC state.
 * Lifetime:
 *   No DRM object, BO, or display pointer is retained after return.
 * Threading:
 *   Pure validation from serialized KMS commit paths. No locks, GSP RPCs, or
 *   hardware programming.
 */
static int
nvkm_dispnv50_window_source_validate(const struct nvkm_dispnv50_state *state,
    const struct drm_crtc *crtc)
{
	const struct drm_display_mode *mode;
	u32 src_x;
	u32 src_y;
	u32 src_w;
	u32 src_h;

	if (state == NULL || crtc == NULL || crtc->state == NULL)
		return (EINVAL);
	mode = &crtc->state->adjusted_mode;
	if (state->scanout_width == 0 || state->scanout_height == 0 ||
	    state->window_crtc_w == 0 || state->window_crtc_h == 0 ||
	    mode->hdisplay <= 0 || mode->vdisplay <= 0)
		return (EINVAL);
	if (state->window_crtc_x != 0 || state->window_crtc_y != 0)
		return (EINVAL);
	if (state->window_crtc_w != (u32)mode->hdisplay ||
	    state->window_crtc_h != (u32)mode->vdisplay)
		return (EINVAL);
	if ((state->window_src_x & 0xffffu) != 0 ||
	    (state->window_src_y & 0xffffu) != 0 ||
	    (state->window_src_w & 0xffffu) != 0 ||
	    (state->window_src_h & 0xffffu) != 0)
		return (EINVAL);

	src_x = state->window_src_x >> 16;
	src_y = state->window_src_y >> 16;
	src_w = state->window_src_w >> 16;
	src_h = state->window_src_h >> 16;
	if (src_w == 0 || src_h == 0)
		return (EINVAL);
	if (src_w != state->window_crtc_w || src_h != state->window_crtc_h)
		return (EINVAL);
	if (src_x > state->scanout_width || src_y > state->scanout_height ||
	    src_w > state->scanout_width - src_x ||
	    src_h > state->scanout_height - src_y)
		return (EINVAL);
	return (0);
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
	return (nvkm_dispnv50_window_source_validate(state, crtc));
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

static void
nvkm_dispnv50_lut_write_entry(struct nvkm_memory *memory, u64 offset,
    u16 red, u16 green, u16 blue)
{
	nvkm_wo32(memory, offset + 0, (u32)red | ((u32)green << 16));
	nvkm_wo32(memory, offset + 4, (u32)blue);
}

static u16
nvkm_dispnv50_lut_identity_u0_16(u32 index)
{
	return ((u16)((index << 16) >> 10));
}

static void
nvkm_dispnv50_ilut_load_blob(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, const struct drm_property_blob *blob)
{
	const struct drm_color_lut *lut = blob != NULL ? blob->data : NULL;
	u32 size = blob != NULL ? drm_color_lut_size(blob) :
	    NVKM_DISPNV50_ILUT_ENTRIES;
	u64 offset;
	u32 i;
	u16 red = 0, green = 0, blue = 0;

	for (i = 0; i < size; i++) {
		if (lut != NULL) {
			red = drm_color_lut_extract(lut[i].red, 16);
			green = drm_color_lut_extract(lut[i].green, 16);
			blue = drm_color_lut_extract(lut[i].blue, 16);
		} else {
			red = green = blue = nvkm_dispnv50_lut_identity_u0_16(i);
		}
		offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES + i) * 8ULL;
		nvkm_dispnv50_lut_write_entry(state->ilut, offset,
		    nvkm_dispnv50_fixed_u0_16_fp16(red),
		    nvkm_dispnv50_fixed_u0_16_fp16(green),
		    nvkm_dispnv50_fixed_u0_16_fp16(blue));
	}

	offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES + size) * 8ULL;
	nvkm_dispnv50_lut_write_entry(state->ilut, offset,
	    nvkm_dispnv50_fixed_u0_16_fp16(red),
	    nvkm_dispnv50_fixed_u0_16_fp16(green),
	    nvkm_dispnv50_fixed_u0_16_fp16(blue));
	nvkm_gsp_bar1_flush(sc);
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

static void
nvkm_dispnv50_olut_load_256(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, const struct drm_color_lut *lut)
{
	u64 offset;
	u32 i, step;
	u16 red, green, blue;
	u16 red_inc, green_inc, blue_inc;

	for (i = 0; i < 256; i++) {
		red = drm_color_lut_extract(lut[i].red, 16);
		green = drm_color_lut_extract(lut[i].green, 16);
		blue = drm_color_lut_extract(lut[i].blue, 16);
		red_inc = green_inc = blue_inc = 0;
		if (i + 1 < 256) {
			red_inc = (drm_color_lut_extract(lut[i + 1].red, 16) -
			    red) / 4;
			green_inc =
			    (drm_color_lut_extract(lut[i + 1].green, 16) -
			    green) / 4;
			blue_inc =
			    (drm_color_lut_extract(lut[i + 1].blue, 16) -
			    blue) / 4;
		}
		for (step = 0; step < 4; step++) {
			offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES +
			    (i * 4) + step) * 8ULL;
			nvkm_dispnv50_lut_write_entry(state->olut, offset,
			    red + red_inc * step, green + green_inc * step,
			    blue + blue_inc * step);
		}
	}

	offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES +
	    NVKM_DISPNV50_ILUT_ENTRIES) * 8ULL;
	nvkm_dispnv50_lut_write_entry(state->olut, offset, red, green, blue);
	nvkm_gsp_bar1_flush(sc);
}

static void
nvkm_dispnv50_olut_load_blob(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, const struct drm_property_blob *blob)
{
	const struct drm_color_lut *lut = blob != NULL ? blob->data : NULL;
	u32 size = blob != NULL ? drm_color_lut_size(blob) :
	    NVKM_DISPNV50_ILUT_ENTRIES;
	u64 offset;
	u32 i;
	u16 red = 0, green = 0, blue = 0;

	if (lut != NULL && size == 256) {
		nvkm_dispnv50_olut_load_256(sc, state, lut);
		return;
	}

	for (i = 0; i < size; i++) {
		if (lut != NULL) {
			red = drm_color_lut_extract(lut[i].red, 16);
			green = drm_color_lut_extract(lut[i].green, 16);
			blue = drm_color_lut_extract(lut[i].blue, 16);
		} else {
			red = green = blue = nvkm_dispnv50_lut_identity_u0_16(i);
		}
		offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES + i) * 8ULL;
		nvkm_dispnv50_lut_write_entry(state->olut, offset, red,
		    green, blue);
	}

	offset = (NVKM_DISPNV50_ILUT_VSS_ENTRIES + size) * 8ULL;
	nvkm_dispnv50_lut_write_entry(state->olut, offset, red, green, blue);
	nvkm_gsp_bar1_flush(sc);
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
	asyw->state.crtc_x = state->window_crtc_x;
	asyw->state.crtc_y = state->window_crtc_y;
	asyw->state.crtc_w = state->window_crtc_w;
	asyw->state.crtc_h = state->window_crtc_h;
	asyw->state.src_x = state->window_src_x;
	asyw->state.src_y = state->window_src_y;
	asyw->state.src_w = state->window_src_w;
	asyw->state.src_h = state->window_src_h;

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

static u32
nvkm_dispnv50_ctm_to_csc(u64 in)
{
	bool sign = (in & (1ULL << 63)) != 0;
	u32 integer = (u32)((in >> 32) & 0x7fffffffu);
	u32 fraction = (u32)in;
	u32 ret;

	if (integer >= 4)
		return ((1u << 18) - (sign ? 0u : 1u));

	ret = (integer << 16) | (fraction >> 16);
	if (sign)
		ret = (u32)(-((int32_t)ret));
	return (ret & 0x7ffffu);
}

static void
nvkm_dispnv50_ctm_fill(struct nv50_wndw_atom *asyw,
    const struct drm_color_ctm *ctm)
{
	int i, j;

	for (j = 0; j < 3; j++) {
		for (i = 0; i < 4; i++) {
			if (i == 3)
				asyw->csc.matrix[j * 4 + i] = 0;
			else
				asyw->csc.matrix[j * 4 + i] =
				    nvkm_dispnv50_ctm_to_csc(
					ctm->matrix[j * 3 + i]);
		}
	}
	asyw->csc.valid = true;
}

static struct nvkm_dispnv50_wndw_armed *
nvkm_dispnv50_wndw_armed_state(struct nvkm_dispnv50_state *state,
    const struct nv50_wndw *wndw);
static bool
nvkm_dispnv50_wndw_programs_xlut(const struct nv50_wndw *wndw);
static bool
nvkm_dispnv50_wndw_programs_csc(const struct nv50_wndw *wndw,
    const struct drm_crtc_state *crtc_state);

static int
nvkm_dispnv50_wndw_csc_set(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_wndw *wndw,
    struct nv50_wndw_atom *asyw, const struct drm_crtc_state *crtc_state)
{
	const struct nvkm_dispnv50_wndw_armed *armed =
	    nvkm_dispnv50_wndw_armed_state(state, wndw);
	const struct drm_color_ctm *ctm;

	if (crtc_state == NULL || crtc_state->ctm == NULL) {
		if (armed != NULL && armed->valid && armed->csc &&
		    wndw->func->csc_clr != NULL)
			return (wndw->func->csc_clr(wndw));
		return (0);
	}
	if (wndw->func->csc_set == NULL)
		return (-ENOSYS);

	ctm = crtc_state->ctm->data;
	memset(&asyw->csc, 0, sizeof(asyw->csc));
	nvkm_dispnv50_ctm_fill(asyw, ctm);
	sc->kms_color_ctm_count++;
	return (wndw->func->csc_set(wndw, asyw));
}

static int
nvkm_dispnv50_wndw_ilut_set(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_wndw *wndw,
    struct nv50_wndw_atom *asyw, const struct drm_crtc_state *crtc_state)
{
	const struct drm_property_blob *blob =
	    crtc_state != NULL ? crtc_state->degamma_lut : NULL;
	u32 size = blob != NULL ? drm_color_lut_size(blob) : 0;
	int ret;

	if (wndw->func->ilut == NULL || wndw->func->xlut_set == NULL ||
	    !wndw->func->ilut_identity)
		return 0;

	ret = nvkm_dispnv50_ilut_ensure(sc, state);
	if (ret != 0)
		return ret;

	memset(&asyw->xlut, 0, sizeof(asyw->xlut));
	wndw->func->ilut(wndw, asyw, size);
	nvkm_dispnv50_ilut_load_blob(sc, state, blob);
	if (blob != NULL)
		sc->kms_color_degamma_lut_count++;
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
    struct nv50_head *head, struct nv50_head_atom *asyh,
    const struct drm_crtc_state *crtc_state)
{
	const struct drm_property_blob *blob =
	    crtc_state != NULL ? crtc_state->gamma_lut : NULL;
	u32 size = blob != NULL ? drm_color_lut_size(blob) : 0;
	int ret;

	if (head->func->olut == NULL || head->func->olut_set == NULL ||
	    !head->func->olut_identity)
		return 0;

	ret = nvkm_dispnv50_olut_ensure(sc, state);
	if (ret != 0)
		return ret;
	if (!head->func->olut(head, asyh, size))
		return -EINVAL;
	nvkm_dispnv50_olut_load_blob(sc, state, blob);
	if (blob != NULL)
		sc->kms_color_gamma_lut_count++;

	asyh->olut.handle = core->chan.vram.handle;
	asyh->olut.offset = state->olut_offset;
	nvkm_infof(sc->dev,
	    "drm: dispnv50 head olut handle=0x%x offset=0x%llx "
	    "size=%u mode=%u output=%u\n",
	    asyh->olut.handle, (unsigned long long)asyh->olut.offset,
	    asyh->olut.size, asyh->olut.mode, asyh->olut.output_mode);
	return head->func->olut_set(head, asyh);
}

static struct nvkm_dispnv50_head_armed *
nvkm_dispnv50_head_armed_state(struct nvkm_dispnv50_state *state, u32 head)
{
	if (state == NULL || head >= nitems(state->head_armed))
		return NULL;
	return &state->head_armed[head];
}

static bool
nvkm_dispnv50_head_programs_olut(const struct nv50_head *head)
{
	return (head != NULL && head->func != NULL &&
	    head->func->olut != NULL && head->func->olut_set != NULL &&
	    head->func->olut_identity);
}

/*
 * Publish the resource state produced by a successful full HEAD program.
 *
 * Ownership:
 *   Mutably borrows the per-head armed record from state.  It copies only scalar
 *   method state and keeps no reference to nvhead, output, IOR, or KMS state.
 *
 * Lifetime:
 *   Call only after the matching core/window UPDATE path has succeeded.  Cursor
 *   visibility is preserved because full modeset enable does not program a new
 *   cursor image; cursor plane commits update it through cursor-specific helpers.
 *
 * Threading:
 *   Serialized KMS commit context only; sysctl may observe intermediate values.
 */
static void
nvkm_dispnv50_head_mark_programmed(struct nvkm_dispnv50_state *state, u32 head,
    const struct nv50_head *nvhead, uint32_t display_id, bool output)
{
	struct nvkm_dispnv50_head_armed *armed;
	bool cursor;

	armed = nvkm_dispnv50_head_armed_state(state, head);
	if (armed == NULL)
		return;

	cursor = armed->valid && armed->cursor;
	armed->valid = true;
	armed->display = display_id != 0;
	armed->output = output;
	armed->olut = nvkm_dispnv50_head_programs_olut(nvhead);
	armed->cursor = cursor;
}

/*
 * Publish a successful head-OLUT update without changing routing ownership.
 *
 * Ownership:
 *   Mutably borrows only the per-head armed record.  No LUT memory or CRTC
 *   state reference is retained.
 *
 * Lifetime:
 *   Call after the core UPDATE that carries the OLUT method has succeeded.
 *
 * Threading:
 *   Serialized KMS commit context only.
 */
static void
nvkm_dispnv50_head_mark_olut_programmed(struct nvkm_dispnv50_state *state,
    u32 head, const struct nv50_head *nvhead)
{
	struct nvkm_dispnv50_head_armed *armed;

	armed = nvkm_dispnv50_head_armed_state(state, head);
	if (armed == NULL)
		return;
	armed->valid = true;
	armed->olut = nvkm_dispnv50_head_programs_olut(nvhead);
}

/*
 * Publish the hardware cursor visibility bit for a HEAD.
 *
 * Ownership:
 *   Mutably borrows only the per-head armed record; cursor BO pin ownership
 *   remains in the KMS prepare/cleanup path.
 *
 * Lifetime:
 *   Call after the core UPDATE that sets or clears the cursor context succeeds.
 *
 * Threading:
 *   Serialized KMS commit context only.
 */
static void
nvkm_dispnv50_head_mark_cursor(struct nvkm_dispnv50_state *state, u32 head,
    bool enabled)
{
	struct nvkm_dispnv50_head_armed *armed;

	armed = nvkm_dispnv50_head_armed_state(state, head);
	if (armed == NULL)
		return;
	armed->valid = true;
	armed->cursor = enabled;
}

/*
 * Publish a fully disabled HEAD resource state.
 *
 * Ownership:
 *   Mutably borrows only the per-head armed record.  No output or cursor object
 *   is released here; callers release those resources after the UPDATE succeeds.
 *
 * Lifetime:
 *   Call after the modeset-disable UPDATE has been accepted.  The record stays
 *   valid so future clears know this HEAD has no nvkm-owned armed resources.
 *
 * Threading:
 *   Serialized KMS commit context only.
 */
static void
nvkm_dispnv50_head_mark_disabled(struct nvkm_dispnv50_state *state, u32 head)
{
	struct nvkm_dispnv50_head_armed *armed;

	armed = nvkm_dispnv50_head_armed_state(state, head);
	if (armed == NULL)
		return;
	memset(armed, 0, sizeof(*armed));
	armed->valid = true;
}

/*
 * Publish a successful window-side color update in the window armed mirror.
 *
 * Ownership:
 *   Mutably borrows the per-window armed record.  It preserves notifier,
 *   semaphore, and image state because color-only commits do not own those
 *   resources.
 *
 * Lifetime:
 *   Call after the window/core UPDATE carrying ILUT/CSC methods succeeds.
 *
 * Threading:
 *   Serialized KMS commit context only.
 */
static void
nvkm_dispnv50_wndw_mark_color_programmed(struct nvkm_dispnv50_state *state,
    const struct nv50_wndw *wndw, const struct drm_crtc_state *crtc_state)
{
	struct nvkm_dispnv50_wndw_armed *armed;

	armed = nvkm_dispnv50_wndw_armed_state(state, wndw);
	if (armed == NULL)
		return;
	armed->valid = true;
	armed->xlut = nvkm_dispnv50_wndw_programs_xlut(wndw);
	armed->csc = nvkm_dispnv50_wndw_programs_csc(wndw, crtc_state);
}

static struct nvkm_dispnv50_wndw_armed *
nvkm_dispnv50_wndw_armed_state(struct nvkm_dispnv50_state *state,
    const struct nv50_wndw *wndw)
{
	if (state == NULL || wndw == NULL || wndw->id < 0 ||
	    (u32)wndw->id >= nitems(state->wndw_armed))
		return NULL;
	return &state->wndw_armed[wndw->id];
}

static bool
nvkm_dispnv50_wndw_programs_xlut(const struct nv50_wndw *wndw)
{
	return (wndw != NULL && wndw->func != NULL &&
	    wndw->func->ilut != NULL && wndw->func->xlut_set != NULL &&
	    wndw->func->ilut_identity);
}

static bool
nvkm_dispnv50_wndw_programs_csc(const struct nv50_wndw *wndw,
    const struct drm_crtc_state *crtc_state)
{
	return (wndw != NULL && wndw->func != NULL &&
	    crtc_state != NULL && crtc_state->ctm != NULL &&
	    wndw->func->csc_set != NULL);
}

/*
 * Emit clear methods for resources that are actually armed.
 *
 * Ownership:
 *   Borrows wndw and the caller-owned armed snapshot for this transaction only.
 *   It does not retain channel, BO, LUT, or notifier ownership.
 *
 * Lifetime:
 *   The emitted methods stay pending in the window push buffer until the caller
 *   submits UPDATE.  The armed snapshot must describe the old hardware-visible
 *   state for the same window.
 *
 * Threading:
 *   Called from serialized KMS commit paths.  It may reserve push-buffer space
 *   through the emitter callbacks and must not run from interrupt context.
 */
static int
nvkm_dispnv50_wndw_clear_armed(struct nv50_wndw *wndw,
    const struct nvkm_dispnv50_wndw_armed *armed, bool clear_ntfy,
    bool clear_sema, bool clear_xlut, bool clear_csc, bool clear_image,
    bool *emitted)
{
	int ret;

	if (emitted != NULL)
		*emitted = false;

	if (wndw == NULL || wndw->func == NULL)
		return -ENODEV;

	if (armed == NULL || !armed->valid) {
		if (!clear_image || wndw->func->image_clr == NULL)
			return 0;
		ret = wndw->func->image_clr(wndw);
		if (ret != 0)
			return ret;
		if (emitted != NULL)
			*emitted = true;
		return 0;
	}

	if (clear_ntfy && armed->ntfy) {
		if (wndw->func->ntfy_clr == NULL)
			return -ENODEV;
		ret = wndw->func->ntfy_clr(wndw);
		if (ret != 0)
			return ret;
		if (emitted != NULL)
			*emitted = true;
	}
	if (clear_sema && armed->sema) {
		if (wndw->func->sema_clr == NULL)
			return -ENODEV;
		ret = wndw->func->sema_clr(wndw);
		if (ret != 0)
			return ret;
		if (emitted != NULL)
			*emitted = true;
	}
	if (clear_xlut && armed->xlut) {
		if (wndw->func->xlut_clr == NULL)
			return -ENODEV;
		ret = wndw->func->xlut_clr(wndw);
		if (ret != 0)
			return ret;
		if (emitted != NULL)
			*emitted = true;
	}
	if (clear_csc && armed->csc) {
		if (wndw->func->csc_clr == NULL)
			return -ENODEV;
		ret = wndw->func->csc_clr(wndw);
		if (ret != 0)
			return ret;
		if (emitted != NULL)
			*emitted = true;
	}
	if (clear_image && armed->image) {
		if (wndw->func->image_clr == NULL)
			return -ENODEV;
		ret = wndw->func->image_clr(wndw);
		if (ret != 0)
			return ret;
		if (emitted != NULL)
			*emitted = true;
	}

	return 0;
}

/*
 * Publish the resource state produced by a successful window set/update.
 *
 * Ownership:
 *   Mutably borrows the per-window armed record from state.  It copies only
 *   scalar booleans and keeps no reference to crtc_state or wndw.
 *
 * Lifetime:
 *   Call only after the window UPDATE has been submitted and, for synchronous
 *   commits, after the notifier proves BEGUN.  Async flips publish image=true at
 *   submit time because the old image was already armed and a later clear must
 *   still clear the image slot.
 *
 * Threading:
 *   Serialized KMS commit context only; sysctl may observe intermediate values.
 */
static void
nvkm_dispnv50_wndw_mark_programmed(struct nvkm_dispnv50_state *state,
    const struct nv50_wndw *wndw, const struct drm_crtc_state *crtc_state,
    bool async, bool program_color)
{
	struct nvkm_dispnv50_wndw_armed *armed;

	armed = nvkm_dispnv50_wndw_armed_state(state, wndw);
	if (armed == NULL)
		return;

	armed->valid = true;
	armed->image = true;
	if (async)
		return;

	armed->ntfy = true;
	armed->sema = false;
	if (program_color) {
		armed->xlut = nvkm_dispnv50_wndw_programs_xlut(wndw);
		armed->csc = nvkm_dispnv50_wndw_programs_csc(wndw,
		    crtc_state);
	}
}

/*
 * Publish a fully disabled window resource state.
 *
 * Ownership:
 *   Mutably borrows the per-window armed record and drops every scalar resource
 *   bit.  No hardware object is owned or freed here.
 *
 * Lifetime:
 *   Call after the modeset-disable UPDATE has been submitted.  The record stays
 *   valid so future clears know this window has no nvkm-owned armed resources.
 *
 * Threading:
 *   Serialized KMS commit context only.
 */
static void
nvkm_dispnv50_wndw_mark_disabled(struct nvkm_dispnv50_state *state,
    const struct nv50_wndw *wndw)
{
	struct nvkm_dispnv50_wndw_armed *armed;

	armed = nvkm_dispnv50_wndw_armed_state(state, wndw);
	if (armed == NULL)
		return;
	memset(armed, 0, sizeof(*armed));
	armed->valid = true;
}

/*
 * Publish a primary-plane disable that used a fresh notifier for completion.
 *
 * Ownership:
 *   Mutably borrows the per-window armed record.  The notifier buffer itself
 *   remains owned by the display sync BO.
 *
 * Lifetime:
 *   The image/color resources are no longer armed after UPDATE, but the
 *   notifier context remains the most recent nvkm-owned window notifier until a
 *   later modeset-disable clear removes it.
 *
 * Threading:
 *   Serialized KMS commit context only.
 */
static void
nvkm_dispnv50_wndw_mark_plane_disabled(struct nvkm_dispnv50_state *state,
    const struct nv50_wndw *wndw)
{
	struct nvkm_dispnv50_wndw_armed *armed;

	armed = nvkm_dispnv50_wndw_armed_state(state, wndw);
	if (armed == NULL)
		return;
	armed->valid = true;
	armed->ntfy = true;
	armed->sema = false;
	armed->xlut = false;
	armed->csc = false;
	armed->image = false;
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

/*
 * Ownership: borrows wndw for the duration of a single display commit.
 * Lifetime: emitted methods are ordered before the caller's window UPDATE;
 * the caller keeps the window channel and its context-DMAs alive until that
 * update has been accepted.
 * Threading: called from the serialized KMS commit path, not from IRQ context.
 *
 * This is narrower than nvkm_dispnv50_wndw_sanitize(): disable still arms a
 * notifier and waits for BEGUN, so clearing the notifier context in the same
 * update would remove the completion source.  Full modeset enable sanitation
 * clears notifier/semaphore before arming a fresh notifier.
 */
static int
nvkm_dispnv50_wndw_disable_resources(struct nvkm_dispnv50_state *state,
    struct nv50_wndw *wndw, bool *emitted)
{
	const struct nvkm_dispnv50_wndw_armed *armed =
	    nvkm_dispnv50_wndw_armed_state(state, wndw);

	return nvkm_dispnv50_wndw_clear_armed(wndw, armed, false, false, true,
	    true, true, emitted);
}

/*
 * Ownership:
 *   Borrows the window channel for one modeset-disable transaction.  The
 *   caller owns the final window/core UPDATE submission.
 *
 * Lifetime:
 *   The emitted clear methods stay pending in the window push buffer until the
 *   caller submits the window UPDATE with the matching interlock set.  No
 *   window notifier is armed for this path; disable completion is owned by the
 *   transaction-level core flush, matching nouveau's atomic disable rule.
 *
 * Threading:
 *   Called from the serialized KMS modeset disable path.  It may sleep in push
 *   buffer reservation, but it must not wait for a window notifier.
 */
static int
nvkm_dispnv50_wndw_modeset_disable_resources(struct nvkm_dispnv50_state *state,
    struct nv50_wndw *wndw, bool *emitted)
{
	const struct nvkm_dispnv50_wndw_armed *armed =
	    nvkm_dispnv50_wndw_armed_state(state, wndw);
	bool clear_context;

	/*
	 * Plane-only disable uses a notifier to prove the image clear.  A full
	 * modeset-disable must still clear that remaining notifier/semaphore
	 * context and pair the window UPDATE with the core HEAD/output detach.
	 * This matches nouveau's flush-disable boundary: image/color may be
	 * cleared by an earlier window-only transaction, but context ownership is
	 * dropped by the final window/core transaction.
	 */
	clear_context = armed == NULL || armed->ntfy || armed->sema ||
	    armed->image || armed->xlut || armed->csc;

	return nvkm_dispnv50_wndw_clear_armed(wndw, armed, clear_context,
	    clear_context, true, true, true, emitted);
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

/*
 * Submit one core UPDATE and sample the core notifier.
 *
 * Ownership:
 *   Borrows the core channel, shared notifier buffer, and interlock array for
 *   one UPDATE.  No references are retained after return.
 *
 * Lifetime:
 *   Strict callers use the core notifier as this UPDATE's completion proof and
 *   receive any wait error.  Best-effort callers still submit the UPDATE, log
 *   notifier failure, and continue teardown/disable cleanup after the command
 *   is hardware-visible.
 *
 * Threading:
 *   Called from serialized KMS commit paths.  It may sleep while waiting for
 *   the notifier and must not run from interrupt context.
 */
static int
nvkm_dispnv50_core_commit_notify_common(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_core *core, u32 *interlock,
    bool best_effort)
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
		return best_effort ? 0 : ret;
	}

	if (sc->kms_push_trace) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 core notifier done status=0x%08x\n",
		    status);
	}
	return 0;
}

static int
nvkm_dispnv50_core_commit_notify(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_core *core, u32 *interlock)
{
	return nvkm_dispnv50_core_commit_notify_common(sc, state, core,
	    interlock, false);
}

/*
 * Ownership:
 *   Borrows the core channel and interlock array for a modeset-disable flush.
 *
 * Lifetime:
 *   The core UPDATE must be accepted before this returns.  The shared core
 *   notifier is diagnostic only; output release and object destruction must not
 *   be skipped once the disable transaction has been submitted.
 *
 * Threading:
 *   Called from serialized KMS teardown/disable paths and may sleep while
 *   waiting for the notifier.
 */
static int
nvkm_dispnv50_core_commit_notify_best_effort(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_core *core, u32 *interlock)
{
	return nvkm_dispnv50_core_commit_notify_common(sc, state, core,
	    interlock, true);
}

/*
 * Commit cursor head-context changes through the nouveau core-update rules.
 *
 * Ownership: borrows the already-owned display core and interlock array.  The
 * helper does not retain references and does not mutate KMS object ownership.
 *
 * Lifetime: the caller must have emitted cursor set/clear methods into the
 * core push buffer.  This function only commits those pending methods.
 *
 * Threading: blockable KMS commit context.  Legacy cursor ioctls use the
 * non-notifying path from Linux nouveau because a cursor-only legacy update
 * should not wait on the full core notifier completion path.
 */
static int
nvkm_dispnv50_cursor_commit_core(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_core *core, u32 *interlock,
    bool legacy_cursor_update)
{
	if (core == NULL || core->func == NULL || core->func->update == NULL)
		return -ENODEV;
	if (legacy_cursor_update)
		return core->func->update(core, interlock, false);
	return nvkm_dispnv50_core_commit_notify(sc, state, core, interlock);
}

/*
 * Submit the cursor channel side of a CORE/CURS interlock pair.
 *
 * Ownership:
 *   Borrows the cursor immediate channel and the caller-owned interlock array.
 *   The helper does not retain channel, BO, or KMS state references.
 *
 * Lifetime:
 *   Callers must emit any head cursor context set/clear methods before this
 *   helper, and must submit the matching core UPDATE after it.  This mirrors
 *   nouveau's order: cursor/window channel UPDATEs first, then the core UPDATE
 *   that names their interlock bits.
 *
 * Threading:
 *   Called only from serialized KMS commit paths.  It may sleep in the cursor
 *   channel wait path and must not run from interrupt context.
 */
static int
nvkm_dispnv50_cursor_commit_wimm(struct nv50_wndw *curs, u32 *interlock)
{
	if (curs == NULL || curs->immd == NULL || curs->immd->update == NULL)
		return -ENODEV;
	return curs->immd->update(curs, interlock);
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

/*
 * Emit window methods for one KMS transaction without submitting UPDATE.
 *
 * Ownership:
 *   Borrows the CRTC, bridge state, window channel, and output atom for the
 *   duration of the call.  It stores no pointer and acquires no KMS reference.
 *
 * Lifetime:
 *   The emitted methods remain pending in the window push buffer until the
 *   caller submits the matching UPDATE.  asyw is caller-owned storage and must
 *   remain valid until any later notifier wait that uses it.
 *
 * Threading:
 *   Called from serialized KMS commit paths.  It may sleep while reserving push
 *   buffer space and must not run from interrupt context.
 */
static int
nvkm_dispnv50_window_emit_program(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct drm_crtc *crtc,
    struct nv50_wndw *wndw, struct nv50_wndw_atom *asyw,
    bool sanitize, bool async, bool program_color)
{
	int ret;

	if (sc == NULL || state == NULL || crtc == NULL || wndw == NULL ||
	    asyw == NULL || wndw->func == NULL ||
	    wndw->func->image_set == NULL)
		return -ENODEV;

	nvkm_dispnv50_wndw_atom_fill(asyw, crtc, state);
	if (sanitize) {
		ret = nvkm_dispnv50_wndw_sanitize(wndw);
		if (ret != 0)
			return ret;
	}

	/*
	 * A page-flip only changes the scanout buffer (the image); notifier,
	 * ILUT, CSC and blend were programmed at modeset/color update and are
	 * unchanged, so the async and image-only paths push image_set + UPDATE
	 * only.  This mirrors nouveau's nv50_wndw_flush_set gating each emitter
	 * on a dirty bit.
	 */
	if (!async) {
		ret = nvkm_dispnv50_wndw_ntfy_enable(sc, state, wndw, asyw);
		if (ret != 0)
			return ret;
	}
	ret = wndw->func->image_set(wndw, asyw);
	if (ret != 0)
		return ret;
	if (!async && program_color) {
		ret = nvkm_dispnv50_wndw_ilut_set(sc, state, wndw, asyw,
		    crtc->state);
		if (ret != 0)
			return ret;
			ret = nvkm_dispnv50_wndw_csc_set(sc, state, wndw, asyw,
			    crtc->state);
		if (ret != 0)
			return ret;
		if (wndw->func->blend_set == NULL)
			return -ENODEV;
		ret = wndw->func->blend_set(wndw, asyw);
		if (ret != 0)
			return ret;
	}

	return 0;
}

/*
 * Submit a pending window UPDATE and, when required, the matching core UPDATE.
 *
 * Ownership:
 *   Borrows the core, window, and interlock array for one commit step.  The
 *   caller owns update_submitted storage and may pass NULL when no rollback
 *   boundary needs to be recorded.
 *
 * Lifetime:
 *   Once update_submitted is set true, the transaction has crossed into
 *   hardware-visible UPDATE submission.  Callers must then treat prepared
 *   output ownership as consumed instead of rolling it back as prepare-only.
 *
 * Threading:
 *   Called from serialized KMS commit paths.  It may sleep in push-buffer and
 *   notifier paths and must not run from interrupt context.
 */
static int
nvkm_dispnv50_window_submit_update(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct nv50_core *core,
    struct nv50_wndw *wndw, u32 *interlock, bool notify_core,
    bool *update_submitted)
{
	bool commit_core;
	int ret;

	if (wndw == NULL || wndw->func == NULL || wndw->func->update == NULL ||
	    interlock == NULL)
		return -ENODEV;

	commit_core = interlock[NV50_DISP_INTERLOCK_CORE] != 0;
	interlock[NV50_DISP_INTERLOCK_WNDW] |= wndw->interlock.data;
	ret = wndw->func->update(wndw, interlock);
	if (ret != 0)
		return ret;
	if (update_submitted != NULL)
		*update_submitted = true;
	if (!commit_core)
		return 0;

	if (core == NULL || core->func == NULL)
		return -ENODEV;
	if (notify_core)
		return nvkm_dispnv50_core_commit_notify(sc, state, core,
		    interlock);
	if (core->func->update == NULL)
		return -ENODEV;
	return core->func->update(core, interlock, false);
}

/*
 * Ownership: borrows the CRTC/window/core state for one KMS commit.  When
 * update_submitted is non-NULL, the caller owns that bool and this helper only
 * sets it after the window UPDATE method has been accepted.
 *
 * Lifetime: after update_submitted becomes true, the commit has crossed from
 * prepare-time programming into hardware-visible update submission.  Callers
 * must not roll back prepared output ownership as if no commit had consumed it.
 *
 * Threading: called from serialized atomic commit paths; it may sleep while
 * waiting for display notifiers and must not be called from interrupt context.
 * When notify_core is false, callers still submit the core UPDATE, but transfer
 * completion ownership to the window notifier for runtime commits where the core
 * notifier is not the reliable completion point.
 */
static int
nvkm_dispnv50_window_program(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state, struct drm_crtc *crtc,
    struct nv50_core *core, struct nv50_wndw *wndw, u32 *interlock,
    bool sanitize, bool async, bool notify_core, bool program_color,
    enum nvkm_dispnv50_audit_op op, u32 head, u32 display_id,
    const char *reason, bool *update_submitted)
{
	struct nv50_wndw_atom asyw;
	int ret;

	if (interlock == NULL)
		return -EINVAL;

	ret = nvkm_dispnv50_window_emit_program(sc, state, crtc, wndw, &asyw,
	    sanitize, async, program_color);
	if (ret != 0)
		goto fail;

	ret = nvkm_dispnv50_window_submit_update(sc, state, core, wndw,
	    interlock, notify_core, update_submitted);
	if (ret != 0)
		goto fail;

	/* Async (page-flip): the UPDATE is kicked; the HW latches the new
	 * scanout at the next vblank and the DRM flip event completes there
	 * (real vblank). Do not block the commit thread on the notifier or
	 * read back channel status. DRM serialises flips via the event. */
	if (async) {
		nvkm_dispnv50_wndw_mark_programmed(state, wndw,
		    crtc->state, true, false);
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

	nvkm_dispnv50_wndw_mark_programmed(state, wndw, crtc->state, false,
	    program_color);
	nvkm_dispnv50_audit_capture(state, &state->audit_current, op, head,
	    (u32)wndw->id, display_id, false, true, false);
	memset(&state->audit_pending, 0, sizeof(state->audit_pending));

	nvkm_infof(sc->dev,
	    "drm: dispnv50 %s armed win=%d scanout=0x%llx offset=0x%llx "
	    "src=%u,%u %ux%u dst=%d,%d %ux%u user=%d\n", reason, wndw->id,
	    (unsigned long long)(state->scanout_user ? state->scanout_offset :
	    nvkm_memory_addr(state->scanout)),
	    (unsigned long long)state->scanout_offset,
	    state->window_src_x >> 16, state->window_src_y >> 16,
	    state->window_src_w >> 16, state->window_src_h >> 16,
	    state->window_crtc_x, state->window_crtc_y,
	    state->window_crtc_w, state->window_crtc_h,
	    state->scanout_user);
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

/*
 * Publish a completed pending primary-plane flip.
 *
 * Ownership:
 *   Mutably borrows only the dispnv50 audit records owned by state.  No window
 *   channel, notifier buffer, BO, framebuffer, or DRM event ownership changes
 *   here; those lifetimes remain owned by KMS prepare/cleanup and the vblank
 *   event path.
 *
 * Lifetime:
 *   Call after drm_atomic_helper_wait_for_flip_done() has observed the
 *   commit's flip completion.  The pending snapshot describes the primary
 *   window update that was already submitted to hardware and completed at the
 *   display flip point; this helper only moves that completed scalar state
 *   into current and clears pending.
 *
 * Threading:
 *   Serialized atomic commit-tail context only.  Sysctl readers are lockless
 *   observers, so the audit fields must not be used as synchronization state.
 */
void
nvkm_dispnv50_publish_pending_flip(struct nvkm_softc *sc)
{
	struct nvkm_dispnv50_state *state;

	if (sc == NULL)
		return;
	state = sc->dispnv50;
	if (state == NULL || !state->audit_pending.valid ||
	    !state->audit_pending.async)
		return;

	state->audit_current = state->audit_pending;
	state->audit_current.armed = true;
	memset(&state->audit_pending, 0, sizeof(state->audit_pending));
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
	bool clear_emitted = false;
	int ret;

	(void)core;
	if (wndw->func->image_clr == NULL)
		return 0;

	memset(&asyw, 0, sizeof(asyw));
	ret = nvkm_dispnv50_wndw_ntfy_enable(sc, state, wndw, &asyw);
	if (ret != 0)
		goto fail;
	ret = nvkm_dispnv50_wndw_disable_resources(state, wndw, &clear_emitted);
	if (ret != 0)
		goto fail;

	if (clear_emitted)
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
	nvkm_dispnv50_wndw_mark_plane_disabled(state, wndw);
	nvkm_dispnv50_audit_capture(state, &state->audit_current,
	    NVKM_DISPNV50_AUDIT_PLANE_DISABLE, head, (u32)wndw->id, display_id,
	    false, true, true);
	/*
	 * The audit above intentionally records which scanout was disabled.  The
	 * live ownership snapshot must then stop advertising a user scanout:
	 * after the window image clear has been accepted, there is no user image
	 * left for light_up() to preserve with a full modeset.
	 */
	state->scanout_user = false;
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

static int
nvkm_dispnv50_dp_rate_khz(uint8_t bw)
{
	switch (bw) {
	case DP_LINK_BW_1_62:
		return 162000;
	case DP_LINK_BW_2_7:
		return 270000;
	case DP_LINK_BW_5_4:
		return 540000;
	case DP_LINK_BW_8_1:
		return 810000;
	default:
		return 0;
	}
}

static int
nvkm_dispnv50_dp_aux_read(struct nvkm_outp *outp, uint32_t addr, uint8_t *data,
    uint8_t size)
{
	uint8_t done = 0;

	if (outp == NULL || outp->func == NULL ||
	    outp->func->dp.aux_xfer == NULL || data == NULL)
		return -ENODEV;

	while (done < size) {
		uint8_t want = MIN((uint8_t)(size - done),
		    (uint8_t)DP_AUX_MAX_PAYLOAD_BYTES);
		uint8_t reply_size = want;
		int ret;

		ret = outp->func->dp.aux_xfer(outp, DP_AUX_NATIVE_READ,
		    addr + done, data + done, &reply_size);
		if (ret < 0)
			return ret;
		if (ret != DP_AUX_NATIVE_REPLY_ACK)
			return -EIO;

		/*
		 * GSP/RM's AUX size field uses the wire encoding internally.
		 * The data buffer is already copied by r535_dp_aux_xfer(), so
		 * advance by the requested chunk once the native transaction is
		 * ACKed.
		 */
		done += want;
	}

	return 0;
}

static uint32_t
nvkm_dispnv50_dp_mode_clock_khz(const struct drm_display_mode *mode)
{
	uint32_t clock;

	if (mode == NULL)
		return 0;
	if (mode->crtc_clock > 0)
		return (uint32_t)mode->crtc_clock;
	if (mode->clock > 0)
		clock = (uint32_t)mode->clock;
	else
		return 0;
	if ((mode->flags & DRM_MODE_FLAG_3D_MASK) ==
	    DRM_MODE_FLAG_3D_FRAME_PACKING) {
		if (clock > UINT32_MAX / 2U)
			return 0;
		clock *= 2U;
	}
	return clock;
}

static int
nvkm_dispnv50_dp_link_limits(struct nvkm_outp *outp,
    const uint8_t dpcd[DP_RECEIVER_CAP_SIZE],
    const struct drm_display_mode *mode, uint8_t bpc, uint32_t *max_rate,
    uint8_t *max_lanes, uint32_t *min_rate)
{
	uint32_t source_rate;
	uint32_t sink_rate;
	uint32_t clock_khz;
	uint8_t source_lanes;
	uint8_t sink_lanes;
	uint8_t lanes;

	clock_khz = nvkm_dispnv50_dp_mode_clock_khz(mode);
	if (clock_khz == 0 || bpc == 0 || max_rate == NULL ||
	    max_lanes == NULL || min_rate == NULL)
		return -EINVAL;
	if (outp == NULL || outp->info.type != DCB_OUTPUT_DP ||
	    dpcd == NULL)
		return -EINVAL;

	source_rate = nvkm_dispnv50_dp_rate_khz(outp->info.dpconf.link_bw);
	sink_rate = nvkm_dispnv50_dp_rate_khz(dpcd[DP_MAX_LINK_RATE]);
	if (source_rate == 0 || sink_rate == 0)
		return -EINVAL;

	source_lanes = outp->info.dpconf.link_nr;
	if (source_lanes == 0)
		source_lanes = 4;
	sink_lanes = dpcd[DP_MAX_LANE_COUNT] &
	    DP_MAX_LANE_COUNT_MASK;
	if (sink_lanes == 0)
		return -EINVAL;

	lanes = MIN(source_lanes, sink_lanes);
	if (lanes >= 4)
		lanes = 4;
	else if (lanes >= 2)
		lanes = 2;
	else
		lanes = 1;

	*max_rate = MIN(source_rate, sink_rate);
	*max_lanes = lanes;
	*min_rate = (clock_khz * (uint32_t)bpc * 3U + 7U) / 8U;
	return 0;
}

static int
nvkm_dispnv50_dp_check_downstream_clock(struct nvkm_outp *outp,
    const uint8_t dpcd[DP_RECEIVER_CAP_SIZE],
    const struct drm_display_mode *mode)
{
	uint8_t port_cap[4];
	uint32_t clock_khz;
	int max_clock;
	int ret;

	if (outp == NULL || dpcd == NULL || mode == NULL)
		return -EINVAL;
	if ((dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_PRESENT) == 0 ||
	    (dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) == 0)
		return 0;

	ret = nvkm_dispnv50_dp_aux_read(outp, DP_DOWNSTREAM_PORT_0, port_cap,
	    sizeof(port_cap));
	if (ret != 0)
		return ret;

	max_clock = drm_dp_downstream_max_clock(dpcd, port_cap);
	clock_khz = nvkm_dispnv50_dp_mode_clock_khz(mode);
	if (max_clock > 0 && clock_khz > (uint32_t)max_clock)
		return -ERANGE;
	return 0;
}

static int
nvkm_dispnv50_dp_sst_calc(const struct drm_display_mode *mode,
    uint8_t bpc, uint8_t link_bw, uint8_t lanes, bool enhanced_framing,
    bool increased_watermark, uint32_t *watermark, uint32_t *hblank_symbols,
    uint32_t *vblank_symbols)
{
	uint32_t adjust = increased_watermark ? NVKM_DISPNV50_DP_WM_ADJUST_INC :
	    NVKM_DISPNV50_DP_WM_ADJUST;
	uint32_t minimum = increased_watermark ? NVKM_DISPNV50_DP_WM_LIMIT_INC :
	    NVKM_DISPNV50_DP_WM_LIMIT;
	uint32_t clock_khz;
	uint32_t link_rate;
	uint32_t active;
	uint32_t total;
	uint32_t hblank;
	uint32_t depth;
	uint32_t symbols_per_line;
	uint32_t steering_bits = 0;
	uint32_t blanking_bits;
	uint32_t min_hblank;
	uint64_t payload_ppm;
	uint64_t variance;
	uint64_t base;
	int64_t hsym;
	int64_t vsym;
	uint32_t remain;

	if (mode == NULL || watermark == NULL || hblank_symbols == NULL ||
	    vblank_symbols == NULL || bpc == 0 || lanes == 0)
		return -EINVAL;

	clock_khz = nvkm_dispnv50_dp_mode_clock_khz(mode);
	link_rate = nvkm_dispnv50_dp_rate_khz(link_bw);
	active = mode->crtc_hdisplay ? mode->crtc_hdisplay : mode->hdisplay;
	total = mode->crtc_htotal ? mode->crtc_htotal : mode->htotal;
	depth = (uint32_t)bpc * 3U;
	if (clock_khz == 0 || link_rate == 0 || total <= active ||
	    active <= 60)
		return -EINVAL;
	hblank = total - active;

	if ((uint64_t)clock_khz * depth >=
	    (uint64_t)8U * link_rate * lanes)
		return -ERANGE;

	payload_ppm = div_u64((uint64_t)clock_khz * depth *
	    NVKM_DISPNV50_DP_PRECISION,
	    (uint32_t)(8U * link_rate * lanes));
	if (payload_ppm >= NVKM_DISPNV50_DP_PRECISION)
		return -ERANGE;

	variance = div_u64(payload_ppm * NVKM_DISPNV50_DP_TU_SIZE *
	    (NVKM_DISPNV50_DP_PRECISION - payload_ppm),
	    NVKM_DISPNV50_DP_PRECISION);
	base = div_u64(2U * div_u64((uint64_t)depth *
	    NVKM_DISPNV50_DP_PRECISION, 8U * lanes) + variance,
	    NVKM_DISPNV50_DP_PRECISION);
	*watermark = adjust + (uint32_t)base;

	symbols_per_line = div_u64((uint64_t)active * depth, 8U * lanes);
	if (*watermark > 39U || *watermark > symbols_per_line)
		return -ERANGE;
	if (*watermark < minimum)
		*watermark = minimum;

	blanking_bits = 3U * 8U * lanes;
	if (enhanced_framing)
		blanking_bits += 3U * 8U * lanes;
	blanking_bits += 3U * 8U * 4U;
	remain = active % lanes;
	if (remain != 0)
		steering_bits = (lanes - remain) * depth;
	blanking_bits += steering_bits;

	min_hblank = div_u64((uint64_t)blanking_bits *
	    NVKM_DISPNV50_DP_PRECISION, 8U * lanes);
	min_hblank = div_u64((uint64_t)min_hblank * clock_khz, link_rate);
	min_hblank = div_u64(min_hblank, NVKM_DISPNV50_DP_PRECISION) + 12U;
	if (min_hblank > hblank)
		return -ERANGE;

	hsym = (int64_t)div_u64((uint64_t)(hblank - min_hblank) *
	    link_rate, clock_khz);
	hsym -= 4;
	hsym -= lanes == 1 ? 9 : lanes == 2 ? 6 : 3;
	*hblank_symbols = hsym < 0 ? 0 : (uint32_t)hsym;

	if (active < 40) {
		*vblank_symbols = 0;
	} else {
		vsym = (int64_t)div_u64((uint64_t)(active - 40U) *
		    link_rate, clock_khz);
		vsym -= 1;
		vsym -= lanes == 1 ? 39 : lanes == 2 ? 21 : 12;
		*vblank_symbols = vsym < 0 ? 0 : (uint32_t)vsym;
	}

	return 0;
}

static uint32_t
nvkm_dispnv50_dp_candidate_rate_khz(
    const struct nvkm_dispnv50_dp_sst_candidate *candidate)
{
	if (candidate == NULL || !candidate->valid)
		return 0;
	return nvkm_dispnv50_dp_rate_khz(candidate->link_bw);
}

static bool
nvkm_dispnv50_dp_candidate_fits(
    const struct nvkm_dispnv50_dp_sst_candidate *candidate,
    uint32_t max_rate, uint8_t max_lanes, uint32_t min_rate)
{
	uint32_t rate;

	rate = nvkm_dispnv50_dp_candidate_rate_khz(candidate);
	if (rate == 0 || rate > max_rate)
		return false;
	if (candidate->lanes == 0 || candidate->lanes > max_lanes)
		return false;
	return rate * candidate->lanes >= min_rate;
}

static bool
nvkm_dispnv50_dp_candidate_same(
    const struct nvkm_dispnv50_dp_sst_candidate *left,
    const struct nvkm_dispnv50_dp_sst_candidate *right)
{
	if (left == NULL || right == NULL || !left->valid || !right->valid)
		return false;
	return left->link_bw == right->link_bw && left->lanes == right->lanes;
}

static int
nvkm_dispnv50_dp_find_sst_candidate(struct nvkm_outp *outp,
    const struct drm_display_mode *mode, uint32_t max_rate,
    uint8_t max_lanes, uint32_t min_rate, uint8_t bpc,
    bool enhanced_framing, struct nvkm_dispnv50_dp_sst_candidate *candidate)
{
	static const uint8_t rates[] = {
		DP_LINK_BW_1_62,
		DP_LINK_BW_2_7,
		DP_LINK_BW_5_4,
		DP_LINK_BW_8_1,
	};
	uint8_t lanes;
	int last_ret = -ERANGE;
	int ret;
	int i;

	if (candidate == NULL)
		return -EINVAL;
	memset(candidate, 0, sizeof(*candidate));
	if (outp == NULL)
		return -ENODEV;
	if (bpc == 0)
		bpc = 8;

	for (lanes = max_lanes; lanes != 0; lanes >>= 1) {
		for (i = 0; i < (int)nitems(rates); i++) {
			uint32_t watermark = 0;
			uint32_t hblank_symbols = 0;
			uint32_t vblank_symbols = 0;
			uint8_t bw = rates[i];
			uint32_t rate = nvkm_dispnv50_dp_rate_khz(bw);

			if (rate == 0 || rate > max_rate)
				continue;
			if (rate * lanes < min_rate)
				continue;

			ret = nvkm_dispnv50_dp_sst_calc(mode, bpc, bw, lanes,
			    enhanced_framing, outp->dp.increased_wm, &watermark,
			    &hblank_symbols, &vblank_symbols);
			if (ret != 0) {
				last_ret = ret;
				continue;
			}

			candidate->valid = true;
			candidate->link_bw = bw;
			candidate->lanes = lanes;
			candidate->watermark = watermark;
			candidate->hblank_symbols = hblank_symbols;
			candidate->vblank_symbols = vblank_symbols;
			return 0;
		}
	}

	return last_ret;
}

static int
nvkm_dispnv50_dp_select_sst_candidate(struct nvkm_softc *sc,
    struct nvkm_outp *outp, const struct drm_display_mode *mode,
    uint32_t max_rate, uint8_t max_lanes, uint32_t min_rate, uint8_t bpc,
    bool enhanced_framing, struct nvkm_dispnv50_dp_sst_candidate *candidate)
{
	int ret;

	ret = nvkm_dispnv50_dp_find_sst_candidate(outp, mode, max_rate,
	    max_lanes, min_rate, bpc, enhanced_framing, candidate);
	if (ret == 0) {
		if (sc != NULL)
			sc->kms_dp_sst_candidate_count++;
		return 0;
	}
	if (sc != NULL && outp != NULL) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 dp sst timing rejected outp=%02x err=%d\n",
		    outp->index, ret);
	}
	return ret;
}

static int
nvkm_dispnv50_dp_program_sst_candidate(struct nvkm_softc *sc,
    struct nvkm_outp *outp, uint32_t head, bool enhanced_framing,
    const struct nvkm_dispnv50_dp_sst_candidate *candidate)
{
	int ret;

	if (outp == NULL || outp->ior == NULL || outp->func == NULL ||
	    outp->func->dp.train == NULL || candidate == NULL ||
	    !candidate->valid)
		return -ENODEV;
	if (outp->ior->func == NULL || outp->ior->func->dp == NULL ||
	    outp->ior->func->dp->sst == NULL)
		return -ENODEV;

	outp->dp.lt.nr = candidate->lanes;
	outp->dp.lt.bw = candidate->link_bw;
	outp->dp.lt.mst = false;

	if (sc != NULL)
		sc->kms_dp_sst_train_count++;
	ret = outp->func->dp.train(outp, false);
	if (ret != 0) {
		if (sc != NULL) {
			sc->kms_dp_sst_train_fail_count++;
			sc->kms_dp_sst_last_error = ret;
		}
		nvkm_infof(sc->dev,
		    "drm: dispnv50 dp train failed outp=%02x lanes=%u "
		    "bw=0x%02x err=%d\n", outp->index, candidate->lanes,
		    candidate->link_bw, ret);
		return ret;
	}

	if (sc != NULL)
		sc->kms_dp_sst_program_count++;
	ret = outp->ior->func->dp->sst(outp->ior, (int)head,
	    enhanced_framing, candidate->watermark,
	    candidate->hblank_symbols, candidate->vblank_symbols);
	if (ret != 0) {
		if (sc != NULL) {
			sc->kms_dp_sst_program_fail_count++;
			sc->kms_dp_sst_last_error = ret;
		}
		nvkm_infof(sc->dev,
		    "drm: dispnv50 dp sst program failed outp=%02x "
		    "lanes=%u bw=0x%02x err=%d\n", outp->index,
		    candidate->lanes, candidate->link_bw, ret);
		return ret;
	}

	if (sc != NULL) {
		sc->kms_dp_sst_enable_success_count++;
		sc->kms_dp_sst_last_error = 0;
	}
	outp->dp.enabled = true;
	nvkm_infof(sc->dev,
	    "drm: dispnv50 dp enabled outp=%02x sor=%d head=%u "
	    "lanes=%u bw=0x%02x wm=%u hsym=%u vsym=%u ef=%d\n",
	    outp->index, outp->ior->id, head, candidate->lanes,
	    candidate->link_bw, candidate->watermark,
	    candidate->hblank_symbols, candidate->vblank_symbols,
	    enhanced_framing);
	return 0;
}

static int
nvkm_dispnv50_dp_enable_candidates(struct nvkm_softc *sc,
    struct nvkm_outp *outp, struct drm_display_mode *mode, uint32_t head,
    uint32_t max_rate, uint8_t max_lanes, uint32_t min_rate, uint8_t bpc,
    bool enhanced_framing,
    const struct nvkm_dispnv50_dp_sst_candidate *preferred)
{
	static const uint8_t rates[] = {
		DP_LINK_BW_1_62,
		DP_LINK_BW_2_7,
		DP_LINK_BW_5_4,
		DP_LINK_BW_8_1,
	};
	uint8_t lanes;
	int last_ret = -ERANGE;
	int ret;
	int i;

	if (outp == NULL || outp->ior == NULL || outp->func == NULL ||
	    outp->func->dp.train == NULL)
		return -ENODEV;
	if (outp->ior->func == NULL || outp->ior->func->dp == NULL ||
	    outp->ior->func->dp->sst == NULL)
		return -ENODEV;
	if (bpc == 0)
		bpc = 8;

	if (nvkm_dispnv50_dp_candidate_fits(preferred, max_rate, max_lanes,
	    min_rate)) {
		ret = nvkm_dispnv50_dp_program_sst_candidate(sc, outp, head,
		    enhanced_framing, preferred);
		if (ret == 0)
			return 0;
		last_ret = ret;
		if (sc != NULL)
			sc->kms_dp_sst_fallback_count++;
	}

	for (lanes = max_lanes; lanes != 0; lanes >>= 1) {
		for (i = 0; i < (int)nitems(rates); i++) {
			struct nvkm_dispnv50_dp_sst_candidate candidate;
			uint8_t bw = rates[i];
			uint32_t rate = nvkm_dispnv50_dp_rate_khz(bw);

			if (rate == 0 || rate > max_rate)
				continue;
			if (rate * lanes < min_rate)
				continue;

			memset(&candidate, 0, sizeof(candidate));
			ret = nvkm_dispnv50_dp_sst_calc(mode, bpc, bw, lanes,
			    enhanced_framing, outp->dp.increased_wm,
			    &candidate.watermark, &candidate.hblank_symbols,
			    &candidate.vblank_symbols);
			if (ret != 0) {
				last_ret = ret;
				nvkm_infof(sc->dev,
				    "drm: dispnv50 dp sst timing rejected"
				    " outp=%02x lanes=%u bw=0x%02x err=%d\n",
				    outp->index, lanes, bw, ret);
				continue;
			}

			candidate.valid = true;
			candidate.link_bw = bw;
			candidate.lanes = lanes;
			if (nvkm_dispnv50_dp_candidate_same(&candidate,
			    preferred))
				continue;

			ret = nvkm_dispnv50_dp_program_sst_candidate(sc, outp,
			    head, enhanced_framing, &candidate);
			if (ret == 0)
				return 0;
			last_ret = ret;
			if (sc != NULL)
				sc->kms_dp_sst_fallback_count++;
		}
	}

	return last_ret;
}

static int
nvkm_dispnv50_dp_prepare(struct nvkm_softc *sc, struct nvkm_outp *outp,
    struct drm_display_mode *mode, uint8_t bpc,
    struct nvkm_dispnv50_output_prepare *prepare)
{
	int ret;

	if (outp == NULL || prepare == NULL)
		return -ENODEV;
	if (bpc == 0)
		bpc = 8;

	ret = nvkm_dispnv50_dp_aux_read(outp, DP_DPCD_REV, outp->dp.dpcd,
	    DP_RECEIVER_CAP_SIZE);
	if (ret != 0)
		return ret;

	memcpy(prepare->dp_dpcd, outp->dp.dpcd,
	    MIN((size_t)NVKM_DISPNV50_DP_DPCD_SIZE,
	    (size_t)DP_RECEIVER_CAP_SIZE));

	ret = nvkm_dispnv50_dp_link_limits(outp, outp->dp.dpcd, mode, bpc,
	    &prepare->dp_max_rate, &prepare->dp_max_lanes,
	    &prepare->dp_min_rate);
	if (ret != 0)
		return ret;

	ret = nvkm_dispnv50_dp_check_downstream_clock(outp, outp->dp.dpcd,
	    mode);
	if (ret != 0)
		return ret;

	prepare->dp_enhanced_framing =
	    !!(outp->dp.dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP);
	return nvkm_dispnv50_dp_select_sst_candidate(sc, outp, mode,
	    prepare->dp_max_rate, prepare->dp_max_lanes,
	    prepare->dp_min_rate, bpc, prepare->dp_enhanced_framing,
	    &prepare->dp_sst);
}

static int
nvkm_dispnv50_dp_enable(struct nvkm_softc *sc, struct nvkm_outp *outp,
    struct drm_display_mode *mode, uint32_t head)
{
	struct nvkm_dispnv50_output_prepare prepare;
	int ret;

	memset(&prepare, 0, sizeof(prepare));
	ret = nvkm_dispnv50_dp_prepare(sc, outp, mode, 8, &prepare);
	if (ret != 0)
		return ret;

	return nvkm_dispnv50_dp_enable_candidates(sc, outp, mode, head,
	    prepare.dp_max_rate, prepare.dp_max_lanes, prepare.dp_min_rate,
	    8, prepare.dp_enhanced_framing, &prepare.dp_sst);
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
nvkm_dispnv50_dp_mode_status_from_error(int ret)
{
	switch (ret) {
	case -ERANGE:
		return MODE_CLOCK_HIGH;
	case -EINVAL:
		return MODE_BAD;
	case -EIO:
	case -ENODEV:
	default:
		return MODE_ERROR;
	}
}

int
nvkm_dispnv50_output_mode_valid(struct nvkm_softc *sc, uint32_t display_id,
    const struct drm_display_mode *mode, uint8_t bpc,
    bool dp_interlace_capable)
{
	struct nvkm_dispnv50_dp_sst_candidate candidate;
	struct nvkm_outp *outp;
	uint8_t dpcd[DP_RECEIVER_CAP_SIZE];
	uint32_t max_rate;
	uint32_t min_rate;
	uint8_t max_lanes;
	bool enhanced_framing;
	int ret;

	if (mode == NULL)
		return MODE_ERROR;
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) &&
	    !dp_interlace_capable)
		return MODE_NO_INTERLACE;
	if (bpc == 0)
		bpc = 8;

	outp = nvkm_dispnv50_find_outp(sc, display_id);
	if (outp == NULL)
		return MODE_ERROR;
	if (outp->info.type != DCB_OUTPUT_DP)
		return MODE_OK;

	ret = nvkm_dispnv50_dp_aux_read(outp, DP_DPCD_REV, dpcd,
	    sizeof(dpcd));
	if (ret != 0)
		return nvkm_dispnv50_dp_mode_status_from_error(ret);

	ret = nvkm_dispnv50_dp_link_limits(outp, dpcd, mode, bpc,
	    &max_rate, &max_lanes, &min_rate);
	if (ret != 0)
		return nvkm_dispnv50_dp_mode_status_from_error(ret);

	ret = nvkm_dispnv50_dp_check_downstream_clock(outp, dpcd, mode);
	if (ret != 0)
		return nvkm_dispnv50_dp_mode_status_from_error(ret);

	enhanced_framing = !!(dpcd[DP_MAX_LANE_COUNT] &
	    DP_ENHANCED_FRAME_CAP);
	ret = nvkm_dispnv50_dp_find_sst_candidate(outp, mode, max_rate,
	    max_lanes, min_rate, bpc, enhanced_framing, &candidate);
	if (ret != 0)
		return nvkm_dispnv50_dp_mode_status_from_error(ret);
	return MODE_OK;
}

int
nvkm_dispnv50_dp_link_check(struct nvkm_softc *sc, uint32_t display_id,
    bool *link_ok)
{
	struct nvkm_outp *outp;
	uint8_t status[DP_LINK_STATUS_SIZE];
	int ret;

	if (link_ok == NULL)
		return -EINVAL;
	*link_ok = true;

	outp = nvkm_dispnv50_find_outp(sc, display_id);
	if (outp == NULL)
		return -ENODEV;
	if (outp->info.type != DCB_OUTPUT_DP)
		return -EINVAL;
	if (outp->ior == NULL || outp->dp.lt.nr == 0)
		return 0;

	ret = nvkm_dispnv50_dp_aux_read(outp, DP_LANE0_1_STATUS, status,
	    sizeof(status));
	if (ret != 0) {
		*link_ok = false;
		return ret;
	}

	*link_ok = drm_dp_channel_eq_ok(status, outp->dp.lt.nr);
	return 0;
}

int
nvkm_dispnv50_dp_retrain_current(struct nvkm_softc *sc, uint32_t display_id,
    bool *link_ok)
{
	struct nvkm_outp *outp;
	int ret;

	if (link_ok == NULL)
		return -EINVAL;
	*link_ok = true;

	outp = nvkm_dispnv50_find_outp(sc, display_id);
	if (outp == NULL)
		return -ENODEV;
	if (outp->info.type != DCB_OUTPUT_DP)
		return -EINVAL;
	if (outp->ior == NULL || outp->dp.lt.nr == 0)
		return 0;
	if (outp->dp.lt.bw == 0 || outp->func == NULL ||
	    outp->func->dp.train == NULL) {
		*link_ok = false;
		return -ENODEV;
	}

	ret = outp->func->dp.train(outp, true);
	if (ret != 0) {
		*link_ok = false;
		return ret;
	}

	return nvkm_dispnv50_dp_link_check(sc, display_id, link_ok);
}

static void nvkm_dispnv50_output_disable_sideband(struct nvkm_outp *,
    uint32_t);

void
nvkm_dispnv50_output_prepare_abort(struct nvkm_softc *sc,
    struct nvkm_dispnv50_output_prepare *prepare)
{
	struct nvkm_outp *outp;

	if (prepare == NULL)
		return;

	if (prepare->valid && prepare->acquired && !prepare->consumed) {
		outp = nvkm_dispnv50_find_outp(sc, prepare->display_id);
		if (outp != NULL && outp->ior != NULL && outp->func != NULL &&
		    outp->func->release != NULL) {
			nvkm_dispnv50_output_disable_sideband(outp,
			    prepare->head);
			outp->func->release(outp);
		}
	}

	memset(prepare, 0, sizeof(*prepare));
}

int
nvkm_dispnv50_output_prepare(struct nvkm_softc *sc,
    struct drm_display_mode *mode, uint32_t head, uint32_t display_id,
    const struct nvkm_dispnv50_head_config *config,
    struct nvkm_dispnv50_output_prepare *prepare)
{
	struct nvkm_dispnv50_state *state;
	struct nvkm_outp *outp;
	struct nv50_core *core;
	int ret = 0;

	if (prepare == NULL)
		return -EINVAL;
	memset(prepare, 0, sizeof(*prepare));
	if (sc == NULL || sc->disp == NULL || mode == NULL || display_id == 0)
		return -ENODEV;

	/*
	 * Output prepare runs before drm_atomic_helper_swap_state().
	 *
	 * Ownership:
	 *   The bridge owns the dispnv50 core channel created here. The prepared
	 *   route only borrows it for synchronous validation and does not retain
	 *   the core pointer.
	 *
	 * Lifetime:
	 *   The core channel remains owned by sc->dispnv50 until KMS teardown.
	 *   A failed prepare leaves no acquired output route behind.
	 *
	 * Threading:
	 *   Called from the atomic commit path while modeset locks serialize
	 *   display state changes. It may sleep in RM/channel allocation and must
	 *   not be called from interrupt context.
	 */
	ret = nvkm_dispnv50_core_init(sc);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	if (state == NULL || state->disp.core == NULL)
		return -ENODEV;
	core = state->disp.core;

	outp = nvkm_dispnv50_find_outp(sc, display_id);
	if (outp == NULL)
		return -ENODEV;

	prepare->valid = true;
	prepare->display_id = display_id;
	prepare->head = head;
	prepare->outp_index = outp->index;
	prepare->output_type = outp->info.type;
	prepare->config.bpc = 8;
	prepare->config.dither_mode = NVKM_DISPNV50_DITHER_MODE_AUTO;
	prepare->config.dither_depth = NVKM_DISPNV50_DITHER_DEPTH_AUTO;
	prepare->config.scaling_mode = DRM_MODE_SCALE_NONE;
	prepare->config.underscan_mode = NVKM_DISPNV50_UNDERSCAN_OFF;
	if (config != NULL)
		prepare->config = *config;

	if (outp->ior == NULL) {
		if (outp->func == NULL || outp->func->acquire == NULL) {
			ret = -ENODEV;
			goto fail;
		}
		ret = outp->func->acquire(outp, prepare->config.audio_enabled);
		if (ret != 0)
			goto fail;
		prepare->acquired = true;
	}

	if (outp->ior == NULL || core->func == NULL ||
	    core->func->sor == NULL || core->func->sor->ctrl == NULL) {
		ret = -ENODEV;
		goto fail;
	}
	prepare->ior_id = outp->ior->id;
	prepare->ior_link = outp->ior->asy.link;
	if (prepare->ior_id < 0 ||
	    (uint32_t)prepare->ior_id >= nvkm_dispnv50_sor_capacity(sc)) {
		ret = -EINVAL;
		goto fail;
	}

	switch (outp->info.type) {
	case DCB_OUTPUT_TMDS:
		if (outp->ior->func == NULL ||
		    outp->ior->func->hdmi == NULL ||
		    outp->ior->func->hdmi->ctrl == NULL) {
			ret = -ENODEV;
			goto fail;
		}
		prepare->sor_proto = (outp->ior->asy.link & 1) ?
		    NVC37D_SOR_SET_CONTROL_PROTOCOL_SINGLE_TMDS_A :
		    NVC37D_SOR_SET_CONTROL_PROTOCOL_SINGLE_TMDS_B;
		break;
	case DCB_OUTPUT_DP:
		if (outp->func == NULL || outp->func->dp.train == NULL ||
		    outp->ior->func == NULL || outp->ior->func->dp == NULL ||
		    outp->ior->func->dp->sst == NULL) {
			ret = -ENODEV;
			goto fail;
		}
		ret = nvkm_dispnv50_dp_prepare(sc, outp, mode,
		    prepare->config.bpc, prepare);
		if (ret != 0)
			goto fail;
		prepare->sor_proto = (outp->ior->asy.link & 1) ?
		    NVC37D_SOR_SET_CONTROL_PROTOCOL_DP_A :
		    NVC37D_SOR_SET_CONTROL_PROTOCOL_DP_B;
		break;
	default:
		nvkm_infof(sc->dev,
		    "drm: dispnv50 prepare unsupported display=0x%x outp=%02x"
		    " type=0x%02x\n", display_id, outp->index,
		    outp->info.type);
		ret = -ENOSYS;
		goto fail;
	}

	nvkm_infof(sc->dev,
	    "drm: dispnv50 prepared display=0x%x outp=%02x sor=%d link=%u"
	    " type=0x%02x acquired=%d audio=%d eld=%u\n", display_id, outp->index,
	    prepare->ior_id, prepare->ior_link, outp->info.type,
	    prepare->acquired, prepare->config.audio_enabled,
	    (unsigned int)prepare->config.eld_size);
	if (prepare->output_type == DCB_OUTPUT_DP && prepare->dp_sst.valid) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 prepared dp outp=%02x lanes=%u "
		    "bw=0x%02x wm=%u hsym=%u vsym=%u ef=%d\n",
		    outp->index, prepare->dp_sst.lanes,
		    prepare->dp_sst.link_bw, prepare->dp_sst.watermark,
		    prepare->dp_sst.hblank_symbols,
		    prepare->dp_sst.vblank_symbols,
		    prepare->dp_enhanced_framing);
	}
	return 0;

fail:
	nvkm_dispnv50_output_prepare_abort(sc, prepare);
	return ret;
}

/*
 * Enable output audio side channels for the routed output.
 *
 * Ownership:
 *   Borrows outp, its assigned IOR, and the prepared ELD snapshot. The helper
 *   does not retain any pointer and does not take ownership of ELD bytes.
 *
 * Lifetime:
 *   Must run after the SOR route has been accepted and before the prepared
 *   commit is consumed. The prepared ELD remains valid for this synchronous
 *   call because nvkm_dispnv50_output_prepare copied it from DRM connector
 *   state.
 *
 * Threading:
 *   Called from serialized KMS atomic commit context. It may issue GSP/RM
 *   controls and must not run from interrupt context.
 */
static void
nvkm_dispnv50_output_enable_audio(struct nvkm_softc *sc, struct nvkm_outp *outp,
    uint32_t head, const struct nvkm_dispnv50_output_prepare *prepare)
{
	struct nvkm_ior *ior;

	if (prepare == NULL || !prepare->config.audio_enabled ||
	    prepare->config.eld_size == 0)
		return;
	if (outp == NULL || outp->ior == NULL || outp->ior->func == NULL)
		return;

	ior = outp->ior;
	if (ior->func->hda == NULL || ior->func->hda->eld == NULL)
		return;
	ior->func->hda->eld(ior, (int)head,
	    (uint8_t *)prepare->config.eld, prepare->config.eld_size);

	switch (outp->info.type) {
	case DCB_OUTPUT_TMDS:
		if (ior->func->hdmi != NULL && ior->func->hdmi->audio != NULL)
			ior->func->hdmi->audio(ior, (int)head, true);
		break;
	case DCB_OUTPUT_DP:
		if (ior->func->dp != NULL && ior->func->dp->audio != NULL)
			ior->func->dp->audio(ior, (int)head, true);
		break;
	default:
		break;
	}

	if (sc != NULL && sc->kms_push_trace) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 audio enabled outp=%02x head=%u eld=%u\n",
		    outp->index, head, (unsigned int)prepare->config.eld_size);
	}
}

static int
nvkm_dispnv50_route_output_prepared(struct nvkm_softc *sc,
    struct nv50_core *core,
    struct nv50_head_atom *asyh, struct drm_display_mode *mode, uint32_t head,
    struct nvkm_dispnv50_output_prepare *prepare)
{
	struct nvkm_outp *outp;
	u32 ctrl;
	int ret;

	if (prepare == NULL || !prepare->valid || prepare->display_id == 0)
		return -EINVAL;

	outp = nvkm_dispnv50_find_outp(sc, prepare->display_id);
	if (outp == NULL || outp->ior == NULL)
		return -ENODEV;
	if (outp->ior->id != prepare->ior_id)
		return -ESTALE;
	if (core == NULL || core->func == NULL ||
	    core->func->sor == NULL || core->func->sor->ctrl == NULL)
		return -ENODEV;

	switch (prepare->output_type) {
	case DCB_OUTPUT_TMDS:
		ret = nvkm_dispnv50_hdmi_enable(sc, outp, mode, head,
		    &prepare->config.hdmi);
		if (ret != 0)
			return ret;
		break;
	case DCB_OUTPUT_DP:
		memcpy(outp->dp.dpcd, prepare->dp_dpcd,
		    MIN((size_t)NVKM_DISPNV50_DP_DPCD_SIZE,
		    (size_t)DP_RECEIVER_CAP_SIZE));
		ret = nvkm_dispnv50_dp_enable_candidates(sc, outp, mode, head,
		    prepare->dp_max_rate, prepare->dp_max_lanes,
		    prepare->dp_min_rate, prepare->config.bpc,
		    prepare->dp_enhanced_framing, &prepare->dp_sst);
		if (ret != 0)
			return ret;
		break;
	default:
		nvkm_infof(sc->dev,
		    "drm: dispnv50 route unsupported display=0x%x outp=%02x"
		    " type=0x%02x\n", prepare->display_id, outp->index,
		    prepare->output_type);
		return -ENOSYS;
	}

	ctrl = NVVAL(NVC37D, SOR_SET_CONTROL, PROTOCOL, prepare->sor_proto) |
	    BIT(head);

	ret = core->func->sor->ctrl(core, outp->ior->id, ctrl, asyh);
	if (ret == 0) {
		nvkm_dispnv50_output_enable_audio(sc, outp, head, prepare);
		nvkm_infof(sc->dev,
		    "drm: dispnv50 route display=0x%x outp=%02x sor=%d link=%u proto=%u head=%u type=0x%02x\n",
		    prepare->display_id, outp->index, outp->ior->id,
		    outp->ior->asy.link, prepare->sor_proto, head,
		    prepare->output_type);
	}
	return ret;
}

/*
 * Disable output-specific side channels for the currently routed output.
 *
 * Ownership:
 *   Borrows outp and its currently assigned IOR for this disable operation.
 *   The caller still owns the route and is responsible for clearing SOR
 *   ownership, when required, and releasing outp after this helper returns.
 *
 * Lifetime:
 *   No pointer is retained. This helper must run before outp->func->release(),
 *   while outp->ior and outp->ior->asy.outp still describe the active route
 *   or the prepared route being rolled back.
 *
 * Threading:
 *   Called from atomic commit disable and prepared-route rollback paths under
 *   DRM modeset serialization. It may issue GSP/RM controls and must not run
 *   from interrupt context.
 */
static void
nvkm_dispnv50_output_disable_sideband(struct nvkm_outp *outp, uint32_t head)
{
	struct nvkm_ior *ior;

	if (outp == NULL || outp->ior == NULL || outp->ior->func == NULL)
		return;

	ior = outp->ior;
	switch (outp->info.type) {
	case DCB_OUTPUT_TMDS:
		if (ior->func->hdmi != NULL && ior->func->hdmi->audio != NULL)
			ior->func->hdmi->audio(ior, (int)head, false);
		if (ior->func->hdmi != NULL && ior->func->hdmi->ctrl != NULL)
			ior->func->hdmi->ctrl(ior, (int)head, false, 0, 0);
		break;
	case DCB_OUTPUT_DP:
		if (ior->func->dp != NULL && ior->func->dp->audio != NULL)
			ior->func->dp->audio(ior, (int)head, false);
		break;
	default:
		break;
	}
	if (ior->func->hda != NULL && ior->func->hda->hpd != NULL)
		ior->func->hda->hpd(ior, (int)head, false);
}

/*
 * Emit the clear half of a modeset transaction.
 *
 * Ownership:
 *   Borrows the bridge-owned head/window/output objects for one atomic commit.
 *   When @submit_disable is false, the emitted methods remain pending in their
 *   push buffers and no output ownership is released.
 *
 * Lifetime:
 *   @submit_disable=true crosses the hardware-visible UPDATE boundary and may
 *   release the old output route after the core update.  @submit_disable=false
 *   defers that boundary to the later enable/update flush in the same atomic
 *   tail, matching nouveau's non-flush-disable path.
 *
 * Threading:
 *   Called only from serialized KMS commit context.  It may sleep while
 *   reserving push-buffer space or waiting for display notifiers when
 *   @submit_disable is true.
 */
int
nvkm_dispnv50_modeset_disable(struct nvkm_softc *sc, uint32_t head,
    uint32_t win, uint32_t display_id, bool submit_disable)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_head_atom asyh;
	struct nv50_head *nvhead;
	struct nv50_core *core;
	struct nv50_wndw *wndw;
	struct nvkm_dispnv50_head_armed *head_armed;
	struct nvkm_dispnv50_wndw_armed *wndw_armed;
	struct nvkm_outp *outp = NULL;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	bool clear_cursor;
	bool clear_display;
	bool clear_olut;
	bool clear_output;
	bool cursor_cleared = false;
	bool split_window_disable = false;
	bool window_clear_emitted = false;
	int ret;

	if (sc == NULL || sc->disp == NULL)
		return -ENODEV;

	ret = nvkm_dispnv50_wndw_init(sc, win);
	if (ret != 0)
		return ret;
	ret = nvkm_dispnv50_head_init(sc, head);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	if (state == NULL || state->disp.core == NULL ||
	    head >= nitems(state->head) || win >= nitems(state->wndw) ||
	    state->wndw[win] == NULL)
		return -ENODEV;

	core = state->disp.core;
	nvhead = &state->head[head];
	wndw = state->wndw[win];
	head_armed = nvkm_dispnv50_head_armed_state(state, head);
	wndw_armed = nvkm_dispnv50_wndw_armed_state(state, wndw);
	if (core->func == NULL || core->func->update == NULL)
		return -ENODEV;
	if (display_id == 0 && state->audit_current.valid &&
	    state->audit_current.head == head)
		display_id = state->audit_current.display_id;

	if (display_id != 0) {
		outp = nvkm_dispnv50_find_outp(sc, display_id);
		if (outp == NULL)
			return -ENODEV;
		if (outp->ior != NULL &&
		    (outp->func == NULL || outp->func->release == NULL))
			return -ENODEV;
		if (outp->ior != NULL &&
		    (core->func == NULL || core->func->sor == NULL ||
		    core->func->sor->ctrl == NULL))
			return -ENODEV;
	}

	if (head_armed != NULL && head_armed->valid) {
		clear_cursor = head_armed->cursor;
		clear_display = head_armed->display;
		clear_olut = head_armed->olut;
		clear_output = head_armed->output;
	} else {
		clear_cursor = false;
		clear_display = display_id != 0;
		clear_olut = nvkm_dispnv50_head_programs_olut(nvhead);
		clear_output = outp != NULL && outp->ior != NULL;
	}
	if (clear_output && (outp == NULL || outp->ior == NULL))
		return -ENODEV;

	memset(&asyh, 0, sizeof(asyh));
	if (clear_cursor) {
		if (nvhead->func == NULL || nvhead->func->curs_clr == NULL)
			return -ENODEV;
		ret = nvhead->func->curs_clr(nvhead);
		if (ret != 0)
			return ret;
		/*
		 * Cursor clear updates only HEAD cursor context.  Nouveau's
		 * cursor clear path does not submit cursor immediate channel
		 * state, so the following core UPDATE must not carry a CURS
		 * interlock partner.
		 */
		interlock[NV50_DISP_INTERLOCK_CORE] = 1;
		cursor_cleared = true;
	}
	if (clear_olut) {
		if (nvhead->func == NULL || nvhead->func->olut_clr == NULL)
			return -ENODEV;
		ret = nvhead->func->olut_clr(nvhead);
		if (ret != 0)
			return ret;
		interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	}

	/*
	 * Cross a real window-disable boundary before detaching the HEAD when
	 * a submit-time modeset disable starts from an armed window image or
	 * window-side color resource.
	 *
	 * Ownership:
	 *   Borrows the current window/core/state objects and consumes no output
	 *   ownership.  nvkm_dispnv50_window_disable() owns only the transient
	 *   notifier wait for this commit boundary.
	 *
	 * Lifetime:
	 *   The old window image/color resources are cleared and published in
	 *   the armed mirror before any core HEAD/output detach is submitted.
	 *   The following modeset-disable clear therefore only deals with
	 *   bookkeeping resources, not with stale scanout-visible state.
	 *
	 * Threading:
	 *   Serialized KMS commit context only.  This path may sleep waiting for
	 *   the window notifier, matching the synchronous submit_disable contract.
	 */
	if (submit_disable && wndw_armed != NULL && wndw_armed->valid &&
	    (wndw_armed->image || wndw_armed->xlut || wndw_armed->csc)) {
		ret = nvkm_dispnv50_window_disable(sc, state, core, wndw);
		if (ret != 0)
			return ret;
		wndw_armed = nvkm_dispnv50_wndw_armed_state(state, wndw);
		split_window_disable = true;
	}

	ret = nvkm_dispnv50_wndw_modeset_disable_resources(state, wndw,
	    &window_clear_emitted);
	if (ret != 0)
		return ret;
	/*
	 * Only submit a window interlock when this transaction emitted window
	 * methods.  A split window-disable above has already crossed the window
	 * UPDATE boundary; forcing a no-op window partner here can leave core
	 * waiting for a state transition that the window channel has no reason
	 * to perform.
	 */
	if (window_clear_emitted)
		interlock[NV50_DISP_INTERLOCK_WNDW] |= wndw->interlock.data;

	/*
	 * Match nouveau's atomic tail ordering: head resource clears are
	 * emitted first, window clears second, and only then is the output path
	 * detached.  Turing validates the core/window interlock against both
	 * sides of that staged transaction; clearing the head display route before
	 * the window image is cleared can leave the core channel waiting for an
	 * impossible window state.
	 */
	if (clear_output && outp != NULL && outp->ior != NULL)
		nvkm_dispnv50_output_disable_sideband(outp, head);
	if (clear_display) {
		if (nvhead->func == NULL || nvhead->func->display_id == NULL)
			return -ENODEV;
		ret = nvhead->func->display_id(nvhead, 0);
		if (ret != 0)
			return ret;
		interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	}
	if (clear_output && outp != NULL && outp->ior != NULL) {
		ret = core->func->sor->ctrl(core, outp->ior->id, 0, &asyh);
		if (ret != 0)
			return ret;
		interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	}

	if (sc->kms_push_trace) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 modeset disable plan head=%u win=%u "
		    "display=0x%x submit=%d split_window=%d clear cursor=%d "
		    "display=%d olut=%d output=%d window_clear=%d "
		    "head_armed=%d/%d/%d/%d/%d "
		    "wndw_armed=%d/%d/%d/%d/%d/%d interlock core=0x%x "
		    "curs=0x%x wndw=0x%x wimm=0x%x\n",
		    head, win, display_id, submit_disable, split_window_disable,
		    clear_cursor, clear_display, clear_olut, clear_output,
		    window_clear_emitted,
		    head_armed != NULL && head_armed->valid,
		    head_armed != NULL && head_armed->display,
		    head_armed != NULL && head_armed->output,
		    head_armed != NULL && head_armed->olut,
		    head_armed != NULL && head_armed->cursor,
		    wndw_armed != NULL && wndw_armed->valid,
		    wndw_armed != NULL && wndw_armed->ntfy,
		    wndw_armed != NULL && wndw_armed->sema,
		    wndw_armed != NULL && wndw_armed->xlut,
		    wndw_armed != NULL && wndw_armed->csc,
		    wndw_armed != NULL && wndw_armed->image,
		    interlock[NV50_DISP_INTERLOCK_CORE],
		    interlock[NV50_DISP_INTERLOCK_CURS],
		    interlock[NV50_DISP_INTERLOCK_WNDW],
		    interlock[NV50_DISP_INTERLOCK_WIMM]);
	}

	if (!submit_disable)
		return 0;

	if ((interlock[NV50_DISP_INTERLOCK_WNDW] & wndw->interlock.data) != 0) {
		if (wndw->func->update == NULL)
			return -ENODEV;
		if (sc->kms_push_trace) {
			nvkm_infof(sc->dev,
			    "drm: dispnv50 modeset disable submit window "
			    "head=%u win=%u interlock core=0x%x curs=0x%x "
			    "wndw=0x%x wimm=0x%x\n", head, win,
			    interlock[NV50_DISP_INTERLOCK_CORE],
			    interlock[NV50_DISP_INTERLOCK_CURS],
			    interlock[NV50_DISP_INTERLOCK_WNDW],
			    interlock[NV50_DISP_INTERLOCK_WIMM]);
		}
		ret = wndw->func->update(wndw, interlock);
		if (ret != 0)
			return ret;
	}
	if (interlock[NV50_DISP_INTERLOCK_CORE] != 0) {
		if (sc->kms_push_trace) {
			nvkm_infof(sc->dev,
			    "drm: dispnv50 modeset disable submit core "
			    "head=%u win=%u interlock core=0x%x curs=0x%x "
			    "wndw=0x%x wimm=0x%x\n", head, win,
			    interlock[NV50_DISP_INTERLOCK_CORE],
			    interlock[NV50_DISP_INTERLOCK_CURS],
			    interlock[NV50_DISP_INTERLOCK_WNDW],
			    interlock[NV50_DISP_INTERLOCK_WIMM]);
		}
		ret = nvkm_dispnv50_core_commit_notify_best_effort(sc, state, core,
		    interlock);
		if (ret != 0)
			return ret;
	}

	if (clear_output && outp != NULL && outp->ior != NULL)
		outp->func->release(outp);
	if (cursor_cleared)
		nvkm_dispnv50_cursor_audit_capture(state, NULL, NULL, head,
		    false, false);
	nvkm_dispnv50_head_mark_disabled(state, head);
	nvkm_dispnv50_wndw_mark_disabled(state, wndw);

	nvkm_dispnv50_audit_capture(state, &state->audit_current,
	    NVKM_DISPNV50_AUDIT_CRTC_DISABLE, head, win, display_id, false,
	    true, true);
	state->scanout_user = false;
	memset(&state->audit_pending, 0, sizeof(state->audit_pending));
	nvkm_infof(sc->dev,
	    "drm: dispnv50 modeset disabled head=%u win=%u display=0x%x\n",
	    head, win, display_id);
	return 0;
}

int
nvkm_dispnv50_cursor_update(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, bool legacy_cursor_update)
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
	/*
	 * Cursor image set writes HEAD cursor context on the core channel and
	 * cursor position on the immediate channel.  On Turing's cursor class
	 * there is no ordinary cursor window image state to interlock with the
	 * core UPDATE; nouveau leaves CURS out of SET_INTERLOCK_FLAGS here.
	 */
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;

	ret = curs->immd->point(curs, &asyw);
	if (ret != 0)
		return ret;
	ret = nvkm_dispnv50_cursor_commit_wimm(curs, interlock);
	if (ret != 0)
		return ret;
	ret = nvkm_dispnv50_cursor_commit_core(sc, state, core, interlock,
	    legacy_cursor_update);
	if (ret != 0)
		return ret;

	nvkm_dispnv50_cursor_audit_capture(state, crtc->cursor->state, bo,
	    head, true, false);
	nvkm_dispnv50_head_mark_cursor(state, head, true);
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
	nvkm_dispnv50_head_mark_cursor(state, head, true);
	state->audit_cursor[head].x = x;
	state->audit_cursor[head].y = y;
	return 0;
}

int
nvkm_dispnv50_cursor_disable(struct nvkm_softc *sc, uint32_t head,
    bool legacy_cursor_update)
{
	struct nvkm_dispnv50_state *state;
	struct nvkm_dispnv50_head_armed *head_armed;
	struct nv50_head *nvhead;
	struct nv50_core *core;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	bool clear_cursor;
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
	head_armed = nvkm_dispnv50_head_armed_state(state, head);
	if (head_armed != NULL && head_armed->valid)
		clear_cursor = head_armed->cursor;
	else
		clear_cursor = head < nitems(state->audit_cursor) &&
		    state->audit_cursor[head].valid &&
		    state->audit_cursor[head].enabled;

	if (!clear_cursor) {
		nvkm_dispnv50_head_mark_cursor(state, head, false);
		nvkm_dispnv50_cursor_audit_capture(state, NULL, NULL, head,
		    false, false);
		nvkm_infof(sc->dev,
		    "drm: dispnv50 cursor already disabled head=%u\n", head);
		return 0;
	}
	if (nvhead->func == NULL || nvhead->func->curs_clr == NULL)
		return -ENODEV;

	ret = nvhead->func->curs_clr(nvhead);
	if (ret != 0)
		return ret;
	/*
	 * Cursor hide clears only HEAD cursor context.  It must neither submit
	 * a cursor immediate UPDATE nor name a CURS interlock partner, because
	 * nouveau's clear path leaves point/update traffic to cursor set/move
	 * commits and sends the clear through the core channel alone.
	 */
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	ret = nvkm_dispnv50_cursor_commit_core(sc, state, core, interlock,
	    legacy_cursor_update);
	if (ret != 0)
		return ret;

	nvkm_dispnv50_cursor_audit_capture(state, NULL, NULL, head, false,
	    false);
	nvkm_dispnv50_head_mark_cursor(state, head, false);
	nvkm_infof(sc->dev, "drm: dispnv50 cursor disabled head=%u\n", head);
	return 0;
}

int
nvkm_dispnv50_color_update(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, uint32_t win)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_head_atom asyh;
	struct nv50_wndw_atom asyw;
	struct nv50_head *nvhead;
	struct nv50_wndw *wndw = NULL;
	struct nv50_core *core;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	bool window_color;
	int ret;

	if (sc == NULL || crtc == NULL || crtc->state == NULL ||
	    !crtc->state->active || sc->disp == NULL)
		return (-ENODEV);

	window_color = crtc->state->degamma_lut != NULL || crtc->state->ctm != NULL;
	if (window_color) {
		ret = nvkm_dispnv50_wndw_init(sc, win);
		if (ret != 0)
			return ret;
	}
	ret = nvkm_dispnv50_head_init(sc, head);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	if (state == NULL || state->disp.core == NULL ||
	    head >= nitems(state->head))
		return (-ENODEV);
	if (window_color && (win >= nitems(state->wndw) ||
	    state->wndw[win] == NULL))
		return (-ENODEV);

	core = state->disp.core;
	nvhead = &state->head[head];
	if (window_color)
		wndw = state->wndw[win];
	if (core->func == NULL || core->func->update == NULL ||
	    (window_color && (wndw->func == NULL || wndw->func->update == NULL)))
		return (-ENODEV);

	memset(&asyw, 0, sizeof(asyw));
	nvkm_dispnv50_head_atom_fill(&asyh, crtc->state);

	ret = nvkm_dispnv50_head_olut_set(sc, state, core, nvhead, &asyh,
	    crtc->state);
	if (ret != 0)
		return ret;
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;

	if (window_color) {
		ret = nvkm_dispnv50_wndw_ilut_set(sc, state, wndw, &asyw,
		    crtc->state);
		if (ret != 0)
			return ret;
		ret = nvkm_dispnv50_wndw_csc_set(sc, state, wndw, &asyw,
		    crtc->state);
		if (ret != 0)
			return ret;
		interlock[NV50_DISP_INTERLOCK_WNDW] |= wndw->interlock.data;

		ret = wndw->func->update(wndw, interlock);
		if (ret != 0)
			return ret;
	}
	/*
	 * Gamma-only commits update the head OLUT and must not rewrite the window
	 * ILUT/CSC.  Runtime head-only updates do not own a reliable core notifier
	 * completion on this path, so they follow the same non-notifying core
	 * UPDATE rule used by legacy cursor updates.  Window-side color is only
	 * part of this runtime commit when the CRTC state carries degamma or CTM
	 * data.  The armed mirrors are updated only after UPDATE succeeds, so a
	 * later modeset disable clears exactly the resources that became visible.
	 */
	if (!window_color) {
		ret = core->func->update(core, interlock, false);
		if (ret == 0)
			nvkm_dispnv50_head_mark_olut_programmed(state, head,
			    nvhead);
		return ret;
	}
	ret = nvkm_dispnv50_core_commit_notify(sc, state, core, interlock);
	if (ret == 0) {
		nvkm_dispnv50_head_mark_olut_programmed(state, head, nvhead);
		nvkm_dispnv50_wndw_mark_color_programmed(state, wndw,
		    crtc->state);
	}
	return ret;
}

int
nvkm_dispnv50_head_update(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, const struct nvkm_dispnv50_head_config *config,
    bool update_view, bool update_dither)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_head_atom asyh;
	struct nv50_head *nvhead;
	struct nv50_core *core;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	int ret;

	if (sc == NULL || crtc == NULL || crtc->state == NULL ||
	    !crtc->state->active || config == NULL || sc->disp == NULL)
		return (-ENODEV);

	ret = nvkm_dispnv50_head_init(sc, head);
	if (ret != 0)
		return ret;

	state = sc->dispnv50;
	if (state == NULL || state->disp.core == NULL ||
	    head >= nitems(state->head))
		return (-ENODEV);

	core = state->disp.core;
	nvhead = &state->head[head];
	if (core->func == NULL || core->func->update == NULL ||
	    nvhead->func == NULL)
		return (-ENODEV);
	if (!update_view && !update_dither)
		return (0);

	nvkm_dispnv50_head_atom_fill(&asyh, crtc->state);
	nvkm_dispnv50_head_apply_view(&asyh, config);
	if (update_dither)
		nvkm_dispnv50_head_apply_config(&asyh, crtc, config);

	/*
	 * Ownership:
	 *   HEAD-only connector updates borrow the committed CRTC mode and
	 *   connector scalar config.  They do not own the active window image,
	 *   output route, SOR ownership, or OLUT memory.
	 *
	 * Lifetime:
	 *   This is nouveau's scaler/dither set path without a modeset.  The
	 *   active scanout route remains armed; only HEAD methods are submitted
	 *   before the core UPDATE boundary.
	 *
	 * Threading:
	 *   Atomic-tail local.  No window or output interlock partner is named,
	 *   because no window/output method is emitted in this transaction.
	 */
	if (update_view && nvhead->func->view != NULL) {
		ret = nvhead->func->view(nvhead, &asyh);
		if (ret != 0)
			return ret;
		interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	}
	if (update_dither && nvhead->func->dither != NULL) {
		ret = nvhead->func->dither(nvhead, &asyh);
		if (ret != 0)
			return ret;
		interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	}

	if (interlock[NV50_DISP_INTERLOCK_CORE] == 0)
		return (0);

	ret = nvkm_dispnv50_core_commit_notify(sc, state, core, interlock);
	if (ret == 0 && sc->kms_push_trace) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 head update head=%u view=%ux%u->%ux%u "
		    "update_view=%d update_dither=%d\n",
		    head, asyh.view.iW, asyh.view.iH, asyh.view.oW,
		    asyh.view.oH, update_view, update_dither);
	}
	return ret;
}

int
nvkm_dispnv50_plane_update(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t win, uint32_t display_id, bool color_update)
{
	struct nvkm_dispnv50_state *state;
	const struct nvkm_dispnv50_wndw_armed *wndw_armed;
	struct nv50_head_atom asyh;
	struct nv50_head *nvhead;
	struct nv50_wndw *wndw;
	struct nv50_core *core;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	u32 head;
	bool async_update;
	bool program_window_color;
	int ret;

	if (sc == NULL || crtc == NULL || crtc->state == NULL || sc->disp == NULL)
		return -ENODEV;

	head = (u32)drm_crtc_index(crtc);
	ret = nvkm_dispnv50_wndw_init(sc, win);
	if (ret != 0)
		return ret;
	if (color_update) {
		ret = nvkm_dispnv50_head_init(sc, head);
		if (ret != 0)
			return ret;
	}

	state = sc->dispnv50;
	if (state == NULL || state->disp.core == NULL ||
	    win >= nitems(state->wndw) || state->wndw[win] == NULL)
		return -ENODEV;
	if (color_update && head >= nitems(state->head))
		return -ENODEV;

	core = state->disp.core;
	wndw = state->wndw[win];
	wndw_armed = nvkm_dispnv50_wndw_armed_state(state, wndw);
	if (display_id == 0 && state->audit_current.valid &&
	    state->audit_current.head == head)
		display_id = state->audit_current.display_id;

	ret = nvkm_dispnv50_select_scanout(sc, state, crtc, true);
	if (ret != 0) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 plane scanout select failed win=%u "
		    "format=0x%08x err=%d\n", win, state->scanout_format, ret);
		return ret;
	}

	if (color_update) {
		nvhead = &state->head[head];
		nvkm_dispnv50_head_atom_fill(&asyh, crtc->state);
		ret = nvkm_dispnv50_head_olut_set(sc, state, core, nvhead, &asyh,
		    crtc->state);
		if (ret != 0)
			return ret;
		interlock[NV50_DISP_INTERLOCK_CORE] = 1;
	}

	/*
	 * User framebuffer flips may use the async image-only path because DRM
	 * completes the page-flip event/out-fence at the later flip-done
	 * barrier.  Kernel/console scanout restore has no userspace flip event
	 * to publish that pending state, so program it synchronously with a
	 * window notifier.
	 */
	async_update = !color_update && state->scanout_user;
	program_window_color = color_update || wndw_armed == NULL ||
	    !wndw_armed->valid || !wndw_armed->image;
	ret = nvkm_dispnv50_window_program(sc, state, crtc, core, wndw,
	    interlock, false, async_update, async_update, program_window_color,
	    NVKM_DISPNV50_AUDIT_PLANE_UPDATE, head, display_id,
	    color_update ? "plane color update" :
	    (async_update ? "plane update" : "console restore"), NULL);
	if (ret == 0 && color_update)
		nvkm_dispnv50_head_mark_olut_programmed(state, head, nvhead);
	return ret;
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

static int
nvkm_dispnv50_atomic_enable_common(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, uint32_t win, uint32_t display_id,
    const struct nvkm_dispnv50_head_config *config,
    struct nvkm_dispnv50_output_prepare *prepare)
{
	struct nvkm_dispnv50_state *state;
	struct nv50_head_atom asyh;
	struct nv50_head *nvhead;
	struct nv50_wndw *wndw;
	struct nv50_core *core;
	struct drm_display_mode *mode;
	struct nvkm_dispnv50_output_prepare local_prepare;
	struct nvkm_dispnv50_output_prepare *route_prepare = prepare;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	bool update_submitted = false;
	bool sanitize_window;
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
	nvkm_dispnv50_head_apply_view(&asyh, config);
	nvkm_dispnv50_head_apply_config(&asyh, crtc, config);
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
	if (route_prepare == NULL) {
		memset(&local_prepare, 0, sizeof(local_prepare));
		ret = nvkm_dispnv50_output_prepare(sc, mode, head, display_id,
		    config, &local_prepare);
		if (ret != 0)
			goto fail;
		route_prepare = &local_prepare;
	}
	ret = nvkm_dispnv50_route_output_prepared(sc, core, &asyh, mode, head,
	    route_prepare);
	if (ret != 0)
		goto fail;
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;

	if (sc->kms_push_trace) {
		nvkm_infof(sc->dev,
		    "drm: dispnv50 head state head=%u view=%ux%u->%ux%u "
		    "wndw_mask=0x%x wndw_owned=0x%x\n",
		    head, asyh.view.iW, asyh.view.iH, asyh.view.oW,
		    asyh.view.oH, asyh.wndw.mask, asyh.wndw.owned);
	}

	/*
	 * Ownership: local scalar snapshot for this enable transaction only.
	 * Lifetime: valid until window_program() below; no state is retained.
	 * Threading: commit-tail local, protected by the DRM modeset locks.
	 *
	 * Sanitize only while importing firmware/GOP window ownership.  After
	 * nvkm has already disabled the window, restore must be a set-only
	 * transaction: the clear phase already ran in modeset_disable(), and
	 * mixing clear+set in the enable half violates nouveau's clr/set split.
	 */
	sanitize_window = core->assign_windows;
	if (core->assign_windows) {
		ret = core->func->wndw.owner(core);
		if (ret != 0)
			goto fail;
		ret = core->func->update(core, interlock, false);
		if (ret != 0)
			goto fail;
		route_prepare->consumed = true;
		core->assign_windows = false;
		memset(interlock, 0, sizeof(interlock));
	}

	ret = nvkm_dispnv50_head_olut_set(sc, state, core, nvhead, &asyh,
	    crtc->state);
	if (ret != 0)
		goto fail;
	interlock[NV50_DISP_INTERLOCK_CORE] = 1;

	ret = nvkm_dispnv50_window_program(sc, state, crtc, core, wndw,
	    interlock, sanitize_window, false, true, true,
	    NVKM_DISPNV50_AUDIT_ATOMIC_ENABLE, head, display_id, "bridge",
	    &update_submitted);
	if (ret != 0)
		goto fail;
	route_prepare->consumed = true;
	nvkm_dispnv50_head_mark_programmed(state, head, nvhead, display_id,
	    true);

	nvkm_infof(sc->dev,
	    "drm: dispnv50 bridge armed head=%u win=%u display=0x%x "
	    "scanout=0x%llx offset=0x%llx src=%u,%u %ux%u "
	    "dst=%d,%d %ux%u\n",
	    head, win, display_id,
	    (unsigned long long)(state->scanout_user ? state->scanout_offset :
	    nvkm_memory_addr(state->scanout)),
	    (unsigned long long)state->scanout_offset,
	    state->window_src_x >> 16, state->window_src_y >> 16,
	    state->window_src_w >> 16, state->window_src_h >> 16,
	    state->window_crtc_x, state->window_crtc_y,
	    state->window_crtc_w, state->window_crtc_h);
	return 0;

fail:
	if (route_prepare != NULL) {
		if (update_submitted)
			route_prepare->consumed = true;
		nvkm_dispnv50_output_prepare_abort(sc, route_prepare);
	}
	nvkm_infof(sc->dev,
	    "drm: dispnv50 bridge failed head=%u win=%u display=0x%x err=%d\n",
	    head, win, display_id, ret);
	if (ret == 0)
		return (-ENODEV);
	return ret;
}

int
nvkm_dispnv50_atomic_enable(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, uint32_t win, uint32_t display_id,
    const struct nvkm_dispnv50_head_config *config)
{
	return nvkm_dispnv50_atomic_enable_common(sc, crtc, head, win,
	    display_id, config, NULL);
}

int
nvkm_dispnv50_atomic_enable_prepared(struct nvkm_softc *sc,
    struct drm_crtc *crtc, uint32_t win,
    struct nvkm_dispnv50_output_prepare *prepare)
{
	if (prepare == NULL || !prepare->valid)
		return -EINVAL;
	return nvkm_dispnv50_atomic_enable_common(sc, crtc, prepare->head, win,
	    prepare->display_id, &prepare->config, prepare);
}

static void
nvkm_dispnv50_state_destroy(struct nvkm_softc *sc,
    struct nvkm_dispnv50_state *state)
{
	u32 i;

	if (state == NULL)
		return;

	/*
	 * Ownership:
	 *   Consumes the bridge-owned dispnv50 state, including display DMA
	 *   channels, push buffers, ctxdma objects, and bridge staging memory.
	 *   DRM mode_config objects and scanout GEM BO references are owned by
	 *   KMS helpers and are not freed here.
	 *
	 * Lifetime:
	 *   Called only after KMS teardown has drained auto-KMS/HPD work and
	 *   requested an atomic shutdown, so no new bridge programming callback
	 *   should observe this state.
	 *
	 * Threading:
	 *   Runs from device teardown context and may sleep in RM free and memory
	 *   unref paths. It must not be called from interrupt context.
	 */
	for (i = 0; i < nitems(state->curs); i++)
		nvkm_dispnv50_wndw_destroy(&state->curs[i]);
	for (i = 0; i < nitems(state->wndw); i++)
		nvkm_dispnv50_wndw_destroy(&state->wndw[i]);
	nvkm_dispnv50_core_destroy(&state->disp.core);

	nvkm_memory_unref(&state->ilut);
	state->ilut_offset = 0;
	nvkm_memory_unref(&state->olut);
	state->olut_offset = 0;
	nvkm_memory_unref(&state->scanout);
	state->scanout_offset = 0;
	nvkm_memory_unref(&state->sync_mem);
	memset(&state->sync_bo, 0, sizeof(state->sync_bo));
	if (sc != NULL)
		sc->dispnv50 = NULL;
	kfree(state);
}

void
nvkm_dispnv50_fini(struct nvkm_softc *sc)
{
	if (sc == NULL || sc->dispnv50 == NULL)
		return;

	nvkm_dispnv50_console_unregister(sc->dispnv50);
	nvkm_dispnv50_state_destroy(sc, sc->dispnv50);
}
