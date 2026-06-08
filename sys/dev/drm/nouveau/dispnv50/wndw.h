/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal window shim for selected dispnv50 MIT method
 * emitters.
 */
#ifndef _DFLY_DISPNV50_WNDW_H_
#define _DFLY_DISPNV50_WNDW_H_

#include "core.h"
#include "atom.h"
#include "head.h"

#include <drm/drm_atomic_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_plane.h>
#include <drm/drm_plane_helper.h>

#ifndef DRM_PLANE_NO_SCALING
#define DRM_PLANE_NO_SCALING DRM_PLANE_HELPER_NO_SCALING
#endif

#ifndef DRM_FORMAT_XBGR16161616F
#define DRM_FORMAT_XBGR16161616F fourcc_code('X', 'B', '4', 'H')
#endif
#ifndef DRM_FORMAT_ABGR16161616F
#define DRM_FORMAT_ABGR16161616F fourcc_code('A', 'B', '4', 'H')
#endif

#ifndef DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D
#define DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h) \
	fourcc_mod_code(NVIDIA, (0x10 | \
		((h) & 0xf) | \
		(((k) & 0xff) << 12) | \
		(((g) & 0x3) << 20) | \
		(((s) & 0x1) << 22) | \
		(((s) & 0x6) << 25) | \
		(((c) & 0x7) << 23)))
#endif

#define NV50_DISP_WNDW_SEM0(c) NV50_DISP_SYNC(1 + (c), 0x00)
#define NV50_DISP_WNDW_NTFY(c) NV50_DISP_SYNC(1 + (c), 0x20)

struct nv50_disp_interlock {
	u32 data;
	u32 wimm;
};

struct nv50_wndw_func;
struct nv50_wimm_func {
	int (*point)(struct nv50_wndw *, struct nv50_wndw_atom *);
	int (*update)(struct nv50_wndw *, u32 *interlock);
};

struct nv50_wndw {
	const struct nv50_wndw_func *func;
	const struct nv50_wimm_func *immd;
	int id;
	struct nv50_disp_interlock interlock;
	struct nv50_dmac wndw;
	struct nv50_dmac wimm;
	u16 ntfy;
	u16 sema;
	u32 data;
};

struct nv50_wndw_func {
	int (*acquire)(struct nv50_wndw *, struct nv50_wndw_atom *,
	    struct nv50_head_atom *);
	void (*release)(struct nv50_wndw *, struct nv50_wndw_atom *,
	    struct nv50_head_atom *);
	void (*prepare)(struct nv50_wndw *, struct nv50_head_atom *,
	    struct nv50_wndw_atom *);
	int (*sema_set)(struct nv50_wndw *, struct nv50_wndw_atom *);
	int (*sema_clr)(struct nv50_wndw *);
	void (*ntfy_reset)(struct nouveau_bo *, u32 offset);
	int (*ntfy_set)(struct nv50_wndw *, struct nv50_wndw_atom *);
	int (*ntfy_clr)(struct nv50_wndw *);
	int (*ntfy_wait_begun)(struct nouveau_bo *, u32 offset,
	    struct nvif_device *);
	void (*ilut)(struct nv50_wndw *, struct nv50_wndw_atom *, int size);
	void (*csc)(struct nv50_wndw *, struct nv50_wndw_atom *,
	    const struct drm_color_ctm *);
	int (*csc_set)(struct nv50_wndw *, struct nv50_wndw_atom *);
	int (*csc_clr)(struct nv50_wndw *);
	bool ilut_identity;
	int ilut_size;
	int (*xlut_set)(struct nv50_wndw *, struct nv50_wndw_atom *);
	int (*xlut_clr)(struct nv50_wndw *);
	int (*image_set)(struct nv50_wndw *, struct nv50_wndw_atom *);
	int (*image_clr)(struct nv50_wndw *);
	int (*blend_set)(struct nv50_wndw *, struct nv50_wndw_atom *);
	int (*update)(struct nv50_wndw *, u32 *interlock);
};

static inline __must_check int
nvif_chan_wait(struct nv50_dmac *dmac, u32 size)
{
	return 0;
}

static inline int
nv50_wndw_new_(const struct nv50_wndw_func *func, struct drm_device *dev,
    enum drm_plane_type type, const char *name, int index,
    const u32 *format, u32 heads, enum nv50_disp_interlock_type interlock_type,
    u32 interlock_data, struct nv50_wndw **pwndw)
{
	(void)func;
	(void)dev;
	(void)type;
	(void)name;
	(void)index;
	(void)format;
	(void)heads;
	(void)interlock_type;
	(void)interlock_data;
	if (pwndw != NULL)
		*pwndw = NULL;
	return -ENOSYS;
}

static inline int
base507c_ntfy_wait_begun(struct nouveau_bo *bo, u32 offset,
    struct nvif_device *device)
{
	(void)bo;
	(void)offset;
	(void)device;
	return -ENOSYS;
}

static inline void
base907c_csc(struct nv50_wndw *wndw, struct nv50_wndw_atom *asyw,
    const struct drm_color_ctm *ctm)
{
	(void)wndw;
	(void)asyw;
	(void)ctm;
}

int wndwc37e_acquire(struct nv50_wndw *, struct nv50_wndw_atom *,
    struct nv50_head_atom *);
void wndwc37e_release(struct nv50_wndw *, struct nv50_wndw_atom *,
    struct nv50_head_atom *);
int wndwc37e_sema_set(struct nv50_wndw *, struct nv50_wndw_atom *);
int wndwc37e_sema_clr(struct nv50_wndw *);
int wndwc37e_ntfy_set(struct nv50_wndw *, struct nv50_wndw_atom *);
int wndwc37e_ntfy_clr(struct nv50_wndw *);
int wndwc37e_image_clr(struct nv50_wndw *);
int wndwc37e_blend_set(struct nv50_wndw *, struct nv50_wndw_atom *);
int wndwc37e_update(struct nv50_wndw *, u32 *);
int wndwc37e_new_(const struct nv50_wndw_func *, struct nouveau_drm *,
    enum drm_plane_type type, int index, s32 oclass, u32 heads,
    struct nv50_wndw **);
int wndwc37e_new(struct nouveau_drm *, enum drm_plane_type, int, s32,
    struct nv50_wndw **);
int wndwc57e_new(struct nouveau_drm *, enum drm_plane_type, int, s32,
    struct nv50_wndw **);
extern const struct nv50_wndw_func wndwc57e;

#endif
