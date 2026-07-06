/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local nvif logging shim for imported MIT nouveau helpers.
 */
#ifndef _DFLY_NVIF_PRINTF_H_
#define _DFLY_NVIF_PRINTF_H_

#include <nvif/object.h>

#ifndef NVIF_DEBUG
#define NVIF_DEBUG(object, fmt, ...) do { } while (0)
#endif

#ifndef NVIF_ERROR
#define NVIF_ERROR(object, fmt, ...) \
	kprintf("nvif: " fmt, ##__VA_ARGS__)
#endif

#ifndef NVIF_ERRON
#define NVIF_ERRON(cond, object, fmt, ...) ({ \
	bool _nvif_erron = (cond); \
	if (_nvif_erron) \
		NVIF_ERROR((object), fmt, ##__VA_ARGS__); \
	_nvif_erron; \
})
#endif

#endif
