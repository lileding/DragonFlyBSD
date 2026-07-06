/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM registration boundary for the native NVIDIA driver.
 *
 * This file will own the drm_driver table and the drm_dev_register() /
 * drm_dev_unregister() lifetime.  It intentionally contains no migrated
 * behavior yet; the old nvkm_drm.c path remains the only active path until
 * each entry point is moved here in a small, buildable step.
 */

#include "nvdrm_drv.h"
