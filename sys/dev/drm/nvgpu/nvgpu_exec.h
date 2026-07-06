/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * EXEC completion boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_EXEC_H_
#define _NVGPU_EXEC_H_

struct nvgpu_device;

/* Handle one EXEC completion event.  gpu is borrowed; wake scheduler state, do not free jobs inline. */
void nvgpu_exec_intr_complete(struct nvgpu_device *gpu);

#endif /* _NVGPU_EXEC_H_ */
