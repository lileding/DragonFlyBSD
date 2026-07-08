/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GEM-backed BO metadata for the native NVIDIA driver.
 */

#include "nvdrm_nouveau_abi.h"
#include "nvgpu_bo.h"
#include "nvgpu_device.h"
#include "nvgpu_proc.h"

#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <vm/vm_param.h>

#include <drm/drmP.h>
#include <drm/drm_gem.h>

static MALLOC_DEFINE(M_NVGPU_BO, "nvgpu_bo", "nvgpu buffer object");

struct nvgpu_bo {
	struct drm_gem_object base;
	uint32_t domain;
	uint32_t tile_mode;
	uint32_t tile_flags;
	bool no_share;
};

static struct nvgpu_bo *
nvgpu_bo_from_gem(struct drm_gem_object *obj)
{
	return ((struct nvgpu_bo *)obj);
}

/* DRM GEM free callback for nvgpu_bo objects. */
void
nvgpu_bo_free(struct drm_gem_object *obj)
{
	struct nvgpu_bo *bo;

	if (obj == NULL)
		return;
	bo = nvgpu_bo_from_gem(obj);
	drm_gem_object_release(&bo->base);
	_kfree(bo, M_NVGPU_BO);
}

/* Create a GEM handle for one BO.  The handle owns the object reference on success. */
int
nvgpu_bo_create_handle(struct nvgpu_proc *proc, struct drm_file *file,
    const struct nvgpu_bo_create_args *args, struct nvgpu_bo_info *info)
{
	struct drm_device *ddev;
	struct nvgpu_bo *bo;
	uint32_t handle = 0;
	uint64_t size;
	uint32_t domain;
	int error;

	if (proc == NULL || file == NULL || args == NULL || info == NULL)
		return (EINVAL);
	ddev = nvgpu_device_get_drm_dev(nvgpu_proc_get_gpu(proc));
	if (ddev == NULL)
		return (ENXIO);

	size = roundup(args->size, PAGE_SIZE);
	if (size == 0)
		return (EINVAL);
	domain = args->domain;
	if ((domain & (NOUVEAU_GEM_DOMAIN_VRAM | NOUVEAU_GEM_DOMAIN_GART)) == 0)
		domain |= NOUVEAU_GEM_DOMAIN_GART;

	bo = kmalloc(sizeof(*bo), M_NVGPU_BO, M_WAITOK | M_ZERO);
	drm_gem_private_object_init(ddev, &bo->base, size);
	bo->domain = domain;
	bo->tile_mode = args->tile_mode;
	bo->tile_flags = args->tile_flags;
	bo->no_share = (domain & NOUVEAU_GEM_DOMAIN_NO_SHARE) != 0;

	error = drm_gem_handle_create(file, &bo->base, &handle);
	if (error != 0) {
		drm_gem_object_put_unlocked(&bo->base);
		return (error);
	}

	info->handle = handle;
	info->domain = bo->domain;
	info->size = size;
	info->offset = 0;
	info->map_handle = 0;
	info->tile_mode = bo->tile_mode;
	info->tile_flags = bo->tile_flags;
	drm_gem_object_put_unlocked(&bo->base);
	return (0);
}

/* Fill BO info for a GEM handle visible from file. */
int
nvgpu_bo_get_info(struct drm_file *file, uint32_t handle,
    struct nvgpu_bo_info *info)
{
	struct drm_gem_object *obj;
	struct nvgpu_bo *bo;

	if (file == NULL || info == NULL)
		return (EINVAL);
	obj = drm_gem_object_lookup(file, handle);
	if (obj == NULL)
		return (ENOENT);
	bo = nvgpu_bo_from_gem(obj);
	info->handle = handle;
	info->domain = bo->domain;
	info->size = obj->size;
	info->offset = 0;
	info->map_handle = 0;
	info->tile_mode = bo->tile_mode;
	info->tile_flags = bo->tile_flags;
	drm_gem_object_put_unlocked(obj);
	return (0);
}
