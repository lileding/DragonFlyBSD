/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM file boundary for one userspace open of the nouveau-compatible ABI.
 *
 * This file will translate DRM open, postclose, and ioctl entry points into
 * nvgpu_proc and nvgpu_sched operations once DRM registration is reattached.
 */

#include "nvdrm_file.h"
