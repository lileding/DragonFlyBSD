/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal head shim for selected dispnv50 MIT method emitters.
 */
#ifndef _DFLY_DISPNV50_HEAD_H_
#define _DFLY_DISPNV50_HEAD_H_

#include "core.h"
#include "atom.h"

#include <asm/io.h>
#include <drm/drm_color_mgmt.h>

struct drm_device;

struct nv50_head {
	const struct nv50_head_func *func;
	struct nv50_disp *disp;
	struct {
		struct {
			struct drm_device *dev;
		} base;
		int index;
	} base;
};

struct nv50_head_func {
	int (*view)(struct nv50_head *, struct nv50_head_atom *);
	int (*mode)(struct nv50_head *, struct nv50_head_atom *);
	bool (*olut)(struct nv50_head *, struct nv50_head_atom *, int size);
	bool (*ilut_check)(struct nv50_head *, struct nv50_wndw_atom *);
	bool olut_identity;
	int olut_size;
	int (*olut_set)(struct nv50_head *, struct nv50_head_atom *);
	int (*olut_clr)(struct nv50_head *);
	void (*core_calc)(struct nv50_head *, struct nv50_head_atom *);
	int (*core_set)(struct nv50_head *, struct nv50_head_atom *);
	int (*core_clr)(struct nv50_head *);
	int (*curs_layout)(struct nv50_head *, struct nv50_wndw_atom *,
	    struct nv50_head_atom *);
	int (*curs_format)(struct nv50_head *, struct nv50_wndw_atom *,
	    struct nv50_head_atom *);
	int (*curs_set)(struct nv50_head *, struct nv50_head_atom *);
	int (*curs_clr)(struct nv50_head *);
	int (*base)(struct nv50_head *, struct nv50_head_atom *);
	int (*ovly)(struct nv50_head *, struct nv50_head_atom *);
	int (*dither)(struct nv50_head *, struct nv50_head_atom *);
	int (*procamp)(struct nv50_head *, struct nv50_head_atom *);
	int (*or)(struct nv50_head *, struct nv50_head_atom *);
	void (*static_wndw_map)(struct nv50_head *, struct nv50_head_atom *);
	int (*display_id)(struct nv50_head *, u32 display_id);
};

static inline void
head907d_olut_load(struct drm_color_lut *in, int size, void __iomem *mem)
{
	(void)in;
	(void)size;
	(void)mem;
}

static inline bool
head907d_ilut_check(struct nv50_head *head, struct nv50_wndw_atom *asyw)
{
	(void)head;
	(void)asyw;
	return true;
}

static inline int
head917d_curs_layout(struct nv50_head *head, struct nv50_wndw_atom *asyw,
    struct nv50_head_atom *asyh)
{
	(void)head;
	(void)asyw;
	(void)asyh;
	return -ENOSYS;
}

int headc37d_view(struct nv50_head *, struct nv50_head_atom *);
int headc37d_curs_format(struct nv50_head *, struct nv50_wndw_atom *,
    struct nv50_head_atom *);
int headc37d_curs_set(struct nv50_head *, struct nv50_head_atom *);
int headc37d_curs_clr(struct nv50_head *);
int headc37d_dither(struct nv50_head *, struct nv50_head_atom *);
void headc37d_static_wndw_map(struct nv50_head *, struct nv50_head_atom *);
extern const struct nv50_head_func headc37d;
extern const struct nv50_head_func headc57d;

#endif
