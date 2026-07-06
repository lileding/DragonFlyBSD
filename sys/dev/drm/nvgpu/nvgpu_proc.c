/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-open GPU process boundary.
 *
 * This file will own process-scoped GPU state: VMM, VM bindings, channels,
 * submit ordering, and the per-process scheduler.
 */

#include "nvgpu_proc.h"
