/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM registration boundary for the native NVIDIA driver.
 */

#ifndef _NVDRM_DRV_H_
#define _NVDRM_DRV_H_

struct drm_device;
struct nvgpu_device;

/*
 * nvdrm_register()
 *
 * Ownership:
 *   Borrows gpu from the PCI-owned nvgpu_device.  On success, the DRM core owns
 *   the public drm_device registration and stores a borrowed pointer back to
 *   gpu.  This function does not transfer or consume the nvgpu_device.
 *
 * Lifetime:
 *   Called once by the GPU boot thread after GSP, BAR, display, and interrupt
 *   setup have completed.  The registered drm_device remains valid until
 *   nvdrm_unregister() completes and all open drm_file and mmap references have
 *   drained.
 *
 * Threading:
 *   Called from the boot LWKT, not from the newbus attach thread.  It may sleep
 *   inside DRM registration paths and must not hold GSP or VM tokens across the
 *   registration call.
 */
int nvdrm_register(struct nvgpu_device *gpu);

/*
 * nvdrm_unregister()
 *
 * Ownership:
 *   Borrows gpu from the PCI detach path.  The function tears down the public
 *   DRM registration but does not free the nvgpu_device itself.
 *
 * Lifetime:
 *   Called only after unload admission has rejected new opens and confirmed no
 *   userspace file, scheduler, or mmap reference remains.  After it returns,
 *   nvdrm_file_open() can no longer be reached for this GPU.
 *
 * Threading:
 *   Runs from device teardown.  It must serialize with DRM open/close through
 *   DRM core locking and must not wait on per-process GPU work that should have
 *   been drained before unload admission succeeded.
 */
void nvdrm_unregister(struct nvgpu_device *gpu);

/*
 * nvdrm_device()
 *
 * Ownership:
 *   Returns a borrowed drm_device pointer owned by the DRM core.
 *
 * Lifetime:
 *   The pointer is valid only while the caller holds a reference or is running
 *   in a path where the enclosing nvgpu_device cannot be detached.
 *
 * Threading:
 *   Locking is provided by the caller's lifetime guarantee; this helper does
 *   not acquire DRM global locks.
 */
struct drm_device *nvdrm_device(struct nvgpu_device *gpu);

#endif /* _NVDRM_DRV_H_ */
