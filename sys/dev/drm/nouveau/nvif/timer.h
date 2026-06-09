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
#include <linux/ktime.h>

struct nvif_device;

#define nvif_msec(device, msec, body...) ({ \
	const ktime_t _nvif_timer_start = ktime_get(); \
	const s64 _nvif_timer_limit = (s64)(msec) * NSEC_PER_MSEC; \
	s64 _nvif_timer_taken = 0; \
	(void)(device); \
	do { \
		body \
		_nvif_timer_taken = ktime_to_ns( \
		    ktime_sub(ktime_get(), _nvif_timer_start)); \
	} while (_nvif_timer_taken <= _nvif_timer_limit); \
	_nvif_timer_taken <= _nvif_timer_limit ? \
	    _nvif_timer_taken : -ETIMEDOUT; \
})

#endif
