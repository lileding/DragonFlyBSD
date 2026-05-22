/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GEM-based BO layer for nvkm. GART-only host-RAM BOs backed by
 * contigmalloc'd physically-contiguous pages, exposed to userspace
 * via DRM's GEM handle + fake mmap-offset mechanism. The pages are
 * mapped through a cdev pager (i915 pattern) — see nvkm_bo.c.
 */

#ifndef _NVKM_BO_H_
#define _NVKM_BO_H_

#include <drm/drmP.h>
#include <drm/drm_gem.h>

struct nvkm_softc;

/* Mesa NVK / libdrm uapi mirror for ioctl arg structs. */
struct drm_nouveau_gem_info {
	uint32_t handle;
	uint32_t domain;
	uint64_t size;
	uint64_t offset;
	uint64_t map_handle;
	uint32_t tile_mode;
	uint32_t tile_flags;
};
struct drm_nouveau_gem_new {
	struct drm_nouveau_gem_info info;
	uint32_t channel_hint;
	uint32_t align;
};
struct drm_nouveau_gem_cpu_prep {
	uint32_t handle;
	uint32_t flags;
};
struct drm_nouveau_gem_cpu_fini {
	uint32_t handle;
};

#define NOUVEAU_GEM_DOMAIN_CPU		(1 << 0)
#define NOUVEAU_GEM_DOMAIN_VRAM		(1 << 1)
#define NOUVEAU_GEM_DOMAIN_GART		(1 << 2)

struct nvkm_bo {
	struct drm_gem_object	base;		/* drm GEM core */
	void			*kva;		/* contigmalloc'd kva */
	vm_paddr_t		paddr;		/* physical start address */
	uint32_t		domain;		/* NOUVEAU_GEM_DOMAIN_GART (only for now) */
	uint32_t		tile_mode;
	uint32_t		tile_flags;
};

static inline struct nvkm_bo *
to_nvkm_bo(struct drm_gem_object *obj)
{
	return (struct nvkm_bo *)obj;
}

/* gem_vm_ops vtable for the DRM driver registration. */
extern struct cdev_pager_ops nvkm_gem_pager_ops;

/* drm_driver.gem_free_object_unlocked callback. */
void nvkm_bo_gem_free(struct drm_gem_object *obj);

/* DRM_NOUVEAU_GEM_* ioctl handlers. */
int nvkm_drm_ioctl_gem_new(struct drm_device *ddev, void *data,
    struct drm_file *file_priv);
int nvkm_drm_ioctl_gem_info(struct drm_device *ddev, void *data,
    struct drm_file *file_priv);
int nvkm_drm_ioctl_gem_cpu_prep(struct drm_device *ddev, void *data,
    struct drm_file *file_priv);
int nvkm_drm_ioctl_gem_cpu_fini(struct drm_device *ddev, void *data,
    struct drm_file *file_priv);

#endif /* _NVKM_BO_H_ */
