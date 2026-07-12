/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 */

#ifndef _NVGPU_PROC_H_
#define _NVGPU_PROC_H_

#include <sys/stdint.h>

struct nvgpu_channel_list;
struct nvgpu_channel;
struct nvgpu_device;
struct nvgpu_fence;
struct nvgpu_proc;
struct nvgpu_vm;
struct reservation_object;

/* Serialize proc-owned object tables across concurrent DRM ioctls. */
void nvgpu_proc_lock(struct nvgpu_proc *proc);
void nvgpu_proc_unlock(struct nvgpu_proc *proc);

/* Create per-open GPU state.  procp receives one owned file reference. */
int nvgpu_proc_create(struct nvgpu_device *gpu, struct nvgpu_proc **procp);

/* Drop the file reference; final teardown runs when all future users release proc. */
void nvgpu_proc_stop(struct nvgpu_proc *proc);

/* Hold or release one asynchronous user of proc-owned state. */
void nvgpu_proc_hold(struct nvgpu_proc *proc);
void nvgpu_proc_release(struct nvgpu_proc *proc);

/* Return the borrowed physical GPU for this proc. */
struct nvgpu_device *nvgpu_proc_get_device(struct nvgpu_proc *proc);

/* Return borrowed proc-owned storage for modules that own the contents. */
struct nvgpu_channel_list *nvgpu_proc_get_channels(struct nvgpu_proc *proc);
struct nvgpu_vm *nvgpu_proc_get_vm(struct nvgpu_proc *proc);
void nvgpu_proc_set_vm(struct nvgpu_proc *proc, struct nvgpu_vm *vm);

/* Return the borrowed per-process reservation object used by no-share BOs. */
struct reservation_object *nvgpu_proc_get_vm_resv(struct nvgpu_proc *proc);

/* Register one accepted EXEC and return an owned last-bind dependency, if any. */
int nvgpu_proc_register_exec(struct nvgpu_proc *proc,
    struct nvgpu_fence *gpu_complete_fence, uint32_t channel_id,
    struct nvgpu_fence **bind_wait_fence,
    struct nvgpu_channel **channelp);

/* Close one channel and snapshot owned dependencies for its release future. */
int nvgpu_proc_register_channel_release(struct nvgpu_proc *proc,
    uint32_t channel_id, struct nvgpu_channel **channelp,
    struct nvgpu_fence **wait_fences, uint32_t capacity,
    uint32_t *wait_count);

/* Atomically snapshot bind dependencies and install done_fence as last bind. */
int nvgpu_proc_register_bind(struct nvgpu_proc *proc,
    struct nvgpu_fence *done_fence, struct nvgpu_fence **wait_fences,
    uint32_t capacity, uint32_t *wait_count);

/* Clear done_fence when it is still the proc's last registered bind. */
void nvgpu_proc_complete_bind(struct nvgpu_proc *proc,
    struct nvgpu_fence *done_fence);

#endif /* _NVGPU_PROC_H_ */
