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
#include <drm/drm_rect.h>

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
	return (ERR_PTR(-ENOSYS));	/* framebuffer import is staged later */
}

static const struct drm_mode_config_funcs nvkm_mode_config_funcs = {
	.fb_create	= nvkm_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= drm_atomic_helper_commit,
};

/* ===== plane: NVC57E window validation ===== */

static const uint32_t nvkm_plane_formats[] = {
	DRM_FORMAT_C8,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_ARGB2101010,
#ifdef DRM_FORMAT_XBGR16161616F
	DRM_FORMAT_XBGR16161616F,
#endif
#ifdef DRM_FORMAT_ABGR16161616F
	DRM_FORMAT_ABGR16161616F,
#endif
};

static const uint64_t nvkm_plane_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static bool
nvkm_plane_format_mod_supported(struct drm_plane *plane, uint32_t format,
    uint64_t modifier)
{
	unsigned int i;

	(void)plane;
	if (modifier != DRM_FORMAT_MOD_LINEAR &&
	    modifier != DRM_FORMAT_MOD_INVALID)
		return (false);
	for (i = 0; i < nitems(nvkm_plane_formats); i++) {
		if (nvkm_plane_formats[i] == format)
			return (true);
	}
	return (false);
}

static int
nvkm_plane_atomic_check(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct drm_framebuffer *fb;
	int ret;

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
	if ((fb->pitches[0] & 0x3fu) != 0)
		return (-EINVAL);
	return (0);
}

static void
nvkm_plane_atomic_update(struct drm_plane *plane,
    struct drm_plane_state *old_state)
{
	(void)plane; (void)old_state;	/* programming is staged in dispnv50 */
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
	.format_mod_supported	= nvkm_plane_format_mod_supported,
};

/* ===== crtc (NVC57D HEAD): real atomic modeset ===== */

struct nvkm_crtc {
	struct drm_crtc		base;
	struct nvkm_softc	*sc;
	uint32_t		head;	/* HEAD index */
	uint32_t		win;	/* primary window index */
};

#define to_nvkm_crtc(c) container_of(c, struct nvkm_crtc, base)


static int
nvkm_crtc_atomic_check(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	struct drm_display_mode *mode = &state->adjusted_mode;

	(void)crtc;
	if (!state->enable)
		return (0);

	drm_mode_set_crtcinfo(mode,
	    CRTC_INTERLACE_HALVE_V | CRTC_STEREO_DOUBLE);

	if (mode->crtc_clock <= 0 ||
	    mode->crtc_clock > (int)(0x7fffffffu / 1000u))
		return (-EINVAL);
	if (mode->crtc_htotal == 0 || mode->crtc_vtotal == 0 ||
	    mode->crtc_hdisplay == 0 || mode->crtc_vdisplay == 0)
		return (-EINVAL);
	if (mode->crtc_hsync_end <= mode->crtc_hsync_start ||
	    mode->crtc_vsync_end <= mode->crtc_vsync_start)
		return (-EINVAL);
	if (mode->crtc_hblank_end <= mode->crtc_hsync_start ||
	    mode->crtc_vblank_end <= mode->crtc_vsync_start)
		return (-EINVAL);
	if (mode->crtc_htotal > 0xffff || mode->crtc_vtotal > 0xffff ||
	    mode->crtc_hdisplay > 0xffff || mode->crtc_vdisplay > 0xffff)
		return (-EINVAL);
	return (0);
}

static void
nvkm_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	(void)crtc; (void)old_state;	/* UPDATE is sequenced in atomic_enable. */
}

static void
nvkm_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct nvkm_crtc *nc = to_nvkm_crtc(crtc);
	struct nvkm_softc *sc = nc->sc;
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct drm_connector *conn = NULL;
	struct nvkm_dispnv50_hdmi_info hdmi;
	uint32_t display_id = 0;
	int err;

	(void)old_state;
	if (sc->disp == NULL)
		return;
	memset(&hdmi, 0, sizeof(hdmi));

	/* Find the output routed to this head in the committed state. */
	list_for_each_entry(conn, &crtc->dev->mode_config.connector_list, head) {
		if (conn->state != NULL && conn->state->crtc == crtc) {
			display_id = to_nvkm_connector(conn)->display_id;
			hdmi.has_infoframe =
			    conn->display_info.has_hdmi_infoframe;
			hdmi.scdc_supported =
			    conn->display_info.hdmi.scdc.supported;
			hdmi.scdc_scrambling =
			    conn->display_info.hdmi.scdc.scrambling.supported;
			hdmi.scdc_low_rates =
			    conn->display_info.hdmi.scdc.scrambling.low_rates;
			break;
		}
	}
	if (display_id == 0) {
		nvkm_infof(sc->dev,
		    "drm: crtc enable head=%u: no connector routed\n", nc->head);
		return;
	}

	err = nvkm_dispnv50_atomic_enable(sc, crtc, nc->head, nc->win,
	    display_id, &hdmi);
	nvkm_infof(sc->dev,
	    "drm: crtc enable head=%u win=%u %ux%u display=0x%x bridge=%d\n",
	    nc->head, nc->win, mode->hdisplay, mode->vdisplay, display_id, err);
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

void
nvkm_drm_kms_fini(struct nvkm_softc *sc)
{
	if (sc == NULL || !sc->kms_task_initialized)
		return;

	while (taskqueue_cancel(taskqueue_thread[0], &sc->kms_task, NULL) != 0)
		taskqueue_drain(taskqueue_thread[0], &sc->kms_task);
	taskqueue_drain(taskqueue_thread[0], &sc->kms_task);
	sc->kms_task_initialized = false;
}

int
nvkm_drm_kms_init(struct drm_device *dev, struct nvkm_softc *sc)
{
	uint32_t supported_mask;
	int id, h, nheads, crtc_mask, count = 0;

	/*
	 * nvkm advertises DRIVER_MODESET while the imported display path is
	 * still being staged.  drm_dev_register() will always call
	 * drm_modeset_register_all(), so these lists must be initialised even
	 * when this attach exposes an empty KMS configuration.
	 */
	drm_mode_config_init(dev);
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = 8192;
	dev->mode_config.max_height = 8192;
	dev->mode_config.funcs = &nvkm_mode_config_funcs;

	if (sc->disp == NULL)
		return (0);		/* no display subsystem; render-only */
	supported_mask = nvkm_gsp_disp_supported_mask(sc);
	if (supported_mask == 0)
		return (0);
	if (!sc->kms_task_initialized) {
		TASK_INIT(&sc->kms_task, 0, nvkm_drm_kms_task, sc);
		sc->kms_task_initialized = true;
	}

	/* (1) One CRTC (HEAD) + primary plane (window) per head. The plane's
	 * possible_crtcs is BIT(h) because CRTCs get index h in creation order. */
	nheads = nvkm_gsp_disp_head_count(sc);
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
		    nitems(nvkm_plane_formats), nvkm_plane_modifiers,
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

			if (!(supported_mask & (1u << id)))
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
	return (ret < 0 ? -ret : 0);
}
