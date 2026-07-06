/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 */

#ifndef _NVGPU_PROC_H_
#define _NVGPU_PROC_H_

struct nvdrm_file;
struct nvgpu_device;
struct nvgpu_proc;

/*
 * struct nvgpu_proc
 *
 * Ownership:
 *   Owned by one nvdrm_file.  It owns per-open GPU state, including the GPU
 *   virtual address space, channel list, VM mappings, submit ordering fences,
 *   and the scheduler LWKT that drains asynchronous work after postclose.
 *
 * Lifetime:
 *   Created during DRM open without constructing the hardware VMM.  The VMM is
 *   created lazily on first GPU use so drmGetDevices-style probe opens stay
 *   cheap.  Destruction is scheduler-owned after nvdrm_file_postclose() asks
 *   the process to stop.
 *
 * Threading:
 *   Mutable VM and submit state is serialized by process-local tokens and the
 *   scheduler.  Cross-process ordering must go through BO fences or explicit
 *   sync objects, not through global device locks.
 */
struct nvgpu_proc;

/*
 * nvgpu_proc_create()
 *
 * Ownership:
 *   Allocates one nvgpu_proc for file and returns it through procp.  The caller
 *   owns the returned reference and must eventually call nvgpu_proc_stop().
 *
 * Lifetime:
 *   Starts CPU-side process state and its scheduler before publication through
 *   nvdrm_file.  It does not allocate GSP VMM state until first GPU use.
 *
 * Threading:
 *   Called from DRM open.  It may sleep for CPU allocations and LWKT creation,
 *   but it must not issue GSP RPCs or touch userspace-visible GPU channels.
 */
int nvgpu_proc_create(struct nvgpu_device *gpu, struct nvdrm_file *file,
    struct nvgpu_proc **procp);

/*
 * nvgpu_proc_stop()
 *
 * Ownership:
 *   Consumes the public file reference to proc by requesting scheduler stop.
 *   The scheduler owns final destruction after it drains or fails all jobs.
 *
 * Lifetime:
 *   Called from nvdrm_file_postclose() after the drm_file has been disconnected
 *   from userspace.  No new ioctl may enter the process after this call.
 *
 * Threading:
 *   Does not wait for GPU completion.  It atomically requests scheduler stop
 *   and wakes the scheduler LWKT, which performs ordered cleanup.
 */
void nvgpu_proc_stop(struct nvgpu_proc *proc);

#endif /* _NVGPU_PROC_H_ */
