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

/* Per-open GPU process state.  Owned by nvdrm_file until postclose hands teardown to the scheduler. */
struct nvgpu_proc;

/* Create per-open GPU state.  procp receives an owned reference; may sleep, no GSP RPC. */
int nvgpu_proc_create(struct nvgpu_device *gpu, struct nvdrm_file *file,
    struct nvgpu_proc **procp);

/* Request async process teardown.  proc is consumed by the scheduler drain path. */
void nvgpu_proc_stop(struct nvgpu_proc *proc);

#endif /* _NVGPU_PROC_H_ */
