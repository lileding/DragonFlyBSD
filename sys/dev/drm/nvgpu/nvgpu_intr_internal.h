/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Private semaphore identity shared by channel and interrupt implementations.
 */

#ifndef _NVGPU_INTR_INTERNAL_H_
#define _NVGPU_INTR_INTERNAL_H_

#include <sys/queue.h>
#include <sys/stdint.h>
#include <stdbool.h>

struct nvgpu_device;
struct nvgpu_future;

struct nvgpu_sema {
	TAILQ_ENTRY(nvgpu_sema) parked_link;
	struct nvgpu_device *device;
	struct nvgpu_future *future;
	volatile uint32_t *address;
	uint32_t target;
	uint32_t chid;
	int error;
	bool parked;
};

/* Admit or stop display-event fanout while KMS callbacks are live. */
void nvgpu_intr_enable_display_dispatch(struct nvgpu_device *device);
void nvgpu_intr_disable_display_dispatch(struct nvgpu_device *device);

/* Queue one GSP-reported channel fault for process-context handling. */
void nvgpu_intr_report_channel_fault(struct nvgpu_device *device,
	uint32_t channel_id);

#endif /* _NVGPU_INTR_INTERNAL_H_ */
