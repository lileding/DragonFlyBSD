/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 */

#ifndef _NVGPU_PROC_H_
#define _NVGPU_PROC_H_

#include <sys/types.h>

struct nvgpu_channel_push;
struct nvgpu_device;
struct nvgpu_fence;
struct nvgpu_proc;
struct nvgpu_vm_bind_op;

/* Native EXEC request adapted by the syscall layer before entering proc. */
struct nvgpu_proc_exec {
	u_int channel_id;
	const struct nvgpu_channel_push *pushes;
	size_t push_count;
	struct nvgpu_fence **waits;
	size_t wait_count;
	struct nvgpu_fence *done;
};

/* Native VM remap request adapted by the syscall layer before entering proc. */
struct nvgpu_proc_remap {
	struct nvgpu_vm_bind_op *ops;
	size_t op_count;
	struct nvgpu_fence **waits;
	size_t wait_count;
	struct nvgpu_fence *done;
};

/*
 * Create one GPU process bound to device and return its initial owned reference.
 * The DRM file owns that reference and releases it from postclose.
 */
int nvgpu_proc_create(struct nvgpu_device *device,
	struct nvgpu_proc **result);

/* Add one asynchronous owner of proc and all proc-owned GPU resources. */
void nvgpu_proc_addref(struct nvgpu_proc *proc);

/*
 * Consume one proc reference.
 *
 * The final release destroys channels, mappings, and VM state, then releases
 * the device/unload hold.  This function is MPSAFE.
 */
void nvgpu_proc_release(struct nvgpu_proc *proc);

/*
 * Create and spawn one EXEC future from a borrowed native request.
 *
 * On zero the future owns all references needed after return.  The wait array,
 * pushes, and done fence remain caller-owned.  Completion is reported through
 * done; a successful return does not mean GPU execution has completed.
 */
int nvgpu_proc_spawn(struct nvgpu_proc *proc,
	struct nvgpu_proc_exec *exec);

/*
 * Create and spawn one VM remap future from a borrowed native request.
 *
 * On zero the future retains its own proc, BO, and done-fence references.
 * The request, operation array, wait array, and fence pointers remain owned by
 * the caller.  Completion is reported through remap->done.
 */
int nvgpu_proc_remap(struct nvgpu_proc *proc,
	struct nvgpu_proc_remap *remap);

#endif /* _NVGPU_PROC_H_ */
