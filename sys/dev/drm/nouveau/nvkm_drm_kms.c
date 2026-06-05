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
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>	/* drm_atomic_set_{crtc,mode,fb}_for_* */
#include <drm/drm_crtc_helper.h>	/* drm_helper_probe_single_connector_modes */
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modeset_helper.h>	/* drm_helper_mode_fill_fb_struct */
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

/* ===== crtc (NVC57D HEAD): real atomic modeset ===== */

struct nvkm_crtc {
	struct drm_crtc		base;
	struct nvkm_softc	*sc;
	uint32_t		head;	/* HEAD index */
	uint32_t		win;	/* primary window index */
};

#define to_nvkm_crtc(c) container_of(c, struct nvkm_crtc, base)

/* drm_display_mode -> HW timing, nouveau headc57d convention. Verified to
 * reproduce the former hardcoded 1080p values exactly. */
static void
nvkm_kms_mode_to_disp(const struct drm_display_mode *mode,
    struct nvkm_disp_mode *m)
{
	uint32_t ha, hse, hbe, hbs, va, vse, vbe, vbs;

	ha  = mode->crtc_htotal;
	hse = mode->crtc_hsync_end - mode->crtc_hsync_start - 1u;
	hbe = mode->crtc_hblank_end - mode->crtc_hsync_start - 1u;
	hbs = hbe + mode->crtc_hdisplay;
	va  = mode->crtc_vtotal;
	vse = mode->crtc_vsync_end - mode->crtc_vsync_start - 1u;
	vbe = mode->crtc_vblank_end - mode->crtc_vsync_start - 1u;
	vbs = vbe + mode->crtc_vdisplay;

	m->raster = ha | (va << 16);
	m->sync   = hse | (vse << 16);
	m->blanke = hbe | (vbe << 16);
	m->blanks = hbs | (vbs << 16);
	m->blank2 = 0x00000001u;			/* progressive */
	m->clk    = (uint32_t)mode->crtc_clock * 1000u;	/* kHz -> Hz */
	m->iw = m->ow = mode->crtc_hdisplay;
	m->ih = m->oh = mode->crtc_vdisplay;
}

static int
nvkm_crtc_atomic_check(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	(void)crtc; (void)state;
	return (0);
}

static void
nvkm_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	(void)crtc; (void)old_state;	/* UPDATE issued inside head/window_set */
}

static void
nvkm_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
	struct nvkm_softc *sc = nc->sc;
	struct nvkm_gsp_disp *disp = sc->gsp_disp;
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct drm_connector *conn;
	struct nvkm_disp_mode m;
	uint32_t display_id = 0, orid = 0, proto = 0;

	(void)old_state;
	if (disp == NULL)
		return;

	/* Find the output routed to this head in the committed state. */
	list_for_each_entry(conn, &crtc->dev->mode_config.connector_list, head) {
		if (conn->state != NULL && conn->state->crtc == crtc) {
			display_id = to_nvkm_connector(conn)->display_id;
			break;
		}
	}
	if (display_id == 0) {
		nvkm_infof(sc->dev,
		    "drm: crtc enable head=%u: no connector routed\n", nc->head);
		return;
	}

	nvkm_kms_mode_to_disp(mode, &m);
	(void)nvkm_gsp_disp_modeset_setup(sc);
	(void)nvkm_gsp_disp_sor_enable(sc, display_id, &orid, &proto);
	(void)nvkm_gsp_disp_head_set(sc, nc->head, nc->win, orid, proto,
	    display_id, &m);
	(void)nvkm_gsp_disp_window_set(sc, nc->win, nc->head, disp->fb_paddr,
	    m.iw * 4u, m.iw, m.ih);

	nvkm_infof(sc->dev,
	    "drm: crtc enable head=%u %ux%u display=0x%x or=%u proto=0x%x\n",
	    nc->head, m.iw, m.ih, display_id, orid, proto);
}

static void
nvkm_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);

	(void)old_state;
	nvkm_infof(nc->sc->dev, "drm: crtc disable head=%u\n", nc->head);
	/* Head blank lands in a later milestone; leave timing latched. */
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
	kfree(to_nvkm_crtc(crtc));
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
		struct nvkm_crtc *ncrtc;
		struct drm_crtc *crtc;

		plane = kzalloc(sizeof(*plane), GFP_KERNEL);
		ncrtc = kzalloc(sizeof(*ncrtc), GFP_KERNEL);
		if (plane == NULL || ncrtc == NULL) {
			kfree(plane);
			kfree(ncrtc);
			break;
		}
		ncrtc->sc = sc;
		ncrtc->head = (uint32_t)h;	/* HEAD index == creation order */
		ncrtc->win = (uint32_t)h;	/* one primary window per head */
		crtc = &ncrtc->base;
		if (drm_universal_plane_init(dev, plane, 1u << h,
		    &nvkm_plane_funcs, nvkm_plane_formats,
		    nitems(nvkm_plane_formats), NULL,
		    DRM_PLANE_TYPE_PRIMARY, NULL) != 0) {
			kfree(plane);
			kfree(ncrtc);
			continue;
		}
		drm_plane_helper_add(plane, &nvkm_plane_helper_funcs);
		if (drm_crtc_init_with_planes(dev, crtc, plane, NULL,
		    &nvkm_crtc_funcs, NULL) != 0) {
			drm_plane_cleanup(plane);
			kfree(plane);
			kfree(ncrtc);
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
		return (ENXIO);
	}

	list_for_each_entry(cc, &dev->mode_config.crtc_list, head) {
		crtc = cc;
		break;
	}
	if (crtc == NULL)
		return (ENXIO);

	fb = nvkm_internal_fb(dev, mode->hdisplay, mode->vdisplay);
	if (fb == NULL)
		return (ENOMEM);

	nvkm_infof(sc->dev, "drm: light_up -- mode %ux%u on crtc %u\n",
	    mode->hdisplay, mode->vdisplay, drm_crtc_index(crtc));

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
	if (ret == 0)
		nvkm_gsp_disp_dump_state(sc);
	return (ret < 0 ? -ret : 0);
}
