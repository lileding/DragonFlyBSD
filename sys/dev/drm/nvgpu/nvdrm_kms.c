/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly DRM/KMS shim for the native NVIDIA GPU driver.
 */

#include "nvdrm_kms.h"
#include "nvdrm_file.h"
#include "nvdrm_nouveau_abi.h"
#include "nvgpu_bo.h"
#include "nvgpu_device.h"
#include "nvgpu_debug.h"
#include "nvgpu_display.h"
#include "nvgpu_intr_internal.h"

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_plane_helper.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <machine/framebuffer.h>
#include <sys/spinlock.h>
#include <sys/spinlock2.h>
#include <sys/rman.h>

#define NVDRM_KMS_MAX_HEADS 4u
#define NVDRM_KMS_EDID_SIZE 1024u

#ifndef DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D
#define DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h) \
	fourcc_mod_code(NVIDIA, (0x10ULL | ((h) & 0xfULL) | \
	(((k) & 0xffULL) << 12) | (((g) & 0x3ULL) << 20) | \
	(((s) & 0x1ULL) << 22) | (((s) & 0x6ULL) << 25) | \
	(((c) & 0x7ULL) << 23)))
#endif

struct nvdrm_kms {
	struct nvgpu_device *gpu;
	struct drm_device *ddev;
	struct drm_crtc *crtcs[NVDRM_KMS_MAX_HEADS];
	struct fb_info console_fb;
	struct nvgpu_display_console console;
	struct work_struct console_work;
	struct work_struct hotplug_work;
	struct lwkt_token console_token;
	struct spinlock hotplug_lock;
	uint32_t hotplug_plug_mask;
	uint32_t hotplug_unplug_mask;
	uint32_t hotplug_dp_irq_mask;
	struct drm_property *dither_mode_property;
	struct drm_property *dither_depth_property;
	struct drm_property *max_bpc_property;
	struct drm_property *underscan_property;
	struct drm_property *underscan_hborder_property;
	struct drm_property *underscan_vborder_property;
	uint32_t head_count;
	bool polling;
	bool console_registered;
	bool shutting_down;
	bool hotplug_enabled;
};

struct nvdrm_crtc {
	struct drm_crtc base;
	struct nvdrm_kms *kms;
	uint32_t head;
	uint32_t window;
};

struct nvdrm_connector {
	struct drm_connector base;
	struct nvdrm_kms *kms;
	uint32_t display_id;
	bool has_audio;
	struct drm_display_mode *native_mode;
};

struct nvdrm_connector_state {
	struct drm_connector_state base;
	uint32_t dither_mode;
	uint32_t dither_depth;
	uint32_t max_bpc;
	uint32_t underscan_mode;
	uint32_t underscan_hborder;
	uint32_t underscan_vborder;
};

struct nvdrm_pending_flip {
	struct drm_crtc *crtc;
	struct drm_pending_vblank_event *event;
};

struct nvdrm_color_config {
	struct nvgpu_display_color_config base;
	struct nvgpu_display_color_lut_entry degamma[NVGPU_DISPLAY_COLOR_LUT_SIZE];
	struct nvgpu_display_color_lut_entry gamma[NVGPU_DISPLAY_COLOR_LUT_SIZE];
};

struct nvdrm_atomic_state {
	struct drm_atomic_state base;
	struct nvgpu_display_prepared_output *prepared[NVDRM_KMS_MAX_HEADS];
	struct nvdrm_pending_flip flips[NVDRM_KMS_MAX_HEADS];
	struct nvgpu_display_head_config head_config[NVDRM_KMS_MAX_HEADS];
	bool head_config_valid[NVDRM_KMS_MAX_HEADS];
	bool head_update_view[NVDRM_KMS_MAX_HEADS];
	bool head_update_dither[NVDRM_KMS_MAX_HEADS];
	struct nvdrm_color_config *color[NVDRM_KMS_MAX_HEADS];
	bool color_update[NVDRM_KMS_MAX_HEADS];
};

struct nvdrm_internal_fb {
	struct drm_framebuffer base;
};

#define to_nvdrm_crtc(crtc) container_of(crtc, struct nvdrm_crtc, base)
#define to_nvdrm_connector(connector) \
	container_of(connector, struct nvdrm_connector, base)
#define to_nvdrm_connector_state(state) \
	container_of(state, struct nvdrm_connector_state, base)
#define to_nvdrm_connector_state_const(state) \
	((const struct nvdrm_connector_state *)(state))
#define to_nvdrm_atomic_state(state) \
	container_of(state, struct nvdrm_atomic_state, base)

static struct drm_framebuffer *nvdrm_kms_create_fb(struct drm_device *ddev,
	struct drm_file *file, const struct drm_mode_fb_cmd2 *cmd);
static int nvdrm_kms_check_atomic(struct drm_device *ddev,
	struct drm_atomic_state *state);
static int nvdrm_kms_commit_atomic(struct drm_device *ddev,
	struct drm_atomic_state *state, bool nonblock);
static struct drm_atomic_state *nvdrm_kms_alloc_atomic_state(
	struct drm_device *ddev);
static void nvdrm_kms_clear_atomic_state(struct drm_atomic_state *state);
static void nvdrm_kms_free_atomic_state(struct drm_atomic_state *state);
static void nvdrm_kms_complete_vblank(void *arg, uint32_t head);
static void nvdrm_kms_complete_pageflip(void *arg, uint32_t head,
	void *cookie);
static void nvdrm_kms_commit_tail(struct drm_atomic_state *state);
static void nvdrm_kms_run_console_work(struct work_struct *work);
static void nvdrm_kms_run_hotplug_work(struct work_struct *work);
static void nvdrm_kms_receive_hotplug(void *arg, uint32_t plug_mask,
	uint32_t unplug_mask);
static void nvdrm_kms_receive_dp_irq(void *arg, uint32_t display_id);
static const struct drm_framebuffer_funcs nvdrm_kms_internal_fb_funcs;
static const struct drm_plane_funcs nvdrm_kms_plane_funcs;
static const struct drm_plane_helper_funcs nvdrm_kms_plane_helper_funcs;
static const struct drm_crtc_funcs nvdrm_kms_crtc_funcs;
static const struct drm_crtc_helper_funcs nvdrm_kms_crtc_helper_funcs;
static const struct drm_connector_funcs nvdrm_kms_connector_funcs;
static const struct drm_connector_helper_funcs nvdrm_kms_connector_helper_funcs;
static const struct drm_encoder_funcs nvdrm_kms_encoder_funcs;

static const struct drm_prop_enum_list nvdrm_kms_dither_mode_enum[] = {
	{ NVGPU_DISPLAY_DITHER_MODE_OFF, "off" },
	{ NVGPU_DISPLAY_DITHER_MODE_ON, "on" },
	{ NVGPU_DISPLAY_DITHER_MODE_DYNAMIC_2X2, "dynamic 2x2" },
	{ NVGPU_DISPLAY_DITHER_MODE_STATIC_2X2, "static 2x2" },
	{ NVGPU_DISPLAY_DITHER_MODE_TEMPORAL, "temporal" },
	{ NVGPU_DISPLAY_DITHER_MODE_AUTO, "auto" },
};

static const struct drm_prop_enum_list nvdrm_kms_dither_depth_enum[] = {
	{ NVGPU_DISPLAY_DITHER_DEPTH_6_BPC, "6 bpc" },
	{ NVGPU_DISPLAY_DITHER_DEPTH_8_BPC, "8 bpc" },
	{ NVGPU_DISPLAY_DITHER_DEPTH_AUTO, "auto" },
};

static const struct drm_prop_enum_list nvdrm_kms_underscan_enum[] = {
	{ NVGPU_DISPLAY_UNDERSCAN_OFF, "off" },
	{ NVGPU_DISPLAY_UNDERSCAN_ON, "on" },
	{ NVGPU_DISPLAY_UNDERSCAN_AUTO, "auto" },
};

static const uint32_t nvdrm_kms_primary_formats[] = {
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

static const uint32_t nvdrm_kms_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

static const uint64_t nvdrm_kms_primary_modifiers[] = {
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 0),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 1),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 2),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 3),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 4),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 5),
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static const uint64_t nvdrm_kms_cursor_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static const struct drm_mode_config_funcs nvdrm_kms_mode_config_funcs = {
	.fb_create = nvdrm_kms_create_fb,
	.atomic_check = nvdrm_kms_check_atomic,
	.atomic_commit = nvdrm_kms_commit_atomic,
	.atomic_state_alloc = nvdrm_kms_alloc_atomic_state,
	.atomic_state_clear = nvdrm_kms_clear_atomic_state,
	.atomic_state_free = nvdrm_kms_free_atomic_state,
};

static const struct nvgpu_display_event_ops nvdrm_kms_event_ops = {
	.vblank = nvdrm_kms_complete_vblank,
	.pageflip = nvdrm_kms_complete_pageflip,
	.hotplug = nvdrm_kms_receive_hotplug,
	.dp_irq = nvdrm_kms_receive_dp_irq,
};

static const struct drm_mode_config_helper_funcs nvdrm_kms_helper_funcs = {
	.atomic_commit_tail = nvdrm_kms_commit_tail,
};

int
nvdrm_kms_create_dumb(struct drm_file *file, struct drm_device *ddev,
    struct drm_mode_create_dumb *args)
{
	struct nvgpu_bo_create_args create_args = { 0 };
	struct nvgpu_bo_info info = { 0 };
	struct nvdrm_file *nvfile;
	struct nvgpu_proc *proc;
	uint64_t pitch;
	uint64_t size;
	int error;

	(void)ddev;
	if (file == NULL || args == NULL || args->flags != 0)
		return (-EINVAL);
	nvfile = nvdrm_file_from_drm(file);
	proc = nvdrm_file_get_proc(nvfile);
	if (proc == NULL)
		return (-ENODEV);

	pitch = roundup2((uint64_t)args->width * howmany(args->bpp, 8), 64);
	if (pitch == 0 || pitch > UINT32_MAX ||
	    args->height > UINT64_MAX / pitch)
		return (-EINVAL);
	size = pitch * args->height;
	if (size == 0 || size > UINT64_MAX - PAGE_MASK)
		return (-EINVAL);
	size = roundup2(size, PAGE_SIZE);

	create_args.size = size;
	create_args.domain = NOUVEAU_GEM_DOMAIN_VRAM |
	    NOUVEAU_GEM_DOMAIN_MAPPABLE;
	error = nvgpu_bo_create_handle(proc, file, &create_args, &info);
	if (error != 0)
		return (error > 0 ? -error : error);

	args->handle = info.handle;
	args->pitch = (uint32_t)pitch;
	args->size = info.size;
	return (0);
}

int
nvdrm_kms_get_dumb_map_offset(struct drm_file *file,
    struct drm_device *ddev, uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *obj;
	struct nvgpu_bo *bo;
	int error;

	(void)ddev;
	if (file == NULL || offset == NULL)
		return (-EINVAL);
	obj = drm_gem_object_lookup(file, handle);
	if (obj == NULL)
		return (-ENOENT);
	if (obj->import_attach != NULL) {
		error = -EINVAL;
		goto done;
	}

	bo = nvgpu_bo_from_gem(obj);
	if (!bo->ttm_backed)
		error = -ENODEV;
	else if (!nvgpu_bo_cpu_mappable(bo))
		error = -ENXIO;
	else {
		*offset = nvgpu_bo_get_mmap_handle(bo);
		error = *offset != 0 ? 0 : -ENXIO;
	}

done:
	drm_gem_object_put_unlocked(obj);
	return (error);
}

int
nvdrm_kms_destroy_dumb(struct drm_file *file, struct drm_device *ddev,
    uint32_t handle)
{
	return (drm_gem_dumb_destroy(file, ddev, handle));
}

/* Create all DRM objects without changing scanout state. */
int
nvdrm_kms_init(struct nvgpu_device *gpu)
{
	struct drm_device *ddev = nvgpu_device_get_drm_dev(gpu);
	struct nvdrm_kms *kms;
	uint32_t output_mask;
	uint32_t crtc_mask;
	uint32_t head;
	uint32_t id;
	int error;

	if (ddev == NULL)
		return (ENODEV);
	if (nvgpu_device_get_kms(gpu) != NULL)
		return (0);
	kms = kzalloc(sizeof(*kms), GFP_KERNEL);
	if (kms == NULL)
		return (ENOMEM);
	kms->gpu = gpu;
	kms->ddev = ddev;
	lwkt_token_init(&kms->console_token, "nvdrmconsole");
	spin_init(&kms->hotplug_lock, "nvdrm hpd");
	INIT_WORK(&kms->console_work, nvdrm_kms_run_console_work);
	INIT_WORK(&kms->hotplug_work, nvdrm_kms_run_hotplug_work);
	kms->hotplug_enabled = true;
	kms->head_count = MIN(nvgpu_display_get_head_count(gpu),
	    NVDRM_KMS_MAX_HEADS);
	nvgpu_device_set_kms(gpu, kms);

	drm_mode_config_init(ddev);
	ddev->mode_config.min_width = 1;
	ddev->mode_config.min_height = 1;
	ddev->mode_config.max_width = 16384;
	ddev->mode_config.max_height = 16384;
	ddev->mode_config.cursor_width = 256;
	ddev->mode_config.cursor_height = 256;
	ddev->mode_config.preferred_depth = 24;
	ddev->mode_config.prefer_shadow = 1;
	ddev->mode_config.quirk_addfb_prefer_xbgr_30bpp = true;
	ddev->mode_config.normalize_zpos = true;
	ddev->mode_config.allow_fb_modifiers = true;
	ddev->mode_config.funcs = &nvdrm_kms_mode_config_funcs;
	ddev->mode_config.helper_private = &nvdrm_kms_helper_funcs;
	kms->dither_mode_property = drm_property_create_enum(ddev, 0,
	    "dithering mode", nvdrm_kms_dither_mode_enum,
	    nitems(nvdrm_kms_dither_mode_enum));
	kms->dither_depth_property = drm_property_create_enum(ddev, 0,
	    "dithering depth", nvdrm_kms_dither_depth_enum,
	    nitems(nvdrm_kms_dither_depth_enum));
	kms->max_bpc_property = drm_property_create_range(ddev, 0,
	    "max bpc", 8, 8);
	kms->underscan_property = drm_property_create_enum(ddev, 0,
	    "underscan", nvdrm_kms_underscan_enum,
	    nitems(nvdrm_kms_underscan_enum));
	kms->underscan_hborder_property = drm_property_create_range(ddev, 0,
	    "underscan hborder", 0, 128);
	kms->underscan_vborder_property = drm_property_create_range(ddev, 0,
	    "underscan vborder", 0, 128);
	if (kms->dither_mode_property == NULL ||
	    kms->dither_depth_property == NULL || kms->max_bpc_property == NULL ||
	    kms->underscan_property == NULL ||
	    kms->underscan_hborder_property == NULL ||
	    kms->underscan_vborder_property == NULL) {
		error = ENOMEM;
		goto fail;
	}

	for (head = 0; head < kms->head_count; head++) {
		struct drm_plane *primary;
		struct drm_plane *cursor;
		struct nvdrm_crtc *nvcrtc;

		primary = kzalloc(sizeof(*primary), GFP_KERNEL);
		cursor = kzalloc(sizeof(*cursor), GFP_KERNEL);
		nvcrtc = kzalloc(sizeof(*nvcrtc), GFP_KERNEL);
		if (primary == NULL || cursor == NULL || nvcrtc == NULL) {
			kfree(primary);
			kfree(cursor);
			kfree(nvcrtc);
			error = ENOMEM;
			goto fail;
		}
		nvcrtc->kms = kms;
		nvcrtc->head = head;
		nvcrtc->window = head;
		error = drm_universal_plane_init(ddev, primary, 1u << head,
		    &nvdrm_kms_plane_funcs, nvdrm_kms_primary_formats,
		    nitems(nvdrm_kms_primary_formats), nvdrm_kms_primary_modifiers,
		    DRM_PLANE_TYPE_PRIMARY, NULL);
		if (error != 0) {
			kfree(primary);
			kfree(cursor);
			kfree(nvcrtc);
			goto fail;
		}
		drm_plane_helper_add(primary, &nvdrm_kms_plane_helper_funcs);
		error = drm_universal_plane_init(ddev, cursor, 1u << head,
		    &nvdrm_kms_plane_funcs, nvdrm_kms_cursor_formats,
		    nitems(nvdrm_kms_cursor_formats), nvdrm_kms_cursor_modifiers,
		    DRM_PLANE_TYPE_CURSOR, NULL);
		if (error != 0) {
			drm_plane_cleanup(primary);
			kfree(primary);
			kfree(cursor);
			kfree(nvcrtc);
			goto fail;
		}
		drm_plane_helper_add(cursor, &nvdrm_kms_plane_helper_funcs);
		error = drm_crtc_init_with_planes(ddev, &nvcrtc->base, primary,
		    cursor, &nvdrm_kms_crtc_funcs, NULL);
		if (error != 0) {
			drm_plane_cleanup(primary);
			drm_plane_cleanup(cursor);
			kfree(primary);
			kfree(cursor);
			kfree(nvcrtc);
			goto fail;
		}
		drm_crtc_helper_add(&nvcrtc->base, &nvdrm_kms_crtc_helper_funcs);
		error = drm_mode_crtc_set_gamma_size(&nvcrtc->base, 256);
		if (error != 0) {
			drm_crtc_cleanup(&nvcrtc->base);
			drm_plane_cleanup(primary);
			drm_plane_cleanup(cursor);
			kfree(primary);
			kfree(cursor);
			kfree(nvcrtc);
			goto fail;
		}
		drm_crtc_enable_color_mgmt(&nvcrtc->base,
		    NVGPU_DISPLAY_COLOR_LUT_SIZE, true,
		    NVGPU_DISPLAY_COLOR_LUT_SIZE);
		kms->crtcs[head] = &nvcrtc->base;
	}
	crtc_mask = kms->head_count == 32 ? UINT32_MAX :
	    (1u << kms->head_count) - 1u;
	output_mask = nvgpu_display_get_output_mask(gpu);
	for (id = 0; id < 32; id++) {
		struct nvgpu_display_output_info info;
		struct nvdrm_connector *connector;
		struct drm_encoder *encoder;
		uint32_t display_id = 1u << id;
		int connector_type;
		int encoder_type;

		if ((output_mask & display_id) == 0)
			continue;
		error = nvgpu_display_get_output(gpu, display_id, &info);
		if (error != 0)
			goto fail;
		connector = kzalloc(sizeof(*connector), GFP_KERNEL);
		encoder = kzalloc(sizeof(*encoder), GFP_KERNEL);
		if (connector == NULL || encoder == NULL) {
			kfree(connector);
			kfree(encoder);
			error = ENOMEM;
			goto fail;
		}
		connector->kms = kms;
		connector->display_id = display_id;
		switch (info.connector_type) {
		case NVGPU_DISPLAY_CONNECTOR_VGA:
		case NVGPU_DISPLAY_CONNECTOR_POD_VGA:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_VGA_0:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_VGA_1:
			connector_type = DRM_MODE_CONNECTOR_VGA;
			break;
		case NVGPU_DISPLAY_CONNECTOR_DVI_A:
			connector_type = DRM_MODE_CONNECTOR_DVIA;
			break;
		case NVGPU_DISPLAY_CONNECTOR_DVI_I_TV_0:
		case NVGPU_DISPLAY_CONNECTOR_DVI_I_TV_1:
		case NVGPU_DISPLAY_CONNECTOR_DVI_I_TV_2:
		case NVGPU_DISPLAY_CONNECTOR_DVI_I:
		case NVGPU_DISPLAY_CONNECTOR_DMS59_0:
		case NVGPU_DISPLAY_CONNECTOR_DMS59_1:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_DVI_I_0:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_DVI_I_1:
			connector_type = DRM_MODE_CONNECTOR_DVII;
			break;
		case NVGPU_DISPLAY_CONNECTOR_DVI_D:
		case NVGPU_DISPLAY_CONNECTOR_DVI_ADC:
		case NVGPU_DISPLAY_CONNECTOR_TMDS:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_DVI_D_0:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_DVI_D_1:
			connector_type = DRM_MODE_CONNECTOR_DVID;
			break;
		case NVGPU_DISPLAY_CONNECTOR_LVDS:
		case NVGPU_DISPLAY_CONNECTOR_LVDS_SPWG:
		case NVGPU_DISPLAY_CONNECTOR_LVDS_REM:
		case NVGPU_DISPLAY_CONNECTOR_LVDS_SPWG_REM:
			connector_type = DRM_MODE_CONNECTOR_LVDS;
			break;
		case NVGPU_DISPLAY_CONNECTOR_EDP:
			connector_type = DRM_MODE_CONNECTOR_eDP;
			break;
		case NVGPU_DISPLAY_CONNECTOR_DP:
		case NVGPU_DISPLAY_CONNECTOR_MINI_DP:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_DP_0:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_DP_1:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_MINI_DP_0:
		case NVGPU_DISPLAY_CONNECTOR_DOCK_MINI_DP_1:
		case NVGPU_DISPLAY_CONNECTOR_DMS59_DP_0:
		case NVGPU_DISPLAY_CONNECTOR_DMS59_DP_1:
		case NVGPU_DISPLAY_CONNECTOR_USB_C:
			connector_type = DRM_MODE_CONNECTOR_DisplayPort;
			break;
		case NVGPU_DISPLAY_CONNECTOR_HDMI_C:
			connector_type = DRM_MODE_CONNECTOR_HDMIB;
			break;
		case NVGPU_DISPLAY_CONNECTOR_HDMI_A_0:
		case NVGPU_DISPLAY_CONNECTOR_HDMI_A_1:
			connector_type = DRM_MODE_CONNECTOR_HDMIA;
			break;
		case NVGPU_DISPLAY_CONNECTOR_WFD:
			connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
			break;
		default:
			connector_type = info.protocol == NVGPU_DISPLAY_PROTOCOL_DP ?
			    DRM_MODE_CONNECTOR_DisplayPort : DRM_MODE_CONNECTOR_HDMIA;
			break;
		}
		switch (connector_type) {
		case DRM_MODE_CONNECTOR_VGA:
		case DRM_MODE_CONNECTOR_DVIA:
			encoder_type = DRM_MODE_ENCODER_DAC;
			break;
		case DRM_MODE_CONNECTOR_LVDS:
		case DRM_MODE_CONNECTOR_eDP:
			encoder_type = DRM_MODE_ENCODER_LVDS;
			break;
		case DRM_MODE_CONNECTOR_VIRTUAL:
			encoder_type = DRM_MODE_ENCODER_VIRTUAL;
			break;
		default:
			encoder_type = DRM_MODE_ENCODER_TMDS;
			break;
		}
		error = drm_connector_init(ddev, &connector->base,
		    &nvdrm_kms_connector_funcs,
		    connector_type);
		if (error != 0) {
			kfree(connector);
			kfree(encoder);
			goto fail;
		}
		error = drm_encoder_init(ddev, encoder, &nvdrm_kms_encoder_funcs,
		    encoder_type, NULL);
		if (error != 0) {
			drm_connector_cleanup(&connector->base);
			kfree(connector);
			kfree(encoder);
			goto fail;
		}
		encoder->possible_crtcs = info.possible_heads != 0 ?
		    info.possible_heads & crtc_mask : crtc_mask;
		error = drm_connector_attach_encoder(&connector->base, encoder);
		if (error != 0) {
			drm_encoder_cleanup(encoder);
			drm_connector_cleanup(&connector->base);
			kfree(connector);
			kfree(encoder);
			goto fail;
		}
		connector->base.polled = DRM_CONNECTOR_POLL_HPD;
		connector->base.doublescan_allowed = true;
		connector->base.interlace_allowed =
		    info.protocol == NVGPU_DISPLAY_PROTOCOL_DP &&
		    info.dp_interlace_capable;
		connector->base.stereo_allowed =
		    connector_type == DRM_MODE_CONNECTOR_DisplayPort ||
		    connector_type == DRM_MODE_CONNECTOR_eDP ||
		    connector_type == DRM_MODE_CONNECTOR_HDMIA;
		drm_connector_helper_add(&connector->base,
		    &nvdrm_kms_connector_helper_funcs);
		error = drm_connector_attach_scaling_mode_property(&connector->base,
		    BIT(DRM_MODE_SCALE_NONE) | BIT(DRM_MODE_SCALE_FULLSCREEN) |
		    BIT(DRM_MODE_SCALE_CENTER) | BIT(DRM_MODE_SCALE_ASPECT));
		if (error != 0) {
			drm_encoder_cleanup(encoder);
			drm_connector_cleanup(&connector->base);
			kfree(connector);
			kfree(encoder);
			goto fail;
		}
		drm_object_attach_property(&connector->base.base,
		    kms->dither_mode_property, NVGPU_DISPLAY_DITHER_MODE_AUTO);
		drm_object_attach_property(&connector->base.base,
		    kms->dither_depth_property, NVGPU_DISPLAY_DITHER_DEPTH_AUTO);
		drm_object_attach_property(&connector->base.base,
		    kms->max_bpc_property, 8);
		if (connector_type == DRM_MODE_CONNECTOR_DVID ||
		    connector_type == DRM_MODE_CONNECTOR_DVII ||
		    connector_type == DRM_MODE_CONNECTOR_HDMIA ||
		    connector_type == DRM_MODE_CONNECTOR_DisplayPort) {
			drm_object_attach_property(&connector->base.base,
			    kms->underscan_property, NVGPU_DISPLAY_UNDERSCAN_OFF);
			drm_object_attach_property(&connector->base.base,
			    kms->underscan_hborder_property, 0);
			drm_object_attach_property(&connector->base.base,
			    kms->underscan_vborder_property, 0);
		}
	}
	if (kms->head_count != 0) {
		error = drm_vblank_init(ddev, kms->head_count);
		if (error != 0)
			goto fail;
		ddev->vblank_disable_immediate = true;
		ddev->irq_enabled = true;
	}
	drm_mode_config_reset(ddev);
	if (output_mask != 0) {
		drm_kms_helper_poll_init(ddev);
		kms->polling = true;
	}
	nvgpu_display_set_event_ops(gpu, &nvdrm_kms_event_ops, kms);
	nvgpu_intr_enable_display_dispatch(gpu);
	nvgpu_log(NVGPU_LOG_INFO, "KMS ready heads=%u outputs=0x%x\n",
	    kms->head_count, output_mask);
	return (0);

fail:
	nvdrm_kms_fini(gpu);
	return (error < 0 ? -error : error);
}

/* Stop KMS producers, disable active heads, then destroy DRM objects. */
void
nvdrm_kms_fini(struct nvgpu_device *gpu)
{
	struct nvdrm_kms *kms = nvgpu_device_get_kms(gpu);

	if (kms == NULL)
		return;
	spin_lock(&kms->hotplug_lock);
	kms->hotplug_enabled = false;
	kms->hotplug_plug_mask = 0;
	kms->hotplug_unplug_mask = 0;
	kms->hotplug_dp_irq_mask = 0;
	spin_unlock(&kms->hotplug_lock);
	nvgpu_display_set_event_ops(gpu, NULL, NULL);
	cancel_work_sync(&kms->hotplug_work);
	lwkt_gettoken(&kms->console_token);
	kms->shutting_down = true;
	lwkt_reltoken(&kms->console_token);
	cancel_work_sync(&kms->console_work);
	if (kms->ddev->mode_config.funcs == &nvdrm_kms_mode_config_funcs)
		drm_atomic_helper_shutdown(kms->ddev);
	nvgpu_intr_disable_display_dispatch(gpu);
	if (kms->polling)
		drm_kms_helper_poll_fini(kms->ddev);
	kms->ddev->irq_enabled = false;
	if (kms->console_registered) {
		unregister_framebuffer(&kms->console_fb);
		kms->console_registered = false;
	}
	nvgpu_display_release_console(gpu);
	drm_mode_config_cleanup(kms->ddev);
	nvgpu_device_set_kms(gpu, NULL);
	spin_uninit(&kms->hotplug_lock);
	lwkt_token_uninit(&kms->console_token);
	kfree(kms);
}

static void
nvdrm_kms_destroy_fb(struct drm_framebuffer *fb)
{
	if (fb->obj[0] != NULL)
		drm_gem_object_put_unlocked(fb->obj[0]);
	drm_framebuffer_cleanup(fb);
	kfree(fb);
}

static int
nvdrm_kms_create_fb_handle(struct drm_framebuffer *fb, struct drm_file *file,
    unsigned int *handle)
{
	if (fb->obj[0] == NULL)
		return (-ENODEV);
	return (drm_gem_handle_create(file, fb->obj[0], handle));
}

static int
nvdrm_kms_dirty_fb(struct drm_framebuffer *fb, struct drm_file *file,
    unsigned int flags, unsigned int color, struct drm_clip_rect *clips,
    unsigned int count)
{
	(void)fb;
	(void)file;
	(void)flags;
	(void)color;
	(void)clips;
	(void)count;
	return (0);
}

static const struct drm_framebuffer_funcs nvdrm_kms_fb_funcs = {
	.destroy = nvdrm_kms_destroy_fb,
	.create_handle = nvdrm_kms_create_fb_handle,
	.dirty = nvdrm_kms_dirty_fb,
};

static void
nvdrm_kms_destroy_internal_fb(struct drm_framebuffer *fb)
{
	drm_framebuffer_cleanup(fb);
	kfree(container_of(fb, struct nvdrm_internal_fb, base));
}

static int
nvdrm_kms_create_internal_fb_handle(struct drm_framebuffer *fb,
    struct drm_file *file, unsigned int *handle)
{
	(void)fb;
	(void)file;
	(void)handle;
	return (-ENODEV);
}

static const struct drm_framebuffer_funcs nvdrm_kms_internal_fb_funcs = {
	.destroy = nvdrm_kms_destroy_internal_fb,
	.create_handle = nvdrm_kms_create_internal_fb_handle,
};

static int
nvdrm_kms_console_set_par(struct fb_info *info)
{
	struct nvdrm_kms *kms = info != NULL ? info->par : NULL;
	bool has_master;

	if (kms == NULL || kms->shutting_down || !kms->console_registered)
		return (0);
	mutex_lock(&kms->ddev->master_mutex);
	has_master = kms->ddev->master != NULL;
	mutex_unlock(&kms->ddev->master_mutex);
	if (!has_master)
		(void)queue_work(system_unbound_wq, &kms->console_work);
	return (0);
}

int
nvdrm_kms_restore_console(struct nvgpu_device *gpu)
{
	struct nvdrm_kms *kms = nvgpu_device_get_kms(gpu);
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_connector_state *connector_state;
	struct drm_crtc_state *crtc_state;
	struct drm_plane_state *plane_state;
	struct drm_plane_state *cursor_state;
	struct drm_connector *connector = NULL;
	struct drm_connector *candidate;
	struct drm_crtc *crtc = NULL;
	struct drm_crtc *candidate_crtc;
	struct drm_display_mode mode;
	struct drm_mode_fb_cmd2 fb_cmd;
	struct nvdrm_internal_fb *internal;
	struct drm_framebuffer *fb;
	struct resource *bar1;
	void *bar1_base;
	bool force_modeset;
	bool new_console;
	int error;

	if (kms == NULL)
		return (ENODEV);
	lwkt_gettoken(&kms->console_token);
	if (kms->shutting_down) {
		lwkt_reltoken(&kms->console_token);
		return (ENODEV);
	}
	bzero(&mode, sizeof(mode));
	mutex_lock(&kms->ddev->mode_config.mutex);
	list_for_each_entry(candidate, &kms->ddev->mode_config.connector_list,
	    head) {
		candidate->funcs->fill_modes(candidate, 8192, 8192);
		if (candidate->status == connector_status_connected &&
		    !list_empty(&candidate->modes)) {
			connector = candidate;
			mode = *list_first_entry(&candidate->modes,
			    struct drm_display_mode, head);
			break;
		}
	}
	mutex_unlock(&kms->ddev->mode_config.mutex);
	if (connector == NULL) {
		drm_modeset_acquire_init(&ctx, 0);
	disable_retry:
		error = drm_atomic_helper_disable_all(kms->ddev, &ctx);
		if (error == -EDEADLK) {
			drm_modeset_backoff(&ctx);
			goto disable_retry;
		}
		drm_modeset_drop_locks(&ctx);
		drm_modeset_acquire_fini(&ctx);
		goto out;
	}
	list_for_each_entry(candidate_crtc, &kms->ddev->mode_config.crtc_list,
	    head) {
		crtc = candidate_crtc;
		break;
	}
	if (crtc == NULL) {
		error = ENXIO;
		goto out;
	}
	force_modeset = crtc->state == NULL || !crtc->state->active ||
	    !drm_mode_equal(&crtc->state->mode, &mode);

	new_console = kms->console.paddr == 0 ||
	    kms->console.width != (uint32_t)mode.hdisplay ||
	    kms->console.height != (uint32_t)mode.vdisplay;
	if (kms->console_registered &&
	    (kms->console.width != (uint32_t)mode.hdisplay ||
	    kms->console.height != (uint32_t)mode.vdisplay)) {
		unregister_framebuffer(&kms->console_fb);
		kms->console_registered = false;
		bzero(&kms->console_fb, sizeof(kms->console_fb));
	}
	error = nvgpu_display_prepare_console(gpu, mode.hdisplay, mode.vdisplay,
	    &kms->console);
	if (error != 0)
		goto out;
	if (kms->console.bar1_gva == 0) {
		error = ENXIO;
		goto out;
	}
	bar1 = nvgpu_device_get_bar(gpu, 1);
	bar1_base = bar1 != NULL ? rman_get_virtual(bar1) : NULL;
	if (bar1 == NULL || bar1_base == NULL ||
	    kms->console.bar1_gva + kms->console.size > rman_get_size(bar1)) {
		error = ENXIO;
		goto out;
	}
	if (new_console)
		bzero((uint8_t *)bar1_base + kms->console.bar1_gva,
		    kms->console.size);

	internal = kzalloc(sizeof(*internal), GFP_KERNEL);
	if (internal == NULL) {
		error = ENOMEM;
		goto out;
	}
	bzero(&fb_cmd, sizeof(fb_cmd));
	fb_cmd.width = mode.hdisplay;
	fb_cmd.height = mode.vdisplay;
	fb_cmd.pixel_format = DRM_FORMAT_XRGB8888;
	fb_cmd.pitches[0] = kms->console.pitch;
	fb_cmd.modifier[0] = DRM_FORMAT_MOD_LINEAR;
	drm_helper_mode_fill_fb_struct(kms->ddev, &internal->base, &fb_cmd);
	error = drm_framebuffer_init(kms->ddev, &internal->base,
	    &nvdrm_kms_internal_fb_funcs);
	if (error != 0) {
		kfree(internal);
		error = error < 0 ? -error : error;
		goto out;
	}
	fb = &internal->base;

	drm_modeset_acquire_init(&ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE);
	state = drm_atomic_state_alloc(kms->ddev);
	if (state == NULL) {
		drm_modeset_acquire_fini(&ctx);
		drm_framebuffer_remove(fb);
		error = ENOMEM;
		goto out;
	}
	state->acquire_ctx = &ctx;
retry:
	connector_state = drm_atomic_get_connector_state(state, connector);
	if (IS_ERR(connector_state)) {
		error = PTR_ERR(connector_state);
		goto commit_out;
	}
	error = drm_atomic_set_crtc_for_connector(connector_state, crtc);
	if (error != 0)
		goto commit_out;
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		error = PTR_ERR(crtc_state);
		goto commit_out;
	}
	error = drm_atomic_set_mode_for_crtc(crtc_state, &mode);
	if (error != 0)
		goto commit_out;
	crtc_state->active = true;
	if (force_modeset) {
		crtc_state->mode_changed = true;
		crtc_state->connectors_changed = true;
		crtc_state->active_changed = true;
	}
	crtc_state->color_mgmt_changed =
	    drm_property_replace_blob(&crtc_state->degamma_lut, NULL);
	crtc_state->color_mgmt_changed |=
	    drm_property_replace_blob(&crtc_state->ctm, NULL);
	crtc_state->color_mgmt_changed |=
	    drm_property_replace_blob(&crtc_state->gamma_lut, NULL);

	plane_state = drm_atomic_get_plane_state(state, crtc->primary);
	if (IS_ERR(plane_state)) {
		error = PTR_ERR(plane_state);
		goto commit_out;
	}
	error = drm_atomic_set_crtc_for_plane(plane_state, crtc);
	if (error != 0)
		goto commit_out;
	drm_atomic_set_fb_for_plane(plane_state, fb);
	plane_state->crtc_x = 0;
	plane_state->crtc_y = 0;
	plane_state->crtc_w = mode.hdisplay;
	plane_state->crtc_h = mode.vdisplay;
	plane_state->src_x = 0;
	plane_state->src_y = 0;
	plane_state->src_w = (uint32_t)mode.hdisplay << 16;
	plane_state->src_h = (uint32_t)mode.vdisplay << 16;
	if (crtc->cursor != NULL) {
		cursor_state = drm_atomic_get_plane_state(state, crtc->cursor);
		if (IS_ERR(cursor_state)) {
			error = PTR_ERR(cursor_state);
			goto commit_out;
		}
		error = __drm_atomic_helper_disable_plane(crtc->cursor,
		    cursor_state);
		if (error != 0)
			goto commit_out;
	}
	error = drm_atomic_commit(state);
commit_out:
	if (error == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	drm_atomic_state_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	if (error != 0) {
		drm_framebuffer_remove(fb);
		error = error < 0 ? -error : error;
		goto out;
	}
	drm_framebuffer_put(fb);

	if (!kms->console_registered) {
		bzero(&kms->console_fb, sizeof(kms->console_fb));
		kms->console_fb.width = kms->console.width;
		kms->console_fb.height = kms->console.height;
		kms->console_fb.stride = kms->console.pitch;
		kms->console_fb.depth = 32;
		kms->console_fb.is_vga_boot_display = 0;
		kms->console_fb.par = kms;
		kms->console_fb.device = nvgpu_device_get_newbus_dev(gpu);
		kms->console_fb.fbops.fb_set_par = nvdrm_kms_console_set_par;
		kms->console_fb.vaddr = (vm_offset_t)((uintptr_t)bar1_base +
		    kms->console.bar1_gva);
		kms->console_fb.paddr = (vm_paddr_t)(rman_get_start(bar1) +
		    kms->console.bar1_gva);
		error = register_framebuffer(&kms->console_fb);
		if (error != 0)
			goto out;
		kms->console_registered = true;
	}
	error = 0;
out:
	lwkt_reltoken(&kms->console_token);
	return (error);
}

static void
nvdrm_kms_run_console_work(struct work_struct *work)
{
	struct nvdrm_kms *kms = container_of(work, struct nvdrm_kms,
	    console_work);

	(void)nvdrm_kms_restore_console(kms->gpu);
}

static void
nvdrm_kms_receive_hotplug(void *arg, uint32_t plug_mask,
    uint32_t unplug_mask)
{
	struct nvdrm_kms *kms = arg;
	bool queue = false;

	if (kms == NULL || (plug_mask | unplug_mask) == 0)
		return;
	spin_lock(&kms->hotplug_lock);
	if (kms->hotplug_enabled) {
		queue = kms->hotplug_plug_mask == 0 &&
		    kms->hotplug_unplug_mask == 0 && kms->hotplug_dp_irq_mask == 0;
		kms->hotplug_plug_mask |= plug_mask;
		kms->hotplug_unplug_mask |= unplug_mask;
	}
	spin_unlock(&kms->hotplug_lock);
	if (queue)
		(void)queue_work(system_unbound_wq, &kms->hotplug_work);
}

static void
nvdrm_kms_receive_dp_irq(void *arg, uint32_t display_id)
{
	struct nvdrm_kms *kms = arg;
	bool queue = false;

	if (kms == NULL || display_id == 0)
		return;
	spin_lock(&kms->hotplug_lock);
	if (kms->hotplug_enabled) {
		queue = kms->hotplug_plug_mask == 0 &&
		    kms->hotplug_unplug_mask == 0 && kms->hotplug_dp_irq_mask == 0;
		kms->hotplug_dp_irq_mask |= display_id;
	}
	spin_unlock(&kms->hotplug_lock);
	if (queue)
		(void)queue_work(system_unbound_wq, &kms->hotplug_work);
}

static void
nvdrm_kms_run_hotplug_work(struct work_struct *work)
{
	struct nvdrm_kms *kms = container_of(work, struct nvdrm_kms,
	    hotplug_work);
	struct drm_connector *connector;
	uint32_t plug_mask;
	uint32_t unplug_mask;
	uint32_t dp_irq_mask;
	uint32_t link_bad_mask = 0;
	bool changed;
	bool has_master;

	spin_lock(&kms->hotplug_lock);
	plug_mask = kms->hotplug_plug_mask;
	unplug_mask = kms->hotplug_unplug_mask;
	dp_irq_mask = kms->hotplug_dp_irq_mask;
	kms->hotplug_plug_mask = 0;
	kms->hotplug_unplug_mask = 0;
	kms->hotplug_dp_irq_mask = 0;
	spin_unlock(&kms->hotplug_lock);

	if (dp_irq_mask != 0) {
		drm_modeset_lock_all(kms->ddev);
		list_for_each_entry(connector,
		    &kms->ddev->mode_config.connector_list, head) {
			struct nvdrm_connector *nvconnector =
			    to_nvdrm_connector(connector);

			if ((dp_irq_mask & nvconnector->display_id) == 0)
				continue;
			if (nvgpu_display_recover_dp_link(kms->gpu,
			    nvconnector->display_id) != 0)
				link_bad_mask |= nvconnector->display_id;
		}
		drm_modeset_unlock_all(kms->ddev);
	}
	if (link_bad_mask != 0) {
		list_for_each_entry(connector,
		    &kms->ddev->mode_config.connector_list, head) {
			struct nvdrm_connector *nvconnector =
			    to_nvdrm_connector(connector);

			if ((link_bad_mask & nvconnector->display_id) != 0)
				drm_connector_set_link_status_property(connector,
				    DRM_LINK_STATUS_BAD);
		}
		drm_kms_helper_hotplug_event(kms->ddev);
	}
	if ((plug_mask | unplug_mask) == 0)
		return;
	changed = drm_helper_hpd_irq_event(kms->ddev);
	if (!changed)
		drm_kms_helper_hotplug_event(kms->ddev);
	mutex_lock(&kms->ddev->master_mutex);
	has_master = kms->ddev->master != NULL;
	mutex_unlock(&kms->ddev->master_mutex);
	if (!has_master && changed)
		(void)queue_work(system_unbound_wq, &kms->console_work);
}

static bool
nvdrm_kms_plane_supports_modifier(struct drm_plane *plane, uint32_t format,
    uint64_t modifier)
{
	const struct drm_format_info *info;
	uint8_t sector_layout;
	uint32_t index;
	bool found = false;

	if (plane != NULL && plane->type == DRM_PLANE_TYPE_CURSOR)
		return (format == DRM_FORMAT_ARGB8888 &&
		    (modifier == DRM_FORMAT_MOD_LINEAR ||
		    modifier == DRM_FORMAT_MOD_INVALID));
	for (index = 0; index < nitems(nvdrm_kms_primary_formats); index++) {
		if (nvdrm_kms_primary_formats[index] == format) {
			found = true;
			break;
		}
	}
	if (!found)
		return (false);
	if (modifier == DRM_FORMAT_MOD_LINEAR ||
	    modifier == DRM_FORMAT_MOD_INVALID)
		return (true);
	for (index = 0; index + 1 < nitems(nvdrm_kms_primary_modifiers); index++) {
		if (nvdrm_kms_primary_modifiers[index] == modifier) {
			found = true;
			break;
		}
		found = false;
	}
	if (!found)
		return (false);
	info = drm_format_info(format);
	if (info == NULL)
		return (false);
	sector_layout = ((modifier >> 22) & 0x1u) |
	    ((modifier >> 25) & 0x6u);
	for (index = 0; index < info->num_planes; index++) {
		if (info->cpp[index] == 3 ||
		    (info->cpp[index] == 2 && sector_layout != 3) ||
		    (info->cpp[index] == 1 && sector_layout != 2) ||
		    (info->cpp[index] >= 4 && sector_layout != 1))
			return (false);
	}
	return (true);
}

static struct drm_framebuffer *
nvdrm_kms_create_fb(struct drm_device *ddev, struct drm_file *file,
    const struct drm_mode_fb_cmd2 *cmd)
{
	const struct drm_format_info *info;
	struct drm_gem_object *object;
	struct drm_framebuffer *fb;
	struct nvgpu_bo *bo;
	uint64_t line;
	uint64_t min_size;
	uint64_t modifier;
	uint8_t kind;
	bool block_linear;
	int error;

	if (cmd->width == 0 || cmd->height == 0)
		return (ERR_PTR(-EINVAL));
	info = drm_get_format_info(ddev, cmd);
	if (info == NULL || info->num_planes != 1 ||
	    !nvdrm_kms_plane_supports_modifier(NULL, cmd->pixel_format,
	    cmd->modifier[0]))
		return (ERR_PTR(-EINVAL));
	line = (uint64_t)cmd->width * info->cpp[0];
	if (cmd->pitches[0] < line || (cmd->pitches[0] & 0x3fu) != 0)
		return (ERR_PTR(-EINVAL));
	min_size = (uint64_t)(cmd->height - 1u) * cmd->pitches[0] + line +
	    cmd->offsets[0];
	object = drm_gem_object_lookup(file, cmd->handles[0]);
	if (object == NULL)
		return (ERR_PTR(-ENOENT));
	if (object->size < min_size) {
		drm_gem_object_put_unlocked(object);
		return (ERR_PTR(-EINVAL));
	}
	bo = nvgpu_bo_from_gem(object);
	if (!nvgpu_bo_is_vram(bo) || bo->paddr == 0) {
		drm_gem_object_put_unlocked(object);
		return (ERR_PTR(-EINVAL));
	}
	modifier = cmd->modifier[0];
	block_linear = modifier != DRM_FORMAT_MOD_LINEAR &&
	    modifier != DRM_FORMAT_MOD_INVALID;
	kind = (modifier >> 12) & 0xffu;
	if ((block_linear && (!bo->vm_bound_tiled || bo->vm_bound_mixed_kind ||
	    bo->vm_bound_kind != kind)) || (!block_linear && bo->vm_bound_tiled)) {
		drm_gem_object_put_unlocked(object);
		return (ERR_PTR(-EINVAL));
	}
	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (fb == NULL) {
		drm_gem_object_put_unlocked(object);
		return (ERR_PTR(-ENOMEM));
	}
	drm_helper_mode_fill_fb_struct(ddev, fb, cmd);
	fb->obj[0] = object;
	error = drm_framebuffer_init(ddev, fb, &nvdrm_kms_fb_funcs);
	if (error != 0) {
		fb->obj[0] = NULL;
		kfree(fb);
		drm_gem_object_put_unlocked(object);
		return (ERR_PTR(error));
	}
	return (fb);
}

static int
nvdrm_kms_check_plane(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct drm_framebuffer *fb;
	struct drm_gem_object *object;
	uint32_t source_x;
	uint32_t source_y;
	uint32_t source_width;
	uint32_t source_height;
	uint64_t min_size;
	uint64_t line;
	int error;

	if (state->crtc == NULL)
		return (state->fb == NULL ? 0 : -EINVAL);
	crtc_state = drm_atomic_get_new_crtc_state(state->state, state->crtc);
	if (crtc_state == NULL)
		return (-EINVAL);
	error = drm_atomic_helper_check_plane_state(state, crtc_state,
	    DRM_PLANE_HELPER_NO_SCALING, DRM_PLANE_HELPER_NO_SCALING, true,
	    plane->type == DRM_PLANE_TYPE_CURSOR);
	if (error != 0 || !state->visible)
		return (error);
	if (!crtc_state->active)
		return (-EINVAL);
	fb = state->fb;
	if (fb == NULL || fb->format == NULL ||
	    !nvdrm_kms_plane_supports_modifier(plane, fb->format->format,
	    fb->modifier))
		return (-EINVAL);
	object = drm_gem_fb_get_obj(fb, 0);
	if (fb->funcs == &nvdrm_kms_internal_fb_funcs) {
		if (plane->type != DRM_PLANE_TYPE_PRIMARY || fb->width == 0 ||
		    fb->height == 0 || fb->pitches[0] < fb->width * 4u)
			return (-EINVAL);
		object = NULL;
	} else if (object == NULL ||
	    !nvgpu_bo_is_vram(nvgpu_bo_from_gem(object)))
		return (-EINVAL);
	line = (uint64_t)fb->width * fb->format->cpp[0];
	if (fb->width == 0 || fb->height == 0 || fb->pitches[0] < line)
		return (-EINVAL);
	min_size = (uint64_t)(fb->height - 1u) * fb->pitches[0] + line +
	    fb->offsets[0];
	if (object != NULL && object->size < min_size)
		return (-EINVAL);
	if (plane->type == DRM_PLANE_TYPE_CURSOR) {
		if (state->crtc_w <= 0 || state->crtc_w != state->crtc_h ||
		    (state->crtc_w != 32 && state->crtc_w != 64 &&
		    state->crtc_w != 128 && state->crtc_w != 256) ||
		    fb->width != (uint32_t)state->crtc_w ||
		    fb->height < (uint32_t)state->crtc_h ||
		    fb->pitches[0] != (uint32_t)state->crtc_w * 4u ||
		    (fb->offsets[0] & 0xffu) != 0 || state->src_x != 0 ||
		    state->src_y != 0 ||
		    state->src_w != ((uint32_t)state->crtc_w << 16) ||
		    state->src_h != ((uint32_t)state->crtc_h << 16))
			return (-EINVAL);
		return (0);
	}
	if (state->crtc_x != 0 || state->crtc_y != 0 ||
	    state->crtc_w != crtc_state->mode.hdisplay ||
	    state->crtc_h != crtc_state->mode.vdisplay ||
	    (state->src_x & 0xffffu) != 0 ||
	    (state->src_y & 0xffffu) != 0 ||
	    (state->src_w & 0xffffu) != 0 ||
	    (state->src_h & 0xffffu) != 0 || (fb->pitches[0] & 0x3fu) != 0)
		return (-EINVAL);
	source_x = state->src_x >> 16;
	source_y = state->src_y >> 16;
	source_width = state->src_w >> 16;
	source_height = state->src_h >> 16;
	if (source_width != (uint32_t)state->crtc_w ||
	    source_height != (uint32_t)state->crtc_h ||
	    source_x > fb->width || source_y > fb->height ||
	    source_width > fb->width - source_x ||
	    source_height > fb->height - source_y)
		return (-EINVAL);
	return (0);
}

static int
nvdrm_kms_prepare_fb(struct drm_plane *plane, struct drm_plane_state *state)
{
	struct drm_gem_object *object;
	struct nvgpu_bo *bo;
	int error;

	if (state == NULL || state->fb == NULL)
		return (0);
	if (state->fb->funcs == &nvdrm_kms_internal_fb_funcs)
		return (0);
	object = drm_gem_fb_get_obj(state->fb, 0);
	if (object == NULL)
		return (-EINVAL);
	bo = nvgpu_bo_from_gem(object);
	error = drm_gem_fb_prepare_fb(plane, state);
	if (error != 0)
		return (error);
	if (state->fence == NULL) {
		error = nvgpu_bo_resv_wait(bo, false, true, false);
		if (error != 0)
			return (error < 0 ? error : -error);
	}
	error = nvgpu_bo_scanout_pin(bo);
	return (error == 0 ? 0 : error < 0 ? error : -error);
}

static void
nvdrm_kms_cleanup_fb(struct drm_plane *plane,
    struct drm_plane_state *old_state)
{
	struct drm_gem_object *object;

	(void)plane;
	if (old_state == NULL || old_state->fb == NULL)
		return;
	if (old_state->fb->funcs == &nvdrm_kms_internal_fb_funcs)
		return;
	object = drm_gem_fb_get_obj(old_state->fb, 0);
	if (object != NULL)
		(void)nvgpu_bo_scanout_unpin(nvgpu_bo_from_gem(object));
}

static void
nvdrm_kms_update_plane(struct drm_plane *plane,
    struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = plane->state;
	struct nvdrm_crtc *nvcrtc;
	struct drm_gem_object *object;
	struct nvgpu_bo *bo;
	bool legacy_update;
	bool internal;
	int error;

	if (state == NULL || state->crtc == NULL || state->fb == NULL ||
	    !state->visible || state->crtc->state == NULL ||
	    !state->crtc->state->active ||
	    drm_atomic_crtc_needs_modeset(state->crtc->state))
		return;
	nvcrtc = to_nvdrm_crtc(state->crtc);
	internal = state->fb->funcs == &nvdrm_kms_internal_fb_funcs;
	object = internal ? NULL : drm_gem_fb_get_obj(state->fb, 0);
	if (!internal && object == NULL)
		return;
	bo = internal ? NULL : nvgpu_bo_from_gem(object);
	legacy_update = old_state != NULL && old_state->state != NULL &&
	    old_state->state->legacy_cursor_update;
	if (plane->type == DRM_PLANE_TYPE_CURSOR) {
		struct nvgpu_display_cursor cursor;

		bzero(&cursor, sizeof(cursor));
		cursor.paddr = bo->paddr + state->fb->offsets[0];
		cursor.width = state->fb->width;
		cursor.height = state->fb->height;
		cursor.x = state->crtc_x;
		cursor.y = state->crtc_y;
		error = nvgpu_display_update_cursor(nvcrtc->kms->gpu,
		    nvcrtc->head, &cursor, legacy_update);
		if (error != 0)
			nvgpu_log(NVGPU_LOG_INFO,
			    "cursor update failed head=%u error=%d\n",
			    nvcrtc->head, error);
		return;
	}

	{
		struct nvgpu_display_scanout scanout;
		struct drm_crtc_state *crtc_state = state->crtc->state;
		struct drm_pending_vblank_event *event = crtc_state->event;
		struct nvdrm_atomic_state *atomic;
		struct nvdrm_pending_flip *pending = NULL;
		uint64_t modifier = state->fb->modifier;
		bool event_ref = false;

		bzero(&scanout, sizeof(scanout));
		if (internal) {
			scanout.paddr = nvcrtc->kms->console.paddr;
			scanout.size = nvcrtc->kms->console.size;
		} else {
			scanout.paddr = bo->paddr + state->fb->offsets[0];
			scanout.size = object->size - state->fb->offsets[0];
		}
		scanout.width = state->fb->width;
		scanout.height = state->fb->height;
		scanout.pitch = state->fb->pitches[0];
		switch (state->fb->format->format) {
		case DRM_FORMAT_XRGB8888:
			scanout.format = NVGPU_DISPLAY_FORMAT_XRGB8888;
			break;
		case DRM_FORMAT_ARGB8888:
			scanout.format = NVGPU_DISPLAY_FORMAT_ARGB8888;
			break;
		case DRM_FORMAT_XBGR8888:
			scanout.format = NVGPU_DISPLAY_FORMAT_XBGR8888;
			break;
		case DRM_FORMAT_ABGR8888:
			scanout.format = NVGPU_DISPLAY_FORMAT_ABGR8888;
			break;
		case DRM_FORMAT_RGB565:
			scanout.format = NVGPU_DISPLAY_FORMAT_RGB565;
			break;
		case DRM_FORMAT_XRGB1555:
		case DRM_FORMAT_ARGB1555:
			scanout.format = NVGPU_DISPLAY_FORMAT_ARGB1555;
			break;
		case DRM_FORMAT_XRGB2101010:
		case DRM_FORMAT_ARGB2101010:
			scanout.format = NVGPU_DISPLAY_FORMAT_ARGB2101010;
			break;
		case DRM_FORMAT_XBGR2101010:
		case DRM_FORMAT_ABGR2101010:
			scanout.format = NVGPU_DISPLAY_FORMAT_ABGR2101010;
			break;
		default:
			return;
		}
		if (modifier != DRM_FORMAT_MOD_LINEAR &&
		    modifier != DRM_FORMAT_MOD_INVALID) {
			scanout.layout = NVGPU_DISPLAY_LAYOUT_BLOCK_LINEAR;
			scanout.kind = (modifier >> 12) & 0xffu;
			scanout.block_height = modifier & 0x0fu;
		} else {
			scanout.layout = NVGPU_DISPLAY_LAYOUT_PITCH;
			scanout.kind = 0;
		}
		scanout.source_x = state->src_x >> 16;
		scanout.source_y = state->src_y >> 16;
		scanout.source_width = state->src_w >> 16;
		scanout.source_height = state->src_h >> 16;
		scanout.output_width = state->crtc_w;
		scanout.output_height = state->crtc_h;
		if (event != NULL && state->state != NULL &&
		    drm_crtc_vblank_get(state->crtc) == 0) {
			atomic = to_nvdrm_atomic_state(state->state);
			pending = &atomic->flips[nvcrtc->head];
			pending->crtc = state->crtc;
			pending->event = event;
			event_ref = true;
		}
		error = nvgpu_display_update_primary(nvcrtc->kms->gpu,
		    nvcrtc->head, nvcrtc->window, &scanout,
		    event_ref ? pending : NULL);
		if (error == 0 && event_ref) {
			crtc_state->event = NULL;
		} else if (event_ref) {
			pending->crtc = NULL;
			pending->event = NULL;
			drm_crtc_vblank_put(state->crtc);
		}
		if (error != 0)
			nvgpu_log(NVGPU_LOG_INFO,
			    "primary update failed head=%u error=%d\n",
			    nvcrtc->head, error);
	}
}

static void
nvdrm_kms_disable_plane(struct drm_plane *plane,
    struct drm_plane_state *old_state)
{
	struct nvdrm_crtc *nvcrtc;
	bool legacy_update;
	int error;

	if (old_state == NULL || old_state->crtc == NULL)
		return;
	nvcrtc = to_nvdrm_crtc(old_state->crtc);
	legacy_update = old_state->state != NULL &&
	    old_state->state->legacy_cursor_update;
	if (plane->type == DRM_PLANE_TYPE_CURSOR)
		error = nvgpu_display_disable_cursor(nvcrtc->kms->gpu,
		    nvcrtc->head, legacy_update);
	else
		error = nvgpu_display_disable_primary(nvcrtc->kms->gpu,
		    nvcrtc->window);
	if (error != 0)
		nvgpu_log(NVGPU_LOG_INFO, "plane disable failed head=%u error=%d\n",
		    nvcrtc->head, error);
}

static int
nvdrm_kms_check_async_plane(struct drm_plane *plane,
    struct drm_plane_state *state)
{
	struct drm_plane_state *old_state;

	if (plane->type != DRM_PLANE_TYPE_CURSOR || state == NULL)
		return (-EINVAL);
	old_state = plane->state;
	if (old_state == NULL || old_state->crtc != state->crtc ||
	    old_state->fb != state->fb || !old_state->visible || !state->visible ||
	    old_state->src_x != state->src_x || old_state->src_y != state->src_y ||
	    old_state->src_w != state->src_w || old_state->src_h != state->src_h ||
	    old_state->crtc_w != state->crtc_w ||
	    old_state->crtc_h != state->crtc_h)
		return (-EINVAL);
	return (nvdrm_kms_check_plane(plane, state));
}

static void
nvdrm_kms_update_async_plane(struct drm_plane *plane,
    struct drm_plane_state *new_state)
{
	struct drm_plane_state *state = plane->state;
	struct nvdrm_crtc *nvcrtc;

	if (state == NULL || new_state == NULL || new_state->crtc == NULL)
		return;
	nvcrtc = to_nvdrm_crtc(new_state->crtc);
	nvgpu_display_move_cursor(nvcrtc->kms->gpu, nvcrtc->head,
	    new_state->crtc_x, new_state->crtc_y);
	state->crtc_x = new_state->crtc_x;
	state->crtc_y = new_state->crtc_y;
}

static void
nvdrm_kms_destroy_plane(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
	kfree(plane);
}

static const struct drm_plane_helper_funcs nvdrm_kms_plane_helper_funcs = {
	.atomic_check = nvdrm_kms_check_plane,
	.prepare_fb = nvdrm_kms_prepare_fb,
	.cleanup_fb = nvdrm_kms_cleanup_fb,
	.atomic_update = nvdrm_kms_update_plane,
	.atomic_disable = nvdrm_kms_disable_plane,
	.atomic_async_check = nvdrm_kms_check_async_plane,
	.atomic_async_update = nvdrm_kms_update_async_plane,
};

static const struct drm_plane_funcs nvdrm_kms_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = nvdrm_kms_destroy_plane,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
	.format_mod_supported = nvdrm_kms_plane_supports_modifier,
};

static enum drm_mode_status
nvdrm_kms_validate_mode(struct drm_crtc *crtc,
    const struct drm_display_mode *mode)
{
	struct drm_display_mode timing = *mode;

	(void)crtc;
	drm_mode_set_crtcinfo(&timing,
	    CRTC_INTERLACE_HALVE_V | CRTC_STEREO_DOUBLE);
	if (timing.crtc_clock <= 0)
		return (MODE_CLOCK_LOW);
	if (timing.crtc_clock > (int)(0x7fffffffu / 1000u))
		return (MODE_CLOCK_HIGH);
	if (timing.crtc_hdisplay == 0 || timing.crtc_htotal == 0 ||
	    timing.crtc_hsync_end <= timing.crtc_hsync_start ||
	    timing.crtc_hblank_end <= timing.crtc_hsync_start)
		return (MODE_H_ILLEGAL);
	if (timing.crtc_vdisplay == 0 || timing.crtc_vtotal == 0 ||
	    timing.crtc_vsync_end <= timing.crtc_vsync_start ||
	    timing.crtc_vblank_end <= timing.crtc_vsync_start)
		return (MODE_V_ILLEGAL);
	if (timing.crtc_hdisplay > 0xffff || timing.crtc_htotal > 0xffff)
		return (MODE_BAD_HVALUE);
	if (timing.crtc_vdisplay > 0xffff || timing.crtc_vtotal > 0xffff)
		return (MODE_BAD_VVALUE);
	return (MODE_OK);
}

static int
nvdrm_kms_check_crtc(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	uint32_t size;

	if (state->color_mgmt_changed) {
		if (state->degamma_lut != NULL) {
			size = drm_color_lut_size(state->degamma_lut);
			if (size != 256 && size != NVGPU_DISPLAY_COLOR_LUT_SIZE)
				return (-EINVAL);
		}
		if (state->gamma_lut != NULL) {
			size = drm_color_lut_size(state->gamma_lut);
			if (size != 256 && size != NVGPU_DISPLAY_COLOR_LUT_SIZE)
				return (-EINVAL);
		}
	}
	if (!state->enable)
		return (0);
	return (nvdrm_kms_validate_mode(crtc, &state->adjusted_mode) == MODE_OK ?
	    0 : -EINVAL);
}

static void
nvdrm_kms_flush_crtc(struct drm_crtc *crtc,
    struct drm_crtc_state *old_state)
{
	(void)crtc;
	(void)old_state;
}

static void
nvdrm_kms_enable_crtc(struct drm_crtc *crtc,
    struct drm_crtc_state *old_state)
{
	struct nvdrm_crtc *nvcrtc = to_nvdrm_crtc(crtc);
	struct nvdrm_atomic_state *atomic;
	struct drm_plane_state *plane_state;
	struct drm_gem_object *object;
	struct nvgpu_display_head_config head_config;
	struct nvgpu_display_scanout scanout;
	struct nvgpu_display_prepared_output *prepared;
	struct nvgpu_bo *bo;
	uint64_t modifier;
	int error;

	if (old_state == NULL || old_state->state == NULL ||
	    crtc->state == NULL || crtc->primary == NULL ||
	    crtc->primary->state == NULL || crtc->primary->state->fb == NULL)
		return;
	atomic = to_nvdrm_atomic_state(old_state->state);
	prepared = atomic->prepared[nvcrtc->head];
	if (prepared == NULL)
		return;
	atomic->prepared[nvcrtc->head] = NULL;
	if (!atomic->head_config_valid[nvcrtc->head]) {
		nvgpu_display_abort_output(nvcrtc->kms->gpu, prepared);
		return;
	}
	head_config = atomic->head_config[nvcrtc->head];
	plane_state = crtc->primary->state;
	object = drm_gem_fb_get_obj(plane_state->fb, 0);
	if (object == NULL &&
	    plane_state->fb->funcs != &nvdrm_kms_internal_fb_funcs) {
		nvgpu_display_abort_output(nvcrtc->kms->gpu, prepared);
		return;
	}
	bo = object != NULL ? nvgpu_bo_from_gem(object) : NULL;
	bzero(&scanout, sizeof(scanout));
	if (plane_state->fb->funcs == &nvdrm_kms_internal_fb_funcs) {
		scanout.paddr = nvcrtc->kms->console.paddr;
		scanout.size = nvcrtc->kms->console.size;
	} else {
		scanout.paddr = bo->paddr + plane_state->fb->offsets[0];
		scanout.size = object->size - plane_state->fb->offsets[0];
	}
	scanout.width = plane_state->fb->width;
	scanout.height = plane_state->fb->height;
	scanout.pitch = plane_state->fb->pitches[0];
	switch (plane_state->fb->format->format) {
	case DRM_FORMAT_XRGB8888: scanout.format = NVGPU_DISPLAY_FORMAT_XRGB8888; break;
	case DRM_FORMAT_ARGB8888: scanout.format = NVGPU_DISPLAY_FORMAT_ARGB8888; break;
	case DRM_FORMAT_XBGR8888: scanout.format = NVGPU_DISPLAY_FORMAT_XBGR8888; break;
	case DRM_FORMAT_ABGR8888: scanout.format = NVGPU_DISPLAY_FORMAT_ABGR8888; break;
	case DRM_FORMAT_RGB565: scanout.format = NVGPU_DISPLAY_FORMAT_RGB565; break;
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555: scanout.format = NVGPU_DISPLAY_FORMAT_ARGB1555; break;
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		scanout.format = NVGPU_DISPLAY_FORMAT_ARGB2101010;
		break;
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		scanout.format = NVGPU_DISPLAY_FORMAT_ABGR2101010;
		break;
	default:
		nvgpu_display_abort_output(nvcrtc->kms->gpu, prepared);
		return;
	}
	modifier = plane_state->fb->modifier;
	if (modifier != DRM_FORMAT_MOD_LINEAR &&
	    modifier != DRM_FORMAT_MOD_INVALID) {
		scanout.layout = NVGPU_DISPLAY_LAYOUT_BLOCK_LINEAR;
		scanout.kind = (modifier >> 12) & 0xffu;
		scanout.block_height = modifier & 0x0fu;
	} else {
		scanout.layout = NVGPU_DISPLAY_LAYOUT_PITCH;
		scanout.kind = 0;
	}
	scanout.source_x = plane_state->src_x >> 16;
	scanout.source_y = plane_state->src_y >> 16;
	scanout.source_width = plane_state->src_w >> 16;
	scanout.source_height = plane_state->src_h >> 16;
	scanout.output_width = plane_state->crtc_w;
	scanout.output_height = plane_state->crtc_h;
	error = nvgpu_display_enable(nvcrtc->kms->gpu, nvcrtc->head,
	    nvcrtc->window, &head_config, &scanout, prepared);
	if (error != 0) {
		nvgpu_log(NVGPU_LOG_INFO, "CRTC enable failed head=%u error=%d\n",
		    nvcrtc->head, error);
		return;
	}
	if (crtc->cursor != NULL && crtc->cursor->state != NULL &&
	    crtc->cursor->state->visible && crtc->cursor->state->fb != NULL) {
		struct drm_plane_state *cursor_state = crtc->cursor->state;
		struct drm_gem_object *cursor_object =
		    drm_gem_fb_get_obj(cursor_state->fb, 0);

		if (cursor_object != NULL) {
			struct nvgpu_display_cursor cursor;
			struct nvgpu_bo *cursor_bo =
			    nvgpu_bo_from_gem(cursor_object);

			bzero(&cursor, sizeof(cursor));
			cursor.paddr = cursor_bo->paddr + cursor_state->fb->offsets[0];
			cursor.width = cursor_state->fb->width;
			cursor.height = cursor_state->fb->height;
			cursor.x = cursor_state->crtc_x;
			cursor.y = cursor_state->crtc_y;
			(void)nvgpu_display_update_cursor(nvcrtc->kms->gpu,
			    nvcrtc->head, &cursor, false);
		}
	}
	drm_crtc_vblank_on(crtc);
}

static void
nvdrm_kms_disable_crtc(struct drm_crtc *crtc,
    struct drm_crtc_state *old_state)
{
	struct nvdrm_crtc *nvcrtc = to_nvdrm_crtc(crtc);
	int error;

	(void)old_state;
	error = nvgpu_display_disable(nvcrtc->kms->gpu, nvcrtc->head,
	    nvcrtc->window);
	if (error == 0)
		drm_crtc_vblank_off(crtc);
	else
		nvgpu_log(NVGPU_LOG_INFO, "CRTC disable failed head=%u error=%d\n",
		    nvcrtc->head, error);
}

static int
nvdrm_kms_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb,
    struct drm_pending_vblank_event *event, uint32_t flags,
    struct drm_modeset_acquire_ctx *ctx)
{
	if ((flags & (DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_TARGET)) != 0)
		return (-EINVAL);
	return (drm_atomic_helper_page_flip(crtc, fb, event, flags, ctx));
}

static void
nvdrm_kms_destroy_crtc(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
	kfree(to_nvdrm_crtc(crtc));
}

static int
nvdrm_kms_enable_crtc_vblank(struct drm_crtc *crtc)
{
	struct nvdrm_crtc *nvcrtc = to_nvdrm_crtc(crtc);

	nvgpu_display_enable_vblank(nvcrtc->kms->gpu, nvcrtc->head);
	return (0);
}

static void
nvdrm_kms_disable_crtc_vblank(struct drm_crtc *crtc)
{
	struct nvdrm_crtc *nvcrtc = to_nvdrm_crtc(crtc);

	nvgpu_display_disable_vblank(nvcrtc->kms->gpu, nvcrtc->head);
}

static const struct drm_crtc_helper_funcs nvdrm_kms_crtc_helper_funcs = {
	.mode_valid = nvdrm_kms_validate_mode,
	.atomic_check = nvdrm_kms_check_crtc,
	.atomic_flush = nvdrm_kms_flush_crtc,
	.atomic_enable = nvdrm_kms_enable_crtc,
	.atomic_disable = nvdrm_kms_disable_crtc,
};

static const struct drm_crtc_funcs nvdrm_kms_crtc_funcs = {
	.enable_vblank = nvdrm_kms_enable_crtc_vblank,
	.disable_vblank = nvdrm_kms_disable_crtc_vblank,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = nvdrm_kms_page_flip,
	.gamma_set = drm_atomic_helper_legacy_gamma_set,
	.destroy = nvdrm_kms_destroy_crtc,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static int
nvdrm_kms_get_connector_modes(struct drm_connector *connector)
{
	struct nvdrm_connector *nvconnector = to_nvdrm_connector(connector);
	struct edid *edid;
	uint8_t *data;
	uint32_t size = NVDRM_KMS_EDID_SIZE;
	int modes = 0;

	data = kzalloc(NVDRM_KMS_EDID_SIZE, GFP_KERNEL);
	if (data == NULL)
		return (0);
	edid = (struct edid *)data;
	nvconnector->has_audio = false;
	if (nvgpu_display_read_edid(nvconnector->kms->gpu,
	    nvconnector->display_id, data, &size) == 0 &&
	    size >= sizeof(*edid) && (size % 128u) == 0 &&
	    drm_edid_is_valid(edid)) {
		struct drm_display_mode *mode;
		struct drm_display_mode *native = NULL;

		drm_connector_update_edid_property(connector, edid);
		modes = drm_add_edid_modes(connector, edid);
		nvconnector->has_audio = drm_detect_monitor_audio(edid);
		list_for_each_entry(mode, &connector->probed_modes, head) {
			if (native == NULL)
				native = mode;
			if ((mode->type & DRM_MODE_TYPE_PREFERRED) != 0) {
				native = mode;
				break;
			}
		}
		if (nvconnector->native_mode != NULL)
			drm_mode_destroy(connector->dev, nvconnector->native_mode);
		nvconnector->native_mode = native != NULL ?
		    drm_mode_duplicate(connector->dev, native) : NULL;
	} else {
		drm_connector_update_edid_property(connector, NULL);
		if (nvconnector->native_mode != NULL) {
			drm_mode_destroy(connector->dev, nvconnector->native_mode);
			nvconnector->native_mode = NULL;
		}
	}
	kfree(data);
	return (modes);
}

static enum drm_mode_status
nvdrm_kms_validate_connector_mode(struct drm_connector *connector,
    struct drm_display_mode *mode)
{
	struct nvdrm_connector *nvconnector = to_nvdrm_connector(connector);
	struct nvgpu_display_output_info info;

	if (nvgpu_display_get_output(nvconnector->kms->gpu,
	    nvconnector->display_id, &info) != 0)
		return (MODE_ERROR);
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) != 0 &&
	    (info.protocol != NVGPU_DISPLAY_PROTOCOL_DP ||
	    !info.dp_interlace_capable))
		return (MODE_NO_INTERLACE);
	if (mode->clock < 25000)
		return (MODE_CLOCK_LOW);
	if (info.protocol == NVGPU_DISPLAY_PROTOCOL_TMDS) {
		uint32_t max_clock = connector->display_info.hdmi.scdc.scrambling.supported ?
		    594000u : 340000u;

		if (connector->display_info.max_tmds_clock > 0 &&
		    (uint32_t)connector->display_info.max_tmds_clock < max_clock)
			max_clock = connector->display_info.max_tmds_clock;
		if ((uint32_t)mode->clock > max_clock)
			return (MODE_CLOCK_HIGH);
	}
	return (MODE_OK);
}

static enum drm_connector_status
nvdrm_kms_detect_connector(struct drm_connector *connector, bool force)
{
	struct nvdrm_connector *nvconnector = to_nvdrm_connector(connector);
	int connected;

	(void)force;
	connected = nvgpu_display_detect_output(nvconnector->kms->gpu,
	    nvconnector->display_id);
	if (connected > 0)
		return (connector_status_connected);
	nvconnector->has_audio = false;
	drm_connector_update_edid_property(connector, NULL);
	return (connector_status_disconnected);
}

static void
nvdrm_kms_destroy_connector(struct drm_connector *connector)
{
	struct nvdrm_connector *nvconnector = to_nvdrm_connector(connector);

	if (nvconnector->native_mode != NULL)
		drm_mode_destroy(connector->dev, nvconnector->native_mode);
	drm_connector_cleanup(connector);
	kfree(nvconnector);
}

static int
nvdrm_kms_check_connector(struct drm_connector *connector,
    struct drm_connector_state *state)
{
	struct nvdrm_connector *nvconnector = to_nvdrm_connector(connector);
	struct nvdrm_connector_state *nvstate = to_nvdrm_connector_state(state);
	struct drm_crtc_state *crtc_state;
	const struct drm_display_mode *target;

	if (nvstate->max_bpc != 8)
		return (-EINVAL);
	if (state->crtc == NULL)
		return (0);
	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return (PTR_ERR(crtc_state));
	if (!crtc_state->enable)
		return (0);
	target = &crtc_state->mode;
	if (state->scaling_mode != DRM_MODE_SCALE_NONE &&
	    nvconnector->native_mode != NULL)
		target = nvconnector->native_mode;
	if (!drm_mode_equal(&crtc_state->adjusted_mode, target)) {
		drm_mode_copy(&crtc_state->adjusted_mode, target);
		crtc_state->mode_changed = true;
	}
	return (0);
}

static void
nvdrm_kms_reset_connector(struct drm_connector *connector)
{
	struct nvdrm_connector_state *state;

	if (connector->state != NULL) {
		__drm_atomic_helper_connector_destroy_state(connector->state);
		kfree(to_nvdrm_connector_state(connector->state));
		connector->state = NULL;
	}
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state != NULL) {
		state->base.link_status = DRM_LINK_STATUS_GOOD;
		state->base.scaling_mode = DRM_MODE_SCALE_NONE;
		state->dither_mode = NVGPU_DISPLAY_DITHER_MODE_AUTO;
		state->dither_depth = NVGPU_DISPLAY_DITHER_DEPTH_AUTO;
		state->max_bpc = 8;
		state->underscan_mode = NVGPU_DISPLAY_UNDERSCAN_OFF;
	}
	__drm_atomic_helper_connector_reset(connector,
	    state != NULL ? &state->base : NULL);
}

static struct drm_connector_state *
nvdrm_kms_duplicate_connector_state(struct drm_connector *connector)
{
	struct nvdrm_connector_state *old_state;
	struct nvdrm_connector_state *state;

	if (WARN_ON(connector->state == NULL))
		return (NULL);
	old_state = to_nvdrm_connector_state(connector->state);
	state = kmalloc(sizeof(*state), M_DRM, GFP_KERNEL);
	if (state == NULL)
		return (NULL);
	*state = *old_state;
	__drm_atomic_helper_connector_duplicate_state(connector, &state->base);
	return (&state->base);
}

static void
nvdrm_kms_destroy_connector_state(struct drm_connector *connector,
    struct drm_connector_state *state)
{
	(void)connector;
	__drm_atomic_helper_connector_destroy_state(state);
	kfree(to_nvdrm_connector_state(state));
}

static int
nvdrm_kms_set_connector_property(struct drm_connector *connector,
    struct drm_connector_state *state, struct drm_property *property,
    uint64_t value)
{
	struct nvdrm_connector *nvconnector = to_nvdrm_connector(connector);
	struct nvdrm_connector_state *nvstate = to_nvdrm_connector_state(state);
	struct nvdrm_kms *kms = nvconnector->kms;

	if (property == kms->dither_mode_property)
		nvstate->dither_mode = value;
	else if (property == kms->dither_depth_property)
		nvstate->dither_depth = value;
	else if (property == kms->max_bpc_property)
		nvstate->max_bpc = value;
	else if (property == kms->underscan_property)
		nvstate->underscan_mode = value;
	else if (property == kms->underscan_hborder_property)
		nvstate->underscan_hborder = value;
	else if (property == kms->underscan_vborder_property)
		nvstate->underscan_vborder = value;
	else
		return (-EINVAL);
	return (0);
}

static int
nvdrm_kms_get_connector_property(struct drm_connector *connector,
    const struct drm_connector_state *state, struct drm_property *property,
    uint64_t *value)
{
	struct nvdrm_connector *nvconnector = to_nvdrm_connector(connector);
	const struct nvdrm_connector_state *nvstate =
	    to_nvdrm_connector_state_const(state);
	struct nvdrm_kms *kms = nvconnector->kms;

	if (property == kms->dither_mode_property)
		*value = nvstate->dither_mode;
	else if (property == kms->dither_depth_property)
		*value = nvstate->dither_depth;
	else if (property == kms->max_bpc_property)
		*value = nvstate->max_bpc;
	else if (property == kms->underscan_property)
		*value = nvstate->underscan_mode;
	else if (property == kms->underscan_hborder_property)
		*value = nvstate->underscan_hborder;
	else if (property == kms->underscan_vborder_property)
		*value = nvstate->underscan_vborder;
	else
		return (-EINVAL);
	return (0);
}

static const struct drm_connector_helper_funcs
nvdrm_kms_connector_helper_funcs = {
	.get_modes = nvdrm_kms_get_connector_modes,
	.mode_valid = nvdrm_kms_validate_connector_mode,
	.atomic_check = nvdrm_kms_check_connector,
};

static const struct drm_connector_funcs nvdrm_kms_connector_funcs = {
	.reset = nvdrm_kms_reset_connector,
	.detect = nvdrm_kms_detect_connector,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = nvdrm_kms_destroy_connector,
	.atomic_duplicate_state = nvdrm_kms_duplicate_connector_state,
	.atomic_destroy_state = nvdrm_kms_destroy_connector_state,
	.atomic_set_property = nvdrm_kms_set_connector_property,
	.atomic_get_property = nvdrm_kms_get_connector_property,
};

static void
nvdrm_kms_destroy_encoder(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
	kfree(encoder);
}

static const struct drm_encoder_funcs nvdrm_kms_encoder_funcs = {
	.destroy = nvdrm_kms_destroy_encoder,
};

static struct drm_atomic_state *
nvdrm_kms_alloc_atomic_state(struct drm_device *ddev)
{
	struct nvdrm_atomic_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state == NULL)
		return (NULL);
	if (drm_atomic_state_init(ddev, &state->base) != 0) {
		kfree(state);
		return (NULL);
	}
	return (&state->base);
}

static void
nvdrm_kms_clear_atomic_state(struct drm_atomic_state *state)
{
	struct nvdrm_atomic_state *nvstate = to_nvdrm_atomic_state(state);
	struct nvgpu_device *gpu = state->dev->dev_private;
	uint32_t head;

	for (head = 0; head < NVDRM_KMS_MAX_HEADS; head++) {
		nvgpu_display_abort_output(gpu, nvstate->prepared[head]);
		nvstate->prepared[head] = NULL;
		kfree(nvstate->color[head]);
		nvstate->color[head] = NULL;
	}
	bzero(nvstate->head_config, sizeof(nvstate->head_config));
	bzero(nvstate->head_config_valid, sizeof(nvstate->head_config_valid));
	bzero(nvstate->head_update_view, sizeof(nvstate->head_update_view));
	bzero(nvstate->head_update_dither, sizeof(nvstate->head_update_dither));
	bzero(nvstate->color_update, sizeof(nvstate->color_update));
	drm_atomic_state_default_clear(state);
}

static void
nvdrm_kms_free_atomic_state(struct drm_atomic_state *state)
{
	struct nvdrm_atomic_state *nvstate = to_nvdrm_atomic_state(state);
	uint32_t head;

	for (head = 0; head < NVDRM_KMS_MAX_HEADS; head++)
		kfree(nvstate->color[head]);
	drm_atomic_state_default_release(state);
	kfree(nvstate);
}

static int
nvdrm_kms_check_atomic(struct drm_device *ddev,
    struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	int error;
	int index;

	for_each_new_crtc_in_state(state, crtc, crtc_state, index) {
		if (!crtc_state->color_mgmt_changed ||
		    (crtc_state->degamma_lut == NULL && crtc_state->ctm == NULL))
			continue;
		error = drm_atomic_add_affected_planes(state, crtc);
		if (error != 0)
			return (error);
	}
	return (drm_atomic_helper_check(ddev, state));
}

static void
nvdrm_kms_run_commit(struct work_struct *work)
{
	struct drm_atomic_state *state = container_of(work,
	    struct drm_atomic_state, commit_work);

	(void)drm_atomic_helper_wait_for_fences(state->dev, state, false);
	drm_atomic_helper_wait_for_dependencies(state);
	nvdrm_kms_commit_tail(state);
	drm_atomic_helper_commit_cleanup_done(state);
	drm_atomic_state_put(state);
}

static void
nvdrm_kms_commit_tail(struct drm_atomic_state *state)
{
	struct nvdrm_atomic_state *nvstate = to_nvdrm_atomic_state(state);
	struct nvdrm_kms *kms = nvgpu_device_get_kms(state->dev->dev_private);
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	uint32_t head;
	int index;

	drm_atomic_helper_commit_modeset_disables(state->dev, state);
	drm_atomic_helper_commit_planes(state->dev, state,
	    DRM_PLANE_COMMIT_NO_DISABLE_AFTER_MODESET);
	drm_atomic_helper_commit_modeset_enables(state->dev, state);
	for (head = 0; head < NVDRM_KMS_MAX_HEADS; head++) {
		int error;

		if (!nvstate->head_config_valid[head] ||
		    (!nvstate->head_update_view[head] &&
		    !nvstate->head_update_dither[head]))
			continue;
		error = nvgpu_display_update_head(state->dev->dev_private, head,
		    &nvstate->head_config[head], nvstate->head_update_view[head],
		    nvstate->head_update_dither[head]);
		if (error != 0)
			nvgpu_log(NVGPU_LOG_INFO,
			    "HEAD property update failed head=%u error=%d\n",
			    head, error);
	}
	for (head = 0; head < NVDRM_KMS_MAX_HEADS; head++) {
		struct nvdrm_crtc *nvcrtc;
		int error;

		if (!nvstate->color_update[head] || nvstate->color[head] == NULL ||
		    kms == NULL || head >= kms->head_count || kms->crtcs[head] == NULL)
			continue;
		nvcrtc = to_nvdrm_crtc(kms->crtcs[head]);
		error = nvgpu_display_update_color(state->dev->dev_private, head,
		    nvcrtc->window, &nvstate->color[head]->base);
		if (error != 0)
			nvgpu_log(NVGPU_LOG_INFO,
			    "CRTC color update failed head=%u error=%d\n", head, error);
	}

	for_each_new_crtc_in_state(state, crtc, crtc_state, index) {
		unsigned long flags;

		if (crtc_state->event == NULL)
			continue;
		if (crtc_state->active)
			drm_crtc_accurate_vblank_count(crtc);
		spin_lock_irqsave(&state->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, crtc_state->event);
		crtc_state->event = NULL;
		spin_unlock_irqrestore(&state->dev->event_lock, flags);
	}
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_wait_for_flip_done(state->dev, state);

	for (head = 0; head < NVDRM_KMS_MAX_HEADS; head++) {
		struct nvdrm_pending_flip *pending = &nvstate->flips[head];
		struct drm_pending_vblank_event *event;
		struct drm_crtc *pending_crtc;
		unsigned long flags;

		if (pending->event == NULL ||
		    !nvgpu_display_cancel_pageflip(state->dev->dev_private, head,
		    pending))
			continue;
		pending_crtc = pending->crtc;
		event = pending->event;
		pending->crtc = NULL;
		pending->event = NULL;
		nvgpu_log(NVGPU_LOG_INFO,
		    "pageflip notifier timed out head=%u\n", head);
		drm_crtc_accurate_vblank_count(pending_crtc);
		spin_lock_irqsave(&state->dev->event_lock, flags);
		drm_crtc_send_vblank_event(pending_crtc, event);
		spin_unlock_irqrestore(&state->dev->event_lock, flags);
		drm_crtc_vblank_put(pending_crtc);
	}
	drm_atomic_helper_cleanup_planes(state->dev, state);
}

static int
nvdrm_kms_commit_atomic(struct drm_device *ddev,
    struct drm_atomic_state *state, bool nonblock)
{
	struct nvdrm_atomic_state *nvstate = to_nvdrm_atomic_state(state);
	struct nvgpu_device *gpu = ddev->dev_private;
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	uint32_t head;
	int error;
	int index;

	if (state->async_update)
		return (drm_atomic_helper_commit(ddev, state, nonblock));
	error = drm_atomic_helper_setup_commit(state, nonblock);
	if (error != 0)
		return (error);
	INIT_WORK(&state->commit_work, nvdrm_kms_run_commit);
	error = drm_atomic_helper_prepare_planes(ddev, state);
	if (error != 0)
		return (error);
	if (!nonblock) {
		error = drm_atomic_helper_wait_for_fences(ddev, state, true);
		if (error != 0)
			goto fail;
	}

	for_each_new_crtc_in_state(state, crtc, crtc_state, index) {
		struct drm_connector *connector = NULL;
		struct drm_connector_state *connector_state;
		struct drm_connector_state *old_connector_state;
		struct drm_plane_state *primary_state;
		struct nvdrm_connector_state *nvconnector_state;
		const struct nvdrm_connector_state *old_nvconnector_state;
		struct nvgpu_display_head_config *head_config;
		struct nvgpu_display_output_config output_config;
		struct nvgpu_display_mode display_mode;
		struct nvdrm_crtc *nvcrtc;
		uint32_t scanout_depth = 24;
		uint32_t dither_mode;
		bool underscan;
		int connector_index;

		if (!crtc_state->active)
			continue;
		nvcrtc = to_nvdrm_crtc(crtc);
		head = nvcrtc->head;
		if (head >= NVDRM_KMS_MAX_HEADS) {
			error = -EINVAL;
			goto fail;
		}
		for_each_new_connector_in_state(state, connector,
		    connector_state, connector_index) {
			if (connector_state->crtc == crtc)
				break;
			connector = NULL;
		}
		if (connector == NULL) {
			struct drm_connector *candidate;

			list_for_each_entry(candidate,
			    &ddev->mode_config.connector_list, head) {
				if (candidate->state != NULL &&
				    candidate->state->crtc == crtc) {
					connector = candidate;
					break;
				}
			}
		}
		if (connector == NULL) {
			error = -ENODEV;
			goto fail;
		}
		connector_state = drm_atomic_get_new_connector_state(state, connector);
		if (connector_state == NULL)
			connector_state = connector->state;
		if (connector_state == NULL) {
			error = -EINVAL;
			goto fail;
		}
		old_connector_state = drm_atomic_get_old_connector_state(state,
		    connector);
		nvconnector_state = to_nvdrm_connector_state(connector_state);
		old_nvconnector_state = old_connector_state != NULL ?
		    to_nvdrm_connector_state_const(old_connector_state) : NULL;
		drm_mode_set_crtcinfo(&crtc_state->adjusted_mode,
		    CRTC_INTERLACE_HALVE_V | CRTC_STEREO_DOUBLE);
		head_config = &nvstate->head_config[head];
		bzero(head_config, sizeof(*head_config));
		head_config->mode.interlace =
		    (crtc_state->adjusted_mode.flags & DRM_MODE_FLAG_INTERLACE) != 0;
		head_config->mode.clock_khz = crtc_state->adjusted_mode.crtc_clock;
		head_config->mode.hdisplay = crtc_state->adjusted_mode.crtc_hdisplay;
		head_config->mode.hsync_start =
		    crtc_state->adjusted_mode.crtc_hsync_start;
		head_config->mode.hsync_end =
		    crtc_state->adjusted_mode.crtc_hsync_end;
		head_config->mode.hblank_end =
		    crtc_state->adjusted_mode.crtc_hblank_end;
		head_config->mode.htotal = crtc_state->adjusted_mode.crtc_htotal;
		head_config->mode.vdisplay = crtc_state->adjusted_mode.crtc_vdisplay;
		head_config->mode.vsync_start =
		    crtc_state->adjusted_mode.crtc_vsync_start;
		head_config->mode.vsync_end =
		    crtc_state->adjusted_mode.crtc_vsync_end;
		head_config->mode.vblank_end =
		    crtc_state->adjusted_mode.crtc_vblank_end;
		head_config->mode.vtotal = crtc_state->adjusted_mode.crtc_vtotal;
		head_config->mode.negative_hsync =
		    (crtc_state->adjusted_mode.flags & DRM_MODE_FLAG_NHSYNC) != 0;
		head_config->mode.negative_vsync =
		    (crtc_state->adjusted_mode.flags & DRM_MODE_FLAG_NVSYNC) != 0;
		head_config->input_width = crtc_state->mode.hdisplay;
		head_config->input_height = crtc_state->mode.vdisplay;
		head_config->output_width = crtc_state->adjusted_mode.hdisplay;
		head_config->output_height = crtc_state->adjusted_mode.vdisplay;
		underscan = nvconnector_state->underscan_mode ==
		    NVGPU_DISPLAY_UNDERSCAN_ON ||
		    (nvconnector_state->underscan_mode ==
		    NVGPU_DISPLAY_UNDERSCAN_AUTO &&
		    connector->display_info.has_hdmi_infoframe);
		if (underscan && head_config->output_width != 0 &&
		    head_config->output_height != 0) {
			uint32_t original_width = head_config->output_width;
			uint32_t original_height = head_config->output_height;
			uint32_t border = nvconnector_state->underscan_hborder;

			if (border == 0)
				border = (head_config->output_width >> 4) + 32u;
			head_config->output_width = original_width > border * 2u ?
			    original_width - border * 2u : 1;
			border = nvconnector_state->underscan_vborder;
			if (border != 0)
				head_config->output_height = original_height > border * 2u ?
				    original_height - border * 2u : 1;
			else
				head_config->output_height =
				    (uint64_t)head_config->output_width * original_height /
				    original_width;
		}
		if (connector_state->scaling_mode == DRM_MODE_SCALE_CENTER) {
			head_config->output_width = MIN(head_config->input_width,
			    head_config->output_width);
			head_config->output_height = MIN(head_config->input_height,
			    head_config->output_height);
		} else if (connector_state->scaling_mode == DRM_MODE_SCALE_ASPECT &&
		    head_config->input_width != 0 && head_config->input_height != 0 &&
		    head_config->output_width != 0 && head_config->output_height != 0) {
			if ((uint64_t)head_config->output_width *
			    head_config->input_height >
			    (uint64_t)head_config->input_width *
			    head_config->output_height)
				head_config->output_width =
				    (uint64_t)head_config->output_height *
				    head_config->input_width / head_config->input_height;
			else
				head_config->output_height =
				    (uint64_t)head_config->output_width *
				    head_config->input_height / head_config->input_width;
		}
		head_config->bpc = nvconnector_state->max_bpc;
		primary_state = drm_atomic_get_new_plane_state(state, crtc->primary);
		if (primary_state == NULL)
			primary_state = crtc->primary->state;
		if (primary_state != NULL && primary_state->fb != NULL &&
		    primary_state->fb->format != NULL &&
		    primary_state->fb->format->depth != 0)
			scanout_depth = primary_state->fb->format->depth;
		dither_mode = nvconnector_state->dither_mode;
		if (dither_mode == NVGPU_DISPLAY_DITHER_MODE_AUTO)
			dither_mode = scanout_depth > head_config->bpc * 3u ?
			    NVGPU_DISPLAY_DITHER_MODE_DYNAMIC_2X2 :
			    NVGPU_DISPLAY_DITHER_MODE_OFF;
		head_config->dither_enabled =
		    dither_mode != NVGPU_DISPLAY_DITHER_MODE_OFF;
		head_config->dither_bits = nvconnector_state->dither_depth ==
		    NVGPU_DISPLAY_DITHER_DEPTH_6_BPC ? 6 : 8;
		head_config->dither_mode =
		    dither_mode == NVGPU_DISPLAY_DITHER_MODE_STATIC_2X2 ? 3 :
		    dither_mode == NVGPU_DISPLAY_DITHER_MODE_TEMPORAL ? 4 : 2;
		nvstate->head_config_valid[head] = true;
		if (drm_atomic_crtc_needs_modeset(crtc_state) ||
		    crtc_state->color_mgmt_changed) {
			struct nvdrm_color_config *color;
			const struct drm_color_lut *lut;
			uint32_t color_index;

			color = kzalloc(sizeof(*color), GFP_KERNEL);
			if (color == NULL) {
				error = -ENOMEM;
				goto fail;
			}
			kfree(nvstate->color[head]);
			nvstate->color[head] = color;
			if (crtc_state->degamma_lut != NULL) {
				color->base.degamma_count =
				    drm_color_lut_size(crtc_state->degamma_lut);
				color->base.degamma = color->degamma;
				lut = crtc_state->degamma_lut->data;
				for (color_index = 0;
				    color_index < color->base.degamma_count; color_index++) {
					color->degamma[color_index].red = lut[color_index].red;
					color->degamma[color_index].green = lut[color_index].green;
					color->degamma[color_index].blue = lut[color_index].blue;
				}
			}
			if (crtc_state->gamma_lut != NULL) {
				color->base.gamma_count =
				    drm_color_lut_size(crtc_state->gamma_lut);
				color->base.gamma = color->gamma;
				lut = crtc_state->gamma_lut->data;
				for (color_index = 0;
				    color_index < color->base.gamma_count; color_index++) {
					color->gamma[color_index].red = lut[color_index].red;
					color->gamma[color_index].green = lut[color_index].green;
					color->gamma[color_index].blue = lut[color_index].blue;
				}
			}
			if (crtc_state->ctm != NULL) {
				const struct drm_color_ctm *ctm = crtc_state->ctm->data;

				color->base.ctm_enabled = true;
				memcpy(color->base.ctm, ctm->matrix,
				    sizeof(color->base.ctm));
			}
			nvstate->color_update[head] = true;
		}
		if (!drm_atomic_crtc_needs_modeset(crtc_state) &&
		    old_nvconnector_state != NULL) {
			nvstate->head_update_view[head] =
			    old_nvconnector_state->base.scaling_mode !=
			    nvconnector_state->base.scaling_mode ||
			    old_nvconnector_state->underscan_mode !=
			    nvconnector_state->underscan_mode ||
			    old_nvconnector_state->underscan_hborder !=
			    nvconnector_state->underscan_hborder ||
			    old_nvconnector_state->underscan_vborder !=
			    nvconnector_state->underscan_vborder;
			nvstate->head_update_dither[head] =
			    old_nvconnector_state->dither_mode !=
			    nvconnector_state->dither_mode ||
			    old_nvconnector_state->dither_depth !=
			    nvconnector_state->dither_depth ||
			    old_nvconnector_state->max_bpc != nvconnector_state->max_bpc;
		}
		if (!drm_atomic_crtc_needs_modeset(crtc_state))
			continue;
		bzero(&display_mode, sizeof(display_mode));
		display_mode.interlace =
		    (crtc_state->adjusted_mode.flags & DRM_MODE_FLAG_INTERLACE) != 0;
		display_mode.clock_khz = crtc_state->adjusted_mode.crtc_clock;
		display_mode.hdisplay = crtc_state->adjusted_mode.crtc_hdisplay;
		display_mode.hsync_start = crtc_state->adjusted_mode.crtc_hsync_start;
		display_mode.hsync_end = crtc_state->adjusted_mode.crtc_hsync_end;
		display_mode.hblank_end = crtc_state->adjusted_mode.crtc_hblank_end;
		display_mode.htotal = crtc_state->adjusted_mode.crtc_htotal;
		display_mode.vdisplay = crtc_state->adjusted_mode.crtc_vdisplay;
		display_mode.vsync_start = crtc_state->adjusted_mode.crtc_vsync_start;
		display_mode.vsync_end = crtc_state->adjusted_mode.crtc_vsync_end;
		display_mode.vblank_end = crtc_state->adjusted_mode.crtc_vblank_end;
		display_mode.vtotal = crtc_state->adjusted_mode.crtc_vtotal;
		display_mode.negative_hsync =
		    (crtc_state->adjusted_mode.flags & DRM_MODE_FLAG_NHSYNC) != 0;
		display_mode.negative_vsync =
		    (crtc_state->adjusted_mode.flags & DRM_MODE_FLAG_NVSYNC) != 0;
		bzero(&output_config, sizeof(output_config));
		output_config.bpc = nvconnector_state->max_bpc;
		output_config.hdmi_scdc_supported =
		    connector->display_info.hdmi.scdc.supported;
		output_config.hdmi_scrambling_supported =
		    connector->display_info.hdmi.scdc.scrambling.supported;
		output_config.hdmi_low_rate_scrambling_supported =
		    connector->display_info.hdmi.scdc.scrambling.low_rates;
		if (to_nvdrm_connector(connector)->has_audio &&
		    (connector->eld[DRM_ELD_VER] & DRM_ELD_VER_MASK) != 0) {
			int eld_size = drm_eld_size(connector->eld);

			if (eld_size > 0 && eld_size <= NVGPU_DISPLAY_ELD_SIZE) {
				output_config.audio_enabled = true;
				output_config.eld_size = eld_size;
				memcpy(output_config.eld, connector->eld, eld_size);
			}
		}
		nvgpu_display_abort_output(gpu, nvstate->prepared[head]);
		nvstate->prepared[head] = NULL;
		error = nvgpu_display_prepare_output(gpu, head,
		    to_nvdrm_connector(connector)->display_id, &display_mode,
		    &output_config, &nvstate->prepared[head]);
		if (error != 0) {
			error = error < 0 ? error : -error;
			goto fail;
		}
	}

	error = drm_atomic_helper_swap_state(state, true);
	if (error != 0)
		goto fail;
	drm_atomic_state_get(state);
	if (nonblock) {
		if (!queue_work(system_unbound_wq, &state->commit_work))
			nvdrm_kms_run_commit(&state->commit_work);
	} else {
		nvdrm_kms_run_commit(&state->commit_work);
	}
	return (0);

fail:
	for (head = 0; head < NVDRM_KMS_MAX_HEADS; head++) {
		nvgpu_display_abort_output(gpu, nvstate->prepared[head]);
		nvstate->prepared[head] = NULL;
	}
	drm_atomic_helper_cleanup_planes(ddev, state);
	return (error);
}

static void
nvdrm_kms_complete_vblank(void *arg, uint32_t head)
{
	struct nvdrm_kms *kms = arg;

	if (kms != NULL && head < kms->head_count && kms->crtcs[head] != NULL)
		drm_crtc_handle_vblank(kms->crtcs[head]);
}

static void
nvdrm_kms_complete_pageflip(void *arg, uint32_t head, void *cookie)
{
	struct nvdrm_kms *kms = arg;
	struct nvdrm_pending_flip *pending = cookie;
	struct drm_pending_vblank_event *event;
	struct drm_crtc *crtc;
	unsigned long flags;

	if (kms == NULL || pending == NULL || head >= kms->head_count ||
	    kms->crtcs[head] == NULL)
		return;
	crtc = pending->crtc;
	event = pending->event;
	if (crtc == NULL || event == NULL)
		return;
	drm_crtc_accurate_vblank_count(crtc);
	spin_lock_irqsave(&kms->ddev->event_lock, flags);
	drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&kms->ddev->event_lock, flags);
	drm_crtc_vblank_put(crtc);
}
