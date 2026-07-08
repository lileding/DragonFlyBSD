/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 */

#ifndef _NVGPU_PROC_H_
#define _NVGPU_PROC_H_

struct nvgpu_device;
struct nvgpu_proc;
struct nvgpu_sched;

/* Per-open GPU process state.  The proc LWKT owns final lifetime after create succeeds. */
struct nvgpu_proc;

/* Create per-open GPU state and start its LWKT.  procp receives a borrowed event target. */
int nvgpu_proc_create(struct nvgpu_device *gpu, struct nvgpu_proc **procp);

/* Request async process teardown.  Final release runs on the proc LWKT. */
void nvgpu_proc_stop(struct nvgpu_proc *proc);

/* Return the borrowed physical GPU for this proc. */
struct nvgpu_device *nvgpu_proc_get_gpu(struct nvgpu_proc *proc);

/* Return the borrowed scheduler state owned by this proc. */
struct nvgpu_sched *nvgpu_proc_get_sched(struct nvgpu_proc *proc);

#endif /* _NVGPU_PROC_H_ */
