/*
 * nvkm_drm_kms.c -- DRM/KMS skeleton (Phase 2, M3a).
 *
 * Register the driver as a KMS (DRIVER_MODESET) device, init mode_config, and
 * create a connector per supported GSP displayId so userspace can enumerate
 * the attached display and its modes (drmModeGetConnector / modetest). detect()
 * and get_modes() proxy to the GSP display subsystem (connect-state + EDID).
 *
 * The full atomic pipeline (CRTC/plane/encoder + commit) lands in M3b/M4; for
 * M3a these are bare connectors using the standard DRM atomic helpers.
 *
 * dfly's DRM provides the entire KMS/atomic framework (~Linux 4.x); we only
 * implement the driver-specific connector hooks here.
 */
#include "nvkm_priv.h"
#include "nvkm_gsp_rm.h"

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>	/* drm_helper_probe_single_connector_modes */
#include <drm/drm_edid.h>

#include <linux/slab.h>

struct nvkm_drm_connector {
	struct drm_connector	base;
	struct nvkm_softc	*sc;
	uint32_t		display_id;	/* GSP displayId, e.g. 0x400 */
};

#define to_nvkm_connector(c) container_of(c, struct nvkm_drm_connector, base)

/* ===== connector helper funcs ===== */

static int
nvkm_connector_get_modes(struct drm_connector *connector)
{
	struct nvkm_drm_connector *nc = to_nvkm_connector(connector);
	uint8_t *buf;
	uint32_t len = 2048;
	int n = 0;

	buf = kzalloc(2048, GFP_KERNEL);
	if (buf == NULL)
		return (0);
	if (nvkm_gsp_disp_read_edid(nc->sc, nc->display_id, buf, &len) == 0) {
		drm_connector_update_edid_property(connector,
		    (struct edid *)buf);
		n = drm_add_edid_modes(connector, (struct edid *)buf);
	}
	kfree(buf);
	return (n);
}

static const struct drm_connector_helper_funcs nvkm_connector_helper_funcs = {
	.get_modes = nvkm_connector_get_modes,
};

/* ===== connector funcs ===== */

static enum drm_connector_status
nvkm_connector_detect(struct drm_connector *connector, bool force)
{
	struct nvkm_drm_connector *nc = to_nvkm_connector(connector);

	(void)force;
	return (nvkm_gsp_disp_connected(nc->sc, nc->display_id) > 0) ?
	    connector_status_connected : connector_status_disconnected;
}

static void
nvkm_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
	kfree(to_nvkm_connector(connector));
}

static const struct drm_connector_funcs nvkm_connector_funcs = {
	.reset			= drm_atomic_helper_connector_reset,
	.detect			= nvkm_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= nvkm_connector_destroy,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

/* ===== mode_config funcs ===== */

static struct drm_framebuffer *
nvkm_fb_create(struct drm_device *dev, struct drm_file *file,
    const struct drm_mode_fb_cmd2 *cmd)
{
	(void)dev; (void)file; (void)cmd;
	return (ERR_PTR(-ENOSYS));	/* scanout framebuffers land in M4 */
}

static const struct drm_mode_config_funcs nvkm_mode_config_funcs = {
	.fb_create	= nvkm_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= drm_atomic_helper_commit,
};

/* ===== init ===== */

int
nvkm_drm_kms_init(struct drm_device *dev, struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	int id, count = 0;

	if (disp == NULL)
		return (0);		/* no display subsystem; render-only */

	drm_mode_config_init(dev);
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = 8192;
	dev->mode_config.max_height = 8192;
	dev->mode_config.funcs = &nvkm_mode_config_funcs;

	for (id = 0; id < 32; id++) {
		struct nvkm_drm_connector *nc;

		if (!(disp->supported_mask & (1u << id)))
			continue;
		nc = kzalloc(sizeof(*nc), GFP_KERNEL);
		if (nc == NULL)
			continue;
		nc->sc = sc;
		nc->display_id = (1u << id);
		drm_connector_init(dev, &nc->base, &nvkm_connector_funcs,
		    DRM_MODE_CONNECTOR_HDMIA);
		drm_connector_helper_add(&nc->base,
		    &nvkm_connector_helper_funcs);
		count++;
	}

	drm_mode_config_reset(dev);
	nvkm_infof(sc->dev, "drm: KMS init -- %d connector(s)\n", count);
	return (0);
}
