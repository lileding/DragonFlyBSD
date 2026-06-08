/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal cursor shim for selected dispnv50 MIT method
 * emitters.
 */
#ifndef _DFLY_DISPNV50_CURS_H_
#define _DFLY_DISPNV50_CURS_H_

#include "wndw.h"

static inline int
curs507a_new_(const struct nv50_wimm_func *func, struct nouveau_drm *drm,
    int head, s32 oclass, u32 interlock_data, struct nv50_wndw **pwndw)
{
	if (pwndw != NULL)
		*pwndw = NULL;
	return -ENOSYS;
}

#endif
