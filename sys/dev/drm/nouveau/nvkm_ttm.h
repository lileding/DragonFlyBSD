/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef _NVKM_TTM_H_
#define _NVKM_TTM_H_

#include <drm/drmP.h>

struct nvkm_softc;
struct nvkm_bo;
struct ttm_bo_device;

int nvkm_ttm_init(struct nvkm_softc *sc, struct drm_device *ddev);
void nvkm_ttm_fini(struct nvkm_softc *sc);
struct ttm_bo_device *nvkm_ttm_bo_device(struct nvkm_softc *sc);
int nvkm_ttm_evict_vram(struct nvkm_softc *sc);
int nvkm_ttm_validate_bo_preferred(struct nvkm_bo *bo);
int nvkm_ttm_validate_last_bound_to_vram(struct nvkm_softc *sc);
int nvkm_ttm_clear_last_bound_move(struct nvkm_softc *sc);

#endif /* _NVKM_TTM_H_ */
