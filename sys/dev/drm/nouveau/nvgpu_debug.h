/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Driver-local logging for the native NVIDIA GPU driver.
 */

#ifndef _NVGPU_DEBUG_H_
#define _NVGPU_DEBUG_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

extern int nvgpu_debug;

void nvgpu_infof(device_t dev, const char *fmt, ...) __printflike(2, 3);
void nvgpu_debugf(device_t dev, const char *fmt, ...) __printflike(2, 3);

#endif /* _NVGPU_DEBUG_H_ */
