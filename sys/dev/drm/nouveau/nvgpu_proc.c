/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 *
 * This file will own process-scoped GPU state: VMM, VM bindings, channels,
 * submit ordering, and the per-process scheduler.  The current state remains in
 * nvkm_drm_file inside nvkm_drm.c until the refactor migrates it in pieces.
 */

#include "nvgpu_proc.h"
