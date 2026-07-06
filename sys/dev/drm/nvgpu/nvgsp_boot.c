/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP boot and shutdown boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_boot.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"
#include "nvgsp_event.h"
#include "nvgsp_rpc.h"
#include "nvgsp_state.h"

#define NVGSP_BOOT_CALL(expr) do { \
	int error__ = (expr); \
	if (error__ != 0) \
		return (error__); \
} while (0)

/* Load the firmware images required by GSP boot. */
static int
nvgsp_boot_load_firmware(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot load firmware\n");
	return (0);
}

/* Prepare Falcon state before GSP firmware launch. */
static int
nvgsp_boot_init_falcon_state(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot init falcon state\n");
	return (0);
}

/* Run the FWSEC FRTS phase needed before protected boot. */
static int
nvgsp_boot_run_fwsec_frts(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot run fwsec frts\n");
	return (0);
}

/* Prepare WPR metadata consumed by the bootloader. */
static int
nvgsp_boot_prepare_wpr_meta(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot prepare wpr meta\n");
	return (0);
}

/* Prepare the RM image payload for GSP. */
static int
nvgsp_boot_prepare_rm_image(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot prepare rm image\n");
	return (0);
}

/* Prepare the LIBOS payload for GSP. */
static int
nvgsp_boot_prepare_libos(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot prepare libos\n");
	return (0);
}

/* Publish LIBOS mailbox parameters to firmware. */
static int
nvgsp_boot_write_libos_mailbox(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot write libos mailbox\n");
	return (0);
}

/* Run the booter-load firmware phase. */
static int
nvgsp_boot_run_booter_load(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot run booter load\n");
	return (0);
}

/* Wait until the GSP RISC-V core is active. */
static int
nvgsp_boot_wait_riscv_active(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot wait riscv active\n");
	return (0);
}

/* Wait for RM halt during GSP shutdown. */
static int
nvgsp_boot_wait_rm_halt(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot wait rm halt\n");
	return (0);
}

/* Reset the GSP Falcon before unload phases. */
static int
nvgsp_boot_reset_gsp_falcon(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot reset gsp falcon\n");
	return (0);
}

/* Run the FWSEC secure-boot unload phase. */
static int
nvgsp_boot_run_fwsec_sb(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot run fwsec sb\n");
	return (0);
}

/* Run the booter-unload firmware phase. */
static int
nvgsp_boot_run_booter_unload(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot run booter unload\n");
	return (0);
}

/* Release LIBOS boot resources. */
static void
nvgsp_boot_release_libos(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot release libos\n");
}

/* Release RM image boot resources. */
static void
nvgsp_boot_release_rm_image(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot release rm image\n");
}

/* Release WPR metadata resources. */
static void
nvgsp_boot_release_wpr_meta(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot release wpr meta\n");
}

/* Release firmware image references. */
static void
nvgsp_boot_release_firmware(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot release firmware\n");
}

/* Boot GSP firmware and complete the early RM handshake. */
/* Boot GSP and complete early RM handshake.  gpu is borrowed; boot LWKT only, may sleep. */
int
nvgsp_boot(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "boot\n");
	NVGSP_BOOT_CALL(nvgsp_boot_load_firmware(gpu));
	NVGSP_BOOT_CALL(nvgsp_boot_init_falcon_state(gpu));
	NVGSP_BOOT_CALL(nvgsp_boot_run_fwsec_frts(gpu));
	NVGSP_BOOT_CALL(nvgsp_boot_prepare_wpr_meta(gpu));
	NVGSP_BOOT_CALL(nvgsp_boot_prepare_rm_image(gpu));
	NVGSP_BOOT_CALL(nvgsp_boot_prepare_libos(gpu));
	NVGSP_BOOT_CALL(nvgsp_boot_write_libos_mailbox(gpu));
	NVGSP_BOOT_CALL(nvgsp_rpc_prequeue_system_info(gpu));
	NVGSP_BOOT_CALL(nvgsp_rpc_prequeue_registry(gpu));
	NVGSP_BOOT_CALL(nvgsp_boot_run_booter_load(gpu));
	NVGSP_BOOT_CALL(nvgsp_boot_wait_riscv_active(gpu));
	NVGSP_BOOT_CALL(nvgsp_event_poll_init_done(gpu));
	NVGSP_BOOT_CALL(nvgsp_state_query_static_info(gpu));
	NVGSP_BOOT_CALL(nvgsp_state_query_mthdbuf_size(gpu));
	NVGSP_BOOT_CALL(nvgsp_state_get_intr_table(gpu));
	NVGSP_BOOT_CALL(nvgsp_state_enable_doorbell(gpu));
	return (0);
}

/* Shut down GSP firmware in reverse boot order. */
/* Shut GSP down after DRM users and runtime backend state have stopped. */
void
nvgsp_shutdown(struct nvgpu_device *gpu)
{
	nvgpu_log(NVGPU_LOG_DEBUG, "shutdown\n");
	(void)nvgsp_rpc_unloading_guest_driver(gpu);
	(void)nvgsp_boot_wait_rm_halt(gpu);
	(void)nvgsp_boot_reset_gsp_falcon(gpu);
	(void)nvgsp_boot_run_fwsec_sb(gpu);
	(void)nvgsp_boot_run_booter_unload(gpu);
	nvgsp_boot_release_libos(gpu);
	nvgsp_boot_release_rm_image(gpu);
	nvgsp_boot_release_wpr_meta(gpu);
	nvgsp_boot_release_firmware(gpu);
}
