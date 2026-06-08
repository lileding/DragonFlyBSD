/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal nouveau_bo shim for selected dispnv50 MIT display
 * notifier helpers.
 */
#ifndef _DFLY_NOUVEAU_BO_H_
#define _DFLY_NOUVEAU_BO_H_

#include <nvif/os.h>

struct nouveau_bo {
	u64 offset;
};

#define NVBO_WR32(bo, offset, args...) do { \
	(void)(bo); \
	(void)(offset); \
} while (0)

#define NVBO_TD32(bo, offset, args...) false

#endif
