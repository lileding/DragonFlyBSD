/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP event dispatch boundary for the native NVIDIA GPU driver.
 */

#include "nvgsp_event.h"
#include "nvgsp_priv.h"


static void
nvgsp_event_log_queue_snapshot(struct nvgsp_state *gsp, const char *tag)
{
	uint8_t *cmdq, *msgq, *slot;
	uint32_t cmdq_tx, cmdq_rx, msgq_tx, msgq_rx;
	uint32_t fn = 0, len = 0, sig = 0;

	if (gsp->gsp_shm.kva == NULL) {
		nvgpu_log(NVGPU_LOG_DEBUG, "gsp %s: shm is not allocated\n", tag);
		return;
	}
	cmdq = (uint8_t *)gsp->gsp_shm.kva + gsp->gsp_shm_cmdq_off;
	msgq = (uint8_t *)gsp->gsp_shm.kva + gsp->gsp_shm_msgq_off;
	cmdq_tx = *(volatile uint32_t *)(cmdq + 0x10);
	cmdq_rx = *(volatile uint32_t *)(cmdq + 0x20);
	msgq_tx = *(volatile uint32_t *)(msgq + 0x10);
	msgq_rx = *(volatile uint32_t *)(msgq + 0x20);

	if (gsp->gsp_msgq_rptr != msgq_tx) {
		slot = msgq + 0x1000 + gsp->gsp_msgq_rptr * 0x1000;
		sig = *(volatile uint32_t *)(slot + 48 + 4);
		len = *(volatile uint32_t *)(slot + 48 + 8);
		fn = *(volatile uint32_t *)(slot + 48 + 12);
	}
	nvgpu_log(NVGPU_LOG_DEBUG,
	    "gsp %s: cmdq(tx=%u rx=%u) msgq(tx=%u rx=%u host=%u) "
	    "next(sig=0x%08x fn=0x%x len=%u)\n", tag,
	    cmdq_tx, cmdq_rx, msgq_tx, msgq_rx, gsp->gsp_msgq_rptr,
	    sig, fn, len);
}

static int
nvgsp_event_on_init_done(void *priv, uint32_t fn, void *repv, uint32_t repc)
{
	struct nvgsp_state *gsp = priv;

	(void)fn;
	(void)repv;
	(void)repc;
	gsp->gsp_running = true;
	nvgpu_log(NVGPU_LOG_DEBUG, "gsp init-done event received\n");
	return (0);
}

static int
nvgsp_event_log_only(void *priv, uint32_t fn, void *repv, uint32_t repc)
{
	(void)priv;
	(void)repv;
	nvgpu_log(NVGPU_LOG_DEBUG, "gsp event fn=0x%x len=%u\n", fn, repc);
	if (fn == 0x1006 && repc > 0) {
		const uint8_t *bytes = repv;
		char text[161];
		uint32_t start = repc >= 12 ? 12 : 0;
		uint32_t n = repc > start ? repc - start : 0;
		uint32_t i;

		if (n >= sizeof(text))
			n = sizeof(text) - 1;
		for (i = 0; i < n; i++) {
			uint8_t c = bytes[start + i];
			text[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
		}
		text[n] = '\0';
		nvgpu_log(NVGPU_LOG_INFO, "gsp os-error-log: %s\n", text);
	}
	return (0);
}

/* Register GSP event handlers before the init-done wait starts. */
int
nvgsp_event_init(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	int error;

	if (gsp == NULL)
		return (ENXIO);
	nvgsp_msg_ntfy_init(gsp);
	error = nvgsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_GSP_INIT_DONE,
	    nvgsp_event_on_init_done, gsp);
	if (error != 0)
		return (error);
	(void)nvgsp_msg_ntfy_add(gsp, 0x1002, nvgsp_seq_msg_handler, gsp);
	(void)nvgsp_msg_ntfy_add(gsp, 0x1020, NULL, NULL);
	(void)nvgsp_msg_ntfy_add(gsp, 0x101c, NULL, NULL);
	(void)nvgsp_msg_ntfy_add(gsp, 0x1003, nvgsp_event_log_only, gsp);
	(void)nvgsp_msg_ntfy_add(gsp, 0x1004, nvgsp_event_log_only, gsp);
	(void)nvgsp_msg_ntfy_add(gsp, 0x1005, nvgsp_event_log_only, gsp);
	(void)nvgsp_msg_ntfy_add(gsp, 0x1006, nvgsp_event_log_only, gsp);
	(void)nvgsp_msg_ntfy_add(gsp, 0x100c, NULL, NULL);
	(void)nvgsp_msg_ntfy_add(gsp, 0x100f, NULL, NULL);
	return (0);
}

/* Wait for the GSP init-done event. */
int
nvgsp_event_poll_init_done(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);
	int spin;

	if (gsp == NULL)
		return (ENXIO);
	for (spin = 0; spin < 5000 && !gsp->gsp_running; spin++) {
		(void)nvgsp_msg_dispatch_all(gsp);
		if (gsp->gsp_running)
			break;
		DELAY(1000);
	}
	if (!gsp->gsp_running)
		nvgsp_event_log_queue_snapshot(gsp, "init-timeout");
	nvgpu_log(NVGPU_LOG_DEBUG, "gsp init-done wait %s after %d ms\n",
	    gsp->gsp_running ? "completed" : "timed out", spin);
	return (gsp->gsp_running ? 0 : ETIMEDOUT);
}

/* Dispatch pending GSP events. */
void
nvgsp_event_dispatch(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);

	if (gsp != NULL)
		(void)nvgsp_msg_dispatch_all(gsp);
}

/* Wake GSP message-queue waiters. */
void
nvgsp_event_wake_msgq(struct nvgpu_device *gpu)
{
	struct nvgsp_state *gsp = nvgsp_state_get(gpu);

	if (gsp != NULL)
		wakeup(&gsp->gsp_msgq_rptr);
}
