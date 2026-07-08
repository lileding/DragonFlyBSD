/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Display policy boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_display.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


/* Dispatch one display vblank event. */
/* Handle one vblank event.  gpu is borrowed; called from display/interrupt fanout. */
void
nvgpu_display_handle_vblank(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "display vblank\n");
}
