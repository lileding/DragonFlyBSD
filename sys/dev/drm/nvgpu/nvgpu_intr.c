/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Interrupt ingress boundary for the native NVIDIA GPU driver.
 */

#include "nvgpu_intr.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"
#include "nvgpu_display.h"
#include "nvgpu_exec.h"
#include "nvgpu_sched.h"
#include "nvgsp_event.h"


/* Decode one interrupt event into subsystem fanout. */
static void
nvgpu_intr_decode(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "intr decode\n");
}

/* Prepare interrupt routing state. */
/* Prepare interrupt state for gpu.  Called from boot before interrupts are enabled. */
int
nvgpu_intr_init(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "intr init\n");
	return (0);
}

/* Enable hardware interrupt delivery. */
/* Enable interrupt delivery for gpu after nvgpu_intr_init(). */
int
nvgpu_intr_enable(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "intr enable\n");
	return (0);
}

/* Disable hardware interrupt delivery. */
/* Disable interrupt delivery before interrupt state teardown. */
void
nvgpu_intr_disable(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "intr disable\n");
}

/* Release interrupt routing state. */
/* Release interrupt state after delivery has been disabled. */
void
nvgpu_intr_fini(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "intr fini\n");
}

/* Handle one hardware interrupt entry. */
/* Top-level interrupt entry.  Must stay short and hand slow work to subsystem workers. */
void
nvgpu_intr_handler(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "intr handler\n");
	nvgpu_intr_decode(gpu);
	nvgpu_exec_intr_complete(gpu);
	nvgpu_sched_post_event(gpu);
	nvgpu_display_vblank(gpu);
	nvgsp_event_wake_msgq(gpu);
	nvgpu_log(NVGPU_LOG_DEBUG, "intr handler\n");
}
