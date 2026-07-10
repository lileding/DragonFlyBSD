/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Driver-local logging for the RDNA+ AMD GPU driver.
 */

#ifndef _AMDGPU_DEBUG_H_
#define _AMDGPU_DEBUG_H_

#include <sys/types.h>

enum amdgpu_log_level {
	AMDGPU_LOG_ERROR = 0,
	AMDGPU_LOG_INFO,
	AMDGPU_LOG_DEBUG,
};

extern int amdgpu_log_level;

int amdgpu_debug_init(void);
void amdgpu_debug_fini(void);

void amdgpu_log(enum amdgpu_log_level level, const char *fmt, ...);

#endif /* _AMDGPU_DEBUG_H_ */
