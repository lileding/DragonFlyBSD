/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP backend state boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_state.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"


int
nvgsp_state_init(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "state init\n");
	return (0);
}

void
nvgsp_state_fini(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "state fini\n");
}

/* Query static GPU information from GSP. */
/* Query static GPU info from GSP during boot. */
int
nvgsp_state_query_static_info(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "state query static info\n");
	return (0);
}

/* Query the method-buffer size from GSP. */
/* Query method-buffer size before channel creation. */
int
nvgsp_state_query_mthdbuf_size(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "state query mthdbuf size\n");
	return (0);
}

/* Retrieve the interrupt table from GSP. */
/* Retrieve interrupt routing metadata before nvgpu_intr_init(). */
int
nvgsp_state_get_intr_table(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "state get intr table\n");
	return (0);
}

/* Enable doorbell support through GSP. */
/* Enable doorbells before channel submission. */
int
nvgsp_state_enable_doorbell(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "state enable doorbell\n");
	return (0);
}
