/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP event dispatch boundary for the native NVIDIA GPU driver.
 */

#ifndef _NVGSP_EVENT_H_
#define _NVGSP_EVENT_H_

struct nvgpu_device;

/* Initialize GSP event state before init-done polling. */
int nvgsp_event_init(struct nvgpu_device *gpu);
/* Wait for GSP init-done during boot; may sleep. */
int nvgsp_event_poll_init_done(struct nvgpu_device *gpu);
/* Dispatch pending GSP events; keep interrupt-facing work short. */
void nvgsp_event_dispatch(struct nvgpu_device *gpu);
/* Wake GSP message-queue waiters; callable from interrupt fanout. */
void nvgsp_event_wake_msgq(struct nvgpu_device *gpu);

#endif /* _NVGSP_EVENT_H_ */
