/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM file boundary for one userspace open of the nouveau-compatible ABI.
 */

#ifndef _NVDRM_FILE_H_
#define _NVDRM_FILE_H_

struct drm_device;
struct drm_file;
struct nvdrm_file;
struct nvgpu_proc;

/* DRM-private state for one open file.  Owned by drm_file::driver_priv; runtime state lives in nvgpu_proc. */
struct nvdrm_file;

/* DRM open callback.  Publishes one nvdrm_file; may sleep for CPU state only. */
int nvdrm_file_open(struct drm_device *ddev, struct drm_file *file);

/* DRM postclose callback.  Disconnects the file and asks nvgpu_proc to stop. */
void nvdrm_file_postclose(struct drm_device *ddev, struct drm_file *file);

/* Return the borrowed nvgpu_proc for a live nvdrm_file; no internal locking. */
struct nvgpu_proc *nvdrm_file_proc(struct nvdrm_file *file);

#endif /* _NVDRM_FILE_H_ */
