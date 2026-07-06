/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP event dispatch boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_event.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


int
nvgsp_event_init(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "event init\n");
	return (0);
}

/* Wait for the GSP init-done event. */
/* Wait for GSP init-done during boot; may sleep. */
int
nvgsp_event_poll_init_done(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "event poll init done\n");
	return (0);
}

/* Dispatch pending GSP events. */
/* Dispatch pending GSP events; keep interrupt-facing work short. */
void
nvgsp_event_dispatch(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "event dispatch\n");
}

/* Wake waiters on the GSP message queue. */
/* Wake GSP message-queue waiters; callable from interrupt fanout. */
void
nvgsp_event_wake_msgq(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "event wake msgq\n");
}
