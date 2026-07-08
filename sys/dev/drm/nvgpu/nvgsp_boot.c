/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP boot and shutdown boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_boot.h"
#include "nvgsp_event.h"
#include "nvgsp_state.h"
#include "nvgsp_priv.h"
#include "nvgsp_falcon.h"

#define NVGSP_BOOT_CALL(expr) do { \
	int error__ = (expr); \
	if (error__ != 0) \
		return (error__); \
} while (0)

static int
nvgsp_boot_write_libos_mailbox(struct nvgsp_state *gsp)
{
	uint64_t paddr;
	int error;

	if (gsp->gsp == NULL || gsp->gsp_libos.kva == NULL)
		return (ENXIO);
	paddr = gsp->gsp_libos.paddr;
	error = nvgsp_falcon_reset_eng(gsp->gsp);
	if (error != 0)
		return (error);
	nvgsp_wr32(gsp, gsp->chip->gsp_base + 0x040,
	    (uint32_t)(paddr & 0xffffffffu));
	nvgsp_wr32(gsp, gsp->chip->gsp_base + 0x044, (uint32_t)(paddr >> 32));
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "gsp libos args paddr 0x%llx written to mailbox\n",
	    (unsigned long long)paddr);
	return (0);
}

static int
nvgsp_boot_run_booter_load(struct nvgsp_state *gsp)
{
	uint64_t meta_paddr;
	int error;

	if (gsp->fw_booter_load == NULL || gsp->wpr_meta.kva == NULL)
		return (ENXIO);
	error = nvgsp_booter_parse(gsp, gsp->fw_booter_load, &gsp->booter);
	if (error != 0)
		return (error);
	meta_paddr = gsp->wpr_meta.paddr;
	return (nvgsp_booter_run(gsp, &gsp->booter,
	    (uint32_t)(meta_paddr & 0xffffffffu), (uint32_t)(meta_paddr >> 32)));
}

static int
nvgsp_boot_wait_riscv_active(struct nvgsp_state *gsp)
{
	uint32_t status = 0;
	int polls;

	if (gsp->gsp == NULL)
		return (ENXIO);
	nvgsp_wr32(gsp, gsp->chip->gsp_base + 0x080, 0);
	for (polls = 0; polls < 100000; polls++) {
		status = nvgsp_rd32(gsp, gsp->chip->gsp_riscv + 0x240);
		if (status & 1)
			break;
		DELAY(10);
	}
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "gsp riscv active poll %d us status=0x%08x\n",
	    polls * 10, status);
	return ((status & 1) ? 0 : ETIMEDOUT);
}

static void
nvgsp_boot_release_resources(struct nvgsp_state *gsp)
{
	nvgsp_booter_release(gsp);
	nvgsp_libos_release(gsp);
	nvgsp_boot_release_image(gsp);
	nvgsp_meta_fini(gsp);
	nvgsp_falcon_fini_state(gsp);
	nvgsp_fw_fini(gsp);
	nvgsp_bios_fini(gsp);
}

/* Boot GSP firmware and complete the early RM handshake. */
int
nvgsp_boot(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	int error;

	if (gsp == NULL)
		return (ENXIO);
	nvgpu_log(NVGPU_LOG_DEBUG, "gsp boot start\n");

	error = nvgsp_bios_init(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_fw_init(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_falcon_init_state(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_fwsec_run_cmd(gsp, NVGSP_FWSEC_CMD_FRTS, 0, 0);
	if (error != 0)
		goto fail;
	error = nvgsp_meta_init(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_boot_prepare_image(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_libos_prepare(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_boot_write_libos_mailbox(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_rpc_set_system_info(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_rpc_set_registry(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_boot_run_booter_load(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_boot_wait_riscv_active(gsp);
	if (error != 0)
		goto fail;
	error = nvgsp_event_poll_init_done(gpu);
	if (error != 0)
		goto fail;
	NVGSP_BOOT_CALL(nvgsp_state_query_static_info(gpu));
	NVGSP_BOOT_CALL(nvgsp_state_query_mthdbuf_size(gpu));
	NVGSP_BOOT_CALL(nvgsp_state_get_intr_table(gpu));
	NVGSP_BOOT_CALL(nvgsp_state_enable_doorbell(gpu));
	nvgpu_log(NVGPU_LOG_DEBUG, "gsp boot completed\n");
	return (0);

fail:
	nvgpu_log(NVGPU_LOG_INFO, "gsp boot failed: %d\n", error);
	nvgsp_boot_release_resources(gsp);
	return (error);
}

/* Shut down GSP firmware in reverse boot order. */
void
nvgsp_shutdown(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);

	if (gsp == NULL)
		return;
	nvgpu_log(NVGPU_LOG_DEBUG, "gsp shutdown start\n");
	nvgsp_shutdown_backend(gsp);
	nvgsp_boot_release_resources(gsp);
}
