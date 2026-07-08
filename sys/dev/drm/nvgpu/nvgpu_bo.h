/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GEM-backed BO metadata for the native NVIDIA driver.
 */

#ifndef _NVGPU_BO_H_
#define _NVGPU_BO_H_

#include <sys/stdint.h>

#include <drm/drm_gem.h>

struct drm_file;
struct nvgpu_proc;

struct nvgpu_bo_create_args {
	uint64_t size;
	uint32_t domain;
	uint32_t align;
	uint32_t tile_mode;
	uint32_t tile_flags;
};

struct nvgpu_bo_info {
	uint32_t handle;
	uint32_t domain;
	uint64_t size;
	uint64_t offset;
	uint64_t map_handle;
	uint32_t tile_mode;
	uint32_t tile_flags;
};

/* Create a GEM handle for one BO.  The handle owns the object reference on success. */
int nvgpu_bo_create_handle(struct nvgpu_proc *proc, struct drm_file *file,
    const struct nvgpu_bo_create_args *args, struct nvgpu_bo_info *info);

/* Fill BO info for a GEM handle visible from file. */
int nvgpu_bo_get_info(struct drm_file *file, uint32_t handle,
    struct nvgpu_bo_info *info);

/* DRM GEM free callback for nvgpu_bo objects. */
void nvgpu_bo_free(struct drm_gem_object *obj);

#endif /* _NVGPU_BO_H_ */
