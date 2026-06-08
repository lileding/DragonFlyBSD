/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal dispnv50 core shim for importing selected MIT
 * nouveau display method emitters.  Upstream dispnv50/*.h files do not all
 * carry explicit file-level licenses in linux-v7.0, so this header defines
 * only the subset required by the files compiled here.
 */
#ifndef _DFLY_DISPNV50_CORE_H_
#define _DFLY_DISPNV50_CORE_H_

#include <nvif/object.h>
#include <nvif/push.h>

struct drm_device;
struct nouveau_bo;
struct nv50_core;
struct nv50_disp;
struct nv50_head_atom;
struct nv50_head_func;
struct nv50_wndw;
struct nvif_device;

struct nouveau_drm {
	struct drm_device *dev;
};

struct nvif_disp {
	struct nvif_object object;
};

struct nv50_chan {
	struct nvif_object user;
	struct nvif_device *device;
};

struct nv50_dmac {
	struct nv50_chan base;
	struct nvif_push push;
	struct nvif_object sync;
	struct nvif_object vram;
};

struct nv50_core {
	const struct nv50_core_func *func;
	struct nv50_disp *disp;
	struct nv50_dmac chan;
	bool assign_windows;
};

enum nv50_disp_interlock_type {
	NV50_DISP_INTERLOCK_CORE = 0,
	NV50_DISP_INTERLOCK_CURS,
	NV50_DISP_INTERLOCK_BASE,
	NV50_DISP_INTERLOCK_OVLY,
	NV50_DISP_INTERLOCK_WNDW,
	NV50_DISP_INTERLOCK_WIMM,
	NV50_DISP_INTERLOCK__SIZE,
};

#define NV50_DISP_SYNC(c, o) ((c) * 0x040 + (o))
#define NV50_DISP_CORE_NTFY NV50_DISP_SYNC(0, 0x00)

struct nv50_disp {
	struct nvif_disp *disp;
	struct nv50_core *core;
	struct nvif_object caps;
	struct nouveau_bo *sync;
};

struct nouveau_encoder {
	struct {
		bool dp_interlace;
	} caps;
};

struct nv50_outp_func {
	int (*ctrl)(struct nv50_core *, int or, u32 ctrl,
	    struct nv50_head_atom *);
	void (*get_caps)(struct nv50_disp *, struct nouveau_encoder *, int or);
};

struct nv50_core_func {
	int (*init)(struct nv50_core *);
	void (*ntfy_init)(struct nouveau_bo *, u32 offset);
	int (*caps_init)(struct nouveau_drm *, struct nv50_disp *);
	u32 caps_class;
	int (*ntfy_wait_done)(struct nouveau_bo *, u32 offset,
	    struct nvif_device *);
	int (*update)(struct nv50_core *, u32 *interlock, bool ntfy);
	struct {
		int (*owner)(struct nv50_core *);
	} wndw;
	const struct nv50_head_func *head;
	const struct nv50_outp_func *dac;
	const struct nv50_outp_func *pior;
	const struct nv50_outp_func *sor;
};

#ifndef NV_ERROR
#define NV_ERROR(drm, fmt, ...) \
	kprintf("nouveau: " fmt, ##__VA_ARGS__)
#endif

struct nv50_disp *nv50_disp(struct drm_device *dev);

static inline int
nv50_dmac_create(struct nouveau_drm *drm, s32 *oclass, int head,
    void *args, u32 argc, int push, struct nv50_dmac *dmac)
{
	(void)drm;
	(void)oclass;
	(void)head;
	(void)args;
	(void)argc;
	(void)push;
	(void)dmac;
	return -ENOSYS;
}

static inline int
core507d_new_(const struct nv50_core_func *func, struct nouveau_drm *drm,
    s32 oclass, struct nv50_core **pcore)
{
	(void)func;
	(void)drm;
	(void)oclass;
	if (pcore != NULL)
		*pcore = NULL;
	return -ENOSYS;
}

void corec37d_ntfy_init(struct nouveau_bo *, u32);
int corec37d_caps_init(struct nouveau_drm *, struct nv50_disp *);
int corec37d_ntfy_wait_done(struct nouveau_bo *, u32, struct nvif_device *);
int corec37d_update(struct nv50_core *, u32 *, bool);
int corec37d_wndw_owner(struct nv50_core *);
extern const struct nv50_outp_func sorc37d;
extern const struct nv50_core_func corec57d;
int corec37d_new(struct nouveau_drm *, s32, struct nv50_core **);
int corec57d_new(struct nouveau_drm *, s32, struct nv50_core **);

#endif
