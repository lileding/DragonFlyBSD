/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local minimal cursor shim for selected dispnv50 MIT method
 * emitters.
 */
#ifndef _DFLY_DISPNV50_CURS_H_
#define _DFLY_DISPNV50_CURS_H_

#include "wndw.h"

/*
 * Ownership:
 *   Creates a cursor nv50_wndw wrapper and transfers ownership to *pwndw on
 *   success. The caller owns the wrapper until display teardown.
 *
 * Lifetime:
 *   The passed nouveau_drm and function table are borrowed for construction.
 *   The function table must remain immutable for the module lifetime.
 *
 * Threading:
 *   Called from display/KMS setup paths that may sleep while allocating the
 *   GSP display channel. It is not safe for IRQ context.
 */
int curs507a_new_(const struct nv50_wimm_func *func, struct nouveau_drm *drm,
    int head, s32 oclass, u32 interlock_data, struct nv50_wndw **pwndw);
/*
 * Ownership/Lifetime/Threading:
 *   Same as curs507a_new_(); this Turing/Ampere cursor constructor only
 *   supplies the NVC37A immediate method emitter table.
 */
int cursc37a_new(struct nouveau_drm *drm, int head, s32 oclass,
    struct nv50_wndw **pwndw);

#endif
