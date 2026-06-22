/*
 * nvkm_drm_kms.c -- DragonFly DRM/KMS setup for nvkm.
 *
 * Register the driver as a KMS (DRIVER_MODESET) device, init mode_config, and
 * create a connector per supported GSP displayId so userspace can enumerate
 * the attached display and its modes (drmModeGetConnector / modetest). detect()
 * and get_modes() proxy to the GSP display subsystem (connect-state + EDID).
 *
 * dfly's DRM provides the entire KMS/atomic framework (~Linux 4.x).  This
 * file owns DragonFly DRM object setup and validation, then hands committed
 * CRTC/window state to nvkm_dispnv50_bridge.c for imported dispnv50 emitters.
 */
#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"
#include "nvkm_bo.h"

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>	/* drm_atomic_set_{crtc,mode,fb}_for_* */
#include <drm/drm_color_mgmt.h>
#include <drm/drm_crtc_helper.h>	/* drm_helper_probe_single_connector_modes */
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper.h>	/* drm_helper_mode_fill_fb_struct */
#include <drm/drm_modeset_lock.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_property.h>
#include <drm/drm_rect.h>

#include <engine/disp.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#ifdef nvkm_rd32
#undef nvkm_rd32
#endif
#ifdef nvkm_wr32
#undef nvkm_wr32
#endif

struct nvkm_bios;

#include <subdev/bios/conn.h>
#include <subdev/bios/dcb.h>

extern const uint64_t wndwc57e_modifiers[];

#define NVKM_KMS_MIN_CLOCK_KHZ	25000U

static uint32_t
nvkm_kms_cap_min(uint32_t value, uint32_t cap)
{
	if (cap != 0 && value > cap)
		return (cap);
	return (value);
}

static uint32_t
nvkm_kms_effective_head_count(struct nvkm_softc *sc)
{
	uint32_t heads;

	heads = nvkm_gsp_disp_head_count(sc);
	heads = nvkm_kms_cap_min(heads, NVKM_DISPLAY_MAX_HEADS);
	if (sc != NULL && sc->chip != NULL) {
		heads = nvkm_kms_cap_min(heads, sc->chip->display_heads);
		heads = nvkm_kms_cap_min(heads, sc->chip->display_windows);
		heads = nvkm_kms_cap_min(heads, sc->chip->display_cursors);
	}
	return (heads);
}

struct nvkm_drm_connector {
	struct drm_connector	base;
	struct nvkm_softc	*sc;
	uint32_t		display_id;	/* GSP displayId, e.g. 0x400 */
	struct nvkm_event_ntfy	dp_irq_ntfy;
	bool			dp_irq_ntfy_initialized;
};

#define to_nvkm_connector(c) container_of(c, struct nvkm_drm_connector, base)

struct nvkm_connector_state {
	struct drm_connector_state base;
	uint32_t dither_mode;
	uint32_t dither_depth;
	uint32_t max_bpc;
};

#define to_nvkm_connector_state(s) \
	container_of(s, struct nvkm_connector_state, base)
#define to_nvkm_connector_state_const(s) \
	((const struct nvkm_connector_state *)(s))

static void nvkm_drm_kms_dp_irq_schedule(struct nvkm_softc *sc,
    uint32_t display_id);

static const struct drm_prop_enum_list nvkm_dither_mode_enum[] = {
	{ NVKM_DISPNV50_DITHER_MODE_OFF, "off" },
	{ NVKM_DISPNV50_DITHER_MODE_ON, "on" },
	{ NVKM_DISPNV50_DITHER_MODE_DYNAMIC2X2, "dynamic 2x2" },
	{ NVKM_DISPNV50_DITHER_MODE_STATIC2X2, "static 2x2" },
	{ NVKM_DISPNV50_DITHER_MODE_TEMPORAL, "temporal" },
	{ NVKM_DISPNV50_DITHER_MODE_AUTO, "auto" },
};

static const struct drm_prop_enum_list nvkm_dither_depth_enum[] = {
	{ NVKM_DISPNV50_DITHER_DEPTH_6BPC, "6 bpc" },
	{ NVKM_DISPNV50_DITHER_DEPTH_8BPC, "8 bpc" },
	{ NVKM_DISPNV50_DITHER_DEPTH_AUTO, "auto" },
};

static int
nvkm_connector_type_from_output(uint8_t output_type)
{
	switch (output_type) {
	case DCB_OUTPUT_ANALOG:
		return (DRM_MODE_CONNECTOR_VGA);
	case DCB_OUTPUT_TV:
		return (DRM_MODE_CONNECTOR_TV);
	case DCB_OUTPUT_TMDS:
		return (DRM_MODE_CONNECTOR_HDMIA);
	case DCB_OUTPUT_LVDS:
		return (DRM_MODE_CONNECTOR_LVDS);
	case DCB_OUTPUT_DP:
		return (DRM_MODE_CONNECTOR_DisplayPort);
	case DCB_OUTPUT_WFD:
		return (DRM_MODE_CONNECTOR_VIRTUAL);
	default:
		return (DRM_MODE_CONNECTOR_Unknown);
	}
}

static int
nvkm_connector_type_from_info(const struct nvkm_gsp_disp_output_info *info)
{
	switch (info->connector_type) {
	case DCB_CONNECTOR_VGA:
	case DCB_CONNECTOR_POD_VGA:
	case DCB_CONNECTOR_DOCK_VGA_0:
	case DCB_CONNECTOR_DOCK_VGA_1:
		return (DRM_MODE_CONNECTOR_VGA);
	case DCB_CONNECTOR_DVI_A:
		return (DRM_MODE_CONNECTOR_DVIA);
	case DCB_CONNECTOR_DVI_I_TV_0:
	case DCB_CONNECTOR_DVI_I_TV_1:
	case DCB_CONNECTOR_DVI_I_TV_2:
	case DCB_CONNECTOR_DVI_I:
	case DCB_CONNECTOR_DMS59_0:
	case DCB_CONNECTOR_DMS59_1:
	case DCB_CONNECTOR_DOCK_DVI_I_0:
	case DCB_CONNECTOR_DOCK_DVI_I_1:
		return (DRM_MODE_CONNECTOR_DVII);
	case DCB_CONNECTOR_DVI_D:
	case DCB_CONNECTOR_DVI_ADC:
	case DCB_CONNECTOR_TMDS:
	case DCB_CONNECTOR_DOCK_DVI_D_0:
	case DCB_CONNECTOR_DOCK_DVI_D_1:
		return (DRM_MODE_CONNECTOR_DVID);
	case DCB_CONNECTOR_TV_0:
	case DCB_CONNECTOR_TV_2:
	case DCB_CONNECTOR_TV_SCART:
	case DCB_CONNECTOR_TV_SCART_D:
	case DCB_CONNECTOR_POD_TV_0:
		return (DRM_MODE_CONNECTOR_Composite);
	case DCB_CONNECTOR_TV_1:
	case DCB_CONNECTOR_POD_TV_1:
		return (DRM_MODE_CONNECTOR_SVIDEO);
	case DCB_CONNECTOR_TV_3:
	case DCB_CONNECTOR_TV_DTERM:
	case DCB_CONNECTOR_POD_TV_3:
		return (DRM_MODE_CONNECTOR_Component);
	case DCB_CONNECTOR_LVDS:
	case DCB_CONNECTOR_LVDS_SPWG:
	case DCB_CONNECTOR_LVDS_REM:
	case DCB_CONNECTOR_LVDS_SPWG_REM:
		return (DRM_MODE_CONNECTOR_LVDS);
	case DCB_CONNECTOR_DP:
	case DCB_CONNECTOR_mDP:
	case DCB_CONNECTOR_DOCK_DP_0:
	case DCB_CONNECTOR_DOCK_DP_1:
	case DCB_CONNECTOR_DOCK_mDP_0:
	case DCB_CONNECTOR_DOCK_mDP_1:
	case DCB_CONNECTOR_DMS59_DP0:
	case DCB_CONNECTOR_DMS59_DP1:
	case DCB_CONNECTOR_USB_C:
		return (DRM_MODE_CONNECTOR_DisplayPort);
	case DCB_CONNECTOR_eDP:
		return (DRM_MODE_CONNECTOR_eDP);
	case DCB_CONNECTOR_HDMI_0:
	case DCB_CONNECTOR_HDMI_1:
		return (DRM_MODE_CONNECTOR_HDMIA);
	case DCB_CONNECTOR_HDMI_C:
		return (DRM_MODE_CONNECTOR_HDMIB);
	case DCB_CONNECTOR_WFD:
		return (DRM_MODE_CONNECTOR_VIRTUAL);
	default:
		return (nvkm_connector_type_from_output(info->output_type));
	}
}

static int
nvkm_encoder_type_from_info(const struct nvkm_gsp_disp_output_info *info)
{
	switch (info->output_type) {
	case DCB_OUTPUT_ANALOG:
		return (DRM_MODE_ENCODER_DAC);
	case DCB_OUTPUT_TV:
		return (DRM_MODE_ENCODER_TVDAC);
	case DCB_OUTPUT_LVDS:
		return (DRM_MODE_ENCODER_LVDS);
	case DCB_OUTPUT_WFD:
		return (DRM_MODE_ENCODER_VIRTUAL);
	case DCB_OUTPUT_TMDS:
	case DCB_OUTPUT_DP:
		return (DRM_MODE_ENCODER_TMDS);
	default:
		return (DRM_MODE_ENCODER_NONE);
	}
}

static bool
nvkm_connector_type_supports_stereo(int connector_type)
{
	return (connector_type == DRM_MODE_CONNECTOR_DisplayPort ||
	    connector_type == DRM_MODE_CONNECTOR_eDP ||
	    connector_type == DRM_MODE_CONNECTOR_HDMIA);
}

static void
nvkm_connector_init_mode_caps(struct drm_connector *connector,
    const struct nvkm_gsp_disp_output_info *info, int connector_type)
{
	/*
	 * Turing follows nouveau's NV50+ userspace contract: doublescan modes
	 * are acceptable at the connector level.  Interlace is more output
	 * specific: non-DP Turing no longer advertises it, while DP follows
	 * the SOR dp_interlace capability read from the display caps block.
	 */
	connector->doublescan_allowed = true;
	connector->interlace_allowed = info->is_dp &&
	    info->dp_interlace_capable;
	connector->stereo_allowed =
	    nvkm_connector_type_supports_stereo(connector_type);
}

static uint32_t
nvkm_possible_crtcs_from_info(const struct nvkm_gsp_disp_output_info *info,
    uint32_t crtc_mask)
{
	if (info->heads == 0)
		return (crtc_mask);

	return (info->heads & crtc_mask);
}

static uint32_t
nvkm_kms_mode_clock_khz(const struct drm_display_mode *mode)
{
	uint32_t clock;

	if (mode == NULL)
		return (0);
	if (mode->crtc_clock > 0)
		return ((uint32_t)mode->crtc_clock);
	if (mode->clock <= 0)
		return (0);

	clock = (uint32_t)mode->clock;
	if ((mode->flags & DRM_MODE_FLAG_3D_MASK) ==
	    DRM_MODE_FLAG_3D_FRAME_PACKING) {
		if (clock > UINT32_MAX / 2U)
			return (0);
		clock *= 2U;
	}
	return (clock);
}

static bool
nvkm_kms_connector_is_hdmi(const struct nvkm_gsp_disp_output_info *info)
{
	if (info == NULL)
		return (false);

	switch (info->connector_type) {
	case DCB_CONNECTOR_HDMI_0:
	case DCB_CONNECTOR_HDMI_1:
	case DCB_CONNECTOR_HDMI_C:
		return (true);
	default:
		return (false);
	}
}

static uint32_t
nvkm_kms_tmds_max_clock_khz(const struct nvkm_gsp_disp_output_info *info,
    int sink_max_tmds_clock, bool hdmi_scdc_scrambling)
{
	uint32_t source_max;

	if (nvkm_kms_connector_is_hdmi(info))
		source_max = hdmi_scdc_scrambling ? 594000U : 340000U;
	else
		source_max = 165000U;

	if (sink_max_tmds_clock > 0 &&
	    (uint32_t)sink_max_tmds_clock < source_max)
		return ((uint32_t)sink_max_tmds_clock);
	return (source_max);
}

static enum drm_mode_status
nvkm_kms_output_mode_valid(struct nvkm_softc *sc, uint32_t display_id,
    const struct nvkm_gsp_disp_output_info *info,
    const struct drm_display_mode *mode, int max_tmds_clock,
    bool hdmi_scdc_scrambling, uint8_t bpc)
{
	uint32_t clock;
	uint32_t max_clock;

	if (sc == NULL || info == NULL || mode == NULL)
		return (MODE_ERROR);
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) &&
	    (!info->is_dp || !info->dp_interlace_capable))
		return (MODE_NO_INTERLACE);

	clock = nvkm_kms_mode_clock_khz(mode);
	if (clock == 0 || clock < NVKM_KMS_MIN_CLOCK_KHZ)
		return (MODE_CLOCK_LOW);

	switch (info->output_type) {
	case DCB_OUTPUT_TMDS:
		max_clock = nvkm_kms_tmds_max_clock_khz(info, max_tmds_clock,
		    hdmi_scdc_scrambling);
		if (clock > max_clock)
			return (MODE_CLOCK_HIGH);
		return (MODE_OK);
	case DCB_OUTPUT_DP:
		return ((enum drm_mode_status)nvkm_dispnv50_output_mode_valid(sc,
		    display_id, mode, bpc, info->dp_interlace_capable));
	default:
		return (MODE_BAD);
	}
}

/* ===== connector helper funcs ===== */

#define NVKM_KMS_EDID_BUFSIZE	2048U

/*
 * Ownership:
 *   The caller owns the EDID buffer; this helper only borrows it.
 * Lifetime:
 *   The EDID storage must remain valid for len bytes for this call.
 * Threading:
 *   Pure validation. No connector mutation, no locks, and no GSP RPCs.
 */
static bool
nvkm_connector_edid_is_valid(struct edid *edid, uint32_t len)
{
	uint32_t blocks;
	uint32_t block;

	if (edid == NULL || len < EDID_LENGTH)
		return (false);

	blocks = (uint32_t)edid->extensions + 1U;
	if (blocks * EDID_LENGTH > len)
		return (false);

	for (block = 0; block < blocks; block++) {
		if (!drm_edid_block_valid((uint8_t *)edid +
		    block * EDID_LENGTH, block, false, NULL))
			return (false);
	}

	return (true);
}

/*
 * Ownership:
 *   Borrows connector and edid. The DRM connector owns the resulting property
 *   blob; the caller keeps ownership of the input EDID storage.
 * Lifetime:
 *   edid, when non-NULL, must remain valid for this call only. No pointer is
 *   retained by nvkm.
 * Threading:
 *   Call from KMS probe/hotplug context where connector state mutation is
 *   allowed. This helper does not take GSP locks or issue RPCs.
 */
static int
nvkm_connector_update_edid(struct drm_connector *connector,
    const struct edid *edid, const char *reason)
{
	struct nvkm_drm_connector *nc = to_nvkm_connector(connector);
	int ret;

	ret = drm_connector_update_edid_property(connector, edid);
	if (ret != 0) {
		nvkm_infof(nc->sc->dev,
		    "drm: connector %s display=0x%x EDID %s failed err=%d\n",
		    connector->name, nc->display_id, reason, ret);
	}

	return (ret);
}

static int
nvkm_connector_get_modes(struct drm_connector *connector)
{
	struct nvkm_drm_connector *nc = to_nvkm_connector(connector);
	struct edid *edid;
	uint8_t *buf;
	uint32_t len = NVKM_KMS_EDID_BUFSIZE;
	int n = 0;

	buf = kzalloc(NVKM_KMS_EDID_BUFSIZE, GFP_KERNEL);
	if (buf == NULL)
		return (0);
	edid = (struct edid *)buf;
	if (nvkm_gsp_disp_read_edid(nc->sc, nc->display_id, buf, &len) == 0 &&
	    nvkm_connector_edid_is_valid(edid, len)) {
		if (nvkm_connector_update_edid(connector, edid,
		    "publish") == 0)
			n = drm_add_edid_modes(connector, edid);
	} else
		nvkm_connector_update_edid(connector, NULL, "clear");
	kfree(buf);
	return (n);
}

static enum drm_mode_status
nvkm_connector_mode_valid(struct drm_connector *connector,
    struct drm_display_mode *mode)
{
	struct nvkm_drm_connector *nc = to_nvkm_connector(connector);
	struct nvkm_gsp_disp_output_info info;
	int ret;

	ret = nvkm_gsp_disp_output_info(nc->sc, nc->display_id, &info);
	if (ret != 0)
		return (MODE_ERROR);
	return (nvkm_kms_output_mode_valid(nc->sc, nc->display_id, &info,
	    mode, connector->display_info.max_tmds_clock,
	    connector->display_info.hdmi.scdc.scrambling.supported, 8));
}

static bool
nvkm_connector_state_changed(const struct nvkm_connector_state *old_state,
    const struct nvkm_connector_state *new_state)
{
	return (old_state == NULL ||
	    old_state->dither_mode != new_state->dither_mode ||
	    old_state->dither_depth != new_state->dither_depth ||
	    old_state->max_bpc != new_state->max_bpc);
}

static int
nvkm_connector_atomic_check(struct drm_connector *connector,
    struct drm_connector_state *state)
{
	struct nvkm_connector_state *new_state = to_nvkm_connector_state(state);
	struct nvkm_connector_state *old_state = connector->state != NULL ?
	    to_nvkm_connector_state(connector->state) : NULL;
	struct drm_crtc_state *crtc_state;

	if (new_state->max_bpc != 8)
		return (-EINVAL);
	if (state->crtc == NULL ||
	    !nvkm_connector_state_changed(old_state, new_state))
		return (0);

	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return (PTR_ERR(crtc_state));
	crtc_state->connectors_changed = true;
	return (0);
}

static const struct drm_connector_helper_funcs nvkm_connector_helper_funcs = {
	.get_modes	= nvkm_connector_get_modes,
	.mode_valid	= nvkm_connector_mode_valid,
	.atomic_check	= nvkm_connector_atomic_check,
};

/* ===== connector funcs ===== */

static enum drm_connector_status
nvkm_connector_detect(struct drm_connector *connector, bool force)
{
	struct nvkm_drm_connector *nc = to_nvkm_connector(connector);
	int connected;

	(void)force;
	connected = nvkm_gsp_disp_connected(nc->sc, nc->display_id);
	if (connected > 0)
		return (connector_status_connected);

	nvkm_connector_update_edid(connector, NULL, "clear-on-detect");
	return (connector_status_disconnected);
}

static int
nvkm_connector_dp_irq(struct nvkm_event_ntfy *ntfy, u32 bits)
{
	struct nvkm_drm_connector *nc;

	if ((bits & NVKM_DPYID_IRQ) == 0)
		return (NVKM_EVENT_KEEP);

	nc = container_of(ntfy, struct nvkm_drm_connector, dp_irq_ntfy);
	nvkm_drm_kms_dp_irq_schedule(nc->sc, nc->display_id);
	return (NVKM_EVENT_KEEP);
}

static void
nvkm_connector_dp_irq_unregister(struct nvkm_drm_connector *connector)
{
	if (connector == NULL || !connector->dp_irq_ntfy_initialized)
		return;

	nvkm_event_ntfy_del(&connector->dp_irq_ntfy);
	connector->dp_irq_ntfy_initialized = false;
}

/*
 * Subscribe a KMS connector to RM DP IRQ notifications.
 *
 * Ownership:
 *   The connector owns the notification record and unregisters it before the
 *   connector is destroyed. The nvkm display event list only borrows the
 *   embedded nvkm_event_ntfy while it is registered.
 *
 * Lifetime:
 *   The callback never dereferences DRM state after scheduling the HPD task.
 *   KMS teardown unregisters all connector notifications before draining the
 *   HPD task, so no new task can be queued by this event after teardown begins.
 *
 * Threading:
 *   The callback may run from GSP event context. It only records a displayId
 *   bit under sc->kms_hpd_lock and queues process-context work; AUX reads and
 *   link-status publication happen later in the HPD task.
 */
static void
nvkm_connector_dp_irq_register(struct nvkm_drm_connector *connector,
    const struct nvkm_gsp_disp_output_info *info)
{
	struct nvkm_softc *sc;
	int id;

	if (connector == NULL || info == NULL ||
	    info->output_type != DCB_OUTPUT_DP)
		return;
	sc = connector->sc;
	if (sc == NULL || sc->disp == NULL || connector->display_id == 0)
		return;

	id = ffs(connector->display_id) - 1;
	if (id < 0)
		return;

	nvkm_event_ntfy_add(&sc->disp->rm.event, id, NVKM_DPYID_IRQ, false,
	    nvkm_connector_dp_irq, &connector->dp_irq_ntfy);
	nvkm_event_ntfy_allow(&connector->dp_irq_ntfy);
	connector->dp_irq_ntfy_initialized = true;
}

static void
nvkm_connector_destroy(struct drm_connector *connector)
{
	struct nvkm_drm_connector *nc = to_nvkm_connector(connector);

	nvkm_connector_dp_irq_unregister(nc);
	drm_connector_cleanup(connector);
	kfree(nc);
}

static void
nvkm_connector_reset(struct drm_connector *connector)
{
	struct nvkm_connector_state *state;

	if (connector->state != NULL) {
		__drm_atomic_helper_connector_destroy_state(connector->state);
		kfree(to_nvkm_connector_state(connector->state));
		connector->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state != NULL) {
		state->base.link_status = DRM_LINK_STATUS_GOOD;
		state->dither_mode = NVKM_DISPNV50_DITHER_MODE_AUTO;
		state->dither_depth = NVKM_DISPNV50_DITHER_DEPTH_AUTO;
		state->max_bpc = 8;
	}
	__drm_atomic_helper_connector_reset(connector,
	    state != NULL ? &state->base : NULL);
}

static struct drm_connector_state *
nvkm_connector_atomic_duplicate_state(struct drm_connector *connector)
{
	struct nvkm_connector_state *old_state;
	struct nvkm_connector_state *state;

	if (WARN_ON(connector->state == NULL))
		return (NULL);

	old_state = to_nvkm_connector_state(connector->state);
	state = kmalloc(sizeof(*state), M_DRM, GFP_KERNEL);
	if (state == NULL)
		return (NULL);

	*state = *old_state;
	__drm_atomic_helper_connector_duplicate_state(connector, &state->base);
	return (&state->base);
}

static void
nvkm_connector_atomic_destroy_state(struct drm_connector *connector,
    struct drm_connector_state *state)
{
	(void)connector;
	__drm_atomic_helper_connector_destroy_state(state);
	kfree(to_nvkm_connector_state(state));
}

static int
nvkm_connector_atomic_set_property(struct drm_connector *connector,
    struct drm_connector_state *state, struct drm_property *property,
    uint64_t value)
{
	struct nvkm_drm_connector *nc = to_nvkm_connector(connector);
	struct nvkm_connector_state *nvkm_state =
	    to_nvkm_connector_state(state);

	if (property == nc->sc->kms_dither_mode_property) {
		nvkm_state->dither_mode = (uint32_t)value;
		return (0);
	}
	if (property == nc->sc->kms_dither_depth_property) {
		nvkm_state->dither_depth = (uint32_t)value;
		return (0);
	}
	if (property == nc->sc->kms_max_bpc_property) {
		nvkm_state->max_bpc = (uint32_t)value;
		return (0);
	}
	return (-EINVAL);
}

static int
nvkm_connector_atomic_get_property(struct drm_connector *connector,
    const struct drm_connector_state *state, struct drm_property *property,
    uint64_t *value)
{
	struct nvkm_drm_connector *nc = to_nvkm_connector(connector);
	const struct nvkm_connector_state *nvkm_state =
	    to_nvkm_connector_state_const(state);

	if (property == nc->sc->kms_dither_mode_property) {
		*value = nvkm_state->dither_mode;
		return (0);
	}
	if (property == nc->sc->kms_dither_depth_property) {
		*value = nvkm_state->dither_depth;
		return (0);
	}
	if (property == nc->sc->kms_max_bpc_property) {
		*value = nvkm_state->max_bpc;
		return (0);
	}
	return (-EINVAL);
}

static const struct drm_connector_funcs nvkm_connector_funcs = {
	.reset			= nvkm_connector_reset,
	.detect			= nvkm_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= nvkm_connector_destroy,
	.atomic_duplicate_state	= nvkm_connector_atomic_duplicate_state,
	.atomic_destroy_state	= nvkm_connector_atomic_destroy_state,
	.atomic_set_property	= nvkm_connector_atomic_set_property,
	.atomic_get_property	= nvkm_connector_atomic_get_property,
};

static int
nvkm_connector_properties_init(struct drm_device *dev, struct nvkm_softc *sc)
{
	if (sc->kms_dither_mode_property == NULL) {
		sc->kms_dither_mode_property = drm_property_create_enum(dev, 0,
		    "dithering mode", nvkm_dither_mode_enum,
		    nitems(nvkm_dither_mode_enum));
		if (sc->kms_dither_mode_property == NULL)
			return (-ENOMEM);
	}
	if (sc->kms_dither_depth_property == NULL) {
		sc->kms_dither_depth_property = drm_property_create_enum(dev, 0,
		    "dithering depth", nvkm_dither_depth_enum,
		    nitems(nvkm_dither_depth_enum));
		if (sc->kms_dither_depth_property == NULL)
			return (-ENOMEM);
	}
	if (sc->kms_max_bpc_property == NULL) {
		sc->kms_max_bpc_property = drm_property_create_range(dev, 0,
		    "max bpc", 8, 8);
		if (sc->kms_max_bpc_property == NULL)
			return (-ENOMEM);
	}
	return (0);
}

static void
nvkm_connector_attach_properties(struct nvkm_drm_connector *connector)
{
	struct nvkm_softc *sc = connector->sc;

	drm_object_attach_property(&connector->base.base,
	    sc->kms_dither_mode_property, NVKM_DISPNV50_DITHER_MODE_AUTO);
	drm_object_attach_property(&connector->base.base,
	    sc->kms_dither_depth_property, NVKM_DISPNV50_DITHER_DEPTH_AUTO);
	drm_object_attach_property(&connector->base.base,
	    sc->kms_max_bpc_property, 8);
}

/* ===== mode_config funcs ===== */

static void
nvkm_user_fb_destroy(struct drm_framebuffer *fb)
{
	struct nvkm_softc *sc = fb->dev != NULL ? fb->dev->dev_private : NULL;

	if (sc != NULL)
		sc->kms_fb_destroy_count++;
	if (fb->obj[0] != NULL)
		drm_gem_object_put_unlocked(fb->obj[0]);
	drm_framebuffer_cleanup(fb);
	kfree(fb);
}

static int
nvkm_user_fb_create_handle(struct drm_framebuffer *fb, struct drm_file *file,
    unsigned int *handle)
{
	if (fb->obj[0] == NULL)
		return (-ENODEV);
	return (drm_gem_handle_create(file, fb->obj[0], handle));
}

/*
 * Acknowledge MODE_DIRTYFB for direct-scanout GEM framebuffers.
 *
 * Ownership:
 *   Borrows the framebuffer and optional clip array for this ioctl call.  The
 *   DRM framebuffer core owns the fb reference and validates/copies clips
 *   before calling us; nvkm does not retain either pointer.
 *
 * Lifetime:
 *   The backing BO is the same object that scanout reads, so there is no
 *   deferred shadow surface to flush and no state survives this call.
 *
 * Threading:
 *   Pure no-op after DRM core validation.  It takes no nvkm locks, issues no
 *   GSP/RM RPCs, and does not touch hardware.
 */
static int
nvkm_user_fb_dirty(struct drm_framebuffer *fb, struct drm_file *file,
    unsigned flags, unsigned color, struct drm_clip_rect *clips,
    unsigned num_clips)
{
	(void)fb;
	(void)file;
	(void)flags;
	(void)color;
	(void)clips;
	(void)num_clips;
	return (0);
}

static const struct drm_framebuffer_funcs nvkm_user_fb_funcs = {
	.destroy	= nvkm_user_fb_destroy,
	.create_handle	= nvkm_user_fb_create_handle,
	.dirty		= nvkm_user_fb_dirty,
};

static bool nvkm_plane_format_mod_supported(struct drm_plane *plane,
    uint32_t format, uint64_t modifier);

static bool
nvkm_modifier_is_linear(uint64_t modifier)
{
	return (modifier == DRM_FORMAT_MOD_LINEAR ||
	    modifier == DRM_FORMAT_MOD_INVALID);
}

static bool
nvkm_modifier_is_supported(uint64_t modifier)
{
	unsigned int i;

	if (modifier == DRM_FORMAT_MOD_INVALID)
		return (true);
	for (i = 0; wndwc57e_modifiers[i] != DRM_FORMAT_MOD_INVALID; i++) {
		if (wndwc57e_modifiers[i] == modifier)
			return (true);
	}
	return (false);
}

static bool
nvkm_modifier_is_blocklinear(uint64_t modifier)
{
	return (!nvkm_modifier_is_linear(modifier) &&
	    nvkm_modifier_is_supported(modifier));
}

static uint8_t
nvkm_modifier_kind(uint64_t modifier)
{
	return ((modifier >> 12) & 0xff);
}

static uint8_t
nvkm_modifier_sector_layout(uint64_t modifier)
{
	return ((modifier >> 22) & 0x1) | ((modifier >> 25) & 0x6);
}

static bool
nvkm_format_modifier_cpp_supported(uint32_t format, uint64_t modifier)
{
	const struct drm_format_info *info;
	uint8_t sector_layout;
	unsigned int i;

	if (nvkm_modifier_is_linear(modifier))
		return (true);
	if (!nvkm_modifier_is_supported(modifier))
		return (false);

	info = drm_format_info(format);
	if (info == NULL)
		return (false);

	/*
	 * Turing block-linear modifiers encode the sector layout separately
	 * from the page kind.  The layout must match the bytes-per-pixel class
	 * or the window hardware will interpret the scanout surface incorrectly.
	 */
	sector_layout = nvkm_modifier_sector_layout(modifier);
	for (i = 0; i < info->num_planes; i++) {
		if (info->cpp[i] == 3)
			return (false);
		if (info->cpp[i] == 2 && sector_layout != 3)
			return (false);
		if (info->cpp[i] == 1 && sector_layout != 2)
			return (false);
		if (info->cpp[i] >= 4 && sector_layout != 1)
			return (false);
	}
	return (true);
}

static struct drm_framebuffer *
nvkm_fb_create_reject(struct nvkm_softc *sc, int error)
{
	/*
	 * Ownership: this helper only updates the nvkm device counters and
	 * returns an ERR_PTR to the caller; it never owns framebuffer, GEM, or
	 * BO references.
	 *
	 * Lifetime: the counter lives with struct nvkm_softc.  The returned
	 * ERR_PTR has no lifetime beyond the DRM fb_create call.
	 *
	 * Threading: fb_create runs under DRM modeset/file serialization for a
	 * single request.  The diagnostic counter is monotonic and intentionally
	 * lockless, matching the surrounding display counters.
	 */
	sc->kms_fb_create_reject_count++;
	return (ERR_PTR(error));
}

static struct drm_framebuffer *
nvkm_fb_create_error(struct nvkm_softc *sc, int error)
{
	/*
	 * Ownership: this helper only records unexpected/internal fb_create
	 * failures and returns an ERR_PTR.  The caller remains responsible for
	 * releasing any GEM or framebuffer objects acquired before the failure.
	 *
	 * Lifetime: the counter lives with struct nvkm_softc.  No borrowed
	 * pointer is stored here.
	 *
	 * Threading: the counter is diagnostic and monotonic.  It is read from
	 * the debug sysctl and release smoke after the operation has returned.
	 */
	sc->kms_fb_create_error_count++;
	return (ERR_PTR(error));
}

static struct drm_framebuffer *
nvkm_fb_create(struct drm_device *dev, struct drm_file *file,
    const struct drm_mode_fb_cmd2 *cmd)
{
	struct nvkm_softc *sc = dev->dev_private;
	const struct drm_format_info *info;
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;
	uint64_t line;
	uint64_t min_size;
	bool blocklinear;
	uint8_t kind;
	int ret;

	sc->kms_fb_create_count++;
	if (cmd->width == 0 || cmd->height == 0)
		return (nvkm_fb_create_reject(sc, -EINVAL));

	info = drm_get_format_info(dev, cmd);
	if (info == NULL || info->num_planes != 1)
		return (nvkm_fb_create_reject(sc, -EINVAL));
	if (!nvkm_plane_format_mod_supported(NULL, cmd->pixel_format,
	    cmd->modifier[0]))
		return (nvkm_fb_create_reject(sc, -EINVAL));
	blocklinear = nvkm_modifier_is_blocklinear(cmd->modifier[0]);
	kind = nvkm_modifier_kind(cmd->modifier[0]);

	line = (uint64_t)cmd->width * info->cpp[0];
	if (cmd->pitches[0] < line || (cmd->pitches[0] & 0x3fu) != 0)
		return (nvkm_fb_create_reject(sc, -EINVAL));
	min_size = (uint64_t)(cmd->height - 1) * cmd->pitches[0] +
	    line + cmd->offsets[0];

	obj = drm_gem_object_lookup(file, cmd->handles[0]);
	if (obj == NULL)
		return (nvkm_fb_create_reject(sc, -ENOENT));
	if (obj->size < min_size) {
		drm_gem_object_put_unlocked(obj);
		return (nvkm_fb_create_reject(sc, -EINVAL));
	}
	bo = to_nvkm_bo(obj);
	if (!(bo->domain & NOUVEAU_GEM_DOMAIN_VRAM)) {
		drm_gem_object_put_unlocked(obj);
		return (nvkm_fb_create_reject(sc, -EINVAL));
	}
	if (blocklinear) {
		if (!bo->vm_bound_tiled || bo->vm_bound_mixed_kind ||
		    bo->vm_bound_kind != kind) {
			nvkm_infof(sc->dev,
			    "drm: reject blocklinear fb handle=%u "
			    "modifier_kind=0x%02x bo_tiled=%d "
			    "bo_kind=0x%02x mixed=%d\n",
			    cmd->handles[0], kind, bo->vm_bound_tiled,
			    bo->vm_bound_kind, bo->vm_bound_mixed_kind);
			drm_gem_object_put_unlocked(obj);
			return (nvkm_fb_create_reject(sc, -EINVAL));
		}
	} else if (bo->vm_bound_tiled) {
		nvkm_infof(sc->dev,
		    "drm: reject implicit-linear fb on tiled bo handle=%u\n",
		    cmd->handles[0]);
		drm_gem_object_put_unlocked(obj);
		return (nvkm_fb_create_reject(sc, -EINVAL));
	}

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (fb == NULL) {
		drm_gem_object_put_unlocked(obj);
		return (nvkm_fb_create_error(sc, -ENOMEM));
	}
	drm_helper_mode_fill_fb_struct(dev, fb, cmd);
	fb->obj[0] = obj;
	ret = drm_framebuffer_init(dev, fb, &nvkm_user_fb_funcs);
	if (ret != 0) {
		fb->obj[0] = NULL;
		kfree(fb);
		drm_gem_object_put_unlocked(obj);
		return (nvkm_fb_create_error(sc, ret));
	}
	if (blocklinear)
		sc->kms_fb_create_blocklinear_count++;
	else
		sc->kms_fb_create_linear_count++;
	return (fb);
}

static int nvkm_atomic_check(struct drm_device *dev,
    struct drm_atomic_state *state);
static int nvkm_atomic_commit(struct drm_device *dev,
    struct drm_atomic_state *state, bool nonblock);
static void nvkm_atomic_finish_prepared_outputs(struct drm_atomic_state *state);
static void nvkm_drm_kms_link_status_bad_schedule(struct nvkm_softc *sc,
    uint32_t display_id);
static struct drm_atomic_state *nvkm_atomic_state_alloc(struct drm_device *dev);
static void nvkm_atomic_state_clear(struct drm_atomic_state *state);
static void nvkm_atomic_state_free(struct drm_atomic_state *state);

/*
 * Driver-private atomic state.
 *
 * Ownership:
 *   The DRM atomic core owns the base refcounted state.  nvkm owns the prepared
 *   output route slots and releases any bridge-acquired SOR/IOR state through
 *   nvkm_atomic_state_clear().
 *
 * Lifetime:
 *   A prepared route lives in the same refcounted atomic state that crosses the
 *   swap_state boundary.  Blocking commits consume it before returning;
 *   nonblocking commits consume it from commit_work before the final state put.
 *
 * Threading:
 *   Routes are filled before swap_state while modeset locks are held.  After
 *   swap_state only the commit worker owning this atomic-state reference reads
 *   or clears them.  CRTC, IRQ, HPD, and async cursor paths never share mutable
 *   route storage.
 */
struct nvkm_atomic_summary {
	bool valid;
	bool lock_core;
	bool flush_disable;
	bool legacy_cursor_update;
	bool async_update;
	uint32_t old_active_head_mask;
	uint32_t new_active_head_mask;
	uint32_t modeset_head_mask;
	uint32_t disable_head_mask;
	uint32_t enable_head_mask;
	uint32_t primary_update_head_mask;
	uint32_t primary_disable_head_mask;
	uint32_t cursor_update_head_mask;
	uint32_t cursor_disable_head_mask;
	uint32_t plane_update_mask;
	uint32_t plane_disable_mask;
	uint32_t prepared_head_mask;
	uint32_t prepared_display_mask;
};

struct nvkm_atomic_state {
	struct drm_atomic_state base;
	struct nvkm_dispnv50_output_prepare prepared_route[NVKM_DISPLAY_MAX_HEADS];
	struct nvkm_atomic_summary summary;
};

#define to_nvkm_atomic_state(s) \
	container_of(s, struct nvkm_atomic_state, base)

/*
 * Build the driver-private atomic transaction summary.
 *
 * Ownership: the summary stores only scalar masks derived from the borrowed DRM
 * atomic state.  It does not acquire references to CRTCs, planes, framebuffers,
 * connectors, or display routes.
 * Lifetime: callers may rebuild the summary while the atomic state is alive;
 * published copies in struct nvkm_softc are diagnostic snapshots of the last
 * commit tail and do not extend the lifetime of the atomic state.
 * Threading: atomic_check fills the summary while modeset locks protect the
 * state graph.  commit_tail may read or rebuild it from the commit worker that
 * owns the atomic-state reference.  Other threads only read the published softc
 * counters through the debug sysctl.
 */
static uint32_t
nvkm_atomic_head_mask(const struct drm_crtc *crtc)
{
	if (crtc == NULL)
		return (0);
	return (drm_crtc_mask(crtc));
}

static bool
nvkm_atomic_plane_visible(const struct drm_plane_state *state)
{
	return (state != NULL && state->crtc != NULL && state->fb != NULL &&
	    state->crtc_w != 0 && state->crtc_h != 0 &&
	    state->src_w != 0 && state->src_h != 0);
}

static bool
nvkm_atomic_plane_changed(const struct drm_plane_state *old_state,
    const struct drm_plane_state *new_state)
{
	if (old_state == NULL || new_state == NULL)
		return (old_state != new_state);

	return (old_state->crtc != new_state->crtc ||
	    old_state->fb != new_state->fb ||
	    old_state->crtc_x != new_state->crtc_x ||
	    old_state->crtc_y != new_state->crtc_y ||
	    old_state->crtc_w != new_state->crtc_w ||
	    old_state->crtc_h != new_state->crtc_h ||
	    old_state->src_x != new_state->src_x ||
	    old_state->src_y != new_state->src_y ||
	    old_state->src_w != new_state->src_w ||
	    old_state->src_h != new_state->src_h ||
	    old_state->alpha != new_state->alpha ||
	    old_state->pixel_blend_mode != new_state->pixel_blend_mode ||
	    old_state->rotation != new_state->rotation ||
	    old_state->color_encoding != new_state->color_encoding ||
	    old_state->color_range != new_state->color_range);
}

static bool
nvkm_atomic_crtc_modeset(struct drm_atomic_state *state,
    struct drm_crtc *crtc)
{
	struct drm_crtc_state *crtc_state;

	if (state == NULL || crtc == NULL)
		return (false);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	return (crtc_state != NULL && drm_atomic_crtc_needs_modeset(crtc_state));
}

static void
nvkm_atomic_summary_account_plane(struct drm_atomic_state *state,
    struct nvkm_atomic_summary *summary, struct drm_plane *plane,
    struct drm_plane_state *old_plane_state,
    struct drm_plane_state *new_plane_state)
{
	bool changed, new_modeset, new_visible, old_modeset, old_visible;
	bool crtc_changed;
	uint32_t new_head_mask, old_head_mask, plane_mask;

	old_visible = nvkm_atomic_plane_visible(old_plane_state);
	new_visible = nvkm_atomic_plane_visible(new_plane_state);
	changed = nvkm_atomic_plane_changed(old_plane_state, new_plane_state);
	old_modeset = old_visible &&
	    nvkm_atomic_crtc_modeset(state, old_plane_state->crtc);
	new_modeset = new_visible &&
	    nvkm_atomic_crtc_modeset(state, new_plane_state->crtc);
	old_head_mask = old_visible ?
	    nvkm_atomic_head_mask(old_plane_state->crtc) : 0;
	new_head_mask = new_visible ?
	    nvkm_atomic_head_mask(new_plane_state->crtc) : 0;
	crtc_changed = old_visible && new_plane_state != NULL &&
	    old_plane_state->crtc != new_plane_state->crtc;
	plane_mask = drm_plane_mask(plane);

	if (old_visible && (!new_visible || crtc_changed || old_modeset))
		summary->plane_disable_mask |= plane_mask;
	if (new_visible && (!old_visible || changed || new_modeset))
		summary->plane_update_mask |= plane_mask;

	if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
		if (old_visible && (!new_visible || crtc_changed ||
		    old_modeset))
			summary->primary_disable_head_mask |= old_head_mask;
		if (new_visible && (!old_visible || changed || new_modeset))
			summary->primary_update_head_mask |= new_head_mask;
	} else if (plane->type == DRM_PLANE_TYPE_CURSOR) {
		if (old_visible && (!new_visible || crtc_changed ||
		    old_modeset))
			summary->cursor_disable_head_mask |= old_head_mask;
		if (new_visible && (!old_visible || changed || new_modeset))
			summary->cursor_update_head_mask |= new_head_mask;
	}
}

static void
nvkm_atomic_build_summary(struct drm_atomic_state *state)
{
	struct nvkm_atomic_state *nv_state = to_nvkm_atomic_state(state);
	struct nvkm_atomic_summary *summary = &nv_state->summary;
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc_state *old_crtc_state;
	struct drm_plane_state *new_plane_state;
	struct drm_plane_state *old_plane_state;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	uint32_t head_mask;
	int i;

	memset(summary, 0, sizeof(*summary));
	summary->valid = true;
	summary->legacy_cursor_update = state->legacy_cursor_update;
	summary->async_update = state->async_update;

	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state,
	    new_crtc_state, i) {
		head_mask = nvkm_atomic_head_mask(crtc);
		if (old_crtc_state != NULL && old_crtc_state->enable)
			summary->old_active_head_mask |= head_mask;
		if (new_crtc_state != NULL && new_crtc_state->enable)
			summary->new_active_head_mask |= head_mask;
		if (new_crtc_state != NULL &&
		    drm_atomic_crtc_needs_modeset(new_crtc_state)) {
			summary->modeset_head_mask |= head_mask;
			if (old_crtc_state != NULL && old_crtc_state->enable)
				summary->disable_head_mask |= head_mask;
			if (new_crtc_state->enable)
				summary->enable_head_mask |= head_mask;
		}
	}

	for_each_oldnew_plane_in_state(state, plane, old_plane_state,
	    new_plane_state, i) {
		nvkm_atomic_summary_account_plane(state, summary, plane,
		    old_plane_state, new_plane_state);
	}

	summary->lock_core = summary->modeset_head_mask != 0 ||
	    summary->primary_update_head_mask != 0 ||
	    summary->primary_disable_head_mask != 0 ||
	    summary->cursor_update_head_mask != 0 ||
	    summary->cursor_disable_head_mask != 0;
	summary->flush_disable = summary->disable_head_mask != 0 ||
	    summary->primary_disable_head_mask != 0 ||
	    summary->cursor_disable_head_mask != 0 ||
	    summary->plane_disable_mask != 0;
}

static void
nvkm_atomic_publish_summary(struct nvkm_softc *sc,
    struct drm_atomic_state *state)
{
	struct nvkm_atomic_state *nv_state;
	struct nvkm_atomic_summary *summary;

	if (sc == NULL || state == NULL)
		return;

	nv_state = to_nvkm_atomic_state(state);
	if (!nv_state->summary.valid)
		nvkm_atomic_build_summary(state);
	summary = &nv_state->summary;

	sc->kms_atomic_summary_count++;
	sc->kms_atomic_last_legacy_cursor_update =
	    summary->legacy_cursor_update ? 1 : 0;
	sc->kms_atomic_last_async_update = summary->async_update ? 1 : 0;
	sc->kms_atomic_last_lock_core = summary->lock_core ? 1 : 0;
	sc->kms_atomic_last_flush_disable = summary->flush_disable ? 1 : 0;
	sc->kms_atomic_last_old_active_heads = summary->old_active_head_mask;
	sc->kms_atomic_last_new_active_heads = summary->new_active_head_mask;
	sc->kms_atomic_last_modeset_heads = summary->modeset_head_mask;
	sc->kms_atomic_last_disable_heads = summary->disable_head_mask;
	sc->kms_atomic_last_enable_heads = summary->enable_head_mask;
	sc->kms_atomic_last_primary_update_heads =
	    summary->primary_update_head_mask;
	sc->kms_atomic_last_primary_disable_heads =
	    summary->primary_disable_head_mask;
	sc->kms_atomic_last_cursor_update_heads =
	    summary->cursor_update_head_mask;
	sc->kms_atomic_last_cursor_disable_heads =
	    summary->cursor_disable_head_mask;
	sc->kms_atomic_last_plane_update_mask = summary->plane_update_mask;
	sc->kms_atomic_last_plane_disable_mask = summary->plane_disable_mask;
	sc->kms_atomic_last_prepared_heads = summary->prepared_head_mask;
	sc->kms_atomic_last_prepared_displays = summary->prepared_display_mask;
}

static const struct drm_mode_config_funcs nvkm_mode_config_funcs = {
	.fb_create	= nvkm_fb_create,
	.atomic_check	= nvkm_atomic_check,
	.atomic_commit	= nvkm_atomic_commit,
	.atomic_state_alloc = nvkm_atomic_state_alloc,
	.atomic_state_clear = nvkm_atomic_state_clear,
	.atomic_state_free = nvkm_atomic_state_free,
};

/*
 * Complete an atomic CRTC event.
 *
 * Ownership: state->event belongs to the atomic commit until this helper
 * consumes it and clears the pointer.  Lifetime: the event object is either
 * queued on the CRTC vblank list or sent synchronously before the helper
 * returns.  Threading: callers run from atomic commit context; event_lock
 * serializes the handoff with vblank IRQ delivery.
 */
static void
nvkm_crtc_complete_event(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	if (state == NULL || state->event == NULL)
		return;

	spin_lock_irqsave(&dev->event_lock, flags);
	if (state->active && drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, state->event);
	else
		drm_crtc_send_vblank_event(crtc, state->event);
	state->event = NULL;
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static void
nvkm_atomic_complete_modeset_events(struct drm_atomic_state *old_state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *new_crtc_state;
	int i;

	for_each_new_crtc_in_state(old_state, crtc, new_crtc_state, i) {
		if (drm_atomic_crtc_needs_modeset(new_crtc_state))
			nvkm_crtc_complete_event(crtc, new_crtc_state);
	}
}

/*
 * Begin a KMS atomic commit-tail diagnostic record.
 *
 * Ownership:
 *   Borrows sc and copies the already-published atomic summary scalars. It
 *   does not own or retain the DRM atomic state, CRTCs, planes, or routes.
 *
 * Lifetime:
 *   The active record describes only the currently executing commit tail. The
 *   copied summary remains as last-tail evidence after completion.
 *
 * Threading:
 *   Called by the commit-tail owner. Sysctl readers may sample partially
 *   updated values; the fields are diagnostics and must not drive behavior.
 */
static void
nvkm_atomic_tail_begin(struct nvkm_softc *sc)
{
	if (sc == NULL)
		return;

	sc->kms_atomic_tail_seq++;
	sc->kms_atomic_tail_active = 1;
	sc->kms_atomic_tail_stage = NVKM_KMS_ATOMIC_TAIL_BEGIN;
	sc->kms_atomic_tail_last_stage = NVKM_KMS_ATOMIC_TAIL_BEGIN;
	sc->kms_atomic_tail_last_lock_core =
	    sc->kms_atomic_last_lock_core;
	sc->kms_atomic_tail_last_flush_disable =
	    sc->kms_atomic_last_flush_disable;
	sc->kms_atomic_tail_last_modeset_heads =
	    sc->kms_atomic_last_modeset_heads;
	sc->kms_atomic_tail_last_disable_heads =
	    sc->kms_atomic_last_disable_heads;
	sc->kms_atomic_tail_last_enable_heads =
	    sc->kms_atomic_last_enable_heads;
	sc->kms_atomic_tail_last_plane_update_mask =
	    sc->kms_atomic_last_plane_update_mask;
	sc->kms_atomic_tail_last_plane_disable_mask =
	    sc->kms_atomic_last_plane_disable_mask;
}

static void
nvkm_atomic_tail_enter(struct nvkm_softc *sc,
    enum nvkm_kms_atomic_tail_stage stage)
{
	if (sc == NULL)
		return;

	sc->kms_atomic_tail_stage = stage;
	sc->kms_atomic_tail_last_stage = stage;
	switch (stage) {
	case NVKM_KMS_ATOMIC_TAIL_MODESET_DISABLES:
		sc->kms_atomic_tail_modeset_disables_count++;
		break;
	case NVKM_KMS_ATOMIC_TAIL_COMMIT_PLANES:
		sc->kms_atomic_tail_commit_planes_count++;
		break;
	case NVKM_KMS_ATOMIC_TAIL_MODESET_ENABLES:
		sc->kms_atomic_tail_modeset_enables_count++;
		break;
	case NVKM_KMS_ATOMIC_TAIL_MODESET_EVENTS:
		sc->kms_atomic_tail_modeset_events_count++;
		break;
	case NVKM_KMS_ATOMIC_TAIL_FAKE_VBLANK:
		sc->kms_atomic_tail_fake_vblank_count++;
		break;
	case NVKM_KMS_ATOMIC_TAIL_HW_DONE:
		sc->kms_atomic_tail_hw_done_count++;
		break;
	case NVKM_KMS_ATOMIC_TAIL_WAIT_FLIP_DONE:
		sc->kms_atomic_tail_wait_flip_done_count++;
		break;
	case NVKM_KMS_ATOMIC_TAIL_CLEANUP_PLANES:
		sc->kms_atomic_tail_cleanup_planes_count++;
		break;
	case NVKM_KMS_ATOMIC_TAIL_FINISH_PREPARED:
		sc->kms_atomic_tail_finish_prepared_count++;
		break;
	default:
		break;
	}
}

static void
nvkm_atomic_tail_finish(struct nvkm_softc *sc)
{
	if (sc == NULL)
		return;

	sc->kms_atomic_tail_complete_count++;
	sc->kms_atomic_tail_stage = NVKM_KMS_ATOMIC_TAIL_IDLE;
	sc->kms_atomic_tail_active = 0;
}

static void
nvkm_atomic_commit_tail(struct drm_atomic_state *old_state)
{
	struct drm_device *dev = old_state->dev;
	struct nvkm_softc *sc = dev->dev_private;

	sc->kms_atomic_commit_tail_count++;
	nvkm_atomic_publish_summary(sc, old_state);
	nvkm_atomic_tail_begin(sc);
	nvkm_atomic_tail_enter(sc, NVKM_KMS_ATOMIC_TAIL_MODESET_DISABLES);
	drm_atomic_helper_commit_modeset_disables(dev, old_state);
	nvkm_atomic_tail_enter(sc, NVKM_KMS_ATOMIC_TAIL_COMMIT_PLANES);
	drm_atomic_helper_commit_planes(dev, old_state,
	    DRM_PLANE_COMMIT_NO_DISABLE_AFTER_MODESET);
	nvkm_atomic_tail_enter(sc, NVKM_KMS_ATOMIC_TAIL_MODESET_ENABLES);
	drm_atomic_helper_commit_modeset_enables(dev, old_state);
	nvkm_atomic_tail_enter(sc, NVKM_KMS_ATOMIC_TAIL_MODESET_EVENTS);
	nvkm_atomic_complete_modeset_events(old_state);
	nvkm_atomic_tail_enter(sc, NVKM_KMS_ATOMIC_TAIL_FAKE_VBLANK);
	drm_atomic_helper_fake_vblank(old_state);
	nvkm_atomic_tail_enter(sc, NVKM_KMS_ATOMIC_TAIL_HW_DONE);
	drm_atomic_helper_commit_hw_done(old_state);
	sc->kms_atomic_flip_done_wait_count++;
	nvkm_atomic_tail_enter(sc, NVKM_KMS_ATOMIC_TAIL_WAIT_FLIP_DONE);
	drm_atomic_helper_wait_for_flip_done(dev, old_state);
	nvkm_atomic_tail_enter(sc, NVKM_KMS_ATOMIC_TAIL_CLEANUP_PLANES);
	drm_atomic_helper_cleanup_planes(dev, old_state);
	nvkm_atomic_tail_enter(sc, NVKM_KMS_ATOMIC_TAIL_FINISH_PREPARED);
	nvkm_atomic_finish_prepared_outputs(old_state);
	nvkm_atomic_tail_finish(sc);
}

static const struct drm_mode_config_helper_funcs nvkm_mode_config_helper_funcs = {
	.atomic_commit_tail = nvkm_atomic_commit_tail,
};

static struct drm_atomic_state *
nvkm_atomic_state_alloc(struct drm_device *dev)
{
	struct nvkm_atomic_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state == NULL)
		return (NULL);
	if (drm_atomic_state_init(dev, &state->base) != 0) {
		kfree(state);
		return (NULL);
	}
	return (&state->base);
}

static void
nvkm_atomic_state_clear(struct drm_atomic_state *state)
{
	nvkm_atomic_finish_prepared_outputs(state);
	drm_atomic_state_default_clear(state);
}

static void
nvkm_atomic_state_free(struct drm_atomic_state *state)
{
	struct nvkm_atomic_state *nv_state = to_nvkm_atomic_state(state);

	drm_atomic_state_default_release(state);
	kfree(nv_state);
}

struct nvkm_crtc {
	struct drm_crtc		base;
	struct nvkm_softc	*sc;
	uint32_t		head;	/* HEAD index */
	uint32_t		win;	/* primary window index */
};

#define to_nvkm_crtc(c) container_of(c, struct nvkm_crtc, base)

/*
 * Stack-local decoded CRTC route.
 *
 * Ownership: this object owns only scalar snapshots. It borrows the DRM CRTC,
 * connector list, atomic connector state, and connector display_info for the
 * duration of the caller.
 * Lifetime: callers must not store it past the current check/commit callback.
 * Threading: no internal locking; pass the current drm_atomic_state before
 * swap_state so route decode sees new connector properties.  Pass NULL only
 * after swap_state, when conn->state is already the committed state.
 */
struct nvkm_kms_crtc_atom {
	struct nvkm_softc *sc;
	struct drm_crtc *crtc;
	struct nvkm_crtc *nc;
	struct nvkm_dispnv50_head_config head;
	struct nvkm_gsp_disp_output_info output;
	uint32_t display_id;
	uint32_t connector_count;
	int max_tmds_clock;
};

static void
nvkm_kms_crtc_atom_init(struct nvkm_kms_crtc_atom *atom, struct nvkm_softc *sc,
    struct drm_crtc *crtc)
{
	memset(atom, 0, sizeof(*atom));
	atom->sc = sc;
	atom->crtc = crtc;
	atom->nc = to_nvkm_crtc(crtc);
	atom->head.bpc = 8;
	atom->head.dither_mode = NVKM_DISPNV50_DITHER_MODE_AUTO;
	atom->head.dither_depth = NVKM_DISPNV50_DITHER_DEPTH_AUTO;
}

static void
nvkm_kms_crtc_atom_take_connector(struct nvkm_kms_crtc_atom *atom,
    struct drm_connector *conn, const struct drm_connector_state *conn_state)
{
	struct nvkm_drm_connector *nvkm_conn = to_nvkm_connector(conn);
	const struct nvkm_connector_state *state = conn_state != NULL ?
	    to_nvkm_connector_state_const(conn_state) : NULL;

	atom->display_id = nvkm_conn->display_id;
	atom->connector_count++;
	atom->head.hdmi.has_infoframe = conn->display_info.has_hdmi_infoframe;
	atom->head.hdmi.scdc_supported =
	    conn->display_info.hdmi.scdc.supported;
	atom->head.hdmi.scdc_scrambling =
	    conn->display_info.hdmi.scdc.scrambling.supported;
	atom->head.hdmi.scdc_low_rates =
	    conn->display_info.hdmi.scdc.scrambling.low_rates;
	atom->max_tmds_clock = conn->display_info.max_tmds_clock;
	if (state != NULL) {
		atom->head.bpc = state->max_bpc;
		atom->head.dither_mode = state->dither_mode;
		atom->head.dither_depth = state->dither_depth;
	}
}

static int
nvkm_kms_crtc_atom_validate_output(struct nvkm_kms_crtc_atom *atom)
{
	const struct nvkm_gsp_disp_output_info *info = &atom->output;

	if (info->heads != 0 && (info->heads & BIT(atom->nc->head)) == 0) {
		nvkm_infof(atom->sc->dev,
		    "drm: crtc route rejects display=0x%x head=%u heads=0x%x\n",
		    atom->display_id, atom->nc->head, info->heads);
		return (-EINVAL);
	}

	switch (info->output_type) {
	case DCB_OUTPUT_TMDS:
		break;
	case DCB_OUTPUT_DP:
		/*
		 * info->mst_capable means the physical DP output can host MST,
		 * not that this KMS connector is an MST virtual sink.  This KMS
		 * layer currently creates only the physical SST connector and no
		 * DPMST encoders, so MST capability must not reject SST routing.
		 */
		break;
	default:
		nvkm_infof(atom->sc->dev,
		    "drm: crtc route rejects display=0x%x head=%u type=0x%x\n",
		    atom->display_id, atom->nc->head, info->output_type);
		return (-ENOSYS);
	}

	return (0);
}

static enum drm_mode_status
nvkm_kms_crtc_atom_validate_mode(struct nvkm_kms_crtc_atom *atom,
    const struct drm_display_mode *mode)
{
	return (nvkm_kms_output_mode_valid(atom->sc, atom->display_id,
	    &atom->output, mode, atom->max_tmds_clock,
	    atom->head.hdmi.scdc_scrambling, atom->head.bpc));
}

static int
nvkm_kms_crtc_atom_route(struct nvkm_kms_crtc_atom *atom,
    struct drm_atomic_state *state, uint32_t connector_mask,
    bool validate_output)
{
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;
	int ret;

	list_for_each_entry(conn, &atom->crtc->dev->mode_config.connector_list,
	    head) {
		if ((connector_mask & drm_connector_mask(conn)) == 0)
			continue;
		conn_state = state != NULL ?
		    drm_atomic_get_new_connector_state(state, conn) : NULL;
		if (conn_state == NULL)
			conn_state = conn->state;
		nvkm_kms_crtc_atom_take_connector(atom, conn, conn_state);
	}
	if (atom->connector_count != 1 || atom->display_id == 0) {
		nvkm_infof(atom->sc->dev,
		    "drm: crtc route rejects head=%u connector_count=%u display=0x%x\n",
		    atom->nc->head, atom->connector_count, atom->display_id);
		return (-EINVAL);
	}

	if (!validate_output)
		return (0);

	ret = nvkm_gsp_disp_output_info(atom->sc, atom->display_id,
	    &atom->output);
	if (ret != 0)
		return (ret);
	return (nvkm_kms_crtc_atom_validate_output(atom));
}

static int
nvkm_atomic_check_crtc_route(struct nvkm_softc *sc, struct drm_crtc *crtc,
    struct drm_atomic_state *state, const struct drm_crtc_state *crtc_state)
{
	struct nvkm_kms_crtc_atom atom;
	enum drm_mode_status mode_status;
	int ret;

	if (crtc_state == NULL || !crtc_state->enable)
		return (0);
	if (sc == NULL || sc->disp == NULL)
		return (-ENODEV);

	nvkm_kms_crtc_atom_init(&atom, sc, crtc);
	ret = nvkm_kms_crtc_atom_route(&atom, state,
	    crtc_state->connector_mask, true);
	if (ret != 0)
		return (ret);

	mode_status = nvkm_kms_crtc_atom_validate_mode(&atom,
	    &crtc_state->adjusted_mode);
	if (mode_status != MODE_OK) {
		nvkm_infof(sc->dev,
		    "drm: crtc route rejects display=0x%x head=%u mode=%s status=%d\n",
		    atom.display_id, atom.nc->head, crtc_state->mode.name,
		    mode_status);
		return (-EINVAL);
	}
	return (0);
}

static int
nvkm_atomic_check_routes(struct drm_device *dev, struct drm_atomic_state *state)
{
	struct nvkm_softc *sc = dev->dev_private;
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc;
	int i;

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		int ret;

		ret = nvkm_atomic_check_crtc_route(sc, crtc, state,
		    new_crtc_state);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static int
nvkm_atomic_check(struct drm_device *dev, struct drm_atomic_state *state)
{
	int ret;

	ret = drm_atomic_helper_check(dev, state);
	if (ret != 0)
		return (ret);

	nvkm_atomic_build_summary(state);
	return (nvkm_atomic_check_routes(dev, state));
}

static void
nvkm_kms_record_result(struct nvkm_softc *sc, uint32_t head, uint32_t win,
    int err, const char *where)
{
	if (sc == NULL || err == 0)
		return;

	sc->kms_commit_error_count++;
	sc->kms_last_error = err;
	sc->kms_last_head = head;
	sc->kms_last_win = win;
	nvkm_infof(sc->dev, "drm: %s failed head=%u win=%u err=%d\n",
	    where, head, win, err);
}

static bool
nvkm_atomic_state_needs_output_prepare(struct drm_atomic_state *state)
{
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc;
	int i;

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		(void)crtc;
		if (new_crtc_state->enable &&
		    drm_atomic_crtc_needs_modeset(new_crtc_state))
			return (true);
	}

	return (false);
}

static struct nvkm_dispnv50_output_prepare *
nvkm_atomic_prepared_route(struct drm_atomic_state *state, uint32_t head)
{
	struct nvkm_atomic_state *nv_state;

	if (state == NULL || head >= NVKM_DISPLAY_MAX_HEADS)
		return (NULL);
	nv_state = to_nvkm_atomic_state(state);
	return (&nv_state->prepared_route[head]);
}

static void
nvkm_atomic_finish_prepared_outputs(struct drm_atomic_state *state)
{
	struct nvkm_softc *sc;
	uint32_t head;

	if (state == NULL || state->dev == NULL)
		return;

	sc = state->dev->dev_private;
	for (head = 0; head < NVKM_DISPLAY_MAX_HEADS; head++)
		nvkm_dispnv50_output_prepare_abort(sc,
		    nvkm_atomic_prepared_route(state, head));
}

static int
nvkm_atomic_prepare_outputs(struct drm_device *dev,
    struct drm_atomic_state *state)
{
	struct nvkm_softc *sc = dev->dev_private;
	struct nvkm_atomic_state *nv_state = to_nvkm_atomic_state(state);
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc;
	int ret = 0;
	int i;

	if (!nv_state->summary.valid)
		nvkm_atomic_build_summary(state);

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
		struct nvkm_kms_crtc_atom atom;
		struct nvkm_dispnv50_output_prepare *route;

		route = nvkm_atomic_prepared_route(state, nc->head);
		if (route == NULL) {
			ret = -EINVAL;
			goto fail;
		}
		nvkm_dispnv50_output_prepare_abort(sc, route);
		if (!new_crtc_state->enable ||
		    !drm_atomic_crtc_needs_modeset(new_crtc_state))
			continue;

		nvkm_kms_crtc_atom_init(&atom, sc, crtc);
		ret = nvkm_kms_crtc_atom_route(&atom, state,
		    new_crtc_state->connector_mask, false);
		if (ret != 0) {
			nvkm_kms_record_result(sc, nc->head, nc->win, ret,
			    "output prepare route");
			goto fail;
		}

		ret = nvkm_dispnv50_output_prepare(sc,
		    &new_crtc_state->adjusted_mode, nc->head,
		    atom.display_id, &atom.head, route);
		if (ret != 0) {
			nvkm_kms_record_result(sc, nc->head, nc->win, ret,
			    "output prepare");
			goto fail;
		}
		nv_state->summary.prepared_head_mask |= drm_crtc_mask(crtc);
		nv_state->summary.prepared_display_mask |= atom.display_id;
	}

	return (0);

fail:
	nvkm_atomic_finish_prepared_outputs(state);
	return (ret);
}

static int
nvkm_atomic_commit_tail_run(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;
	struct nvkm_softc *sc = dev->dev_private;
	int ret;

	ret = drm_atomic_helper_wait_for_fences(dev, state, false);
	if (ret != 0)
		nvkm_kms_record_result(sc, 0, 0, ret, "atomic fence wait");
	drm_atomic_helper_wait_for_dependencies(state);
	nvkm_atomic_commit_tail(state);
	drm_atomic_helper_commit_cleanup_done(state);
	drm_atomic_state_put(state);
	return (ret);
}

static void
nvkm_atomic_commit_work(struct work_struct *work)
{
	struct drm_atomic_state *state;

	state = container_of(work, struct drm_atomic_state, commit_work);
	(void)nvkm_atomic_commit_tail_run(state);
}

static int
nvkm_atomic_commit_prepared(struct drm_device *dev,
    struct drm_atomic_state *state, bool nonblock)
{
	int ret;

	ret = drm_atomic_helper_setup_commit(state, nonblock);
	if (ret != 0)
		return (ret);

	INIT_WORK(&state->commit_work, nvkm_atomic_commit_work);

	ret = drm_atomic_helper_prepare_planes(dev, state);
	if (ret != 0)
		return (ret);

	if (!nonblock) {
		ret = drm_atomic_helper_wait_for_fences(dev, state, true);
		if (ret != 0)
			goto err;
	}

	ret = nvkm_atomic_prepare_outputs(dev, state);
	if (ret != 0)
		goto err;

	ret = drm_atomic_helper_swap_state(state, true);
	if (ret != 0)
		goto err;

	drm_atomic_state_get(state);
	if (nonblock) {
		if (!queue_work(system_unbound_wq, &state->commit_work))
			(void)nvkm_atomic_commit_tail_run(state);
	} else {
		(void)nvkm_atomic_commit_tail_run(state);
	}
	return (0);

err:
	nvkm_atomic_finish_prepared_outputs(state);
	drm_atomic_helper_cleanup_planes(dev, state);
	return (ret);
}

static int
nvkm_atomic_commit(struct drm_device *dev, struct drm_atomic_state *state,
    bool nonblock)
{
	if (state->async_update)
		return (drm_atomic_helper_commit(dev, state, nonblock));
	if (!nvkm_atomic_state_needs_output_prepare(state))
		return (drm_atomic_helper_commit(dev, state, nonblock));
	return (nvkm_atomic_commit_prepared(dev, state, nonblock));
}

static int
nvkm_crtc_disable_plane(struct nvkm_crtc *nc, struct drm_plane *plane)
{
	int err;

	if (nc == NULL || nc->sc == NULL || plane == NULL ||
	    nc->sc->disp == NULL)
		return (0);

	if (plane->type == DRM_PLANE_TYPE_CURSOR) {
		nc->sc->kms_cursor_disable_count++;
		err = nvkm_dispnv50_cursor_disable(nc->sc, nc->head);
		if (err != 0)
			nc->sc->kms_cursor_error_count++;
		nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
		    "crtc disable cursor");
		return (err);
	}

	if (plane != nc->base.primary)
		return (0);

	nc->sc->kms_plane_disable_count++;
	err = nvkm_dispnv50_plane_disable(nc->sc, nc->win);
	nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
	    "crtc disable primary");
	return (err);
}

static int
nvkm_crtc_disable_planes(struct nvkm_crtc *nc,
    const struct drm_crtc_state *old_state)
{
	struct drm_plane *plane;
	int first_err = 0;

	if (old_state == NULL)
		return (0);

	drm_atomic_crtc_state_for_each_plane(plane, old_state) {
		int err;

		err = nvkm_crtc_disable_plane(nc, plane);
		if (err != 0 && first_err == 0)
			first_err = err;
	}

	return (first_err);
}

/* ===== plane: NVC57E window validation ===== */

static const uint32_t nvkm_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_ARGB2101010,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_ABGR2101010,
};

static const uint32_t nvkm_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

static const uint64_t nvkm_cursor_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static bool
nvkm_plane_format_mod_supported(struct drm_plane *plane, uint32_t format,
    uint64_t modifier)
{
	unsigned int i;

	if (plane != NULL && plane->type == DRM_PLANE_TYPE_CURSOR) {
		return (format == DRM_FORMAT_ARGB8888 &&
		    nvkm_modifier_is_linear(modifier));
	}

	for (i = 0; i < nitems(nvkm_plane_formats); i++) {
		if (nvkm_plane_formats[i] == format)
			return (nvkm_format_modifier_cpp_supported(format,
			    modifier));
	}
	return (false);
}

static bool
nvkm_cursor_size_supported(uint32_t size)
{
	return (size == 32 || size == 64 || size == 128 || size == 256);
}

static int
nvkm_framebuffer_plane0_min_size(const struct drm_framebuffer *fb,
    uint64_t *min_size)
{
	uint64_t line;

	if (fb == NULL || fb->format == NULL || min_size == NULL)
		return (-EINVAL);
	if (fb->format->num_planes < 1 || fb->format->cpp[0] == 0 ||
	    fb->width == 0 || fb->height == 0)
		return (-EINVAL);

	line = (uint64_t)fb->width * fb->format->cpp[0];
	if (fb->pitches[0] < line)
		return (-EINVAL);

	*min_size = (uint64_t)(fb->height - 1) * fb->pitches[0] +
	    line + fb->offsets[0];
	return (0);
}

static int
nvkm_framebuffer_bo_size_check(const struct drm_framebuffer *fb,
    const struct drm_gem_object *obj, bool allow_missing_obj)
{
	uint64_t min_size;
	int ret;

	ret = nvkm_framebuffer_plane0_min_size(fb, &min_size);
	if (ret != 0)
		return (ret);
	if (obj == NULL)
		return (allow_missing_obj ? 0 : -EINVAL);
	if (obj->size < min_size)
		return (-EINVAL);
	return (0);
}

/*
 * Ownership:
 *   Borrows state and fb from the DRM atomic check path.
 * Lifetime:
 *   No pointer is retained; the bridge later snapshots the same committed
 *   plane state into scalar window fields before programming hardware.
 * Threading:
 *   Pure validation under DRM atomic locks. No nvkm locks or GSP RPCs.
 */
static bool
nvkm_primary_source_is_supported(const struct drm_plane_state *state,
    const struct drm_crtc_state *crtc_state, const struct drm_framebuffer *fb)
{
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;

	if (state == NULL || crtc_state == NULL || fb == NULL ||
	    state->crtc_w <= 0 || state->crtc_h <= 0)
		return (false);
	if (state->crtc_x != 0 || state->crtc_y != 0)
		return (false);
	if (state->crtc_w != crtc_state->mode.hdisplay ||
	    state->crtc_h != crtc_state->mode.vdisplay)
		return (false);
	if ((state->src_x & 0xffffu) != 0 ||
	    (state->src_y & 0xffffu) != 0 ||
	    (state->src_w & 0xffffu) != 0 ||
	    (state->src_h & 0xffffu) != 0)
		return (false);

	src_x = state->src_x >> 16;
	src_y = state->src_y >> 16;
	src_w = state->src_w >> 16;
	src_h = state->src_h >> 16;
	if (src_w == 0 || src_h == 0)
		return (false);
	if (src_w != state->crtc_w || src_h != state->crtc_h)
		return (false);
	if (src_x > fb->width || src_y > fb->height ||
	    src_w > fb->width - src_x || src_h > fb->height - src_y)
		return (false);
	return (true);
}

static int
nvkm_cursor_atomic_check(struct drm_plane *plane,
    struct drm_plane_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;
	uint32_t size;
	int ret;

	if (state->crtc == NULL)
		return (state->fb == NULL ? 0 : -EINVAL);

	crtc_state = drm_atomic_get_new_crtc_state(state->state, state->crtc);
	if (crtc_state == NULL)
		return (-EINVAL);

	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
	    DRM_PLANE_HELPER_NO_SCALING, DRM_PLANE_HELPER_NO_SCALING,
	    true, true);
	if (ret != 0 || !state->visible)
		return (ret);
	if (!crtc_state->active)
		return (-EINVAL);

	fb = state->fb;
	if (fb == NULL || fb->format == NULL)
		return (-EINVAL);
	if (!nvkm_plane_format_mod_supported(plane, fb->format->format,
	    fb->modifier))
		return (-EINVAL);
	if (state->crtc_w <= 0 || state->crtc_h <= 0 ||
	    state->crtc_w != state->crtc_h ||
	    fb->width != (uint32_t)state->crtc_w)
		return (-EINVAL);
	if (fb->height < (uint32_t)state->crtc_h)
		return (-EINVAL);
	if (state->src_x != 0 || state->src_y != 0 ||
	    state->src_w != ((uint32_t)state->crtc_w << 16) ||
	    state->src_h != ((uint32_t)state->crtc_h << 16))
		return (-EINVAL);

	size = fb->width;
	if (!nvkm_cursor_size_supported(size))
		return (-EINVAL);
	if (fb->pitches[0] != size * 4 || (fb->offsets[0] & 0xffu) != 0)
		return (-EINVAL);

	obj = drm_gem_fb_get_obj(fb, 0);
	if (obj == NULL)
		return (-EINVAL);
	ret = nvkm_framebuffer_bo_size_check(fb, obj, false);
	if (ret != 0)
		return (ret);
	bo = to_nvkm_bo(obj);
	if (!(bo->domain & NOUVEAU_GEM_DOMAIN_VRAM) || bo->paddr == 0)
		return (-EINVAL);
	return (0);
}

static int
nvkm_plane_atomic_check(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	int ret;

	if (plane->type == DRM_PLANE_TYPE_CURSOR)
		return (nvkm_cursor_atomic_check(plane, state));

	if (state->crtc == NULL)
		return (state->fb == NULL ? 0 : -EINVAL);

	crtc_state = drm_atomic_get_new_crtc_state(state->state, state->crtc);
	if (crtc_state == NULL)
		return (-EINVAL);

	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
	    DRM_PLANE_HELPER_NO_SCALING, DRM_PLANE_HELPER_NO_SCALING,
	    true, false);
	if (ret != 0 || !state->visible)
		return (ret);

	fb = state->fb;
	if (fb == NULL || fb->format == NULL)
		return (-EINVAL);
	if (!nvkm_plane_format_mod_supported(plane, fb->format->format,
	    fb->modifier))
		return (-EINVAL);
	if (!nvkm_primary_source_is_supported(state, crtc_state, fb))
		return (-EINVAL);
	if ((fb->pitches[0] & 0x3fu) != 0)
		return (-EINVAL);
	obj = drm_gem_fb_get_obj(fb, 0);
	ret = nvkm_framebuffer_bo_size_check(fb, obj, true);
	if (ret != 0)
		return (ret);
	return (0);
}

static void
nvkm_plane_atomic_update(struct drm_plane *plane,
    struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = plane->state;
	struct drm_crtc_state *crtc_state;
	struct nvkm_crtc *nc;
	struct nvkm_kms_crtc_atom atom;
	int err;

	(void)old_state;
	if (state == NULL || state->crtc == NULL || state->fb == NULL ||
	    !state->visible)
		return;
	if (plane->type == DRM_PLANE_TYPE_CURSOR) {
		crtc_state = state->crtc->state;
		if (crtc_state == NULL || !crtc_state->active ||
		    drm_atomic_crtc_needs_modeset(crtc_state))
			return;
		nc = to_nvkm_crtc(state->crtc);
		if (nc->sc->disp == NULL)
			return;

		/*
		 * Cursor programming is head-local, but a full cursor image
		 * update still belongs to the committed CRTC route.  Decode the
		 * same stack atom used by check/enable/primary update before
		 * touching the head cursor context.
		 */
		nvkm_kms_crtc_atom_init(&atom, nc->sc, state->crtc);
		err = nvkm_kms_crtc_atom_route(&atom, NULL,
		    crtc_state->connector_mask, false);
		if (err != 0) {
			nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
			    "cursor route");
			return;
		}

		nc->sc->kms_cursor_update_count++;
		err = nvkm_dispnv50_cursor_update(nc->sc, state->crtc,
		    nc->head);
		if (err != 0)
			nc->sc->kms_cursor_error_count++;
		nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
		    "cursor update");
		return;
	}
	if (state->crtc->primary != plane)
		return;

	crtc_state = state->crtc->state;
	if (crtc_state == NULL || !crtc_state->active)
		return;
	if (drm_atomic_crtc_needs_modeset(crtc_state)) {
		/*
		 * drm_atomic_helper_commit_tail() commits planes before
		 * modeset enables.  Full modesets are programmed from
		 * crtc atomic_enable(); this hook is for same-mode primary
		 * plane flips and console restore only.
		 */
		return;
	}

	nc = to_nvkm_crtc(state->crtc);
	if (nc->sc->disp == NULL)
		return;

	nvkm_kms_crtc_atom_init(&atom, nc->sc, state->crtc);
	err = nvkm_kms_crtc_atom_route(&atom, NULL,
	    crtc_state->connector_mask, false);
	if (err != 0) {
		nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
		    "plane route");
		return;
	}

	nc->sc->kms_plane_update_count++;
	err = nvkm_dispnv50_plane_update(nc->sc, state->crtc, nc->win,
	    atom.display_id);
	nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
	    "plane update");
}

static void
nvkm_plane_atomic_disable(struct drm_plane *plane,
    struct drm_plane_state *old_state)
{
	struct nvkm_crtc *nc;
	int err;

	if (old_state == NULL || old_state->crtc == NULL)
		return;
	if (plane->type == DRM_PLANE_TYPE_CURSOR) {
		nc = to_nvkm_crtc(old_state->crtc);
		if (nc->sc->disp == NULL)
			return;
		nc->sc->kms_cursor_disable_count++;
		err = nvkm_dispnv50_cursor_disable(nc->sc, nc->head);
		if (err != 0)
			nc->sc->kms_cursor_error_count++;
		nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
		    "cursor disable");
		return;
	}
	if (old_state->crtc->primary != plane)
		return;

	nc = to_nvkm_crtc(old_state->crtc);
	if (nc->sc->disp == NULL)
		return;

	nc->sc->kms_plane_disable_count++;
	err = nvkm_dispnv50_plane_disable(nc->sc, nc->win);
	nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
	    "plane disable");
}

/*
 * nvkm_plane_prepare_fb()
 *
 * Ownership:
 *   Borrows the plane state and framebuffer references owned by the atomic
 *   helper.  A successful call creates one scanout pin record on the backing
 *   BO; cleanup_fb owns and consumes that record.
 *
 * Lifetime:
 *   The framebuffer's GEM reference keeps the BO alive from prepare_fb until
 *   cleanup_fb.  The scanout pin keeps TTM from evicting the allocation while
 *   the display engine can read it.
 *
 * Threading:
 *   Runs from atomic commit preparation and may sleep while waiting on
 *   reservation fences or reserving TTM.  Explicit IN_FENCE_FD must override
 *   implicit BO reservation waits, matching drm_atomic_set_fence_for_plane()
 *   semantics.  It must not be called from IRQ.
 */
static int
nvkm_plane_prepare_fb(struct drm_plane *plane,
    struct drm_plane_state *state)
{
	struct nvkm_softc *sc = plane->dev->dev_private;
	struct drm_gem_object *obj;
	struct nvkm_bo *bo;
	bool cursor;
	int ret;

	if (state == NULL || state->fb == NULL)
		return (0);

	sc->kms_prepare_fb_count++;
	cursor = plane->type == DRM_PLANE_TYPE_CURSOR;
	obj = drm_gem_fb_get_obj(state->fb, 0);
	if (obj == NULL) {
		/*
		 * Driver-internal light_up uses a bare framebuffer with no GEM
		 * object.  The real scanout BO for that path is owned by the
		 * CRTC enable hook, so there is no user BO to wait on or pin.
		 */
		return (0);
	}

	bo = to_nvkm_bo(obj);

	ret = drm_gem_fb_prepare_fb(plane, state);
	if (ret != 0) {
		sc->kms_prepare_fb_error_count++;
		return (ret);
	}

	if (state->fence == NULL) {
		ret = nvkm_bo_resv_wait(bo, false, true, false);
		if (ret != 0) {
			sc->kms_prepare_fb_error_count++;
			return (ret);
		}
	}

	ret = nvkm_bo_scanout_pin(bo);
	if (ret != 0) {
		sc->kms_prepare_fb_error_count++;
		return (ret < 0 ? ret : -ret);
	}
	sc->kms_scanout_pin_count++;
	if (cursor)
		sc->kms_cursor_pin_count++;

	return (0);
}

/*
 * nvkm_plane_cleanup_fb()
 *
 * Ownership:
 *   Consumes one scanout pin record created by nvkm_plane_prepare_fb().
 *
 * Lifetime:
 *   old_state->fb still owns the GEM reference while cleanup_fb runs.
 *
 * Threading:
 *   Runs from atomic helper cleanup and may sleep in TTM reservation code.
 */
static void
nvkm_plane_cleanup_fb(struct drm_plane *plane,
    struct drm_plane_state *old_state)
{
	struct nvkm_softc *sc = plane->dev->dev_private;
	struct drm_gem_object *obj;
	bool cursor;
	int ret;

	if (old_state == NULL || old_state->fb == NULL)
		return;

	sc->kms_cleanup_fb_count++;
	cursor = plane->type == DRM_PLANE_TYPE_CURSOR;
	obj = drm_gem_fb_get_obj(old_state->fb, 0);
	if (obj == NULL) {
		/* Matches the internal light_up framebuffer handled above. */
		return;
	}
	ret = nvkm_bo_scanout_unpin(to_nvkm_bo(obj));
	if (ret == 0) {
		sc->kms_scanout_unpin_count++;
		if (cursor)
			sc->kms_cursor_unpin_count++;
	} else {
		sc->kms_prepare_fb_error_count++;
	}
}

static int
nvkm_plane_atomic_async_check(struct drm_plane *plane,
    struct drm_plane_state *state)
{
	struct drm_plane_state *old_state;

	if (plane->type != DRM_PLANE_TYPE_CURSOR || state == NULL)
		return (-EINVAL);

	old_state = plane->state;
	if (old_state == NULL || old_state->crtc != state->crtc ||
	    old_state->fb != state->fb)
		return (-EINVAL);
	if (!old_state->visible || !state->visible)
		return (-EINVAL);
	if (old_state->src_x != state->src_x ||
	    old_state->src_y != state->src_y ||
	    old_state->src_w != state->src_w ||
	    old_state->src_h != state->src_h ||
	    old_state->crtc_w != state->crtc_w ||
	    old_state->crtc_h != state->crtc_h)
		return (-EINVAL);

	return (nvkm_cursor_atomic_check(plane, state));
}

static void
nvkm_plane_atomic_async_update(struct drm_plane *plane,
    struct drm_plane_state *new_state)
{
	struct drm_plane_state *state = plane->state;
	struct nvkm_crtc *nc;
	int err;

	if (plane->type != DRM_PLANE_TYPE_CURSOR || state == NULL ||
	    new_state == NULL || new_state->crtc == NULL)
		return;

	nc = to_nvkm_crtc(new_state->crtc);
	if (nc->sc->disp == NULL)
		return;

	nc->sc->kms_cursor_async_update_count++;
	err = nvkm_dispnv50_cursor_async_update(nc->sc, new_state->crtc,
	    nc->head, new_state->crtc_x, new_state->crtc_y);
	if (err != 0) {
		nc->sc->kms_cursor_error_count++;
		nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
		    "cursor async update");
		return;
	}

	state->crtc_x = new_state->crtc_x;
	state->crtc_y = new_state->crtc_y;
	state->src_x = new_state->src_x;
	state->src_y = new_state->src_y;
}

static const struct drm_plane_helper_funcs nvkm_plane_helper_funcs = {
	.atomic_check	= nvkm_plane_atomic_check,
	.prepare_fb	= nvkm_plane_prepare_fb,
	.cleanup_fb	= nvkm_plane_cleanup_fb,
	.atomic_update	= nvkm_plane_atomic_update,
	.atomic_disable	= nvkm_plane_atomic_disable,
	.atomic_async_check = nvkm_plane_atomic_async_check,
	.atomic_async_update = nvkm_plane_atomic_async_update,
};

static void
nvkm_plane_destroy(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
	kfree(plane);
}

static const struct drm_plane_funcs nvkm_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= nvkm_plane_destroy,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
	.format_mod_supported	= nvkm_plane_format_mod_supported,
};

/* ===== crtc (NVC57D HEAD): real atomic modeset ===== */

#define NVKM_KMS_LEGACY_GAMMA_SIZE	256
#define NVKM_KMS_COLOR_LUT_SIZE		1024

static int
nvkm_crtc_check_lut_size(const struct drm_property_blob *blob,
    unsigned int expected_size)
{
	unsigned int size;

	if (blob == NULL)
		return (0);
	size = drm_color_lut_size(blob);
	if (size != expected_size && size != NVKM_KMS_LEGACY_GAMMA_SIZE)
		return (-EINVAL);
	return (0);
}

/*
 * Validate a CRTC timing mode and fill the derived crtc_* fields.
 *
 * Ownership:
 *   The caller owns @mode. This helper only borrows it for the duration of the
 *   call and updates the derived timing fields in place.
 *
 * Lifetime:
 *   No pointer is retained after return. Atomic check passes the live adjusted
 *   mode; GETCONNECTOR mode probing passes a stack copy through
 *   nvkm_crtc_mode_valid().
 *
 * Threading:
 *   Runs under DRM modeset/probe locking and touches only caller-owned mode
 *   storage. It does not acquire nvkm locks or program hardware.
 */
static enum drm_mode_status
nvkm_crtc_validate_timing(struct drm_display_mode *mode)
{
	drm_mode_set_crtcinfo(mode,
	    CRTC_INTERLACE_HALVE_V | CRTC_STEREO_DOUBLE);

	if (mode->crtc_clock <= 0)
		return (MODE_CLOCK_LOW);
	if (mode->crtc_clock > (int)(0x7fffffffu / 1000u))
		return (MODE_CLOCK_HIGH);
	if (mode->crtc_hdisplay == 0 || mode->crtc_htotal == 0)
		return (MODE_H_ILLEGAL);
	if (mode->crtc_vdisplay == 0 || mode->crtc_vtotal == 0)
		return (MODE_V_ILLEGAL);
	if (mode->crtc_hsync_end <= mode->crtc_hsync_start)
		return (MODE_H_ILLEGAL);
	if (mode->crtc_vsync_end <= mode->crtc_vsync_start)
		return (MODE_V_ILLEGAL);
	if (mode->crtc_hblank_end <= mode->crtc_hsync_start)
		return (MODE_H_ILLEGAL);
	if (mode->crtc_vblank_end <= mode->crtc_vsync_start)
		return (MODE_V_ILLEGAL);
	if (mode->crtc_hdisplay > 0xffff || mode->crtc_htotal > 0xffff)
		return (MODE_BAD_HVALUE);
	if (mode->crtc_vdisplay > 0xffff || mode->crtc_vtotal > 0xffff)
		return (MODE_BAD_VVALUE);
	return (MODE_OK);
}

static enum drm_mode_status
nvkm_crtc_mode_valid(struct drm_crtc *crtc,
    const struct drm_display_mode *mode)
{
	struct drm_display_mode crtc_mode = *mode;

	(void)crtc;
	return (nvkm_crtc_validate_timing(&crtc_mode));
}

static int
nvkm_crtc_atomic_check(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
	struct drm_display_mode *mode = &state->adjusted_mode;
	enum drm_mode_status mode_status;
	int ret;

	if (state->color_mgmt_changed) {
		nc->sc->kms_color_check_count++;
		ret = nvkm_crtc_check_lut_size(state->degamma_lut,
		    NVKM_KMS_COLOR_LUT_SIZE);
		if (ret != 0)
			goto color_fail;
		ret = nvkm_crtc_check_lut_size(state->gamma_lut,
		    NVKM_KMS_COLOR_LUT_SIZE);
		if (ret != 0)
			goto color_fail;
	}
	if (!state->enable)
		return (0);

	mode_status = nvkm_crtc_validate_timing(mode);
	if (mode_status != MODE_OK)
		return (-EINVAL);
	return (0);

color_fail:
	nc->sc->kms_color_reject_count++;
	return (ret);
}

static void
nvkm_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
	int color_err;

	(void)old_state;	/* UPDATE is sequenced in atomic_enable. */

	if (crtc->state->active && crtc->state->color_mgmt_changed &&
	    !crtc->state->mode_changed) {
		color_err = nvkm_dispnv50_color_update(nc->sc, crtc,
		    nc->head, nc->win);
		nvkm_kms_record_result(nc->sc, nc->head, nc->win, color_err,
		    "crtc color");
	}

	if (crtc->state->event == NULL)
		return;
	if (drm_atomic_crtc_needs_modeset(crtc->state))
		return;
	nvkm_crtc_complete_event(crtc, crtc->state);
}

static void
nvkm_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
	struct nvkm_softc *sc = nc->sc;
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct nvkm_kms_crtc_atom atom;
	struct nvkm_dispnv50_output_prepare *route;
	uint32_t display_id = 0;
	uint8_t output_type = 0;
	int err;

	if (sc->disp == NULL)
		return;

	route = old_state != NULL ?
	    nvkm_atomic_prepared_route(old_state->state, nc->head) : NULL;
	if (route != NULL && route->valid) {
		display_id = route->display_id;
		output_type = route->output_type;
		err = nvkm_dispnv50_atomic_enable_prepared(sc, crtc, nc->win,
		    route);
	} else {
		nvkm_kms_crtc_atom_init(&atom, sc, crtc);
		err = nvkm_kms_crtc_atom_route(&atom, NULL,
		    crtc->state->connector_mask, true);
		if (err != 0) {
			nvkm_kms_record_result(sc, nc->head, nc->win, err,
			    "crtc route");
			return;
		}
		display_id = atom.display_id;
		output_type = atom.output.output_type;
		err = nvkm_dispnv50_atomic_enable(sc, crtc, nc->head, nc->win,
		    atom.display_id, &atom.head);
	}

	nvkm_kms_record_result(sc, nc->head, nc->win, err, "crtc enable");
	if (err != 0 && output_type == DCB_OUTPUT_DP)
		nvkm_drm_kms_link_status_bad_schedule(sc, display_id);
	if (err == 0 && crtc->cursor != NULL && crtc->cursor->state != NULL &&
	    crtc->cursor->state->visible && crtc->cursor->state->fb != NULL) {
		int cursor_err;

		sc->kms_cursor_update_count++;
		cursor_err = nvkm_dispnv50_cursor_update(sc, crtc, nc->head);
		if (cursor_err != 0)
			sc->kms_cursor_error_count++;
		nvkm_kms_record_result(sc, nc->head, nc->win, cursor_err,
		    "cursor enable");
	}
	if (err == 0)
		drm_crtc_vblank_on(crtc);
	nvkm_infof(sc->dev,
	    "drm: crtc enable head=%u win=%u %ux%u display=0x%x bridge=%d\n",
	    nc->head, nc->win, mode->hdisplay, mode->vdisplay,
	    display_id, err);
}

static uint32_t
nvkm_crtc_old_display_id(struct drm_crtc *crtc,
    const struct drm_crtc_state *old_state)
{
	struct drm_connector *conn;

	if (crtc == NULL || old_state == NULL)
		return (0);

	list_for_each_entry(conn, &crtc->dev->mode_config.connector_list, head) {
		if ((old_state->connector_mask & drm_connector_mask(conn)) == 0)
			continue;
		return (to_nvkm_connector(conn)->display_id);
	}

	return (0);
}

static void
nvkm_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
	uint32_t display_id;
	int plane_err;
	int err;

	display_id = nvkm_crtc_old_display_id(crtc, old_state);
	plane_err = nvkm_crtc_disable_planes(nc, old_state);
	nvkm_kms_record_result(nc->sc, nc->head, nc->win, plane_err,
	    "crtc disable planes");
	err = nvkm_dispnv50_atomic_disable(nc->sc, nc->head, display_id);
	nvkm_kms_record_result(nc->sc, nc->head, nc->win, err,
	    "crtc disable");
	if (err == 0) {
		nc->sc->kms_atomic_disable_vblank_off_count++;
		drm_crtc_vblank_off(crtc);
	} else {
		nc->sc->kms_atomic_disable_vblank_keep_count++;
		nc->sc->kms_atomic_disable_vblank_keep_head = nc->head;
		nc->sc->kms_atomic_disable_vblank_keep_error = err;
		nvkm_infof(nc->sc->dev,
		    "drm: keep vblank on after failed crtc disable "
		    "head=%u err=%d\n", nc->head, err);
	}
	nvkm_infof(nc->sc->dev,
	    "drm: crtc disable head=%u display=0x%x bridge=%d\n", nc->head,
	    display_id, err);
}

static int
nvkm_crtc_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb,
    struct drm_pending_vblank_event *event, uint32_t flags,
    struct drm_modeset_acquire_ctx *ctx)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
	int ret;

	nc->sc->kms_page_flip_count++;
	if (event != NULL)
		nc->sc->kms_page_flip_event_count++;
	if ((flags & (DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_TARGET)) !=
	    0) {
		nc->sc->kms_page_flip_error_count++;
		return (-EINVAL);
	}
	ret = drm_atomic_helper_page_flip(crtc, fb, event, flags, ctx);
	if (ret != 0)
		nc->sc->kms_page_flip_error_count++;
	return (ret);
}

static const struct drm_crtc_helper_funcs nvkm_crtc_helper_funcs = {
	.mode_valid	= nvkm_crtc_mode_valid,
	.atomic_check	= nvkm_crtc_atomic_check,
	.atomic_flush	= nvkm_crtc_atomic_flush,
	.atomic_enable	= nvkm_crtc_atomic_enable,
	.atomic_disable	= nvkm_crtc_atomic_disable,
};

static void
nvkm_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
	kfree(to_nvkm_crtc(crtc));
}

static int
nvkm_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
	uint32_t en = 0x611d80 + nc->head * 4;

	/* r535_head_vblank_get(): clear pending and enable delivery. */
	nvkm_wr32(nc->sc, 0x611800 + nc->head * 4, 0x00000002);
	nvkm_wr32(nc->sc, en, nvkm_rd32(nc->sc, en) | 0x00000002u);
	return (0);
}

static void
nvkm_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
	uint32_t en = 0x611d80 + nc->head * 4;

	nvkm_wr32(nc->sc, en, nvkm_rd32(nc->sc, en) & ~0x00000002u);
}

static const struct drm_crtc_funcs nvkm_crtc_funcs = {
	.enable_vblank		= nvkm_crtc_enable_vblank,
	.disable_vblank		= nvkm_crtc_disable_vblank,
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= nvkm_crtc_page_flip,
	.gamma_set		= drm_atomic_helper_legacy_gamma_set,
	.destroy		= nvkm_crtc_destroy,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
};

/* ===== encoder: SOR ===== */

static void
nvkm_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
	kfree(encoder);
}

static const struct drm_encoder_funcs nvkm_encoder_funcs = {
	.destroy = nvkm_encoder_destroy,
};

/* ===== init ===== */

static void
nvkm_drm_kms_task(void *arg, int pending)
{
	struct nvkm_softc *sc = arg;
	int ret;

	(void)pending;
	if (sc == NULL || sc->drm_dev == NULL)
		return;

	ret = nvkm_drm_kms_light_up(sc);
	nvkm_infof(sc->dev, "drm: auto KMS commit -> %d\n", ret);
}

int
nvkm_drm_kms_schedule(struct nvkm_softc *sc, const char *reason)
{
	int ret;

	if (sc == NULL || !sc->kms_task_initialized || sc->drm_dev == NULL)
		return (ENODEV);

	if (reason != NULL && strcmp(reason, "hotplug") == 0)
		sc->kms_hotplug_count++;
	else
		sc->kms_auto_count++;

	nvkm_infof(sc->dev, "drm: schedule auto KMS (%s)\n",
	    reason != NULL ? reason : "unspecified");
	ret = taskqueue_enqueue(taskqueue_thread[0], &sc->kms_task);
	return (ret);
}

static bool
nvkm_drm_kms_has_master(struct drm_device *dev)
{
	bool has_master;

	if (dev == NULL)
		return (false);

	mutex_lock(&dev->master_mutex);
	has_master = dev->master != NULL;
	mutex_unlock(&dev->master_mutex);
	return (has_master);
}

/*
 * Publish a failed DP link through the standard KMS link-status property.
 *
 * Ownership:
 *   Borrows sc, dev, and the connector list for this task invocation. DRM owns
 *   the connector states; this helper only updates their standard link-status
 *   scalar through the DRM connector API.
 *
 * Lifetime:
 *   Runs from the KMS HPD task after atomic_enable queued the display mask.
 *   KMS teardown cancels and drains this same task before releasing connector
 *   objects, so no connector pointer escapes this call.
 *
 * Threading:
 *   Must run in process context with no modeset locks held. This matches
 *   drm_kms_helper_hotplug_event() and avoids calling it from atomic_enable(),
 *   where blocking commits may still hold modeset locks.
 */
static void
nvkm_drm_kms_link_status_bad_task(struct nvkm_softc *sc,
    struct drm_device *dev, uint32_t link_bad_mask)
{
	struct drm_connector *conn;
	bool changed = false;

	if (sc == NULL || dev == NULL || link_bad_mask == 0)
		return;

	list_for_each_entry(conn, &dev->mode_config.connector_list, head) {
		struct nvkm_drm_connector *nvkm_conn = to_nvkm_connector(conn);

		if ((link_bad_mask & nvkm_conn->display_id) == 0)
			continue;

		drm_connector_set_link_status_property(conn,
		    DRM_LINK_STATUS_BAD);
		changed = true;
		nvkm_infof(sc->dev,
		    "drm: connector %s display=0x%x link-status=Bad\n",
		    conn->name, nvkm_conn->display_id);
	}

	if (!changed)
		return;

	sc->kms_link_status_bad_count++;
	sc->kms_hotplug_count++;
	drm_kms_helper_hotplug_event(dev);
}

/*
 * Retry a live DP link after an HPD IRQ reports failed channel EQ.
 *
 * Ownership:
 *   Borrows sc, dev, and the current display route for this HPD task
 *   invocation. The bridge owns the nvkm_outp/IOR objects and the saved DP
 *   link-training parameters; this helper only asks it to retrain the current
 *   active route and returns a scalar link_ok result to the caller.
 *
 * Lifetime:
 *   No connector, output, IOR, AUX, or DRM state pointer escapes this call.
 *   KMS teardown unregisters DP IRQ notifications before draining HPD work, so
 *   no new recovery task can race connector destruction.
 *
 * Threading:
 *   Runs in process context from the HPD task. It takes all DRM modeset locks
 *   to serialize runtime retraining against atomic commits, then drops them
 *   before the caller can publish link-status=Bad or send hotplug events.
 */
static int
nvkm_drm_kms_dp_retrain_task(struct nvkm_softc *sc, struct drm_device *dev,
    uint32_t display_id, bool *link_ok)
{
	int ret;

	if (link_ok == NULL)
		return (-EINVAL);
	*link_ok = false;
	if (sc == NULL || dev == NULL || display_id == 0)
		return (-ENODEV);

	drm_modeset_lock_all(dev);
	ret = nvkm_dispnv50_dp_retrain_current(sc, display_id, link_ok);
	drm_modeset_unlock_all(dev);
	return (ret);
}

static uint32_t
nvkm_drm_kms_dp_irq_task(struct nvkm_softc *sc, struct drm_device *dev,
    uint32_t dp_irq_mask)
{
	struct drm_connector *conn;
	uint32_t link_bad_mask = 0;

	if (sc == NULL || dev == NULL || dp_irq_mask == 0)
		return (0);

	list_for_each_entry(conn, &dev->mode_config.connector_list, head) {
		struct nvkm_drm_connector *nvkm_conn = to_nvkm_connector(conn);
		bool link_ok = true;
		int ret;

		if ((dp_irq_mask & nvkm_conn->display_id) == 0)
			continue;

		sc->kms_dp_irq_count++;
		ret = nvkm_dispnv50_dp_link_check(sc, nvkm_conn->display_id,
		    &link_ok);
		if (ret != 0) {
			sc->kms_dp_irq_error_count++;
			link_bad_mask |= nvkm_conn->display_id;
			nvkm_infof(sc->dev,
			    "drm: connector %s display=0x%x DP IRQ link check"
			    " failed err=%d\n", conn->name,
			    nvkm_conn->display_id, ret);
			continue;
		}
		if (!link_ok) {
			sc->kms_dp_irq_link_bad_count++;
			sc->kms_dp_irq_retrain_count++;
			ret = nvkm_drm_kms_dp_retrain_task(sc, dev,
			    nvkm_conn->display_id, &link_ok);
			if (ret == 0 && link_ok) {
				sc->kms_dp_irq_retrain_ok_count++;
				nvkm_infof(sc->dev,
				    "drm: connector %s display=0x%x DP IRQ"
				    " retrain restored link\n", conn->name,
				    nvkm_conn->display_id);
				continue;
			}

			sc->kms_dp_irq_retrain_fail_count++;
			link_bad_mask |= nvkm_conn->display_id;
			if (ret != 0) {
				nvkm_infof(sc->dev,
				    "drm: connector %s display=0x%x DP IRQ"
				    " retrain failed err=%d\n", conn->name,
				    nvkm_conn->display_id, ret);
			} else {
				nvkm_infof(sc->dev,
				    "drm: connector %s display=0x%x DP IRQ"
				    " link still bad after retrain\n",
				    conn->name, nvkm_conn->display_id);
			}
			continue;
		}

		sc->kms_dp_irq_link_good_count++;
		nvkm_infof(sc->dev,
		    "drm: connector %s display=0x%x DP IRQ link good\n",
		    conn->name, nvkm_conn->display_id);
	}

	return (link_bad_mask);
}

static void
nvkm_drm_kms_hpd_task(void *arg, int pending)
{
	struct nvkm_softc *sc = arg;
	struct drm_device *dev;
	uint32_t plug_mask;
	uint32_t unplug_mask;
	uint32_t link_bad_mask;
	uint32_t dp_irq_mask;
	bool changed;
	int ret;

	(void)pending;
	if (sc == NULL || sc->drm_dev == NULL)
		return;
	dev = sc->drm_dev;

	spin_lock(&sc->kms_hpd_lock);
	plug_mask = sc->kms_hpd_pending_plug_mask;
	unplug_mask = sc->kms_hpd_pending_unplug_mask;
	link_bad_mask = sc->kms_hpd_pending_link_bad_mask;
	dp_irq_mask = sc->kms_hpd_pending_dp_irq_mask;
	sc->kms_hpd_pending_plug_mask = 0;
	sc->kms_hpd_pending_unplug_mask = 0;
	sc->kms_hpd_pending_link_bad_mask = 0;
	sc->kms_hpd_pending_dp_irq_mask = 0;
	sc->kms_hpd_last_plug_mask = plug_mask;
	sc->kms_hpd_last_unplug_mask = unplug_mask;
	sc->kms_hpd_last_link_bad_mask = link_bad_mask;
	sc->kms_hpd_last_dp_irq_mask = dp_irq_mask;
	spin_unlock(&sc->kms_hpd_lock);

	link_bad_mask |= nvkm_drm_kms_dp_irq_task(sc, dev, dp_irq_mask);
	sc->kms_hpd_last_link_bad_mask = link_bad_mask;
	nvkm_drm_kms_link_status_bad_task(sc, dev, link_bad_mask);

	if ((plug_mask | unplug_mask) == 0)
		return;

	sc->kms_hotplug_count++;
	changed = drm_helper_hpd_irq_event(dev);
	if (changed)
		sc->kms_hotplug_changed_count++;
	else {
		sc->kms_hotplug_nochange_count++;
		/*
		 * RM reports plug/unplug as an explicit display event.  The
		 * generic helper only sends a userspace event when the coarse
		 * connected/disconnected status changes, but a same-connector
		 * sink swap can leave that status unchanged while EDID and mode
		 * capabilities changed.  Detection has already run, so notify
		 * userspace to re-enumerate the connector.
		 */
		drm_kms_helper_hotplug_event(dev);
	}

	if (nvkm_drm_kms_has_master(dev)) {
		sc->kms_hotplug_notify_only_count++;
		nvkm_infof(sc->dev,
		    "drm: HPD plug=0x%08x unplug=0x%08x changed=%d"
		    " notified=1 master=1\n", plug_mask, unplug_mask,
		    changed);
		return;
	}

	if (!changed) {
		nvkm_infof(sc->dev,
		    "drm: HPD plug=0x%08x unplug=0x%08x changed=0"
		    " notified=1 master=0\n",
		    plug_mask, unplug_mask);
		return;
	}

	sc->kms_hotplug_auto_kms_count++;
	ret = nvkm_drm_kms_schedule(sc, "hotplug-auto");
	if (ret != 0) {
		sc->kms_hotplug_enqueue_error_count++;
		nvkm_infof(sc->dev,
		    "drm: HPD auto KMS enqueue failed plug=0x%08x "
		    "unplug=0x%08x err=%d\n", plug_mask, unplug_mask, ret);
	}
}

static void
nvkm_drm_kms_link_status_bad_schedule(struct nvkm_softc *sc,
    uint32_t display_id)
{
	int ret;

	if (sc == NULL || sc->drm_dev == NULL ||
	    !sc->kms_hpd_task_initialized || display_id == 0)
		return;

	spin_lock(&sc->kms_hpd_lock);
	sc->kms_hpd_pending_link_bad_mask |= display_id;
	spin_unlock(&sc->kms_hpd_lock);

	ret = taskqueue_enqueue(taskqueue_thread[0], &sc->kms_hpd_task);
	if (ret != 0)
		sc->kms_hotplug_enqueue_error_count++;
}

static void
nvkm_drm_kms_dp_irq_schedule(struct nvkm_softc *sc, uint32_t display_id)
{
	int ret;

	if (sc == NULL || sc->drm_dev == NULL ||
	    !sc->kms_hpd_task_initialized || display_id == 0)
		return;

	spin_lock(&sc->kms_hpd_lock);
	sc->kms_hpd_pending_dp_irq_mask |= display_id;
	spin_unlock(&sc->kms_hpd_lock);

	ret = taskqueue_enqueue(taskqueue_thread[0], &sc->kms_hpd_task);
	if (ret != 0)
		sc->kms_hotplug_enqueue_error_count++;
}

void
nvkm_drm_kms_hpd_schedule(struct nvkm_softc *sc, uint32_t plug_mask,
    uint32_t unplug_mask)
{
	int ret;

	if (sc == NULL || sc->drm_dev == NULL ||
	    !sc->kms_hpd_task_initialized ||
	    (plug_mask | unplug_mask) == 0)
		return;

	spin_lock(&sc->kms_hpd_lock);
	sc->kms_hpd_pending_plug_mask |= plug_mask;
	sc->kms_hpd_pending_unplug_mask |= unplug_mask;
	spin_unlock(&sc->kms_hpd_lock);

	ret = taskqueue_enqueue(taskqueue_thread[0], &sc->kms_hpd_task);
	if (ret != 0)
		sc->kms_hotplug_enqueue_error_count++;
}

static void
nvkm_drm_kms_dp_irq_unregister_all(struct nvkm_softc *sc)
{
	struct drm_connector *conn;

	if (sc == NULL || sc->drm_dev == NULL)
		return;

	list_for_each_entry(conn, &sc->drm_dev->mode_config.connector_list,
	    head)
		nvkm_connector_dp_irq_unregister(to_nvkm_connector(conn));
}

void
nvkm_drm_kms_fini(struct nvkm_softc *sc)
{
	if (sc == NULL)
		return;

	if (sc->kms_task_initialized) {
		sc->kms_task_initialized = false;
		while (taskqueue_cancel(taskqueue_thread[0], &sc->kms_task,
		    NULL) != 0)
			taskqueue_drain(taskqueue_thread[0], &sc->kms_task);
		taskqueue_drain(taskqueue_thread[0], &sc->kms_task);
	}

	/*
	 * Ownership:
	 *   Borrows sc->drm_dev and all mode_config objects. DRM still owns the
	 *   CRTC/plane/connector state; this function only asks the atomic
	 *   helper to transition them to disabled state.
	 *
	 * Lifetime:
	 *   Called after drm_dev_unregister() has unpublished the device, or from
	 *   register-error cleanup before drm_dev_put() drops the final device
	 *   reference. The DragonFly-local auto-KMS worker has been stopped so it
	 *   cannot queue light_up while shutdown disables planes, heads, and
	 *   outputs. HPD/DP IRQ notification and polling are still registered
	 *   until shutdown returns, matching nouveau's display_fini ordering: the
	 *   display hardware is first disabled through the normal atomic path,
	 *   then event producers are blocked and drained.
	 *
	 * Threading:
	 *   Runs from device teardown context and may sleep in atomic commit,
	 *   fence waits, and display notifier waits. The auto-KMS task may also
	 *   take modeset locks, so it is cancelled before this point. HPD work is
	 *   process-context work and is drained after shutdown, before connector
	 *   objects are released by mode_config cleanup.
	 */
	if (sc->drm_dev != NULL &&
	    sc->drm_dev->mode_config.funcs == &nvkm_mode_config_funcs)
		drm_atomic_helper_shutdown(sc->drm_dev);

	nvkm_drm_kms_dp_irq_unregister_all(sc);
	if (sc->kms_hpd_task_initialized) {
		sc->kms_hpd_task_initialized = false;
		while (taskqueue_cancel(taskqueue_thread[0], &sc->kms_hpd_task,
		    NULL) != 0)
			taskqueue_drain(taskqueue_thread[0], &sc->kms_hpd_task);
		taskqueue_drain(taskqueue_thread[0], &sc->kms_hpd_task);
	}
	if (sc->drm_dev != NULL && sc->drm_dev->mode_config.poll_enabled)
		drm_kms_helper_poll_fini(sc->drm_dev);
}

int
nvkm_drm_kms_init(struct drm_device *dev, struct nvkm_softc *sc)
{
	uint32_t crtc_mask;
	uint32_t supported_mask;
	int ret;
	int id, h, nheads, count = 0;

	/*
	 * nvkm advertises DRIVER_MODESET while the imported display path is
	 * still being staged.  drm_dev_register() will always call
	 * drm_modeset_register_all(), so these lists must be initialised even
	 * when this attach exposes an empty KMS configuration.
	 */
	drm_mode_config_init(dev);
	dev->mode_config.min_width = 1;
	dev->mode_config.min_height = 1;
	dev->mode_config.max_width = 16384;
	dev->mode_config.max_height = 16384;
	dev->mode_config.cursor_width = 256;
	dev->mode_config.cursor_height = 256;
	dev->mode_config.preferred_depth = 24;
	dev->mode_config.prefer_shadow = 1;
	dev->mode_config.quirk_addfb_prefer_xbgr_30bpp = true;
	dev->mode_config.normalize_zpos = true;
	dev->mode_config.allow_fb_modifiers = true;
	dev->mode_config.funcs = &nvkm_mode_config_funcs;
	dev->mode_config.helper_private = &nvkm_mode_config_helper_funcs;

	if (sc->disp == NULL)
		return (0);		/* no display subsystem; render-only */
	supported_mask = nvkm_gsp_disp_supported_mask(sc);
	if (supported_mask == 0)
		return (0);
	ret = nvkm_connector_properties_init(dev, sc);
	if (ret != 0)
		return (ret);
	if (!sc->kms_task_initialized) {
		TASK_INIT(&sc->kms_task, 0, nvkm_drm_kms_task, sc);
		sc->kms_task_initialized = true;
	}
	if (!sc->kms_hpd_task_initialized) {
		TASK_INIT(&sc->kms_hpd_task, 0, nvkm_drm_kms_hpd_task, sc);
		sc->kms_hpd_task_initialized = true;
	}

	/* (1) One CRTC (HEAD) + primary plane (window) per head. The plane's
	 * possible_crtcs is BIT(h) because CRTCs get index h in creation order. */
	nheads = nvkm_kms_effective_head_count(sc);
	for (h = 0; h < nheads; h++) {
		struct drm_plane *plane;
		struct drm_plane *cursor;
		struct nvkm_crtc *ncrtc;
		struct drm_crtc *crtc;

		plane = kzalloc(sizeof(*plane), GFP_KERNEL);
		cursor = kzalloc(sizeof(*cursor), GFP_KERNEL);
		ncrtc = kzalloc(sizeof(*ncrtc), GFP_KERNEL);
		if (plane == NULL || cursor == NULL || ncrtc == NULL) {
			kfree(plane);
			kfree(cursor);
			kfree(ncrtc);
			break;
		}
		ncrtc->sc = sc;
		ncrtc->head = (uint32_t)h;	/* HEAD index == creation order */
		ncrtc->win = (uint32_t)h;	/* one primary window per head */
		crtc = &ncrtc->base;
		if (drm_universal_plane_init(dev, plane, 1u << h,
		    &nvkm_plane_funcs, nvkm_plane_formats,
		    nitems(nvkm_plane_formats), wndwc57e_modifiers,
			    DRM_PLANE_TYPE_PRIMARY, NULL) != 0) {
			kfree(plane);
			kfree(cursor);
			kfree(ncrtc);
			continue;
		}
		drm_plane_helper_add(plane, &nvkm_plane_helper_funcs);
		if (drm_universal_plane_init(dev, cursor, 1u << h,
		    &nvkm_plane_funcs, nvkm_cursor_formats,
		    nitems(nvkm_cursor_formats), nvkm_cursor_modifiers,
		    DRM_PLANE_TYPE_CURSOR, NULL) != 0) {
			drm_plane_cleanup(plane);
			kfree(plane);
			kfree(cursor);
			kfree(ncrtc);
			continue;
		}
		drm_plane_helper_add(cursor, &nvkm_plane_helper_funcs);
		if (drm_crtc_init_with_planes(dev, crtc, plane, cursor,
		    &nvkm_crtc_funcs, NULL) != 0) {
			drm_plane_cleanup(plane);
			drm_plane_cleanup(cursor);
			kfree(plane);
			kfree(cursor);
			kfree(ncrtc);
			continue;
		}
		drm_crtc_helper_add(crtc, &nvkm_crtc_helper_funcs);
		if (drm_mode_crtc_set_gamma_size(crtc,
		    NVKM_KMS_LEGACY_GAMMA_SIZE) != 0) {
			drm_crtc_cleanup(crtc);
			drm_plane_cleanup(plane);
			drm_plane_cleanup(cursor);
			kfree(plane);
			kfree(cursor);
			kfree(ncrtc);
			continue;
		}
		drm_crtc_enable_color_mgmt(crtc, NVKM_KMS_COLOR_LUT_SIZE,
		    true, NVKM_KMS_COLOR_LUT_SIZE);
		if (h < NVKM_DISPLAY_MAX_HEADS)
			sc->kms_crtc[h] = crtc;
	}
	crtc_mask = (1u << nheads) - 1u;
	if (nheads > 0) {
		dev->vblank_disable_immediate = true;
		drm_vblank_init(dev, nheads);
		/* No drm_irq_install(); our GSP IRQ (nvkm_pci.c) drives
		 * drm_crtc_handle_vblank, so advertise vblank as available. */
		dev->irq_enabled = true;
	}

	/* (2) One connector + encoder per supported displayId. Connector and
	 * encoder type are projected from GSP/RM outp/conn capability data. */
	for (id = 0; id < 32; id++) {
		struct nvkm_gsp_disp_output_info info;
		struct nvkm_drm_connector *nc;
		struct drm_encoder *enc;
		uint32_t display_id;
		uint32_t possible_crtcs;
		int connector_type;
		int encoder_type;

		display_id = (1u << id);
		if (!(supported_mask & display_id))
			continue;
		nc = kzalloc(sizeof(*nc), GFP_KERNEL);
		enc = kzalloc(sizeof(*enc), GFP_KERNEL);
		if (nc == NULL || enc == NULL) {
			kfree(nc);
			kfree(enc);
			continue;
		}
		nc->sc = sc;
		nc->display_id = display_id;
		ret = nvkm_gsp_disp_output_info(sc, display_id, &info);
		if (ret != 0) {
			nvkm_infof(sc->dev,
			    "drm: skip display=0x%x: missing output info err=%d\n",
			    display_id, ret);
			kfree(nc);
			kfree(enc);
			continue;
		}
		connector_type = nvkm_connector_type_from_info(&info);
		encoder_type = nvkm_encoder_type_from_info(&info);
		possible_crtcs = nvkm_possible_crtcs_from_info(&info, crtc_mask);
		if (possible_crtcs == 0) {
			nvkm_infof(sc->dev,
			    "drm: skip display=0x%x: heads=0x%x outside "
			    "crtc_mask=0x%x\n",
			    display_id, info.heads, crtc_mask);
			kfree(nc);
			kfree(enc);
			continue;
		}

		ret = drm_connector_init(dev, &nc->base, &nvkm_connector_funcs,
		    connector_type);
		if (ret != 0) {
			nvkm_infof(sc->dev,
			    "drm: skip display=0x%x: connector init failed err=%d\n",
			    display_id, ret);
			kfree(nc);
			kfree(enc);
			continue;
		}
		nvkm_connector_init_mode_caps(&nc->base, &info, connector_type);
		ret = drm_encoder_init(dev, enc, &nvkm_encoder_funcs,
		    encoder_type, NULL);
		if (ret != 0) {
			nvkm_infof(sc->dev,
			    "drm: skip display=0x%x: encoder init failed err=%d\n",
			    display_id, ret);
			drm_connector_cleanup(&nc->base);
			kfree(nc);
			kfree(enc);
			continue;
		}
		enc->possible_crtcs = possible_crtcs;
		ret = drm_connector_attach_encoder(&nc->base, enc);
		if (ret != 0) {
			nvkm_infof(sc->dev,
			    "drm: skip display=0x%x: encoder attach failed err=%d\n",
			    display_id, ret);
			drm_encoder_cleanup(enc);
			drm_connector_cleanup(&nc->base);
			kfree(nc);
			kfree(enc);
			continue;
		}
		nc->base.polled = DRM_CONNECTOR_POLL_HPD;
		drm_connector_helper_add(&nc->base,
		    &nvkm_connector_helper_funcs);
		nvkm_connector_attach_properties(nc);
		nvkm_connector_dp_irq_register(nc, &info);
		nvkm_infof(sc->dev,
		    "drm: display=0x%x connector=%d encoder=%d heads=0x%x"
		    " type=0x%02x conn=0x%02x mst_capable=%d"
		    " dp_interlace=%d\n",
		    display_id, connector_type, encoder_type, possible_crtcs,
		    info.output_type, info.connector_type, info.mst_capable,
		    info.dp_interlace_capable);
		count++;
	}

	drm_mode_config_reset(dev);
	if (count > 0 && !dev->mode_config.poll_enabled)
		drm_kms_helper_poll_init(dev);
	nvkm_infof(sc->dev,
	    "drm: KMS init -- %d head(s), %d connector(s)\n", nheads, count);
	return (0);
}

/* ===== internal framebuffer (light_up only; no GEM backing) ===== */

static int
nvkm_fb_create_handle(struct drm_framebuffer *fb, struct drm_file *file,
    unsigned int *handle)
{
	(void)fb; (void)file; (void)handle;
	return (-ENODEV);		/* internal fb: no userspace handle */
}

static void
nvkm_fb_destroy(struct drm_framebuffer *fb)
{
	drm_framebuffer_cleanup(fb);
	kfree(fb);
}

static const struct drm_framebuffer_funcs nvkm_internal_fb_funcs = {
	.destroy	= nvkm_fb_destroy,
	.create_handle	= nvkm_fb_create_handle,
};

/* A bare drm_framebuffer to satisfy the plane state during light_up. The
 * actual scanout surface is the disp test fb (disp->fb_paddr) pushed by the
 * crtc atomic_enable hook, so this object needs no memory backing. */
static struct drm_framebuffer *
nvkm_internal_fb(struct drm_device *dev, uint32_t w, uint32_t h)
{
	struct drm_mode_fb_cmd2 cmd;
	struct drm_framebuffer *fb;

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (fb == NULL)
		return (NULL);
	memset(&cmd, 0, sizeof(cmd));
	cmd.width = w;
	cmd.height = h;
	cmd.pixel_format = DRM_FORMAT_XRGB8888;
	cmd.pitches[0] = w * 4u;
	drm_helper_mode_fill_fb_struct(dev, fb, &cmd);
	if (drm_framebuffer_init(dev, fb, &nvkm_internal_fb_funcs) != 0) {
		kfree(fb);
		return (NULL);
	}
	return (fb);
}

/*
 * Disable the current console/display scanout when no connected output can be
 * selected for light_up().
 *
 * Ownership:
 *   Borrows sc->drm_dev and the current mode_config state. The DRM atomic
 *   helper owns the temporary state it creates and consumes it before return.
 *
 * Lifetime:
 *   No connector, CRTC, plane, or framebuffer pointer escapes this call. Any
 *   active scanout BO is released later by the normal atomic cleanup path.
 *
 * Threading:
 *   Runs from process context, normally the no-master HPD/console worker. It
 *   may sleep while taking modeset locks, waiting for fences, and committing
 *   the disable state.
 */
static int
nvkm_drm_kms_dark_down(struct nvkm_softc *sc)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_device *dev;
	int ret;

	if (sc == NULL || sc->drm_dev == NULL)
		return (ENODEV);
	dev = sc->drm_dev;
	sc->kms_dark_down_count++;

	drm_modeset_acquire_init(&ctx, 0);
retry:
	ret = drm_atomic_helper_disable_all(dev, &ctx);
	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	if (ret != 0) {
		sc->kms_dark_down_error_count++;
		sc->kms_dark_down_last_error = ret;
	} else {
		sc->kms_dark_down_last_error = 0;
	}
	nvkm_infof(sc->dev, "drm: dark_down commit -> %d\n", ret);
	return (ret < 0 ? -ret : 0);
}

/* ===== light_up: driver-internal atomic modeset =====
 * Picks the first connected output + its preferred mode and drives a full
 * atomic commit, which fans out to nvkm_crtc_atomic_enable -> the real EVO
 * modeset path. Triggered from a sysctl so it is repeatable. */
int
nvkm_drm_kms_light_up(struct nvkm_softc *sc)
{
	struct drm_device *dev = sc->drm_dev;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_connector *conn = NULL, *c;
	struct drm_crtc *crtc = NULL, *cc;
	struct drm_connector_state *cstate;
	struct drm_crtc_state *crtc_state;
	struct drm_plane_state *pstate;
	struct drm_display_mode *mode = NULL;
	struct drm_framebuffer *fb;
	bool force_modeset;
	int ret;

	nvkm_infof(sc->dev, "drm: light_up entry drm_dev=%p\n", dev);
	if (dev == NULL)
		return (ENODEV);

	/* Probe connectors; pick the first connected one that has a mode. */
	mutex_lock(&dev->mode_config.mutex);
	list_for_each_entry(c, &dev->mode_config.connector_list, head) {
		c->funcs->fill_modes(c, 8192, 8192);
		if (!list_empty(&c->modes)) {
			conn = c;
			mode = list_first_entry(&c->modes,
			    struct drm_display_mode, head);
			break;
		}
	}
	mutex_unlock(&dev->mode_config.mutex);
	if (conn == NULL || mode == NULL) {
		nvkm_infof(sc->dev, "drm: light_up -- no connected mode\n");
		return (nvkm_drm_kms_dark_down(sc));
	}

	list_for_each_entry(cc, &dev->mode_config.crtc_list, head) {
		crtc = cc;
		break;
	}
	if (crtc == NULL)
		return (ENXIO);

	force_modeset = crtc->state == NULL || !crtc->state->active ||
	    !drm_mode_equal(&crtc->state->mode, mode);

	fb = nvkm_internal_fb(dev, mode->hdisplay, mode->vdisplay);
	if (fb == NULL)
		return (ENOMEM);

	nvkm_infof(sc->dev,
	    "drm: light_up -- mode %ux%u on crtc %u full_modeset=%d\n",
	    mode->hdisplay, mode->vdisplay, drm_crtc_index(crtc),
	    force_modeset);

	drm_modeset_acquire_init(&ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE);
	state = drm_atomic_state_alloc(dev);
	if (state == NULL) {
		drm_modeset_acquire_fini(&ctx);
		drm_framebuffer_remove(fb);
		return (ENOMEM);
	}
	state->acquire_ctx = &ctx;
retry:
	cstate = drm_atomic_get_connector_state(state, conn);
	if (IS_ERR(cstate)) { ret = PTR_ERR(cstate); goto out; }
	ret = drm_atomic_set_crtc_for_connector(cstate, crtc);
	if (ret != 0)
		goto out;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) { ret = PTR_ERR(crtc_state); goto out; }
	ret = drm_atomic_set_mode_for_crtc(crtc_state, mode);
	if (ret != 0)
		goto out;
	crtc_state->active = true;
	if (force_modeset) {
		crtc_state->mode_changed = true;
		crtc_state->connectors_changed = true;
		crtc_state->active_changed = true;
	}

	pstate = drm_atomic_get_plane_state(state, crtc->primary);
	if (IS_ERR(pstate)) { ret = PTR_ERR(pstate); goto out; }
	ret = drm_atomic_set_crtc_for_plane(pstate, crtc);
	if (ret != 0)
		goto out;
	drm_atomic_set_fb_for_plane(pstate, fb);
	pstate->crtc_x = 0;
	pstate->crtc_y = 0;
	pstate->crtc_w = mode->hdisplay;
	pstate->crtc_h = mode->vdisplay;
	pstate->src_x = 0;
	pstate->src_y = 0;
	pstate->src_w = (uint32_t)mode->hdisplay << 16;
	pstate->src_h = (uint32_t)mode->vdisplay << 16;

	ret = drm_atomic_commit(state);
out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	if (ret != 0)
		drm_framebuffer_remove(fb);
	nvkm_infof(sc->dev, "drm: light_up commit -> %d\n", ret);
	return (ret < 0 ? -ret : 0);
}
