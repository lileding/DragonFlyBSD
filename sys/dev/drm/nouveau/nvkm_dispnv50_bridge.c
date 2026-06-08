/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly-local boundary for wiring DRM/KMS commits into imported nouveau
 * dispnv50 emitters.
 *
 * The replaced display path carried its own GSP display object, RAMIN,
 * core/window pushbuffers, and notifier ctxdma state.  That state is gone.
 * The active display owner is now the imported nouveau display engine at
 * sc->disp; the dispnv50 channel ABI still needs to be staged before this
 * bridge can program scanout.
 */
#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include "core.h"

#include <drm/drm_crtc.h>

struct nv50_disp *
nv50_disp(struct drm_device *dev)
{
	(void)dev;

	/*
	 * The imported dispnv50 method emitters expect Linux nouveau_display()
	 * to expose a staged nv50_disp object.  DragonFly has not wired that
	 * object yet; keep the symbol local to this bridge and fail before any
	 * caller dereferences it.
	 */
	return (NULL);
}

int
nvkm_dispnv50_atomic_enable(struct nvkm_softc *sc, struct drm_crtc *crtc,
    uint32_t head, uint32_t win, uint32_t display_id)
{
	if (sc == NULL || crtc == NULL || crtc->state == NULL || sc->disp == NULL)
		return (-ENODEV);

	nvkm_infof(sc->dev,
	    "drm: dispnv50 bridge deferred: display channel ABI not staged "
	    "head=%u win=%u display=0x%x\n",
	    head, win, display_id);
	return (-ENODEV);
}
