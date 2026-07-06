/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM registration boundary for the native NVIDIA driver.
 *
 * This file owns the future drm_driver table and the DRM device
 * registration lifetime.  It currently stays minimal while device boot is
 * being rebuilt on the new owner model.
 */

#include "nvdrm_drv.h"
