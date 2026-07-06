/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * EXEC completion boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_exec.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


/* Dispatch one EXEC completion interrupt event. */
/* Handle one EXEC completion event.  gpu is borrowed; wake scheduler state, do not free jobs inline. */
void
nvgpu_exec_intr_complete(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "exec intr complete\n");
}
