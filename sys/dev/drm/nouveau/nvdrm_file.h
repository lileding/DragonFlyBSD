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

/*
 * struct nvdrm_file
 *
 * Ownership:
 *   Owned by one drm_file and stored in drm_file::driver_priv.  It owns the
 *   corresponding nvgpu_proc, which owns per-open GPU state such as the VMM,
 *   channels, scheduler, and VM bindings.
 *
 * Lifetime:
 *   Created by nvdrm_file_open() and disconnected by nvdrm_file_postclose().
 *   Postclose drops the userspace-visible reference immediately, then asks the
 *   process scheduler to drain and destroy the nvgpu_proc asynchronously.
 *
 * Threading:
 *   DRM open/close paths serialize publication through the DRM core.  Runtime
 *   ioctl paths borrow it from drm_file::driver_priv and then delegate mutable
 *   GPU state to nvgpu_proc locks, tokens, or the per-process scheduler.
 */
struct nvdrm_file;

/*
 * nvdrm_file_open()
 *
 * Ownership:
 *   Allocates and publishes one nvdrm_file into file->driver_priv on success.
 *   The caller remains the owner of ddev and file.
 *
 * Lifetime:
 *   Called by the DRM core while opening a render or primary node.  Probe-only
 *   opens must remain cheap: GPU VMM construction is deferred until the first
 *   ioctl that actually needs GPU execution or GPU virtual memory.
 *
 * Threading:
 *   Refuses opens after unload admission has started.  It may sleep while
 *   allocating CPU-side process state but must not perform GSP RPC boot work.
 */
int nvdrm_file_open(struct drm_device *ddev, struct drm_file *file);

/*
 * nvdrm_file_postclose()
 *
 * Ownership:
 *   Consumes the drm_file::driver_priv association and drops the public file
 *   reference to the owned nvdrm_file.
 *
 * Lifetime:
 *   Called exactly once by the DRM core after userspace close.  It must make
 *   later ioctls impossible before it asks nvgpu_proc to stop.
 *
 * Threading:
 *   Does not synchronously wait for GPU completion.  It requests scheduler stop
 *   and lets the scheduler LWKT drain or fail parked work before freeing GPU
 *   process state.
 */
void nvdrm_file_postclose(struct drm_device *ddev, struct drm_file *file);

struct nvgpu_proc *nvdrm_file_proc(struct nvdrm_file *file);

#endif /* _NVDRM_FILE_H_ */
