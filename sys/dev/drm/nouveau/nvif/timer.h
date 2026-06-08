/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal nvif timer shim.  The linux-v7.0 nvif/timer.h has
 * no file-level license, so this keeps only the macro shape needed by the
 * imported MIT display files.
 */
#ifndef _DFLY_NVIF_TIMER_H_
#define _DFLY_NVIF_TIMER_H_

#include <nvif/os.h>
#include <linux/delay.h>

struct nvif_device;

#define nvif_msec(device, msec, body...) ({ \
	s64 _nvif_timer_ret = -ETIMEDOUT; \
	do { \
		body \
	} while (0); \
	_nvif_timer_ret; \
})

#endif
