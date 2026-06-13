/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GEM-based BO layer for nvkm. GART host-RAM and VRAM BOs use DragonFly's
 * TTM device, placement, mmap, and TT/VRAM managers.
 */

#ifndef _NVKM_BO_H_
#define _NVKM_BO_H_

#include <drm/drmP.h>
#include <drm/drm_gem.h>
#include <drm/ttm/ttm_bo_api.h>
#include <linux/dma-fence.h>
#include <linux/reservation.h>
#include <vm/vm_page.h>

struct nvkm_softc;
struct drm_mode_create_dumb;

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
#define NOUVEAU_GEM_DOMAIN_MAPPABLE	(1 << 3)
#define NOUVEAU_GEM_DOMAIN_COHERENT	(1 << 4)
#define NOUVEAU_GEM_DOMAIN_NO_SHARE	(1 << 5)

struct nvkm_bo {
	struct drm_gem_object	base;		/* drm GEM core */
	struct ttm_buffer_object tbo;		/* TTM BO for GEM backing */
	struct reservation_object resv;		/* BO busy lifetime fences */
	void			*kva;		/* legacy page-aligned sysmem KVA */
	vm_page_t		*pages;		/* legacy sysmem page vector */
	uint32_t		page_count;
	struct nvkm_vram_alloc	*vram_alloc;	/* owned GEM VRAM allocation */
	uint64_t		paddr;		/* first system paddr or VRAM physical start */
	uint64_t		bar1_gva;	/* BAR1 GVA for CPU mmap of VRAM BOs */
	uint64_t		bar1_size;
	uint32_t		domain;
	uint32_t		tile_mode;
	uint32_t		tile_flags;
	bool			bar1_mappable;	/* VRAM BO can fault in a BAR1 mmap */
	bool			no_share;	/* reject PRIME export */
	bool			ttm_backed;	/* GEM BO owned by TTM */
	bool			accounted;	/* active byte counters include this BO */
	uint8_t			account_kind;	/* counter bucket charged at alloc */
	bool			vm_bound_tiled;	/* ever VM_BINDed with kind!=0 */
	uint8_t			vm_bound_kind;	/* single non-zero VM_BIND kind */
	uint32_t		vm_bind_pin_count; /* active VM_BIND records pin TTM */
	bool			vm_bound_mixed_kind;
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
bool nvkm_bo_cpu_mappable(const struct nvkm_bo *bo);
bool nvkm_bo_has_sysmem(const struct nvkm_bo *bo);
int nvkm_bo_ensure_ttm_populated(struct nvkm_bo *bo);
int nvkm_bo_paddr_at(const struct nvkm_bo *bo, uint64_t offset,
    vm_paddr_t *paddr);
int nvkm_bo_read32(struct nvkm_bo *bo, uint64_t offset,
    uint32_t *value);
int nvkm_bo_vm_bind_pin(struct nvkm_bo *bo);
int nvkm_bo_vm_bind_unpin(struct nvkm_bo *bo);
struct reservation_object *nvkm_bo_resv(struct nvkm_bo *bo);
void nvkm_bo_resv_add_excl_fence(struct nvkm_bo *bo, struct dma_fence *fence);
int nvkm_bo_resv_wait(struct nvkm_bo *bo, bool intr);
int nvkm_bo_dumb_create(struct drm_file *file_priv, struct drm_device *ddev,
    struct drm_mode_create_dumb *args);
int nvkm_bo_dumb_map_offset(struct drm_file *file_priv,
    struct drm_device *ddev, uint32_t handle, uint64_t *offset);
int nvkm_bo_dumb_destroy(struct drm_file *file_priv, struct drm_device *ddev,
    uint32_t handle);

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
