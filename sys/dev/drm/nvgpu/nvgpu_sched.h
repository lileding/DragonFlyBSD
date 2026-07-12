/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Module-global future scheduler for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_SCHED_H_
#define _NVGPU_SCHED_H_

struct nvgpu_future;

/*
 * Start the module-global scheduler and one worker bound to each CPU.
 *
 * Returns zero after all workers are running or EALREADY if already started.
 * A failed partial start joins every worker it created before returning.
 */
int nvgpu_sched_start(void);

/*
 * Synchronously drain the active queue and stop every scheduler worker.
 *
 * Callers must first prevent new submissions and drain all callback- and
 * interrupt-owned futures.  No future may remain registered on return.
 */
void nvgpu_sched_stop(void);

/*
 * Transfer one future to the scheduler's active queue without sleeping.
 *
 * This function is MPSAFE and may run in ithread or fence callback context.
 * On zero the scheduler owns future.  On error ownership remains with caller.
 */
int nvgpu_sched_put(struct nvgpu_future *future);

#endif /* _NVGPU_SCHED_H_ */
