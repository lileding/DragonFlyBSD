/*
 * Copyright 2018 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "head.h"
#include "atom.h"
#include "core.h"

#include <nvif/pushc37b.h>

#include <nvhw/class/clc37d.h>
#include <nvhw/class/clc57d.h>

#define NVKM_DISP_SCALE_1X 1024U

static int
headc37d_window_count(struct nv50_head *head)
{
	if (head != NULL && head->disp != NULL)
		return nv50_core_window_count(head->disp->core);
	return 8;
}

static bool
headc37d_is_c57d(struct nv50_head *head)
{
#ifdef NVKM_DFLY_GSP_DISPLAY_ONLY
	struct nv50_core *core;

	if (head == NULL || head->disp == NULL)
		return false;
	core = head->disp->core;
	if (core == NULL)
		return false;
	return (core->chan.dfly_oclass & 0xff) == 0x7d;
#else
	(void)head;
	return false;
#endif
}

static u32
headc37d_scale_factor(u16 in, u16 out)
{
	u64 factor;

	if (in == 0 || out == 0 || in <= out)
		return NVKM_DISP_SCALE_1X;

	factor = ((u64)in * NVKM_DISP_SCALE_1X + out - 1) / out;
	if (factor > 0xffff)
		return 0xffff;
	return (u32)factor;
}

static u32
headc37d_max_pixels_fetched_per_line(u16 width)
{
	u32 pixels;

	pixels = (((u32)width + 14U) * NVKM_DISP_SCALE_1X +
	    NVKM_DISP_SCALE_1X - 1U) >> 10;
	pixels += 8U;
	if (pixels > 0x7fff)
		return 0x7fff;
	return pixels;
}

static int
headc37d_window_first(struct nv50_head *head)
{
	return head->base.index * 2;
}

static int
headc37d_window_end(struct nv50_head *head)
{
	int end;
	int windows;

	end = headc37d_window_first(head) + 2;
	windows = headc37d_window_count(head);
	if (end - 2 >= windows)
		return end - 2;
	if (end > windows)
		end = windows;
	return end;
}

static int
headc37d_or(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &nv50_disp(head->base.base.dev)->core->chan.push;
	const int i = head->base.index;
	u8 depth;
	int ret;

	/*XXX: This is a dirty hack until OR depth handling is
	 *     improved later for deep colour etc.
	 */
	switch (asyh->or.depth) {
	case 6: depth = 5; break;
	case 5: depth = 4; break;
	case 2: depth = 1; break;
	case 0:	depth = 4; break;
	default:
		depth = asyh->or.depth;
		WARN_ON(1);
		break;
	}

	if ((ret = PUSH_WAIT(push, 2)))
		return ret;

	PUSH_MTHD(push, NVC37D, HEAD_SET_CONTROL_OUTPUT_RESOURCE(i),
		  NVVAL(NVC37D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, CRC_MODE, asyh->or.crc_raster) |
		  NVVAL(NVC37D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, HSYNC_POLARITY, asyh->or.nhsync) |
		  NVVAL(NVC37D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, VSYNC_POLARITY, asyh->or.nvsync) |
		  NVVAL(NVC37D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, PIXEL_DEPTH, depth) |
		  NVDEF(NVC37D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, COLOR_SPACE_OVERRIDE, DISABLE));
	return 0;
}

static int
headc37d_procamp(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &nv50_disp(head->base.base.dev)->core->chan.push;
	const int i = head->base.index;
	int ret;

	if ((ret = PUSH_WAIT(push, 2)))
		return ret;

	PUSH_MTHD(push, NVC37D, HEAD_SET_PROCAMP(i),
		  NVDEF(NVC37D, HEAD_SET_PROCAMP, COLOR_SPACE, RGB) |
		  NVDEF(NVC37D, HEAD_SET_PROCAMP, CHROMA_LPF, DISABLE) |
		  NVVAL(NVC37D, HEAD_SET_PROCAMP, SAT_COS, asyh->procamp.sat.cos) |
		  NVVAL(NVC37D, HEAD_SET_PROCAMP, SAT_SINE, asyh->procamp.sat.sin) |
		  NVDEF(NVC37D, HEAD_SET_PROCAMP, DYNAMIC_RANGE, VESA) |
		  NVDEF(NVC37D, HEAD_SET_PROCAMP, RANGE_COMPRESSION, DISABLE) |
		  NVDEF(NVC37D, HEAD_SET_PROCAMP, BLACK_LEVEL, GRAPHICS));
	return 0;
}

int
headc37d_dither(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &nv50_disp(head->base.base.dev)->core->chan.push;
	const int i = head->base.index;
	int ret;

	if ((ret = PUSH_WAIT(push, 2)))
		return ret;

	PUSH_MTHD(push, NVC37D, HEAD_SET_DITHER_CONTROL(i),
		  NVVAL(NVC37D, HEAD_SET_DITHER_CONTROL, ENABLE, asyh->dither.enable) |
		  NVVAL(NVC37D, HEAD_SET_DITHER_CONTROL, BITS, asyh->dither.bits) |
		  NVDEF(NVC37D, HEAD_SET_DITHER_CONTROL, OFFSET_ENABLE, DISABLE) |
		  NVVAL(NVC37D, HEAD_SET_DITHER_CONTROL, MODE, asyh->dither.mode) |
		  NVVAL(NVC37D, HEAD_SET_DITHER_CONTROL, PHASE, 0));
	return 0;
}

int
headc37d_curs_clr(struct nv50_head *head)
{
	struct nvif_push *push = &nv50_disp(head->base.base.dev)->core->chan.push;
	const int i = head->base.index;
	int ret;

	if ((ret = PUSH_WAIT(push, 4)))
		return ret;

	PUSH_MTHD(push, NVC37D, HEAD_SET_CONTROL_CURSOR(i),
		  NVDEF(NVC37D, HEAD_SET_CONTROL_CURSOR, ENABLE, DISABLE) |
		  NVDEF(NVC37D, HEAD_SET_CONTROL_CURSOR, FORMAT, A8R8G8B8));

	PUSH_MTHD(push, NVC37D, HEAD_SET_CONTEXT_DMA_CURSOR(i, 0), 0x00000000);
	return 0;
}

int
headc37d_curs_set(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &nv50_disp(head->base.base.dev)->core->chan.push;
	const int i = head->base.index;
	int ret;

	if ((ret = PUSH_WAIT(push, 7)))
		return ret;

	PUSH_MTHD(push, NVC37D, HEAD_SET_CONTROL_CURSOR(i),
		  NVDEF(NVC37D, HEAD_SET_CONTROL_CURSOR, ENABLE, ENABLE) |
		  NVVAL(NVC37D, HEAD_SET_CONTROL_CURSOR, FORMAT, asyh->curs.format) |
		  NVVAL(NVC37D, HEAD_SET_CONTROL_CURSOR, SIZE, asyh->curs.layout) |
		  NVVAL(NVC37D, HEAD_SET_CONTROL_CURSOR, HOT_SPOT_X, 0) |
		  NVVAL(NVC37D, HEAD_SET_CONTROL_CURSOR, HOT_SPOT_Y, 0) |
		  NVDEF(NVC37D, HEAD_SET_CONTROL_CURSOR, DE_GAMMA, NONE),

				HEAD_SET_CONTROL_CURSOR_COMPOSITION(i),
		  NVVAL(NVC37D, HEAD_SET_CONTROL_CURSOR_COMPOSITION, K1, 0xff) |
		  NVDEF(NVC37D, HEAD_SET_CONTROL_CURSOR_COMPOSITION, CURSOR_COLOR_FACTOR_SELECT,
								     K1) |
		  NVDEF(NVC37D, HEAD_SET_CONTROL_CURSOR_COMPOSITION, VIEWPORT_COLOR_FACTOR_SELECT,
								     NEG_K1_TIMES_SRC) |
		  NVDEF(NVC37D, HEAD_SET_CONTROL_CURSOR_COMPOSITION, MODE, BLEND));

	PUSH_MTHD(push, NVC37D, HEAD_SET_CONTEXT_DMA_CURSOR(i, 0), asyh->curs.handle);
	PUSH_MTHD(push, NVC37D, HEAD_SET_OFFSET_CURSOR(i, 0), asyh->curs.offset >> 8);
	return 0;
}

int
headc37d_curs_format(struct nv50_head *head, struct nv50_wndw_atom *asyw,
		     struct nv50_head_atom *asyh)
{
	asyh->curs.format = asyw->image.format;
	return 0;
}

static int
headc37d_olut_clr(struct nv50_head *head)
{
	struct nvif_push *push = &nv50_disp(head->base.base.dev)->core->chan.push;
	const int i = head->base.index;
	int ret;

	if ((ret = PUSH_WAIT(push, 2)))
		return ret;

	PUSH_MTHD(push, NVC37D, HEAD_SET_CONTEXT_DMA_OUTPUT_LUT(i), 0x00000000);
	return 0;
}

static int
headc37d_olut_set(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &nv50_disp(head->base.base.dev)->core->chan.push;
	const int i = head->base.index;
	int ret;

	if ((ret = PUSH_WAIT(push, 4)))
		return ret;

	PUSH_MTHD(push, NVC37D, HEAD_SET_CONTROL_OUTPUT_LUT(i),
		  NVVAL(NVC37D, HEAD_SET_CONTROL_OUTPUT_LUT, SIZE, asyh->olut.size) |
		  NVVAL(NVC37D, HEAD_SET_CONTROL_OUTPUT_LUT, RANGE, asyh->olut.range) |
		  NVVAL(NVC37D, HEAD_SET_CONTROL_OUTPUT_LUT, OUTPUT_MODE, asyh->olut.output_mode),

				HEAD_SET_OFFSET_OUTPUT_LUT(i), asyh->olut.offset >> 8,
				HEAD_SET_CONTEXT_DMA_OUTPUT_LUT(i), asyh->olut.handle);
	return 0;
}

static bool
headc37d_olut(struct nv50_head *head, struct nv50_head_atom *asyh, int size)
{
	if (size != 256 && size != 1024)
		return false;

	asyh->olut.size = size == 1024 ? NVC37D_HEAD_SET_CONTROL_OUTPUT_LUT_SIZE_SIZE_1025 :
					 NVC37D_HEAD_SET_CONTROL_OUTPUT_LUT_SIZE_SIZE_257;
	asyh->olut.range = NVC37D_HEAD_SET_CONTROL_OUTPUT_LUT_RANGE_UNITY;
	asyh->olut.output_mode = NVC37D_HEAD_SET_CONTROL_OUTPUT_LUT_OUTPUT_MODE_INTERPOLATE;
	asyh->olut.load = head907d_olut_load;
	return true;
}

static int
headc37d_mode(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &nv50_disp(head->base.base.dev)->core->chan.push;
	struct nv50_head_mode *m = &asyh->mode;
	const int i = head->base.index;
	int ret;

	if ((ret = PUSH_WAIT(push, 15)))
		return ret;

	PUSH_MTHD(push, NVC37D, HEAD_SET_RASTER_SIZE(i),
		  NVVAL(NVC37D, HEAD_SET_RASTER_SIZE, WIDTH, m->h.active) |
		  NVVAL(NVC37D, HEAD_SET_RASTER_SIZE, HEIGHT, m->v.active),

				HEAD_SET_RASTER_SYNC_END(i),
		  NVVAL(NVC37D, HEAD_SET_RASTER_SYNC_END, X, m->h.synce) |
		  NVVAL(NVC37D, HEAD_SET_RASTER_SYNC_END, Y, m->v.synce),

				HEAD_SET_RASTER_BLANK_END(i),
		  NVVAL(NVC37D, HEAD_SET_RASTER_BLANK_END, X, m->h.blanke) |
		  NVVAL(NVC37D, HEAD_SET_RASTER_BLANK_END, Y, m->v.blanke),

				HEAD_SET_RASTER_BLANK_START(i),
		  NVVAL(NVC37D, HEAD_SET_RASTER_BLANK_START, X, m->h.blanks) |
		  NVVAL(NVC37D, HEAD_SET_RASTER_BLANK_START, Y, m->v.blanks));

	//XXX:
	PUSH_NVSQ(push, NVC37D, 0x2074 + (i * 0x400), m->v.blank2e << 16 | m->v.blank2s);
	PUSH_NVSQ(push, NVC37D, 0x2008 + (i * 0x400), m->interlace);

	PUSH_MTHD(push, NVC37D, HEAD_SET_PIXEL_CLOCK_FREQUENCY(i),
		  NVVAL(NVC37D, HEAD_SET_PIXEL_CLOCK_FREQUENCY, HERTZ, m->clock * 1000));

	PUSH_MTHD(push, NVC37D, HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(i),
		  NVVAL(NVC37D, HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX, HERTZ, m->clock * 1000));

	return 0;
}

int
headc37d_view(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &nv50_disp(head->base.base.dev)->core->chan.push;
	const int i = head->base.index;
	const int first = headc37d_window_first(head);
	const int end = headc37d_window_end(head);
	const bool c57d = headc37d_is_c57d(head);
	const bool vupscale = asyh->view.oH > asyh->view.iH;
	const u32 hfactor =
	    headc37d_scale_factor(asyh->view.iW, asyh->view.oW);
	const u32 vfactor =
	    headc37d_scale_factor(asyh->view.iH, asyh->view.oH);
	const u32 max_pixels =
	    headc37d_max_pixels_fetched_per_line(asyh->view.iW);
	int win;
	int ret;

	if ((ret = PUSH_WAIT(push, 12 + (end - first) * 2)))
		return ret;

	if (c57d) {
		PUSH_MTHD(push, NVC57D, HEAD_SET_CONTROL_OUTPUT_SCALER(i),
			  NVDEF(NVC57D, HEAD_SET_CONTROL_OUTPUT_SCALER, VERTICAL_TAPS, TAPS_2) |
			  NVDEF(NVC57D, HEAD_SET_CONTROL_OUTPUT_SCALER, HORIZONTAL_TAPS, TAPS_2));

		PUSH_MTHD(push, NVC57D, HEAD_SET_VIEWPORT_SIZE_IN(i),
			  NVVAL(NVC57D, HEAD_SET_VIEWPORT_SIZE_IN, WIDTH, asyh->view.iW) |
			  NVVAL(NVC57D, HEAD_SET_VIEWPORT_SIZE_IN, HEIGHT, asyh->view.iH));

		PUSH_MTHD(push, NVC57D, HEAD_SET_VIEWPORT_POINT_OUT_ADJUST(i),
			  NVVAL(NVC57D, HEAD_SET_VIEWPORT_POINT_OUT_ADJUST, X, 0) |
			  NVVAL(NVC57D, HEAD_SET_VIEWPORT_POINT_OUT_ADJUST, Y, 0));

		PUSH_MTHD(push, NVC57D, HEAD_SET_VIEWPORT_SIZE_OUT(i),
			  NVVAL(NVC57D, HEAD_SET_VIEWPORT_SIZE_OUT, WIDTH, asyh->view.oW) |
			  NVVAL(NVC57D, HEAD_SET_VIEWPORT_SIZE_OUT, HEIGHT, asyh->view.oH));

		PUSH_MTHD(push, NVC57D, HEAD_SET_MAX_OUTPUT_SCALE_FACTOR(i),
			  NVVAL(NVC57D, HEAD_SET_MAX_OUTPUT_SCALE_FACTOR, HORIZONTAL, hfactor) |
			  NVVAL(NVC57D, HEAD_SET_MAX_OUTPUT_SCALE_FACTOR, VERTICAL, vfactor));

		for (win = first; win < end; win++) {
			PUSH_MTHD(push, NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS(win),
				  NVVAL(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, MAX_PIXELS_FETCHED_PER_LINE, max_pixels) |
				  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, ILUT_ALLOWED, TRUE) |
				  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, INPUT_SCALER_TAPS, TAPS_2) |
				  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, UPSCALING_ALLOWED, FALSE));
		}

		PUSH_MTHD(push, NVC57D, HEAD_SET_HEAD_USAGE_BOUNDS(i),
			  NVDEF(NVC57D, HEAD_SET_HEAD_USAGE_BOUNDS, CURSOR, USAGE_W256_H256) |
			  NVDEF(NVC57D, HEAD_SET_HEAD_USAGE_BOUNDS, OLUT_ALLOWED, TRUE) |
			  NVDEF(NVC57D, HEAD_SET_HEAD_USAGE_BOUNDS, OUTPUT_SCALER_TAPS, TAPS_2) |
			  NVVAL(NVC57D, HEAD_SET_HEAD_USAGE_BOUNDS, UPSCALING_ALLOWED, vupscale));
	} else {
		PUSH_MTHD(push, NVC37D, HEAD_SET_CONTROL_OUTPUT_SCALER(i),
			  NVDEF(NVC37D, HEAD_SET_CONTROL_OUTPUT_SCALER, VERTICAL_TAPS, TAPS_2) |
			  NVDEF(NVC37D, HEAD_SET_CONTROL_OUTPUT_SCALER, HORIZONTAL_TAPS, TAPS_2));

		PUSH_MTHD(push, NVC37D, HEAD_SET_VIEWPORT_SIZE_IN(i),
			  NVVAL(NVC37D, HEAD_SET_VIEWPORT_SIZE_IN, WIDTH, asyh->view.iW) |
			  NVVAL(NVC37D, HEAD_SET_VIEWPORT_SIZE_IN, HEIGHT, asyh->view.iH));

		PUSH_MTHD(push, NVC37D, HEAD_SET_VIEWPORT_POINT_OUT_ADJUST(i),
			  NVVAL(NVC37D, HEAD_SET_VIEWPORT_POINT_OUT_ADJUST, X, 0) |
			  NVVAL(NVC37D, HEAD_SET_VIEWPORT_POINT_OUT_ADJUST, Y, 0));

		PUSH_MTHD(push, NVC37D, HEAD_SET_VIEWPORT_SIZE_OUT(i),
			  NVVAL(NVC37D, HEAD_SET_VIEWPORT_SIZE_OUT, WIDTH, asyh->view.oW) |
			  NVVAL(NVC37D, HEAD_SET_VIEWPORT_SIZE_OUT, HEIGHT, asyh->view.oH));

		PUSH_MTHD(push, NVC37D, HEAD_SET_MAX_OUTPUT_SCALE_FACTOR(i),
			  NVVAL(NVC37D, HEAD_SET_MAX_OUTPUT_SCALE_FACTOR, HORIZONTAL, hfactor) |
			  NVVAL(NVC37D, HEAD_SET_MAX_OUTPUT_SCALE_FACTOR, VERTICAL, vfactor));

		for (win = first; win < end; win++) {
			PUSH_MTHD(push, NVC37D, WINDOW_SET_WINDOW_USAGE_BOUNDS(win),
				  NVVAL(NVC37D, WINDOW_SET_WINDOW_USAGE_BOUNDS, MAX_PIXELS_FETCHED_PER_LINE, max_pixels) |
				  NVDEF(NVC37D, WINDOW_SET_WINDOW_USAGE_BOUNDS, INPUT_LUT, USAGE_1025) |
				  NVDEF(NVC37D, WINDOW_SET_WINDOW_USAGE_BOUNDS, INPUT_SCALER_TAPS, TAPS_2) |
				  NVDEF(NVC37D, WINDOW_SET_WINDOW_USAGE_BOUNDS, UPSCALING_ALLOWED, FALSE));
		}

		PUSH_MTHD(push, NVC37D, HEAD_SET_HEAD_USAGE_BOUNDS(i),
			  NVDEF(NVC37D, HEAD_SET_HEAD_USAGE_BOUNDS, CURSOR, USAGE_W256_H256) |
			  NVDEF(NVC37D, HEAD_SET_HEAD_USAGE_BOUNDS, OUTPUT_LUT, USAGE_1025) |
			  NVVAL(NVC37D, HEAD_SET_HEAD_USAGE_BOUNDS, UPSCALING_ALLOWED, vupscale));
	}
	return 0;
}

void
headc37d_static_wndw_map(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	int i, end, windows;

	if (head == NULL || asyh == NULL)
		return;

	windows = headc37d_window_count(head);
	for (i = head->base.index * 2, end = i + 2; i < end; i++)
		if (i < windows)
			asyh->wndw.owned |= BIT(i);
}

const struct nv50_head_func
headc37d = {
	.view = headc37d_view,
	.mode = headc37d_mode,
	.olut = headc37d_olut,
	.ilut_check = head907d_ilut_check,
	.olut_size = 1024,
	.olut_set = headc37d_olut_set,
	.olut_clr = headc37d_olut_clr,
	.curs_layout = head917d_curs_layout,
	.curs_format = headc37d_curs_format,
	.curs_set = headc37d_curs_set,
	.curs_clr = headc37d_curs_clr,
	.dither = headc37d_dither,
	.procamp = headc37d_procamp,
	.or = headc37d_or,
	.static_wndw_map = headc37d_static_wndw_map,
};
