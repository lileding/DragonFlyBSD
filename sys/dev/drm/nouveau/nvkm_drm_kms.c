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
#include <drm/drm_fourcc.h>
#include <drm/drm_plane_helper.h>

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

/* ===== plane (M3b skeleton: NVC57E window; EVO push lands in M4) ===== */

static const uint32_t nvkm_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static int
nvkm_plane_atomic_check(struct drm_plane *plane, struct drm_plane_state *state)
{
	(void)plane; (void)state;
	return (0);			/* M4: validate against window caps */
}

static void
nvkm_plane_atomic_update(struct drm_plane *plane,
    struct drm_plane_state *old_state)
{
	(void)plane; (void)old_state;	/* M4: push NVC57E SET_* methods */
}

static const struct drm_plane_helper_funcs nvkm_plane_helper_funcs = {
	.atomic_check	= nvkm_plane_atomic_check,
	.atomic_update	= nvkm_plane_atomic_update,
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
};

/* ===== crtc (M3b skeleton: NVC57D HEAD) ===== */

static int
nvkm_crtc_atomic_check(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	(void)crtc; (void)state;
	return (0);			/* M4: validate timing against the head */
}

static void
nvkm_crtc_atomic_flush(struct drm_crtc *crtc,
    struct drm_crtc_state *old_state)
{
	(void)crtc; (void)old_state;	/* M4: kick the core channel update */
}

static void
nvkm_crtc_atomic_enable(struct drm_crtc *crtc,
    struct drm_crtc_state *old_state)
{
	(void)crtc; (void)old_state;	/* M4: program the head timing */
}

static void
nvkm_crtc_atomic_disable(struct drm_crtc *crtc,
    struct drm_crtc_state *old_state)
{
	(void)crtc; (void)old_state;	/* M4: blank the head */
}

static const struct drm_crtc_helper_funcs nvkm_crtc_helper_funcs = {
	.atomic_check	= nvkm_crtc_atomic_check,
	.atomic_flush	= nvkm_crtc_atomic_flush,
	.atomic_enable	= nvkm_crtc_atomic_enable,
	.atomic_disable	= nvkm_crtc_atomic_disable,
};

static void
nvkm_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
	kfree(crtc);
}

static const struct drm_crtc_funcs nvkm_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.destroy		= nvkm_crtc_destroy,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
};

/* ===== encoder (M3b skeleton: SOR) ===== */

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

int
nvkm_drm_kms_init(struct drm_device *dev, struct nvkm_softc *sc)
{
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	int id, h, nheads, crtc_mask, count = 0;

	if (disp == NULL)
		return (0);		/* no display subsystem; render-only */

	drm_mode_config_init(dev);
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = 8192;
	dev->mode_config.max_height = 8192;
	dev->mode_config.funcs = &nvkm_mode_config_funcs;

	/* (1) One CRTC (HEAD) + primary plane (window) per head. The plane's
	 * possible_crtcs is BIT(h) because CRTCs get index h in creation order. */
	nheads = disp->num_heads ? disp->num_heads : 1;
	if (nheads > 4)
		nheads = 4;
	for (h = 0; h < nheads; h++) {
		struct drm_plane *plane;
		struct drm_crtc *crtc;

		plane = kzalloc(sizeof(*plane), GFP_KERNEL);
		crtc = kzalloc(sizeof(*crtc), GFP_KERNEL);
		if (plane == NULL || crtc == NULL) {
			kfree(plane);
			kfree(crtc);
			break;
		}
		if (drm_universal_plane_init(dev, plane, 1u << h,
		    &nvkm_plane_funcs, nvkm_plane_formats,
		    nitems(nvkm_plane_formats), NULL,
		    DRM_PLANE_TYPE_PRIMARY, NULL) != 0) {
			kfree(plane);
			kfree(crtc);
			continue;
		}
		drm_plane_helper_add(plane, &nvkm_plane_helper_funcs);
		if (drm_crtc_init_with_planes(dev, crtc, plane, NULL,
		    &nvkm_crtc_funcs, NULL) != 0) {
			drm_plane_cleanup(plane);
			kfree(plane);
			kfree(crtc);
			continue;
		}
		drm_crtc_helper_add(crtc, &nvkm_crtc_helper_funcs);
	}
	crtc_mask = (1u << nheads) - 1u;

	/* (2) One connector + encoder per supported displayId; any head can
	 * drive any output, so possible_crtcs is all heads. */
	for (id = 0; id < 32; id++) {
		struct nvkm_drm_connector *nc;
		struct drm_encoder *enc;

		if (!(disp->supported_mask & (1u << id)))
			continue;
		nc = kzalloc(sizeof(*nc), GFP_KERNEL);
		enc = kzalloc(sizeof(*enc), GFP_KERNEL);
		if (nc == NULL || enc == NULL) {
			kfree(nc);
			kfree(enc);
			continue;
		}
		nc->sc = sc;
		nc->display_id = (1u << id);
		drm_connector_init(dev, &nc->base, &nvkm_connector_funcs,
		    DRM_MODE_CONNECTOR_HDMIA);
		drm_connector_helper_add(&nc->base,
		    &nvkm_connector_helper_funcs);

		if (drm_encoder_init(dev, enc, &nvkm_encoder_funcs,
		    DRM_MODE_ENCODER_TMDS, NULL) == 0) {
			enc->possible_crtcs = crtc_mask;
			drm_connector_attach_encoder(&nc->base, enc);
		} else {
			kfree(enc);
		}
		count++;
	}

	drm_mode_config_reset(dev);
	nvkm_infof(sc->dev,
	    "drm: KMS init -- %d head(s), %d connector(s)\n", nheads, count);
	return (0);
}
