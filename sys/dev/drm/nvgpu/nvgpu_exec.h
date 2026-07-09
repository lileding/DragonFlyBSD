/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * EXEC completion boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_EXEC_H_
#define _NVGPU_EXEC_H_

struct drm_file;
struct nvgpu_device;
struct nvgpu_proc;

/* Submit a synchronization-only fake EXEC future used to validate scheduler/fence plumbing. */
int nvgpu_exec_submit_fake(struct nvgpu_proc *proc, struct drm_file *file,
    void *data);

/* Handle one EXEC completion event.  gpu is borrowed; wake scheduler state, do not free futures inline. */
void nvgpu_exec_complete_from_intr(struct nvgpu_device *gpu);

#endif /* _NVGPU_EXEC_H_ */
