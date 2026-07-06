/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal atom state shim for selected dispnv50 MIT method
 * emitters.
 */
#ifndef _DFLY_DISPNV50_ATOM_H_
#define _DFLY_DISPNV50_ATOM_H_

#include <nvif/os.h>
#include <drm/drm_atomic.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_crtc.h>
#include <drm/drm_plane.h>

struct nv50_head_mode {
	bool interlace;
	u32 clock;
	struct {
		u16 active;
		u16 synce;
		u16 blanke;
		u16 blanks;
	} h;
	struct {
		u32 active;
		u16 synce;
		u16 blanke;
		u16 blanks;
		u16 blank2s;
		u16 blank2e;
		u16 blankus;
	} v;
};

struct nv50_lut_state {
	u64 offset;
	u8 mode;
	u16 size;
	u8 range;
	u8 output_mode;
	void (*load)(struct drm_color_lut *, int size, void __iomem *);
};

struct nv50_head_atom {
	struct drm_crtc_state state;
	struct {
		u32 mask;
		u32 owned;
		u32 olut;
	} wndw;
	struct {
		u16 iW;
		u16 iH;
		u16 oW;
		u16 oH;
	} view;
	struct nv50_head_mode mode;
	struct {
		bool visible;
		u32 handle;
		u64 offset;
		u8 buffer;
		u8 mode;
		u16 size;
		u8 range;
		u8 output_mode;
		void (*load)(struct drm_color_lut *, int size, void __iomem *);
	} olut;
	struct {
		bool visible;
		u32 handle;
		u64 offset;
		u8 format;
		u8 layout;
	} curs;
	struct {
		u8 depth;
		u8 crc_raster;
		u8 nhsync;
		u8 nvsync;
		u8 bpc;
	} or;
	struct {
		bool enable;
		u8 bits;
		u8 mode;
	} dither;
	struct {
		struct {
			u16 cos;
			u16 sin;
		} sat;
	} procamp;
};

struct nv50_wndw_atom {
	struct drm_plane_state state;
	struct {
		u32 interval;
		u32 mode;
		u16 w;
		u16 h;
		u8 blockh;
		u8 layout;
		u8 colorspace;
		u8 format;
		u32 blocks[4];
		u32 pitch[4];
		u32 handle[6];
		u64 offset[6];
	} image;
	struct {
		u32 matrix[12];
		bool valid;
	} csc;
	struct {
		u32 handle;
		struct nv50_lut_state i;
		struct nv50_lut_state o;
	} xlut;
	struct {
		u8 depth;
		u16 k1;
		u8 src_color;
		u8 dst_color;
	} blend;
	struct {
		u32 handle;
		u32 awaken;
		u64 offset;
	} ntfy;
	struct {
		u32 offset;
		u32 acquire;
		u32 release;
		u32 handle;
	} sema;
	struct {
		u16 x;
		u16 y;
	} point;
};

#endif
