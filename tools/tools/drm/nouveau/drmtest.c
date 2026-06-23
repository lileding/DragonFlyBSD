#include <sys/event.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_NOUVEAU_CHANNEL_ALLOC	0x02
#define DRM_NOUVEAU_CHANNEL_FREE	0x03
#define DRM_NOUVEAU_EXEC		0x12

#define NOUVEAU_FIFO_ENGINE_GR		0x01
#define DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ	0x1

#define NVKM_DRMTEST_UNDERSCAN_OFF	0
#define NVKM_DRMTEST_UNDERSCAN_ON	1

struct drm_nouveau_channel_alloc {
	uint32_t fb_ctxdma_handle;
	uint32_t tt_ctxdma_handle;
	int32_t channel;
	uint32_t pushbuf_domains;
	uint32_t notifier_handle;
	struct {
		uint32_t handle;
		uint32_t grclass;
	} subchan[8];
	uint32_t nr_subchan;
};

struct drm_nouveau_channel_free {
	int32_t channel;
};

struct drm_nouveau_sync {
	uint32_t flags;
	uint32_t handle;
	uint64_t timeline_value;
};

struct drm_nouveau_exec {
	uint32_t channel;
	uint32_t push_count;
	uint32_t wait_count;
	uint32_t sig_count;
	uint64_t wait_ptr;
	uint64_t sig_ptr;
	uint64_t push_ptr;
};

#define DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_ALLOC, \
	    struct drm_nouveau_channel_alloc)
#define DRM_IOCTL_NOUVEAU_CHANNEL_FREE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NOUVEAU_CHANNEL_FREE, \
	    struct drm_nouveau_channel_free)
#define DRM_IOCTL_NOUVEAU_EXEC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NOUVEAU_EXEC, struct drm_nouveau_exec)

static int failures;

struct atomic_plane_snapshot {
	uint64_t fb_id;
	uint64_t crtc_id;
	uint64_t crtc_x;
	uint64_t crtc_y;
	uint64_t crtc_w;
	uint64_t crtc_h;
	uint64_t src_x;
	uint64_t src_y;
	uint64_t src_w;
	uint64_t src_h;
};

struct exec_sync_file {
	int fd;
	bool was_pending;
};

struct cursor_counter_snapshot {
	uint64_t plane_update_count;
	uint64_t cursor_update_count;
	uint64_t cursor_async_update_count;
	uint64_t cursor_disable_count;
	uint64_t cursor_error_count;
	uint64_t cursor_pin_count;
	uint64_t cursor_unpin_count;
	uint64_t atomic_last_legacy_cursor_update;
	uint64_t atomic_last_async_update;
	uint64_t head_cursor_enabled;
	uint64_t head_cursor_fb;
	uint64_t head_cursor_bo;
};

struct pageflip_counter_snapshot {
	uint64_t page_flip_count;
	uint64_t page_flip_event_count;
	uint64_t page_flip_reject_count;
	uint64_t page_flip_error_count;
	uint64_t commit_error_count;
	uint64_t atomic_tail_active;
	uint64_t atomic_tail_stage;
	uint64_t display_audit_pending_valid;
};

struct pageflip_event_state {
	uint32_t count;
	unsigned int sequence;
	unsigned int tv_sec;
	unsigned int tv_usec;
	unsigned int crtc_id;
};

struct sequence_event_state {
	uint32_t count;
	uint64_t sequence;
	uint64_t ns;
	uint64_t user_data;
};

struct raw_vblank_event {
	struct drm_event_vblank event;
};

struct modeset_counter_snapshot {
	uint64_t atomic_tail_disable_op_count;
	uint64_t atomic_tail_enable_op_count;
	uint64_t atomic_tail_last_disable_op_count;
	uint64_t atomic_tail_last_enable_op_count;
	uint64_t atomic_tail_last_disable_heads;
	uint64_t atomic_tail_last_enable_heads;
	uint64_t atomic_tail_last_new_active_heads;
	uint64_t atomic_disable_vblank_off_count;
	uint64_t atomic_disable_vblank_keep_count;
	uint64_t commit_error_count;
	uint64_t atomic_tail_active;
	uint64_t atomic_tail_stage;
	uint64_t display_audit_pending_valid;
	char display_audit_current_op[32];
	char display_audit_pending_op[32];
};

struct color_counter_snapshot {
	uint64_t atomic_tail_color_op_count;
	uint64_t atomic_tail_last_color_op_count;
	uint64_t color_degamma_lut_count;
	uint64_t color_ctm_count;
	uint64_t color_gamma_lut_count;
	uint64_t commit_error_count;
	uint64_t atomic_tail_active;
	uint64_t atomic_tail_stage;
	uint64_t display_audit_pending_valid;
};

static bool atomic_add_plane_property(int fd, drmModeAtomicReqPtr req,
    uint32_t plane_id, const char *name, uint64_t value);
static bool atomic_add_connector_property(int fd, drmModeAtomicReqPtr req,
    uint32_t connector_id, const char *name, uint64_t value);
static bool create_dumb_buffer_for(int fd, uint32_t width, uint32_t height,
    uint32_t bpp, uint32_t *handle_out, uint32_t *pitch_out,
    const char *what);
static void destroy_dumb_buffer_for(int fd, uint32_t handle,
    const char *what);
static bool clear_dumb_buffer(int fd, uint32_t handle, uint32_t pitch,
    uint32_t height, const char *what);
static bool add_linear_framebuffer(int fd, uint32_t width, uint32_t height,
    uint32_t format, uint32_t handle, uint32_t pitch, uint32_t *fb_id_out,
    const char *what);
static void remove_framebuffer(int fd, uint32_t fb_id, const char *what);
static void close_fb2_handles(int fd, const drmModeFB2 *fb2,
    const char *what);
static bool read_color_counter_snapshot(
    struct color_counter_snapshot *snapshot, const char *stage);
static bool read_pageflip_counter_snapshot(
    struct pageflip_counter_snapshot *snapshot, const char *stage);
static bool read_connector_fill_modes_count(uint64_t *count_out,
    const char *stage);

static void
check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

static void
check_drm_cap(int fd, uint64_t capability, uint64_t expected,
    const char *name)
{
	uint64_t value = 0;
	char text[160];
	int ret;

	errno = 0;
	ret = drmGetCap(fd, capability, &value);
	snprintf(text, sizeof(text), "DRM cap %s is readable", name);
	check(ret == 0, text);
	if (ret != 0) {
		printf("    errno=%d\n", errno);
		return;
	}

	printf("    %s=%llu\n", name, (unsigned long long)value);
	snprintf(text, sizeof(text), "DRM cap %s is %llu", name,
	    (unsigned long long)expected);
	check(value == expected, text);
}

static void
check_client_cap(int fd, uint64_t capability, const char *name)
{
	char text[160];

	errno = 0;
	snprintf(text, sizeof(text), "DRM client cap %s is accepted", name);
	check(drmSetClientCap(fd, capability, 1) == 0, text);
	if (errno != 0)
		printf("    errno=%d\n", errno);
}

static void
check_client_cap_error(int fd, uint64_t capability, int expected_errno,
    const char *name)
{
	char text[160];
	int saved_errno;
	int ret;

	errno = 0;
	ret = drmSetClientCap(fd, capability, 1);
	saved_errno = errno;
	snprintf(text, sizeof(text), "DRM client cap %s is rejected", name);
	check(ret != 0, text);
	if (ret == 0)
		return;

	printf("    %s errno=%d\n", name, saved_errno);
	snprintf(text, sizeof(text), "DRM client cap %s fails with errno %d",
	    name, expected_errno);
	check(saved_errno == expected_errno, text);
}

static void
check_client_cap_value_error(int fd, uint64_t capability, uint64_t value,
    int expected_errno, const char *name)
{
	char text[192];
	int saved_errno;
	int ret;

	errno = 0;
	ret = drmSetClientCap(fd, capability, value);
	saved_errno = errno;
	snprintf(text, sizeof(text), "DRM client cap %s value %llu is rejected",
	    name, (unsigned long long)value);
	check(ret != 0, text);
	if (ret == 0)
		return;

	printf("    %s value=%llu errno=%d\n", name,
	    (unsigned long long)value, saved_errno);
	snprintf(text, sizeof(text),
	    "DRM client cap %s value %llu fails with errno %d", name,
	    (unsigned long long)value, expected_errno);
	check(saved_errno == expected_errno, text);
}

static void
check_mode_config_contract(int fd, const drmModeRes *resources)
{
	printf("mode_config: min=%ux%u max=%ux%u\n",
	    resources->min_width, resources->min_height,
	    resources->max_width, resources->max_height);
	check(resources->min_width == 1, "mode_config min_width is 1");
	check(resources->min_height == 1, "mode_config min_height is 1");
	check(resources->max_width == 16384, "mode_config max_width is 16384");
	check(resources->max_height == 16384,
	    "mode_config max_height is 16384");

	check_drm_cap(fd, DRM_CAP_DUMB_PREFERRED_DEPTH, 24,
	    "DUMB_PREFERRED_DEPTH");
	check_drm_cap(fd, DRM_CAP_DUMB_PREFER_SHADOW, 1,
	    "DUMB_PREFER_SHADOW");
	check_drm_cap(fd, DRM_CAP_DUMB_BUFFER, 1, "DUMB_BUFFER");
	check_drm_cap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, 1,
	    "TIMESTAMP_MONOTONIC");
	check_drm_cap(fd, DRM_CAP_VBLANK_HIGH_CRTC, 1,
	    "VBLANK_HIGH_CRTC");
	check_drm_cap(fd, DRM_CAP_ASYNC_PAGE_FLIP, 0,
	    "ASYNC_PAGE_FLIP");
	check_drm_cap(fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, 0,
	    "ATOMIC_ASYNC_PAGE_FLIP");
	check_drm_cap(fd, DRM_CAP_PAGE_FLIP_TARGET, 0,
	    "PAGE_FLIP_TARGET");
	check_drm_cap(fd, DRM_CAP_CURSOR_WIDTH, 256, "CURSOR_WIDTH");
	check_drm_cap(fd, DRM_CAP_CURSOR_HEIGHT, 256, "CURSOR_HEIGHT");
	check_drm_cap(fd, DRM_CAP_ADDFB2_MODIFIERS, 1,
	    "ADDFB2_MODIFIERS");
	check_drm_cap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, 1,
	    "CRTC_IN_VBLANK_EVENT");
	check_drm_cap(fd, DRM_CAP_SYNCOBJ, 1, "SYNCOBJ");
	check_drm_cap(fd, DRM_CAP_SYNCOBJ_TIMELINE, 1,
	    "SYNCOBJ_TIMELINE");
	check_drm_cap(fd, DRM_CAP_PRIME,
	    DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT, "PRIME");
	check_client_cap(fd, DRM_CLIENT_CAP_STEREO_3D, "STEREO_3D");
}

/*
 * check_deprecated_master_mode_ioctl_contract()
 *
 * Ownership:
 *   Borrows a connected connector object ID.  No KMS object or GEM handle is
 *   created, retained, or destroyed by this helper.
 *
 * Lifetime:
 *   MODE_ATTACHMODE and MODE_DETACHMODE are deprecated no-op ioctls in this
 *   DRM core.  This helper only proves the master-visible ABI result and does
 *   not change the connector's mode list or active route.
 *
 * Threading:
 *   Single-threaded probe.  The caller must pass the current DRM master fd so
 *   the DRM_MASTER gate is not the result under test.
 */
static void
check_deprecated_master_mode_ioctl_contract(int fd, uint32_t connector_id)
{
	struct drm_mode_mode_cmd mode_cmd;
	int saved_errno;
	int ret;

	memset(&mode_cmd, 0, sizeof(mode_cmd));
	mode_cmd.connector_id = connector_id;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_ATTACHMODE, &mode_cmd);
	saved_errno = errno;
	check(ret == 0, "master legacy AttachMode no-op succeeds");
	if (ret != 0)
		printf("    master legacy AttachMode errno=%d\n", saved_errno);

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_DETACHMODE, &mode_cmd);
	saved_errno = errno;
	check(ret == 0, "master legacy DetachMode no-op succeeds");
	if (ret != 0)
		printf("    master legacy DetachMode errno=%d\n", saved_errno);
}

static bool
raw_getconnector_for_reprobe(int fd, uint32_t connector_id, const char *label,
    uint32_t *count_modes_out)
{
	struct drm_mode_get_connector get_connector;
	char text[192];
	int saved_errno;
	int ret;

	*count_modes_out = 0;
	memset(&get_connector, 0, sizeof(get_connector));
	get_connector.connector_id = connector_id;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &get_connector);
	saved_errno = errno;
	snprintf(text, sizeof(text), "%s GETCONNECTOR count_modes=0 succeeds",
	    label);
	check(ret == 0, text);
	if (ret != 0) {
		printf("    %s GETCONNECTOR errno=%d\n", label, saved_errno);
		return false;
	}

	snprintf(text, sizeof(text), "%s GETCONNECTOR returns requested connector",
	    label);
	check(get_connector.connector_id == connector_id, text);
	snprintf(text, sizeof(text), "%s GETCONNECTOR returns cached modes",
	    label);
	check(get_connector.count_modes > 0, text);
	*count_modes_out = get_connector.count_modes;
	return true;
}

/*
 * check_getconnector_reprobe_master_contract()
 *
 * Ownership:
 *   Borrows a connected connector object ID from the caller.  Opens one
 *   secondary DRM fd and closes it before return.  The helper does not create,
 *   retain, or destroy any KMS object.
 *
 * Lifetime:
 *   The fill_modes counter is a diagnostic snapshot from dev.drm.0.state.  It
 *   is only compared across the immediately adjacent raw GETCONNECTOR calls.
 *   A later hotplug, modeset owner, or probe may legitimately change it.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  The caller keeps the original fd as the
 *   current master.  The secondary fd is intentionally a non-current-master
 *   reader and must not trigger the connector fill_modes path.
 */
static void
check_getconnector_reprobe_master_contract(int master_fd,
    uint32_t connector_id)
{
	uint64_t before_master;
	uint64_t after_master;
	uint64_t before_non_master;
	uint64_t after_non_master;
	uint32_t mode_count;
	int secondary_fd;

	if (!read_connector_fill_modes_count(&before_master,
	    "master GETCONNECTOR reprobe"))
		return;
	if (raw_getconnector_for_reprobe(master_fd, connector_id, "master",
	    &mode_count)) {
		if (read_connector_fill_modes_count(&after_master,
		    "master GETCONNECTOR reprobe result")) {
			check(after_master > before_master,
			    "master GETCONNECTOR count_modes=0 triggers connector fill_modes");
		}
	}

	secondary_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	check(secondary_fd >= 0, "non-master GETCONNECTOR opens secondary card fd");
	if (secondary_fd < 0)
		return;

	if (!read_connector_fill_modes_count(&before_non_master,
	    "non-master GETCONNECTOR reprobe")) {
		check(close(secondary_fd) == 0,
		    "non-master GETCONNECTOR closes secondary card fd");
		return;
	}
	if (raw_getconnector_for_reprobe(secondary_fd, connector_id,
	    "non-master", &mode_count)) {
		if (read_connector_fill_modes_count(&after_non_master,
		    "non-master GETCONNECTOR reprobe result")) {
			check(after_non_master == before_non_master,
			    "non-master GETCONNECTOR count_modes=0 does not trigger connector fill_modes");
		}
	}
	check(close(secondary_fd) == 0,
	    "non-master GETCONNECTOR closes secondary card fd");
}

static const char *
connector_status_name(int status)
{
	switch (status) {
	case DRM_MODE_CONNECTED:
		return "connected";
	case DRM_MODE_DISCONNECTED:
		return "disconnected";
	case DRM_MODE_UNKNOWNCONNECTION:
		return "unknown";
	default:
		return "invalid";
	}
}

static const char *
property_type_name(uint32_t flags)
{
	if (flags & DRM_MODE_PROP_RANGE)
		return "range";
	if (flags & DRM_MODE_PROP_ENUM)
		return "enum";
	if (flags & DRM_MODE_PROP_BITMASK)
		return "bitmask";
	if (flags & DRM_MODE_PROP_BLOB)
		return "blob";
	if (flags & DRM_MODE_PROP_OBJECT)
		return "object";
	if (flags & DRM_MODE_PROP_SIGNED_RANGE)
		return "signed-range";
	return "unknown";
}

static bool
id_in_list(const uint32_t *ids, int count, uint32_t id)
{
	for (int i = 0; i < count; i++) {
		if (ids[i] == id)
			return true;
	}
	return false;
}

/*
 * id_index_in_list()
 *
 * Ownership:
 *   Borrows a libdrm-owned ID array and writes the matching index to the
 *   caller-owned out parameter when the ID is found.
 *
 * Lifetime:
 *   The returned index is valid only for the same resources snapshot that owns
 *   ids.  It must not be reused after a later hotplug or resources refresh.
 *
 * Threading:
 *   Single-threaded read-only helper.  No driver-private locks are held.
 */
static bool
id_index_in_list(const uint32_t *ids, int count, uint32_t id, int *index_out)
{
	for (int i = 0; i < count; i++) {
		if (ids[i] != id)
			continue;
		*index_out = i;
		return true;
	}
	return false;
}

/*
 * check_unique_ids()
 *
 * Ownership:
 *   Borrows a libdrm-owned ID array for the duration of the check.  The helper
 *   does not take references to DRM objects and does not close or free the
 *   array.
 *
 * Lifetime:
 *   Valid only for the current libdrm resources snapshot.  Later hotplug or
 *   modeset activity may legitimately change future snapshots.
 *
 * Threading:
 *   Single-threaded read-only UAPI validation.  No driver-private locks are
 *   held or required.
 */
static void
check_unique_ids(const uint32_t *ids, int count, const char *message)
{
	bool unique = true;

	for (int i = 0; i < count; i++) {
		for (int j = i + 1; j < count; j++) {
			if (ids[i] != ids[j])
				continue;
			printf("    duplicate id %u at indexes %d and %d\n",
			    ids[i], i, j);
			unique = false;
		}
	}
	check(unique, message);
}

static const char *
fourcc_name(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
		return "XRGB8888";
	case DRM_FORMAT_ARGB8888:
		return "ARGB8888";
	case DRM_FORMAT_RGB565:
		return "RGB565";
	default:
		return "other";
	}
}

static const char *
modifier_name(uint64_t modifier)
{
	if (modifier == DRM_FORMAT_MOD_LINEAR)
		return "LINEAR";
	if ((modifier >> 56) == DRM_FORMAT_MOD_VENDOR_NVIDIA)
		return "NVIDIA";
	return "other";
}

static drmModePropertyPtr
get_property_by_name(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, uint64_t *value)
{
	drmModeObjectPropertiesPtr props;
	drmModePropertyPtr prop = NULL;

	props = drmModeObjectGetProperties(fd, object_id, object_type);
	if (props == NULL)
		return NULL;

	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyPtr candidate;

		candidate = drmModeGetProperty(fd, props->props[i]);
		if (candidate == NULL)
			continue;
		if (strcmp(candidate->name, name) == 0) {
			if (value != NULL)
				*value = props->prop_values[i];
			prop = candidate;
			break;
		}
		drmModeFreeProperty(candidate);
	}

	drmModeFreeObjectProperties(props);
	return prop;
}

static int
get_plane_type(int fd, uint32_t plane_id)
{
	drmModePropertyPtr prop;
	uint64_t value = 0;

	prop = get_property_by_name(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "type", &value);
	if (prop == NULL)
		return -1;
	drmModeFreeProperty(prop);
	return (int)value;
}

/*
 * check_legacy_plane_visibility_without_universal_cap()
 *
 * Ownership:
 *   Borrows the DRM fd before DRM_CLIENT_CAP_UNIVERSAL_PLANES is enabled.
 *   Plane resource snapshots are owned by this helper and freed before return.
 *
 * Lifetime:
 *   Valid only before the caller toggles UNIVERSAL_PLANES on this fd.  The
 *   probe does not retain plane IDs or property pointers after it returns.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  It only reads plane resources and property
 *   metadata; it must not mutate plane state.
 */
static void
check_legacy_plane_visibility_without_universal_cap(int fd)
{
	drmModePlaneResPtr plane_resources;
	bool no_primary_or_cursor = true;

	plane_resources = drmModeGetPlaneResources(fd);
	check(plane_resources != NULL,
	    "legacy plane resources readable before UNIVERSAL_PLANES");
	if (plane_resources == NULL)
		return;

	for (uint32_t i = 0; i < plane_resources->count_planes; i++) {
		int type;

		type = get_plane_type(fd, plane_resources->planes[i]);
		if (type < 0) {
			check(false,
			    "legacy plane type property is readable before UNIVERSAL_PLANES");
			no_primary_or_cursor = false;
			continue;
		}
		if (type == DRM_PLANE_TYPE_PRIMARY || type == DRM_PLANE_TYPE_CURSOR)
			no_primary_or_cursor = false;
	}
	check(no_primary_or_cursor,
	    "legacy plane resources expose no primary or cursor planes");

	drmModeFreePlaneResources(plane_resources);
}

static bool
get_property_id(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, uint32_t *property_id)
{
	drmModePropertyPtr prop;
	uint64_t value;

	prop = get_property_by_name(fd, object_id, object_type, name, &value);
	if (prop == NULL)
		return false;
	*property_id = prop->prop_id;
	drmModeFreeProperty(prop);
	return true;
}

static bool
has_property(int fd, uint32_t object_id, uint32_t object_type,
    const char *name)
{
	drmModePropertyPtr prop;
	uint64_t value;

	prop = get_property_by_name(fd, object_id, object_type, name, &value);
	if (prop == NULL)
		return false;
	drmModeFreeProperty(prop);
	return true;
}

/*
 * check_atomic_properties_hidden_without_client_cap()
 *
 * Ownership:
 *   Borrows the DRM fd.  Resource snapshots returned by libdrm are owned by
 *   this helper and freed before return.
 *
 * Lifetime:
 *   Must run before DRM_CLIENT_CAP_ATOMIC is enabled on the fd.  The fd may
 *   already have UNIVERSAL_PLANES enabled so plane objects are visible while
 *   atomic-only properties remain hidden.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  It only reads object property lists and
 *   does not mutate KMS state.
 */
static void
check_atomic_properties_hidden_without_client_cap(int fd)
{
	drmModePlaneResPtr plane_resources;
	drmModeRes *resources;

	resources = drmModeGetResources(fd);
	check(resources != NULL,
	    "DRM atomic property visibility resources are readable before ATOMIC");
	if (resources != NULL) {
		if (resources->count_crtcs > 0) {
			uint32_t crtc_id = resources->crtcs[0];

			check(!has_property(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
			    "MODE_ID"),
			    "CRTC MODE_ID is hidden before ATOMIC client cap");
			check(!has_property(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
			    "ACTIVE"),
			    "CRTC ACTIVE is hidden before ATOMIC client cap");
			check(!has_property(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
			    "OUT_FENCE_PTR"),
			    "CRTC OUT_FENCE_PTR is hidden before ATOMIC client cap");
		}
		if (resources->count_connectors > 0) {
			uint32_t connector_id = resources->connectors[0];

			check(!has_property(fd, connector_id,
			    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"),
			    "connector CRTC_ID is hidden before ATOMIC client cap");
		}
		drmModeFreeResources(resources);
	}

	plane_resources = drmModeGetPlaneResources(fd);
	check(plane_resources != NULL,
	    "plane resources readable before ATOMIC client cap");
	if (plane_resources != NULL) {
		if (plane_resources->count_planes > 0) {
			uint32_t plane_id = plane_resources->planes[0];

			check(!has_property(fd, plane_id, DRM_MODE_OBJECT_PLANE,
			    "FB_ID"),
			    "plane FB_ID is hidden before ATOMIC client cap");
			check(!has_property(fd, plane_id, DRM_MODE_OBJECT_PLANE,
			    "CRTC_ID"),
			    "plane CRTC_ID is hidden before ATOMIC client cap");
			check(!has_property(fd, plane_id, DRM_MODE_OBJECT_PLANE,
			    "SRC_W"),
			    "plane SRC_W is hidden before ATOMIC client cap");
			check(!has_property(fd, plane_id, DRM_MODE_OBJECT_PLANE,
			    "IN_FENCE_FD"),
			    "plane IN_FENCE_FD is hidden before ATOMIC client cap");
		}
		drmModeFreePlaneResources(plane_resources);
	}
}

static void
check_atomic_ioctl_error(int fd, uint32_t flags, uint32_t reserved,
    int expected_errno, const char *what, const char *errno_what)
{
	struct drm_mode_atomic atomic;
	int saved_errno;
	int ret;

	memset(&atomic, 0, sizeof(atomic));
	atomic.flags = flags;
	atomic.reserved = reserved;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
	saved_errno = errno;
	check(ret != 0, what);
	check(saved_errno == expected_errno, errno_what);
	if (ret == 0 || saved_errno != expected_errno)
		printf("    MODE_ATOMIC ret=%d errno=%d expected=%d\n",
		    ret, saved_errno, expected_errno);
}

/*
 * check_atomic_ioctl_flag_contract()
 *
 * Ownership:
 *   Borrows a DRM fd after DRM_CLIENT_CAP_ATOMIC has been enabled.
 *
 * Lifetime:
 *   Uses empty atomic requests so no KMS object IDs, property arrays, fences, or
 *   events are retained by either userspace or the kernel after each ioctl.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  Each request must fail in common DRM flag
 *   validation before object parsing or driver atomic_check is reached.
 */
static void
check_atomic_ioctl_flag_contract(int fd)
{
	check_atomic_ioctl_error(fd, 0x80000000u, 0, EINVAL,
	    "DRM atomic ioctl rejects unknown flags",
	    "DRM atomic ioctl unknown flags fail with EINVAL");
	check_atomic_ioctl_error(fd, 0, 1, EINVAL,
	    "DRM atomic ioctl rejects non-zero reserved field",
	    "DRM atomic ioctl reserved field fails with EINVAL");
	check_atomic_ioctl_error(fd, DRM_MODE_PAGE_FLIP_ASYNC, 0, EINVAL,
	    "DRM atomic ioctl rejects unsupported ASYNC flag",
	    "DRM atomic ioctl ASYNC flag fails with EINVAL");
	check_atomic_ioctl_error(fd,
	    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_PAGE_FLIP_EVENT, 0, EINVAL,
	    "DRM atomic ioctl rejects TEST_ONLY event request",
	    "DRM atomic ioctl TEST_ONLY event fails with EINVAL");
}

static void
check_cursor_ioctl_error(int fd, bool cursor2, uint32_t flags,
    int expected_errno, const char *what, const char *errno_what)
{
	struct drm_mode_cursor cursor;
	struct drm_mode_cursor2 cursor2_req;
	int saved_errno;
	int ret;

	errno = 0;
	if (cursor2) {
		memset(&cursor2_req, 0, sizeof(cursor2_req));
		cursor2_req.flags = flags;
		ret = drmIoctl(fd, DRM_IOCTL_MODE_CURSOR2, &cursor2_req);
	} else {
		memset(&cursor, 0, sizeof(cursor));
		cursor.flags = flags;
		ret = drmIoctl(fd, DRM_IOCTL_MODE_CURSOR, &cursor);
	}
	saved_errno = errno;
	check(ret != 0, what);
	check(saved_errno == expected_errno, errno_what);
	if (ret == 0 || saved_errno != expected_errno)
		printf("    cursor2=%d flags=0x%x ret=%d errno=%d expected=%d\n",
		    cursor2 ? 1 : 0, flags, ret, saved_errno, expected_errno);
}

/*
 * check_cursor_ioctl_flag_contract()
 *
 * Ownership:
 *   Borrows the DRM master fd.  No GEM handle, cursor framebuffer, or CRTC
 *   state is created or retained.
 *
 * Lifetime:
 *   Uses invalid flag combinations that must fail before CRTC lookup or cursor
 *   plane programming, so no display state can change.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  Both legacy cursor ioctl versions share
 *   the same common-DRM flag validation boundary.
 */
static void
check_cursor_ioctl_flag_contract(int fd)
{
	check_cursor_ioctl_error(fd, false, 0, EINVAL,
	    "DRM cursor ioctl rejects empty flags",
	    "DRM cursor ioctl empty flags fail with EINVAL");
	check_cursor_ioctl_error(fd, false, 0x80000000u, EINVAL,
	    "DRM cursor ioctl rejects unknown flags",
	    "DRM cursor ioctl unknown flags fail with EINVAL");
	check_cursor_ioctl_error(fd, true, 0, EINVAL,
	    "DRM cursor2 ioctl rejects empty flags",
	    "DRM cursor2 ioctl empty flags fail with EINVAL");
	check_cursor_ioctl_error(fd, true, 0x80000000u, EINVAL,
	    "DRM cursor2 ioctl rejects unknown flags",
	    "DRM cursor2 ioctl unknown flags fail with EINVAL");
}

static void
check_wait_vblank_error(int fd, unsigned int type, int expected_errno,
    const char *what, const char *errno_what)
{
	union drm_wait_vblank vblank;
	int saved_errno;
	int ret;

	memset(&vblank, 0, sizeof(vblank));
	vblank.request.type = type;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_WAIT_VBLANK, &vblank);
	saved_errno = errno;
	check(ret != 0, what);
	check(saved_errno == expected_errno, errno_what);
	if (ret == 0 || saved_errno != expected_errno)
		printf("    WAIT_VBLANK type=0x%x ret=%d errno=%d expected=%d\n",
		    type, ret, saved_errno, expected_errno);
}

/*
 * check_wait_vblank_flag_contract()
 *
 * Ownership:
 *   Borrows the DRM fd only.  No event is reserved and no CRTC object reference
 *   is retained.
 *
 * Lifetime:
 *   Uses invalid request types that must fail before pipe lookup, vblank
 *   acquire, blocking wait, or event queueing.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  It verifies common DRM validation before
 *   any driver vblank state can be touched.
 */
static void
check_wait_vblank_flag_contract(int fd)
{
	check_wait_vblank_error(fd, DRM_VBLANK_RELATIVE | DRM_VBLANK_SIGNAL,
	    EINVAL, "WAIT_VBLANK rejects SIGNAL requests",
	    "WAIT_VBLANK SIGNAL requests fail with EINVAL");
	check_wait_vblank_error(fd, DRM_VBLANK_RELATIVE | 0x80000000u,
	    EINVAL, "WAIT_VBLANK rejects unknown type bits",
	    "WAIT_VBLANK unknown type bits fail with EINVAL");
}

static void
check_pageflip_ioctl_error(int fd, uint32_t flags, uint32_t sequence,
    int expected_errno, const char *what, const char *errno_what)
{
	struct drm_mode_crtc_page_flip_target page_flip;
	int saved_errno;
	int ret;

	memset(&page_flip, 0, sizeof(page_flip));
	page_flip.flags = flags;
	page_flip.sequence = sequence;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &page_flip);
	saved_errno = errno;
	check(ret != 0, what);
	check(saved_errno == expected_errno, errno_what);
	if (ret == 0 || saved_errno != expected_errno) {
		printf("    PAGE_FLIP flags=0x%x sequence=%u ret=%d errno=%d expected=%d\n",
		    flags, sequence, ret, saved_errno, expected_errno);
	}
}

/*
 * check_pageflip_ioctl_flag_contract()
 *
 * Ownership:
 *   Borrows the DRM fd only.  No framebuffer, event, or CRTC object reference
 *   is owned or retained.
 *
 * Lifetime:
 *   Uses invalid PAGE_FLIP flags and target combinations that must fail before
 *   CRTC lookup, event reservation, vblank acquisition, or driver flip hooks.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  It validates the common DRM flag/sequence
 *   boundary without submitting a flip.
 */
static void
check_pageflip_ioctl_flag_contract(int fd)
{
	check_pageflip_ioctl_error(fd, 0x80000000u, 0, EINVAL,
	    "PAGE_FLIP rejects unknown flags",
	    "PAGE_FLIP unknown flags fail with EINVAL");
	check_pageflip_ioctl_error(fd, 0, 1, EINVAL,
	    "PAGE_FLIP rejects sequence without target flag",
	    "PAGE_FLIP sequence without target flag fails with EINVAL");
	check_pageflip_ioctl_error(fd,
	    DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE |
	    DRM_MODE_PAGE_FLIP_TARGET_RELATIVE,
	    0, EINVAL, "PAGE_FLIP rejects both target flags",
	    "PAGE_FLIP both target flags fail with EINVAL");
}

static void
check_property_read_error_contract(int fd)
{
	struct drm_mode_get_property get_property;
	struct drm_mode_obj_get_properties get_object_properties;
	int saved_errno;
	int ret;

	memset(&get_property, 0, sizeof(get_property));
	get_property.prop_id = 0;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &get_property);
	saved_errno = errno;
	check(ret != 0, "DRM GETPROPERTY rejects bad property id");
	check(saved_errno == ENOENT,
	    "DRM GETPROPERTY bad property id fails with ENOENT");

	memset(&get_object_properties, 0, sizeof(get_object_properties));
	get_object_properties.obj_id = 0;
	get_object_properties.obj_type = DRM_MODE_OBJECT_CRTC;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES,
	    &get_object_properties);
	saved_errno = errno;
	check(ret != 0, "DRM OBJ_GETPROPERTIES rejects bad object id");
	check(saved_errno == ENOENT,
	    "DRM OBJ_GETPROPERTIES bad object id fails with ENOENT");
}

/*
 * check_property_set_error_contract()
 *
 * Ownership:
 *   Borrows the DRM fd.  The temporary resource snapshot is owned by this
 *   function and freed before return.  No property value is changed by the
 *   invalid requests.
 *
 * Lifetime:
 *   Bad object ids must fail before property lookup.  Bad property ids use a
 *   real connector object and must fail before atomic state allocation or
 *   driver property callbacks.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  The DRM core serializes real property
 *   mutation paths; this function only checks common lookup error boundaries.
 */
static void
check_property_set_error_contract(int fd)
{
	struct drm_mode_connector_set_property connector_set_property;
	struct drm_mode_obj_set_property object_set_property;
	drmModeResPtr resources;
	uint32_t connector_id;
	int saved_errno;
	int ret;

	memset(&connector_set_property, 0, sizeof(connector_set_property));
	connector_set_property.connector_id = 0;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_SETPROPERTY,
	    &connector_set_property);
	saved_errno = errno;
	check(ret != 0, "DRM SETPROPERTY rejects bad connector id");
	check(saved_errno == ENOENT,
	    "DRM SETPROPERTY bad connector id fails with ENOENT");

	memset(&object_set_property, 0, sizeof(object_set_property));
	object_set_property.obj_id = 0;
	object_set_property.obj_type = DRM_MODE_OBJECT_CONNECTOR;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY,
	    &object_set_property);
	saved_errno = errno;
	check(ret != 0, "DRM OBJ_SETPROPERTY rejects bad object id");
	check(saved_errno == ENOENT,
	    "DRM OBJ_SETPROPERTY bad object id fails with ENOENT");

	resources = drmModeGetResources(fd);
	check(resources != NULL,
	    "DRM property set bad-property probe reads resources");
	if (resources == NULL)
		return;
	check(resources->count_connectors > 0,
	    "DRM property set bad-property probe finds connector");
	if (resources->count_connectors == 0) {
		drmModeFreeResources(resources);
		return;
	}
	connector_id = resources->connectors[0];

	memset(&connector_set_property, 0, sizeof(connector_set_property));
	connector_set_property.connector_id = connector_id;
	connector_set_property.prop_id = 0;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_SETPROPERTY,
	    &connector_set_property);
	saved_errno = errno;
	check(ret != 0, "DRM SETPROPERTY rejects bad property id");
	check(saved_errno == EINVAL,
	    "DRM SETPROPERTY bad property id fails with EINVAL");

	memset(&object_set_property, 0, sizeof(object_set_property));
	object_set_property.obj_id = connector_id;
	object_set_property.obj_type = DRM_MODE_OBJECT_CONNECTOR;
	object_set_property.prop_id = 0;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY,
	    &object_set_property);
	saved_errno = errno;
	check(ret != 0, "DRM OBJ_SETPROPERTY rejects bad property id");
	check(saved_errno == EINVAL,
	    "DRM OBJ_SETPROPERTY bad property id fails with EINVAL");

	drmModeFreeResources(resources);
}

static void
check_resource_lookup_error_contract(int fd)
{
	struct drm_mode_crtc crtc;
	struct drm_mode_get_connector connector;
	struct drm_mode_get_encoder encoder;
	struct drm_mode_get_plane plane;
	int saved_errno;
	int ret;

	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = 0;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc);
	saved_errno = errno;
	check(ret != 0, "DRM GETCRTC rejects bad CRTC id");
	check(saved_errno == ENOENT,
	    "DRM GETCRTC bad CRTC id fails with ENOENT");

	memset(&connector, 0, sizeof(connector));
	connector.connector_id = 0;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector);
	saved_errno = errno;
	check(ret != 0, "DRM GETCONNECTOR rejects bad connector id");
	check(saved_errno == ENOENT,
	    "DRM GETCONNECTOR bad connector id fails with ENOENT");

	memset(&encoder, 0, sizeof(encoder));
	encoder.encoder_id = 0;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder);
	saved_errno = errno;
	check(ret != 0, "DRM GETENCODER rejects bad encoder id");
	check(saved_errno == ENOENT,
	    "DRM GETENCODER bad encoder id fails with ENOENT");

	memset(&plane, 0, sizeof(plane));
	plane.plane_id = 0;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane);
	saved_errno = errno;
	check(ret != 0, "DRM GETPLANE rejects bad plane id");
	check(saved_errno == ENOENT,
	    "DRM GETPLANE bad plane id fails with ENOENT");
}

static bool
get_property_blob_raw(int fd, uint32_t blob_id, uint8_t *buffer,
    uint32_t length, uint32_t *actual_length_out, int *saved_errno_out)
{
	struct drm_mode_get_blob get_blob;
	int saved_errno;
	int ret;

	memset(&get_blob, 0, sizeof(get_blob));
	get_blob.blob_id = blob_id;
	get_blob.length = length;
	get_blob.data = (uintptr_t)buffer;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &get_blob);
	saved_errno = errno;
	if (actual_length_out != NULL)
		*actual_length_out = get_blob.length;
	if (saved_errno_out != NULL)
		*saved_errno_out = saved_errno;

	return ret == 0;
}

/*
 * check_property_blob_lifetime_contract()
 *
 * Ownership:
 *   Creates one user property blob owned by the caller's DRM file.  A second
 *   card fd only borrows the global blob ID for lookup and must not acquire
 *   destroy authority.  The owner destroys the blob before return.
 *
 * Lifetime:
 *   The blob is never installed into an atomic state or a KMS object property;
 *   after owner destroy, the ID must be gone and later lookups/destroys must
 *   fail with ENOENT.
 *
 * Threading:
 *   Single-threaded UAPI probe.  The foreign fd check models cross-file access
 *   without racing blob destroy against lookup.
 */
static void
check_property_blob_lifetime_contract(int fd)
{
	const uint8_t payload[] = { 0x4e, 0x56, 0x4b, 0x4d, 0x2d, 0x4b, 0x4d, 0x53 };
	uint8_t readback[sizeof(payload)];
	uint32_t actual_length;
	uint32_t blob_id = 0;
	uint32_t zero_blob = 0;
	int saved_errno;
	int secondary_fd = -1;
	int ret;

	errno = 0;
	ret = drmModeCreatePropertyBlob(fd, payload, 0, &zero_blob);
	saved_errno = errno;
	check(ret != 0, "DRM property blob rejects zero length create");
	check(saved_errno == EINVAL,
	    "DRM property blob zero length create fails with EINVAL");
	if (ret == 0)
		check(drmModeDestroyPropertyBlob(fd, zero_blob) == 0,
		    "destroy unexpected zero-length property blob");

	errno = 0;
	ret = drmModeCreatePropertyBlob(fd, payload, sizeof(payload), &blob_id);
	saved_errno = errno;
	check(ret == 0, "DRM property blob create succeeds");
	if (ret != 0) {
		printf("    CREATEPROPBLOB errno=%d\n", saved_errno);
		return;
	}
	check(blob_id != 0, "DRM property blob create returns non-zero id");

	actual_length = 0;
	saved_errno = 0;
	check(get_property_blob_raw(fd, blob_id, NULL, 0, &actual_length,
	    &saved_errno), "DRM property blob length query succeeds");
	check(actual_length == sizeof(payload),
	    "DRM property blob length query returns payload size");

	memset(readback, 0, sizeof(readback));
	actual_length = 0;
	saved_errno = 0;
	check(get_property_blob_raw(fd, blob_id, readback, sizeof(readback),
	    &actual_length, &saved_errno),
	    "DRM property blob owner read succeeds");
	check(actual_length == sizeof(payload),
	    "DRM property blob owner read reports payload size");
	check(memcmp(readback, payload, sizeof(payload)) == 0,
	    "DRM property blob owner read matches payload");

	secondary_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	check(secondary_fd >= 0,
	    "DRM property blob opens secondary card fd");
	if (secondary_fd >= 0) {
		memset(readback, 0, sizeof(readback));
		actual_length = 0;
		saved_errno = 0;
		check(get_property_blob_raw(secondary_fd, blob_id, readback,
		    sizeof(readback), &actual_length, &saved_errno),
		    "DRM property blob foreign fd read succeeds");
		check(actual_length == sizeof(payload),
		    "DRM property blob foreign fd read reports payload size");
		check(memcmp(readback, payload, sizeof(payload)) == 0,
		    "DRM property blob foreign fd read matches payload");

		errno = 0;
		ret = drmModeDestroyPropertyBlob(secondary_fd, blob_id);
		saved_errno = errno;
		check(ret != 0,
		    "DRM property blob foreign fd destroy is denied");
		check(saved_errno == EPERM,
		    "DRM property blob foreign fd destroy fails with EPERM");
		check(close(secondary_fd) == 0,
		    "DRM property blob closes secondary card fd");
	}

	check(drmModeDestroyPropertyBlob(fd, blob_id) == 0,
	    "DRM property blob owner destroy succeeds");

	actual_length = 0;
	saved_errno = 0;
	check(!get_property_blob_raw(fd, blob_id, NULL, 0, &actual_length,
	    &saved_errno), "DRM property blob destroyed id is unreadable");
	check(saved_errno == ENOENT,
	    "DRM property blob destroyed id read fails with ENOENT");

	errno = 0;
	ret = drmModeDestroyPropertyBlob(fd, blob_id);
	saved_errno = errno;
	check(ret != 0, "DRM property blob double destroy is rejected");
	check(saved_errno == ENOENT,
	    "DRM property blob double destroy fails with ENOENT");
}

static bool
create_identity_lut_blob(int fd, uint32_t count, uint32_t *blob_id_out,
    const char *what)
{
	struct drm_color_lut *lut;
	bool ok;

	lut = calloc(count, sizeof(*lut));
	check(lut != NULL, "allocate identity LUT buffer");
	if (lut == NULL)
		return false;

	for (uint32_t i = 0; i < count; i++) {
		uint16_t value;

		value = count > 1 ? (uint16_t)((uint64_t)i * 0xffffu /
		    (count - 1)) : 0;
		lut[i].red = value;
		lut[i].green = value;
		lut[i].blue = value;
	}

	ok = drmModeCreatePropertyBlob(fd, lut, count * sizeof(*lut),
	    blob_id_out) == 0;
	check(ok, what);
	free(lut);
	return ok;
}

static bool
create_identity_ctm_blob(int fd, uint32_t *blob_id_out, const char *what)
{
	struct drm_color_ctm ctm;
	bool ok;

	memset(&ctm, 0, sizeof(ctm));
	ctm.matrix[0] = 1ULL << 32;
	ctm.matrix[4] = 1ULL << 32;
	ctm.matrix[8] = 1ULL << 32;
	ok = drmModeCreatePropertyBlob(fd, &ctm, sizeof(ctm), blob_id_out) == 0;
	check(ok, what);
	return ok;
}

static void
destroy_property_blob(int fd, uint32_t blob_id, const char *what)
{
	if (blob_id == 0)
		return;
	check(drmModeDestroyPropertyBlob(fd, blob_id) == 0, what);
}

static void
require_property(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, const char *object_name)
{
	char text[160];

	snprintf(text, sizeof(text), "%s has property %s", object_name, name);
	check(has_property(fd, object_id, object_type, name), text);
}

static void
check_property_absent(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, const char *object_name, const char *object_label)
{
	char text[192];

	snprintf(text, sizeof(text),
	    "%s does not expose unsupported %s property %s",
	    object_name, object_label, name);
	check(!has_property(fd, object_id, object_type, name), text);
}

static bool
get_property_value_checked(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, uint64_t *value_out, const char *object_name)
{
	drmModePropertyPtr prop;
	uint64_t value = 0;
	char text[192];

	prop = get_property_by_name(fd, object_id, object_type, name, &value);
	snprintf(text, sizeof(text), "%s has property %s", object_name, name);
	check(prop != NULL, text);
	if (prop == NULL)
		return false;

	*value_out = value;
	drmModeFreeProperty(prop);
	return true;
}

static void
check_property_value_is_zero(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, const char *object_name, const char *what)
{
	uint64_t value = 0;

	if (!get_property_value_checked(fd, object_id, object_type, name,
	    &value, object_name))
		return;
	check(value == 0, what);
}

static void
check_enum_property_default(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, const char *object_name, const char *expected_enum)
{
	drmModePropertyPtr prop;
	uint64_t value = 0;
	char text[192];
	bool found = false;
	bool matches = false;

	prop = get_property_by_name(fd, object_id, object_type, name, &value);
	snprintf(text, sizeof(text), "%s has property %s", object_name, name);
	check(prop != NULL, text);
	if (prop == NULL)
		return;

	snprintf(text, sizeof(text), "%s %s is enum", object_name, name);
	check((prop->flags & DRM_MODE_PROP_ENUM) != 0, text);
	for (int i = 0; i < prop->count_enums; i++) {
		if (strcmp(prop->enums[i].name, expected_enum) != 0)
			continue;
		found = true;
		matches = prop->enums[i].value == value;
		break;
	}

	snprintf(text, sizeof(text), "%s %s has enum %s", object_name, name,
	    expected_enum);
	check(found, text);
	snprintf(text, sizeof(text), "%s %s defaults to %s", object_name, name,
	    expected_enum);
	check(matches, text);
	drmModeFreeProperty(prop);
}

static void
check_range_property_value(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, const char *object_name, uint64_t expected_min,
    uint64_t expected_max, uint64_t expected_value)
{
	drmModePropertyPtr prop;
	uint64_t value = 0;
	char text[192];
	bool has_range;

	prop = get_property_by_name(fd, object_id, object_type, name, &value);
	snprintf(text, sizeof(text), "%s has property %s", object_name, name);
	check(prop != NULL, text);
	if (prop == NULL)
		return;

	snprintf(text, sizeof(text), "%s %s is range", object_name, name);
	has_range = (prop->flags & DRM_MODE_PROP_RANGE) != 0 &&
	    prop->count_values >= 2;
	check(has_range, text);
	if (has_range) {
		snprintf(text, sizeof(text), "%s %s min is %llu",
		    object_name, name, (unsigned long long)expected_min);
		check(prop->values[0] == expected_min, text);
		snprintf(text, sizeof(text), "%s %s max is %llu",
		    object_name, name, (unsigned long long)expected_max);
		check(prop->values[1] == expected_max, text);
	}

	snprintf(text, sizeof(text), "%s %s value is %llu", object_name, name,
	    (unsigned long long)expected_value);
	check(value == expected_value, text);
	drmModeFreeProperty(prop);
}

static void
check_range_property_current(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, const char *object_name, uint64_t expected_value)
{
	drmModePropertyPtr prop;
	uint64_t value = 0;
	char text[192];

	prop = get_property_by_name(fd, object_id, object_type, name, &value);
	snprintf(text, sizeof(text), "%s has property %s", object_name, name);
	check(prop != NULL, text);
	if (prop == NULL)
		return;

	snprintf(text, sizeof(text), "%s %s is range", object_name, name);
	check((prop->flags & DRM_MODE_PROP_RANGE) != 0, text);
	snprintf(text, sizeof(text), "%s %s value is %llu", object_name, name,
	    (unsigned long long)expected_value);
	check(value == expected_value, text);
	drmModeFreeProperty(prop);
}

static void
check_signed_range_property_value(int fd, uint32_t object_id,
    uint32_t object_type, const char *name, const char *object_name,
    int64_t expected_min, int64_t expected_max, int64_t expected_value)
{
	drmModePropertyPtr prop;
	uint64_t value = 0;
	char text[192];
	bool has_signed_range;

	prop = get_property_by_name(fd, object_id, object_type, name, &value);
	snprintf(text, sizeof(text), "%s has property %s", object_name, name);
	check(prop != NULL, text);
	if (prop == NULL)
		return;

	snprintf(text, sizeof(text), "%s %s is signed range", object_name, name);
	has_signed_range = (prop->flags & DRM_MODE_PROP_SIGNED_RANGE) != 0 &&
	    prop->count_values >= 2;
	check(has_signed_range, text);
	if (has_signed_range) {
		snprintf(text, sizeof(text), "%s %s min is %lld",
		    object_name, name, (long long)expected_min);
		check(prop->values[0] == (uint64_t)expected_min, text);
		snprintf(text, sizeof(text), "%s %s max is %lld",
		    object_name, name, (long long)expected_max);
		check(prop->values[1] == (uint64_t)expected_max, text);
	}

	snprintf(text, sizeof(text), "%s %s value is %lld", object_name, name,
	    (long long)expected_value);
	check(value == (uint64_t)expected_value, text);
	drmModeFreeProperty(prop);
}

static void
check_blob_property_default_zero(int fd, uint32_t object_id,
    uint32_t object_type, const char *name, const char *object_name)
{
	drmModePropertyPtr prop;
	uint64_t value = 0;
	char text[192];

	prop = get_property_by_name(fd, object_id, object_type, name, &value);
	snprintf(text, sizeof(text), "%s has property %s", object_name, name);
	check(prop != NULL, text);
	if (prop == NULL)
		return;

	snprintf(text, sizeof(text), "%s %s is blob", object_name, name);
	check((prop->flags & DRM_MODE_PROP_BLOB) != 0, text);
	snprintf(text, sizeof(text), "%s %s defaults to 0", object_name,
	    name);
	check(value == 0, text);
	drmModeFreeProperty(prop);
}

/*
 * check_connector_property_contract()
 *
 * Ownership:
 *   Borrows the DRM connector object ID and opens each property through libdrm.
 *   Every drmModePropertyPtr returned by libdrm is released before return.
 *   connector_type is the libdrm snapshot for the same connector.
 *
 * Lifetime:
 *   Reads only public KMS properties.  It does not create an atomic request,
 *   mutate connector state, or trigger hotplug/link training.
 *
 * Threading:
 *   Single-threaded probe.  Values are snapshots from the DRM property UAPI;
 *   a later hotplug may legitimately change link-status in another run.
 */
static void
check_connector_property_contract(int fd, uint32_t connector_id,
    uint32_t connector_type, const char *object_name)
{
	check_enum_property_default(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "link-status", object_name, "Good");
	if (connector_type != DRM_MODE_CONNECTOR_TV) {
		check_enum_property_default(fd, connector_id,
		    DRM_MODE_OBJECT_CONNECTOR, "scaling mode", object_name,
		    "None");
	}
	check_enum_property_default(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "dithering mode", object_name, "auto");
	check_enum_property_default(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "dithering depth", object_name, "auto");
	check_range_property_value(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "max bpc", object_name, 8, 8, 8);
	check_property_absent(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "HDR_OUTPUT_METADATA", object_name, "connector");
	check_property_absent(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "Colorspace", object_name, "connector");
	check_property_absent(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "content type", object_name, "connector");
	check_property_absent(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "vrr_capable", object_name, "connector");
	switch (connector_type) {
	case DRM_MODE_CONNECTOR_DVID:
	case DRM_MODE_CONNECTOR_DVII:
	case DRM_MODE_CONNECTOR_HDMIA:
	case DRM_MODE_CONNECTOR_DisplayPort:
		check_enum_property_default(fd, connector_id,
		    DRM_MODE_OBJECT_CONNECTOR, "underscan", object_name,
		    "off");
		check_range_property_value(fd, connector_id,
		    DRM_MODE_OBJECT_CONNECTOR, "underscan hborder",
		    object_name, 0, 128, 0);
		check_range_property_value(fd, connector_id,
		    DRM_MODE_OBJECT_CONNECTOR, "underscan vborder",
		    object_name, 0, 128, 0);
		break;
	default:
		break;
	}
}

/*
 * check_crtc_color_property_contract()
 *
 * Ownership:
 *   Borrows the DRM CRTC object ID and opens each property through libdrm.
 *   Every drmModePropertyPtr returned by libdrm is released before return.
 *
 * Lifetime:
 *   Reads only public KMS color properties.  It does not create blobs, mutate
 *   CRTC state, or trigger an atomic commit.
 *
 * Threading:
 *   Single-threaded probe.  Values are snapshots from the DRM property UAPI;
 *   a compositor may set nonzero LUT/CTM blobs in a separate X11 phase.
 */
static void
check_crtc_color_property_contract(int fd, uint32_t crtc_id,
    const char *object_name)
{
	check_blob_property_default_zero(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "DEGAMMA_LUT", object_name);
	check_range_property_current(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "DEGAMMA_LUT_SIZE", object_name, 1024);
	check_blob_property_default_zero(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "CTM", object_name);
	check_blob_property_default_zero(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "GAMMA_LUT", object_name);
	check_range_property_current(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "GAMMA_LUT_SIZE", object_name, 1024);
}

/*
 * check_crtc_sync_property_contract()
 *
 * Ownership:
 *   Borrows the DRM CRTC object ID and opens OUT_FENCE_PTR through libdrm.
 *   The drmModePropertyPtr returned by libdrm is released before return.
 *
 * Lifetime:
 *   Reads only explicit-sync property metadata and the current default value.
 *   It does not install an out-fence pointer or trigger an atomic commit.
 *
 * Threading:
 *   Single-threaded probe.  The property contract is static for the CRTC even
 *   if another client later performs atomic commits.
 */
static void
check_crtc_sync_property_contract(int fd, uint32_t crtc_id,
    const char *object_name)
{
	check_range_property_value(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "OUT_FENCE_PTR", object_name, 0, UINT64_MAX, 0);
}

/*
 * check_crtc_unsupported_extension_contract()
 *
 * Ownership:
 *   Borrows the DRM CRTC object ID and opens properties through libdrm.
 *   Every drmModePropertyPtr returned by libdrm is released before return.
 *
 * Lifetime:
 *   Reads only public KMS property metadata.  It does not mutate CRTC state or
 *   trigger an atomic commit.
 *
 * Threading:
 *   Single-threaded probe.  The absence contract is static for this driver
 *   until the corresponding hardware semantics are implemented end-to-end.
 */
static void
check_crtc_unsupported_extension_contract(int fd, uint32_t crtc_id,
    const char *object_name)
{
	check_property_absent(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "VRR_ENABLED", object_name, "CRTC");
}

/*
 * check_plane_sync_property_contract()
 *
 * Ownership:
 *   Borrows the DRM plane object ID and opens IN_FENCE_FD through libdrm.
 *   The drmModePropertyPtr returned by libdrm is released before return.
 *
 * Lifetime:
 *   Reads only explicit-sync property metadata and the current default value.
 *   It does not pass a sync fd to the kernel or trigger an atomic commit.
 *
 * Threading:
 *   Single-threaded probe.  The property contract is static for the plane even
 *   if another client later performs atomic commits.
 */
static void
check_plane_sync_property_contract(int fd, uint32_t plane_id,
    const char *object_name)
{
	check_signed_range_property_value(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "IN_FENCE_FD", object_name, -1, INT_MAX, -1);
}

static bool
get_plane_snapshot(int fd, uint32_t plane_id,
    struct atomic_plane_snapshot *snapshot, const char *object_name)
{
	bool ok = true;

	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "FB_ID", &snapshot->fb_id, object_name);
	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "CRTC_ID", &snapshot->crtc_id, object_name);
	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "CRTC_X", &snapshot->crtc_x, object_name);
	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "CRTC_Y", &snapshot->crtc_y, object_name);
	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "CRTC_W", &snapshot->crtc_w, object_name);
	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "CRTC_H", &snapshot->crtc_h, object_name);
	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "SRC_X", &snapshot->src_x, object_name);
	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "SRC_Y", &snapshot->src_y, object_name);
	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "SRC_W", &snapshot->src_w, object_name);
	ok &= get_property_value_checked(fd, plane_id, DRM_MODE_OBJECT_PLANE,
	    "SRC_H", &snapshot->src_h, object_name);

	return ok;
}

static bool
atomic_add_crtc_property(int fd, drmModeAtomicReqPtr req, uint32_t crtc_id,
    const char *name, uint64_t value)
{
	uint32_t property_id = 0;

	if (!get_property_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, name,
	    &property_id)) {
		printf("crtc %u property %s unavailable\n", crtc_id, name);
		return false;
	}
	return drmModeAtomicAddProperty(req, crtc_id, property_id, value) >= 0;
}

static int
wait_sync_file_readable(int fd, int timeout_ms, int *saved_errno)
{
	struct kevent change;
	struct kevent event;
	struct timespec timeout;
	int kq;
	int ret;

	kq = kqueue();
	if (kq < 0) {
		*saved_errno = errno;
		return -1;
	}

	EV_SET(&change, (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE |
	    EV_ONESHOT, 0, 0, NULL);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;

	errno = 0;
	ret = kevent(kq, &change, 1, &event, 1, &timeout);
	*saved_errno = errno;
	if (close(kq) != 0 && ret >= 0) {
		*saved_errno = errno;
		return -1;
	}
	if (ret != 1)
		return ret == 0 ? 0 : -1;
	if ((event.flags & EV_ERROR) != 0) {
		*saved_errno = (int)event.data;
		return -1;
	}
	return 1;
}

static void
pageflip_event_handler2(int fd, unsigned int sequence, unsigned int tv_sec,
    unsigned int tv_usec, unsigned int crtc_id, void *user_data)
{
	struct pageflip_event_state *state = user_data;

	(void)fd;
	state->count++;
	state->sequence = sequence;
	state->tv_sec = tv_sec;
	state->tv_usec = tv_usec;
	state->crtc_id = crtc_id;
}

static void
sequence_event_handler(int fd, uint64_t sequence, uint64_t ns,
    uint64_t user_data)
{
	struct sequence_event_state *state = (void *)(uintptr_t)user_data;

	(void)fd;
	if (state == NULL)
		return;
	state->count++;
	state->sequence = sequence;
	state->ns = ns;
	state->user_data = user_data;
}

/*
 * wait_pageflip_event()
 *
 * Ownership:
 *   Borrows the DRM fd and caller-owned event state.  It owns one temporary
 *   kqueue descriptor and closes it before return.
 *
 * Lifetime:
 *   Waits for one DRM pageflip event after drmModePageFlip() has queued it.
 *   The callback writes only into the caller-provided event state.
 *
 * Threading:
 *   Single-threaded userspace wait.  The kernel may deliver unrelated DRM
 *   events on the same fd; libdrm dispatches them synchronously here.
 */
static int
wait_pageflip_event(int fd, struct pageflip_event_state *state,
    int timeout_ms, int *saved_errno)
{
	drmEventContext context;
	struct kevent change;
	struct kevent event;
	struct timespec timeout;
	int kq;
	int ret;

	memset(&context, 0, sizeof(context));
	context.version = DRM_EVENT_CONTEXT_VERSION;
	context.page_flip_handler2 = pageflip_event_handler2;

	kq = kqueue();
	if (kq < 0) {
		*saved_errno = errno;
		return -1;
	}

	EV_SET(&change, (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE |
	    EV_ONESHOT, 0, 0, NULL);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;

	while (state->count == 0) {
		errno = 0;
		ret = kevent(kq, &change, 1, &event, 1, &timeout);
		*saved_errno = errno;
		if (ret != 1) {
			if (close(kq) != 0 && ret >= 0)
				*saved_errno = errno;
			return ret == 0 ? 0 : -1;
		}
		if ((event.flags & EV_ERROR) != 0) {
			*saved_errno = (int)event.data;
			(void)close(kq);
			return -1;
		}
		errno = 0;
		ret = drmHandleEvent(fd, &context);
		*saved_errno = errno;
		if (ret != 0) {
			(void)close(kq);
			return -1;
		}
	}

	if (close(kq) != 0) {
		*saved_errno = errno;
		return -1;
	}
	return 1;
}

/*
 * read_raw_vblank_event()
 *
 * Ownership:
 *   Borrows the DRM fd and caller-owned output storage.  It does not retain
 *   the event after copying it into the caller's structure.
 *
 * Lifetime:
 *   Waits for one queued DRM_EVENT_VBLANK and consumes exactly that userspace
 *   event from the fd.  The caller must queue the vblank event before calling.
 *
 * Threading:
 *   Single-threaded userspace wait.  No libdrm event callback is used because
 *   the legacy vblank callback ABI does not expose drm_event_vblank.crtc_id.
 */
static int
read_raw_vblank_event(int fd, struct raw_vblank_event *raw_event,
    int timeout_ms, int *saved_errno)
{
	struct drm_event_vblank event;
	ssize_t bytes;
	int ret;

	ret = wait_sync_file_readable(fd, timeout_ms, saved_errno);
	if (ret != 1)
		return ret;

	memset(&event, 0, sizeof(event));
	errno = 0;
	bytes = read(fd, &event, sizeof(event));
	*saved_errno = errno;
	if (bytes < 0)
		return -1;
	if ((size_t)bytes != sizeof(event)) {
		*saved_errno = EIO;
		return -1;
	}
	if (event.base.type != DRM_EVENT_VBLANK ||
	    event.base.length != sizeof(event)) {
		*saved_errno = EPROTO;
		return -1;
	}

	raw_event->event = event;
	return 1;
}

/*
 * wait_sequence_event()
 *
 * Ownership:
 *   Borrows the DRM fd and caller-owned event state.  It owns one temporary
 *   kqueue descriptor and closes it before return.
 *
 * Lifetime:
 *   Waits for one DRM CRTC sequence event after drmCrtcQueueSequence() has
 *   queued it.  The callback writes only into the caller-provided event state.
 *
 * Threading:
 *   Single-threaded userspace wait.  The kernel may deliver unrelated DRM
 *   events on the same fd; libdrm dispatches them synchronously here.
 */
static int
wait_sequence_event(int fd, struct sequence_event_state *state,
    int timeout_ms, int *saved_errno)
{
	drmEventContext context;
	struct kevent change;
	struct kevent event;
	struct timespec timeout;
	int kq;
	int ret;

	memset(&context, 0, sizeof(context));
	context.version = DRM_EVENT_CONTEXT_VERSION;
	context.sequence_handler = sequence_event_handler;

	kq = kqueue();
	if (kq < 0) {
		*saved_errno = errno;
		return -1;
	}

	EV_SET(&change, (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE |
	    EV_ONESHOT, 0, 0, NULL);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;

	while (state->count == 0) {
		errno = 0;
		ret = kevent(kq, &change, 1, &event, 1, &timeout);
		*saved_errno = errno;
		if (ret != 1) {
			if (close(kq) != 0 && ret >= 0)
				*saved_errno = errno;
			return ret == 0 ? 0 : -1;
		}
		if ((event.flags & EV_ERROR) != 0) {
			*saved_errno = (int)event.data;
			(void)close(kq);
			return -1;
		}
		errno = 0;
		ret = drmHandleEvent(fd, &context);
		*saved_errno = errno;
		if (ret != 0) {
			(void)close(kq);
			return -1;
		}
	}

	if (close(kq) != 0) {
		*saved_errno = errno;
		return -1;
	}
	return 1;
}

static bool
syncobj_create_handle(int fd, uint32_t *handle_out)
{
	struct drm_syncobj_create req;

	memset(&req, 0, sizeof(req));
	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &req) != 0)
		return false;
	*handle_out = req.handle;
	return true;
}

static void
syncobj_destroy_handle(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy req;

	if (handle == 0)
		return;
	memset(&req, 0, sizeof(req));
	req.handle = handle;
	(void)drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &req);
}

static bool
syncobj_export_sync_file(int fd, uint32_t handle, int *sync_fd_out)
{
	struct drm_syncobj_handle req;

	memset(&req, 0, sizeof(req));
	req.handle = handle;
	req.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
	req.fd = -1;
	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &req) != 0)
		return false;
	*sync_fd_out = req.fd;
	return true;
}

static bool
syncobj_signal_handle(int fd, uint32_t handle)
{
	struct drm_syncobj_array req;
	uint32_t handles[1] = { handle };

	memset(&req, 0, sizeof(req));
	req.handles = (uint64_t)(uintptr_t)handles;
	req.count_handles = 1;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &req) == 0;
}

static bool
syncobj_wait_handle(int fd, uint32_t handle)
{
	struct drm_syncobj_wait req;
	uint32_t handles[1] = { handle };

	memset(&req, 0, sizeof(req));
	req.handles = (uint64_t)(uintptr_t)handles;
	req.count_handles = 1;
	req.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &req) == 0;
}

static bool
syncobj_timeline_signal_handle(int fd, uint32_t handle, uint64_t point)
{
	struct drm_syncobj_timeline_array req;
	uint32_t handles[1] = { handle };
	uint64_t points[1] = { point };

	memset(&req, 0, sizeof(req));
	req.handles = (uint64_t)(uintptr_t)handles;
	req.points = (uint64_t)(uintptr_t)points;
	req.count_handles = 1;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &req) == 0;
}

static bool
syncobj_timeline_wait_point(int fd, uint32_t handle, uint64_t point)
{
	struct drm_syncobj_timeline_wait req;
	uint32_t handles[1] = { handle };
	uint64_t points[1] = { point };

	memset(&req, 0, sizeof(req));
	req.handles = (uint64_t)(uintptr_t)handles;
	req.points = (uint64_t)(uintptr_t)points;
	req.count_handles = 1;
	req.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &req) == 0;
}

static bool
syncobj_transfer_point(int fd, uint32_t dst_handle, uint64_t dst_point,
    uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
	struct drm_syncobj_transfer req;
	int ret;

	memset(&req, 0, sizeof(req));
	req.src_handle = src_handle;
	req.dst_handle = dst_handle;
	req.src_point = src_point;
	req.dst_point = dst_point;
	req.flags = flags;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &req);
	if (ret != 0) {
		printf("    SYNCOBJ_TRANSFER errno=%d src=%u:%llu dst=%u:%llu flags=0x%x\n",
		    errno, src_handle, (unsigned long long)src_point,
		    dst_handle, (unsigned long long)dst_point, flags);
	}
	return ret == 0;
}

static bool
syncobj_transfer_wait_for_submit_point(int fd, uint32_t dst_handle,
    uint64_t dst_point, uint32_t src_handle, uint64_t src_point)
{
	int status;
	pid_t child;
	pid_t waited;
	bool transfer_ok;
	bool child_ok;

	child = fork();
	if (child < 0)
		return false;
	if (child == 0) {
		usleep(100000);
		_exit(syncobj_timeline_signal_handle(fd, src_handle, src_point) ?
		    0 : 1);
	}

	transfer_ok = syncobj_transfer_point(fd, dst_handle, dst_point,
	    src_handle, src_point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);

	child_ok = waited == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 0;
	return transfer_ok && child_ok;
}

static void
check_syncobj_transfer_contract(int fd)
{
	uint32_t binary_src = 0;
	uint32_t binary_src_b = 0;
	uint32_t binary_dst = 0;
	uint32_t timeline_src = 0;
	uint32_t timeline_dst = 0;
	uint32_t tmp_timeline = 0;
	uint32_t chain_dst = 0;
	uint32_t missing_dst = 0;
	bool ok;
	bool chain_ok;

	ok = syncobj_create_handle(fd, &binary_src);
	check(ok, "SYNCOBJ_TRANSFER creates binary source syncobj");
	ok = ok && syncobj_create_handle(fd, &timeline_dst);
	check(ok, "SYNCOBJ_TRANSFER creates destination syncobj");
	if (!ok)
		goto out;

	check(syncobj_signal_handle(fd, binary_src),
	    "SYNCOBJ_TRANSFER binary source signal succeeds");
	check(syncobj_transfer_point(fd, timeline_dst, 5, binary_src, 0, 0),
	    "SYNCOBJ_TRANSFER binary source to timeline point succeeds");
	check(syncobj_timeline_wait_point(fd, timeline_dst, 5),
	    "SYNCOBJ_TRANSFER destination timeline point waits successfully");

	ok = syncobj_create_handle(fd, &timeline_src);
	check(ok, "SYNCOBJ_TRANSFER creates timeline source syncobj");
	if (ok) {
		check(syncobj_timeline_signal_handle(fd, timeline_src, 7),
		    "SYNCOBJ_TRANSFER source timeline signal succeeds");
		check(syncobj_transfer_point(fd, timeline_dst, 9,
		    timeline_src, 7, 0),
		    "SYNCOBJ_TRANSFER timeline source to timeline point succeeds");
		check(syncobj_timeline_wait_point(fd, timeline_dst, 9),
		    "SYNCOBJ_TRANSFER copied timeline point waits successfully");
		check(!syncobj_transfer_point(fd, timeline_dst, 11,
		    timeline_src, 99, 0),
		    "SYNCOBJ_TRANSFER rejects missing source point");
		check(syncobj_transfer_wait_for_submit_point(fd, timeline_dst,
		    13, timeline_src, 13),
		    "SYNCOBJ_TRANSFER WAIT_FOR_SUBMIT waits for source point");
		check(syncobj_timeline_wait_point(fd, timeline_dst, 13),
		    "SYNCOBJ_TRANSFER WAIT_FOR_SUBMIT destination point waits successfully");

		ok = syncobj_create_handle(fd, &binary_dst);
		check(ok, "SYNCOBJ_TRANSFER creates binary destination syncobj");
		if (ok) {
			check(syncobj_transfer_point(fd, binary_dst, 0,
			    timeline_src, 7, 0),
			    "SYNCOBJ_TRANSFER timeline source to binary succeeds");
			check(syncobj_wait_handle(fd, binary_dst),
			    "SYNCOBJ_TRANSFER binary destination waits successfully");
		}
	}

	chain_ok = syncobj_create_handle(fd, &binary_src_b);
	check(chain_ok, "SYNCOBJ_TRANSFER creates second binary source syncobj");
	if (chain_ok)
		check(syncobj_signal_handle(fd, binary_src_b),
		    "SYNCOBJ_TRANSFER second binary source signal succeeds");
	chain_ok = chain_ok && syncobj_create_handle(fd, &tmp_timeline);
	check(chain_ok, "SYNCOBJ_TRANSFER creates temporary timeline syncobj");
	chain_ok = chain_ok && syncobj_create_handle(fd, &chain_dst);
	check(chain_ok, "SYNCOBJ_TRANSFER creates chain destination syncobj");
	if (chain_ok) {
		check(syncobj_transfer_point(fd, tmp_timeline, 1,
		    binary_src, 0, 0),
		    "SYNCOBJ_TRANSFER first wait into temporary timeline");
		check(syncobj_transfer_point(fd, tmp_timeline, 2,
		    binary_src_b, 0, 0),
		    "SYNCOBJ_TRANSFER second wait into temporary timeline");
		check(syncobj_transfer_point(fd, chain_dst, 20,
		    tmp_timeline, 0, 0),
		    "SYNCOBJ_TRANSFER whole temporary chain to timeline succeeds");
		check(syncobj_timeline_wait_point(fd, chain_dst, 20),
		    "SYNCOBJ_TRANSFER copied whole chain waits successfully");
	}

	ok = syncobj_create_handle(fd, &missing_dst);
	check(ok, "SYNCOBJ_TRANSFER creates missing-source destination syncobj");
	if (ok)
		check(!syncobj_transfer_point(fd, missing_dst, 3,
		    missing_dst, 0, 0),
		    "SYNCOBJ_TRANSFER rejects source with no fence");

out:
	syncobj_destroy_handle(fd, missing_dst);
	syncobj_destroy_handle(fd, chain_dst);
	syncobj_destroy_handle(fd, tmp_timeline);
	syncobj_destroy_handle(fd, timeline_src);
	syncobj_destroy_handle(fd, timeline_dst);
	syncobj_destroy_handle(fd, binary_dst);
	syncobj_destroy_handle(fd, binary_src_b);
	syncobj_destroy_handle(fd, binary_src);
}

static bool
nouveau_channel_alloc(int fd, int32_t *channel_out)
{
	struct drm_nouveau_channel_alloc req;

	memset(&req, 0, sizeof(req));
	req.fb_ctxdma_handle = ~0u;
	req.tt_ctxdma_handle = NOUVEAU_FIFO_ENGINE_GR;
	if (drmIoctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, &req) != 0)
		return false;
	*channel_out = req.channel;
	return true;
}

static void
nouveau_channel_free(int fd, int32_t channel)
{
	struct drm_nouveau_channel_free req;

	if (channel < 0)
		return;
	memset(&req, 0, sizeof(req));
	req.channel = channel;
	(void)drmIoctl(fd, DRM_IOCTL_NOUVEAU_CHANNEL_FREE, &req);
}

static bool
nouveau_exec_signal_timeline(int fd, int32_t channel, uint32_t handle,
    uint64_t point)
{
	struct drm_nouveau_sync sig;
	struct drm_nouveau_exec exec;

	memset(&sig, 0, sizeof(sig));
	sig.flags = DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ;
	sig.handle = handle;
	sig.timeline_value = point;

	memset(&exec, 0, sizeof(exec));
	exec.channel = (uint32_t)channel;
	exec.sig_count = 1;
	exec.sig_ptr = (uint64_t)(uintptr_t)&sig;

	return drmIoctl(fd, DRM_IOCTL_NOUVEAU_EXEC, &exec) == 0;
}

static bool
create_pending_exec_sync_file(int fd, int32_t channel,
    struct exec_sync_file *sync_file)
{
	static const uint32_t batch_counts[] = {
		64, 256, 1024, 4096, 16384, 65536
	};
	int saved_errno = 0;

	sync_file->fd = -1;
	sync_file->was_pending = false;

	for (size_t b = 0; b < sizeof(batch_counts) / sizeof(batch_counts[0]);
	    b++) {
		uint32_t handle = 0;
		int sync_fd = -1;
		int wait_ret;
		bool ok = true;

		if (!syncobj_create_handle(fd, &handle))
			return false;

		for (uint32_t i = 1; i <= batch_counts[b]; i++) {
			if (!nouveau_exec_signal_timeline(fd, channel, handle,
			    i)) {
				ok = false;
				break;
			}
		}
		if (ok)
			ok = syncobj_export_sync_file(fd, handle, &sync_fd);
		syncobj_destroy_handle(fd, handle);
		if (!ok) {
			if (sync_fd >= 0)
				(void)close(sync_fd);
			return false;
		}

		wait_ret = wait_sync_file_readable(sync_fd, 0, &saved_errno);
		if (wait_ret == 0) {
			sync_file->fd = sync_fd;
			sync_file->was_pending = true;
			printf("    IN_FENCE_FD producer batch=%u pending\n",
			    batch_counts[b]);
			return true;
		}
		if (wait_ret < 0) {
			printf("    IN_FENCE_FD producer wait errno=%d\n",
			    saved_errno);
			(void)close(sync_fd);
			return false;
		}

		(void)close(sync_fd);
	}

	return false;
}

static int
atomic_crtc_color_test_only_commit(int fd, uint32_t crtc_id,
    uint32_t degamma_blob, uint32_t ctm_blob, uint32_t gamma_blob,
    int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	if (!atomic_add_crtc_property(fd, req, crtc_id, "DEGAMMA_LUT",
	    degamma_blob) ||
	    !atomic_add_crtc_property(fd, req, crtc_id, "CTM", ctm_blob) ||
	    !atomic_add_crtc_property(fd, req, crtc_id, "GAMMA_LUT",
	    gamma_blob)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req,
	    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

static int
atomic_crtc_color_commit(int fd, uint32_t crtc_id, uint32_t degamma_blob,
    uint32_t ctm_blob, uint32_t gamma_blob, int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	if (!atomic_add_crtc_property(fd, req, crtc_id, "DEGAMMA_LUT",
	    degamma_blob) ||
	    !atomic_add_crtc_property(fd, req, crtc_id, "CTM", ctm_blob) ||
	    !atomic_add_crtc_property(fd, req, crtc_id, "GAMMA_LUT",
	    gamma_blob)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

static int
atomic_primary_out_fence_commit(int fd, uint32_t crtc_id, uint32_t plane_id,
    const struct atomic_plane_snapshot *snapshot, int32_t *out_fence_fd,
    int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	*out_fence_fd = -1;
	if (!atomic_add_crtc_property(fd, req, crtc_id, "OUT_FENCE_PTR",
	    (uint64_t)(uintptr_t)out_fence_fd) ||
	    !atomic_add_plane_property(fd, req, plane_id, "FB_ID",
	    snapshot->fb_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_ID",
	    snapshot->crtc_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_X",
	    snapshot->crtc_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_Y",
	    snapshot->crtc_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_W",
	    snapshot->crtc_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_H",
	    snapshot->crtc_h) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_X",
	    snapshot->src_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_Y",
	    snapshot->src_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_W",
	    snapshot->src_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_H",
	    snapshot->src_h)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

static int
atomic_primary_in_fence_commit(int fd, uint32_t plane_id,
    const struct atomic_plane_snapshot *snapshot, int in_fence_fd,
    int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	if (!atomic_add_plane_property(fd, req, plane_id, "IN_FENCE_FD",
	    (uint64_t)in_fence_fd) ||
	    !atomic_add_plane_property(fd, req, plane_id, "FB_ID",
	    snapshot->fb_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_ID",
	    snapshot->crtc_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_X",
	    snapshot->crtc_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_Y",
	    snapshot->crtc_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_W",
	    snapshot->crtc_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_H",
	    snapshot->crtc_h) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_X",
	    snapshot->src_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_Y",
	    snapshot->src_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_W",
	    snapshot->src_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_H",
	    snapshot->src_h)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

static int
atomic_primary_commit(int fd, uint32_t plane_id,
    const struct atomic_plane_snapshot *snapshot, int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	if (!atomic_add_plane_property(fd, req, plane_id, "FB_ID",
	    snapshot->fb_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_ID",
	    snapshot->crtc_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_X",
	    snapshot->crtc_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_Y",
	    snapshot->crtc_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_W",
	    snapshot->crtc_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_H",
	    snapshot->crtc_h) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_X",
	    snapshot->src_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_Y",
	    snapshot->src_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_W",
	    snapshot->src_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_H",
	    snapshot->src_h)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

/*
 * check_atomic_out_fence_runtime_contract()
 *
 * Ownership:
 *   Borrows the active CRTC and primary plane IDs.  The existing framebuffer ID
 *   remains owned by the KMS state; this probe only reuses the same value in a
 *   no-visual-change atomic commit.  The returned sync_file fd is owned by the
 *   probe after drmModeAtomicCommit() succeeds and is always closed before
 *   return.
 *
 * Lifetime:
 *   Commits the current primary plane tuple unchanged while installing
 *   OUT_FENCE_PTR on the active CRTC.  The commit must produce a sync_file fd,
 *   and that fd must become readable once the display completion point signals.
 *
 * Threading:
 *   Single-threaded userspace probe.  The kernel evaluates the commit under
 *   normal modeset locks; the sync_file readiness is observed through
 *   DragonFly's EVFILT_READ shim for sync_file.
 */
static void
check_atomic_out_fence_runtime_contract(int fd, uint32_t crtc_id,
    uint32_t plane_id, const char *object_name)
{
	struct atomic_plane_snapshot snapshot;
	int32_t out_fence_fd = -1;
	int saved_errno = 0;
	int ret;

	if (!get_plane_snapshot(fd, plane_id, &snapshot, object_name))
		return;
	check(snapshot.fb_id != 0,
	    "active primary plane has framebuffer for OUT_FENCE_PTR probe");
	check(snapshot.crtc_id == crtc_id,
	    "active primary plane is attached to active CRTC for OUT_FENCE_PTR probe");
	if (snapshot.fb_id == 0 || snapshot.crtc_id != crtc_id)
		return;

	ret = atomic_primary_out_fence_commit(fd, crtc_id, plane_id, &snapshot,
	    &out_fence_fd, &saved_errno);
	if (ret != 0) {
		printf("    OUT_FENCE_PTR commit errno=%d\n", saved_errno);
		check(false, "atomic commit with OUT_FENCE_PTR succeeds");
		if (out_fence_fd >= 0)
			check(close(out_fence_fd) == 0,
			    "close OUT_FENCE_PTR fd after failed commit");
		return;
	}

	check(true, "atomic commit with OUT_FENCE_PTR succeeds");
	check(out_fence_fd >= 0, "OUT_FENCE_PTR returns a sync_file fd");
	if (out_fence_fd < 0)
		return;

	ret = wait_sync_file_readable(out_fence_fd, 2000, &saved_errno);
	if (ret != 1) {
		printf("    OUT_FENCE_PTR wait ret=%d errno=%d\n", ret,
		    saved_errno);
		check(false, "OUT_FENCE_PTR sync_file becomes readable");
	} else {
		check(true, "OUT_FENCE_PTR sync_file becomes readable");
	}
	check(close(out_fence_fd) == 0, "close OUT_FENCE_PTR sync_file fd");
}

/*
 * check_atomic_in_fence_runtime_contract()
 *
 * Ownership:
 *   Borrows the active CRTC and primary plane IDs.  Owns a temporary nouveau
 *   channel, syncobj handle, and exported sync_file fd while constructing the
 *   incoming fence.  The syncobj is destroyed after export; the sync_file fd
 *   remains owned by this probe and is closed before return.
 *
 * Lifetime:
 *   Creates a real pending nvkm EXEC fence, exports it as sync_file, proves the
 *   fd is pending with a zero-time EVFILT_READ probe, then submits an unchanged
 *   primary-plane atomic commit with IN_FENCE_FD set to that fd.  The commit
 *   must not return until the incoming fence has signaled.
 *
 * Threading:
 *   Single-threaded userspace probe.  The GPU completion and sync_file kqueue
 *   notification run asynchronously in the kernel; the atomic commit itself is
 *   the synchronization point being verified.
 */
static void
check_atomic_in_fence_runtime_contract(int fd, uint32_t crtc_id,
    uint32_t plane_id, const char *object_name)
{
	struct atomic_plane_snapshot snapshot;
	struct exec_sync_file sync_file;
	int32_t channel = -1;
	int saved_errno = 0;
	int wait_ret;
	int ret;

	if (!get_plane_snapshot(fd, plane_id, &snapshot, object_name))
		return;
	check(snapshot.fb_id != 0,
	    "active primary plane has framebuffer for IN_FENCE_FD probe");
	check(snapshot.crtc_id == crtc_id,
	    "active primary plane is attached to active CRTC for IN_FENCE_FD probe");
	if (snapshot.fb_id == 0 || snapshot.crtc_id != crtc_id)
		return;

	if (!nouveau_channel_alloc(fd, &channel)) {
		printf("    IN_FENCE_FD channel alloc errno=%d\n", errno);
		check(false, "nouveau channel alloc succeeds for IN_FENCE_FD probe");
		return;
	}
	check(true, "nouveau channel alloc succeeds for IN_FENCE_FD probe");

	if (!create_pending_exec_sync_file(fd, channel, &sync_file)) {
		check(false, "pending EXEC sync_file available for IN_FENCE_FD probe");
		goto out_channel;
	}
	check(sync_file.was_pending,
	    "IN_FENCE_FD sync_file is pending before atomic commit");

	ret = atomic_primary_in_fence_commit(fd, plane_id, &snapshot,
	    sync_file.fd, &saved_errno);
	if (ret != 0) {
		printf("    IN_FENCE_FD commit errno=%d\n", saved_errno);
		check(false, "atomic commit with IN_FENCE_FD succeeds");
		goto out_sync_file;
	}
	check(true, "atomic commit with IN_FENCE_FD succeeds");

	wait_ret = wait_sync_file_readable(sync_file.fd, 0, &saved_errno);
	if (wait_ret != 1) {
		printf("    IN_FENCE_FD post-commit wait ret=%d errno=%d\n",
		    wait_ret, saved_errno);
		check(false, "IN_FENCE_FD sync_file is readable after commit");
	} else {
		check(true, "IN_FENCE_FD sync_file is readable after commit");
	}

out_sync_file:
	check(close(sync_file.fd) == 0, "close IN_FENCE_FD sync_file fd");
out_channel:
	nouveau_channel_free(fd, channel);
}

static bool
legacy_pageflip_with_event(int fd, uint32_t crtc_id, uint32_t fb_id,
    const char *what)
{
	struct pageflip_event_state event_state;
	char text[192];
	int saved_errno;
	int ret;

	memset(&event_state, 0, sizeof(event_state));
	errno = 0;
	ret = drmModePageFlip(fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT,
	    &event_state);
	saved_errno = errno;
	snprintf(text, sizeof(text), "%s pageflip ioctl succeeds", what);
	if (ret != 0) {
		printf("    drmModePageFlip errno=%d\n", saved_errno);
		check(false, text);
		return false;
	}
	check(true, text);

	ret = wait_pageflip_event(fd, &event_state, 2000, &saved_errno);
	snprintf(text, sizeof(text), "%s pageflip event arrives", what);
	if (ret != 1) {
		printf("    pageflip event wait ret=%d errno=%d\n", ret,
		    saved_errno);
		check(false, text);
		return false;
	}
	printf("    %s event sequence=%u time=%u.%06u crtc_id=%u\n", what,
	    event_state.sequence, event_state.tv_sec, event_state.tv_usec,
	    event_state.crtc_id);
	check(true, text);
	snprintf(text, sizeof(text), "%s pageflip event reports CRTC id", what);
	check(event_state.crtc_id == crtc_id, text);
	return true;
}

static void
check_legacy_pageflip_reject_snapshot(
    const struct pageflip_counter_snapshot *before,
    const struct pageflip_counter_snapshot *after, const char *what)
{
	char text[192];

	snprintf(text, sizeof(text), "%s keeps page_flip_reject_count monotonic",
	    what);
	check(after->page_flip_reject_count >= before->page_flip_reject_count,
	    text);
	snprintf(text, sizeof(text), "%s does not increment page_flip_count",
	    what);
	check(after->page_flip_count == before->page_flip_count, text);
	snprintf(text, sizeof(text), "%s does not increment page_flip_event_count",
	    what);
	check(after->page_flip_event_count == before->page_flip_event_count,
	    text);
	snprintf(text, sizeof(text), "%s does not increment page_flip_error_count",
	    what);
	check(after->page_flip_error_count == before->page_flip_error_count,
	    text);
	snprintf(text, sizeof(text), "%s does not increment commit_error_count",
	    what);
	check(after->commit_error_count == before->commit_error_count, text);
	snprintf(text, sizeof(text), "%s leaves no active tail transaction", what);
	check(after->atomic_tail_active == 0 && after->atomic_tail_stage == 0,
	    text);
	snprintf(text, sizeof(text), "%s leaves no pending display audit", what);
	check(after->display_audit_pending_valid == 0, text);
}

/*
 * check_legacy_pageflip_reject_contract()
 *
 * Ownership:
 *   Borrows the active CRTC and a caller-owned framebuffer ID.  No event object,
 *   framebuffer, or GEM handle is owned or retained by this helper.
 *
 * Lifetime:
 *   Issues one unsupported legacy pageflip request and requires immediate
 *   EINVAL rejection.  Since the request must not queue a flip, no DRM event is
 *   requested and no restore is required.
 *
 * Threading:
 *   Single-threaded userspace probe.  The kernel may inspect KMS state under
 *   modeset locks, but this helper itself performs no synchronization beyond
 *   the synchronous ioctl return.
 */
static bool
check_legacy_pageflip_reject_contract(int fd, uint32_t crtc_id,
    uint32_t fb_id, uint32_t flags, bool target, const char *what,
    const struct pageflip_counter_snapshot *before,
    struct pageflip_counter_snapshot *after)
{
	char text[192];
	int saved_errno;
	int ret;

	errno = 0;
	if (target) {
		ret = drmModePageFlipTarget(fd, crtc_id, fb_id, flags, NULL, 1);
	} else {
		ret = drmModePageFlip(fd, crtc_id, fb_id, flags, NULL);
	}
	saved_errno = errno;
	snprintf(text, sizeof(text), "%s is rejected", what);
	if (ret == 0) {
		check(false, text);
		return false;
	}
	check(true, text);
	snprintf(text, sizeof(text), "%s is rejected with EINVAL", what);
	check(saved_errno == EINVAL, text);
	if (saved_errno != EINVAL)
		printf("    %s errno=%d\n", what, saved_errno);

	if (!read_pageflip_counter_snapshot(after, what))
		return false;
	check_legacy_pageflip_reject_snapshot(before, after, what);
	return true;
}

/*
 * check_legacy_pageflip_runtime_contract()
 *
 * Ownership:
 *   Borrows the active CRTC and primary plane IDs.  Owns one temporary dumb BO
 *   and framebuffer while the legacy pageflip ioctl borrows the FB ID.  The
 *   probe restores the original FB before removing the temporary FB.
 *
 * Lifetime:
 *   Performs a real legacy MODE_PAGE_FLIP with EVENT to a same-sized linear
 *   framebuffer and waits for the DRM pageflip event.  The previous fb may be
 *   a kernel-owned console fb that is released by old-fb cleanup after the
 *   flip, so restore uses a fresh file-owned linear fb through an atomic
 *   primary commit instead of reusing the stale original FB_ID.
 *
 * Threading:
 *   Single-threaded userspace probe.  The kernel serializes the pageflip
 *   through normal modeset locks and signals completion through the DRM event
 *   queue attached to this fd.
 */
static void
check_legacy_pageflip_runtime_contract(int fd, uint32_t crtc_id,
    uint32_t plane_id, uint32_t crtc_width, uint32_t crtc_height,
    const char *object_name)
{
	struct atomic_plane_snapshot before_snapshot;
	struct atomic_plane_snapshot after_snapshot;
	struct atomic_plane_snapshot restore_snapshot;
	struct pageflip_counter_snapshot before;
	struct pageflip_counter_snapshot after_async_reject;
	struct pageflip_counter_snapshot after_target_reject;
	struct pageflip_counter_snapshot after_flip;
	struct pageflip_counter_snapshot after_restore;
	uint32_t handle = 0;
	uint32_t pitch = 0;
	uint32_t fb_id = 0;
	uint32_t restore_handle = 0;
	uint32_t restore_pitch = 0;
	uint32_t restore_fb = 0;
	bool flipped_to_temp = false;
	bool restored = false;
	bool have_after_flip = false;
	int saved_errno;
	int ret;

	if (!get_plane_snapshot(fd, plane_id, &before_snapshot, object_name))
		return;
	check(before_snapshot.fb_id != 0,
	    "active primary plane has framebuffer for legacy pageflip probe");
	check(before_snapshot.crtc_id == crtc_id,
	    "active primary plane is attached to active CRTC for legacy pageflip probe");
	if (before_snapshot.fb_id == 0 || before_snapshot.crtc_id != crtc_id)
		return;
	if (!read_pageflip_counter_snapshot(&before, "legacy pageflip probe"))
		return;

	if (!create_dumb_buffer_for(fd, crtc_width, crtc_height, 32, &handle,
	    &pitch, "CREATE_DUMB succeeds for legacy pageflip probe"))
		goto out;
	if (!clear_dumb_buffer(fd, handle, pitch, crtc_height,
	    "MAP_DUMB succeeds for legacy pageflip probe"))
		goto out;
	if (!add_linear_framebuffer(fd, crtc_width, crtc_height,
	    DRM_FORMAT_XRGB8888, handle, pitch, &fb_id,
	    "ADDFB2 accepts XRGB8888 linear legacy pageflip probe"))
		goto out;
	if (!create_dumb_buffer_for(fd, crtc_width, crtc_height, 32,
	    &restore_handle, &restore_pitch,
	    "CREATE_DUMB succeeds for legacy pageflip restore probe"))
		goto out;
	if (!clear_dumb_buffer(fd, restore_handle, restore_pitch, crtc_height,
	    "MAP_DUMB succeeds for legacy pageflip restore probe"))
		goto out;
	if (!add_linear_framebuffer(fd, crtc_width, crtc_height,
	    DRM_FORMAT_XRGB8888, restore_handle, restore_pitch, &restore_fb,
	    "ADDFB2 accepts XRGB8888 linear legacy pageflip restore probe"))
		goto out;

	if (!check_legacy_pageflip_reject_contract(fd, crtc_id, fb_id,
	    DRM_MODE_PAGE_FLIP_ASYNC, false, "legacy pageflip ASYNC flag",
	    &before, &after_async_reject))
		goto out;
	if (!check_legacy_pageflip_reject_contract(fd, crtc_id, fb_id,
	    DRM_MODE_PAGE_FLIP_TARGET_RELATIVE, true,
	    "legacy pageflip TARGET flag", &after_async_reject,
	    &after_target_reject))
		goto out;

	flipped_to_temp = legacy_pageflip_with_event(fd, crtc_id, fb_id,
	    "legacy pageflip to temporary FB");
	if (read_pageflip_counter_snapshot(&after_flip,
	    "legacy pageflip temporary FB")) {
		have_after_flip = true;
		check(after_flip.page_flip_count > before.page_flip_count,
		    "legacy pageflip increments page_flip_count");
		check(after_flip.page_flip_event_count >
		    before.page_flip_event_count,
		    "legacy pageflip increments page_flip_event_count");
		check(after_flip.page_flip_error_count ==
		    before.page_flip_error_count,
		    "legacy pageflip does not increment page_flip_error_count");
		check(after_flip.commit_error_count == before.commit_error_count,
		    "legacy pageflip does not increment commit_error_count");
		check(after_flip.atomic_tail_active == 0 &&
		    after_flip.atomic_tail_stage == 0,
		    "legacy pageflip leaves no active tail transaction");
		check(after_flip.display_audit_pending_valid == 0,
		    "legacy pageflip leaves no pending display audit");
	}
	if (!flipped_to_temp)
		goto out;

	restore_snapshot = before_snapshot;
	restore_snapshot.fb_id = restore_fb;
	ret = atomic_primary_commit(fd, plane_id, &restore_snapshot,
	    &saved_errno);
	if (ret != 0) {
		printf("    atomic pageflip restore errno=%d\n", saved_errno);
		check(false, "atomic restore after legacy pageflip succeeds");
	} else {
		check(true, "atomic restore after legacy pageflip succeeds");
		restored = true;
	}

	if (restored && get_plane_snapshot(fd, plane_id, &after_snapshot,
	    object_name)) {
		check(after_snapshot.fb_id == restore_fb,
		    "legacy pageflip restore installs restore FB_ID");
		check(after_snapshot.crtc_id == before_snapshot.crtc_id,
		    "legacy pageflip restore keeps primary CRTC_ID");
	}
	if (restored && read_pageflip_counter_snapshot(&after_restore,
	    "legacy pageflip restore")) {
		check(!have_after_flip || after_restore.page_flip_count ==
		    after_flip.page_flip_count,
		    "atomic pageflip restore does not increment page_flip_count");
		check(!have_after_flip || after_restore.page_flip_event_count ==
		    after_flip.page_flip_event_count,
		    "atomic pageflip restore does not increment page_flip_event_count");
		check(after_restore.page_flip_error_count ==
		    before.page_flip_error_count,
		    "legacy pageflip restore does not increment page_flip_error_count");
		check(after_restore.commit_error_count == before.commit_error_count,
		    "legacy pageflip restore does not increment commit_error_count");
		check(after_restore.atomic_tail_active == 0 &&
		    after_restore.atomic_tail_stage == 0,
		    "legacy pageflip restore leaves no active tail transaction");
		check(after_restore.display_audit_pending_valid == 0,
		    "legacy pageflip restore leaves no pending display audit");
	}

out:
	if (!flipped_to_temp || restored) {
		remove_framebuffer(fd, fb_id,
		    "RMFB succeeds for legacy pageflip probe");
		if (!flipped_to_temp)
			remove_framebuffer(fd, restore_fb,
			    "RMFB succeeds for unused legacy pageflip restore probe");
	} else {
		printf("    legacy pageflip probe left temporary FB active; "
		    "skip RMFB/DESTROY_DUMB\n");
	}
	if (!flipped_to_temp || restored) {
		destroy_dumb_buffer_for(fd, handle,
		    "DESTROY_DUMB succeeds for legacy pageflip probe");
		if (!flipped_to_temp)
			destroy_dumb_buffer_for(fd, restore_handle,
			    "DESTROY_DUMB succeeds for unused legacy pageflip restore probe");
	}
	if (restored)
		printf("    legacy pageflip restore fb=%u handle=%u kept until fd close\n",
		    restore_fb, restore_handle);
}

/*
 * check_atomic_crtc_color_contract()
 *
 * Ownership:
 *   Owns temporary KMS property blobs and destroys them before return.  The
 *   driver borrows the blob IDs only for each TEST_ONLY atomic request.
 *
 * Lifetime:
 *   No CRTC state is committed.  Positive requests prove 1024-entry native
 *   LUTs, identity CTM, and 256-entry legacy LUTs are accepted; the negative
 *   request isolates the unsupported LUT-size gate.
 *
 * Threading:
 *   Single-threaded userspace probe.  The driver evaluates atomic_check under
 *   normal modeset locks, and TEST_ONLY must not program display hardware.
 */
static void
check_atomic_crtc_color_contract(int fd, uint32_t crtc_id)
{
	uint32_t lut1024_blob = 0;
	uint32_t lut256_blob = 0;
	uint32_t invalid_lut_blob = 0;
	uint32_t ctm_blob = 0;
	int saved_errno;
	int ret;

	if (!create_identity_lut_blob(fd, 1024, &lut1024_blob,
	    "CREATE_BLOB succeeds for 1024-entry identity LUT"))
		goto out;
	if (!create_identity_lut_blob(fd, 256, &lut256_blob,
	    "CREATE_BLOB succeeds for legacy 256-entry identity LUT"))
		goto out;
	if (!create_identity_lut_blob(fd, 17, &invalid_lut_blob,
	    "CREATE_BLOB succeeds for invalid-size identity LUT"))
		goto out;
	if (!create_identity_ctm_blob(fd, &ctm_blob,
	    "CREATE_BLOB succeeds for identity CTM"))
		goto out;

	ret = atomic_crtc_color_test_only_commit(fd, crtc_id, lut1024_blob,
	    ctm_blob, lut1024_blob, &saved_errno);
	check(ret == 0,
	    "atomic TEST_ONLY accepts 1024-entry CRTC color state");

	ret = atomic_crtc_color_test_only_commit(fd, crtc_id, lut256_blob,
	    ctm_blob, lut256_blob, &saved_errno);
	check(ret == 0,
	    "atomic TEST_ONLY accepts legacy 256-entry CRTC color state");

	ret = atomic_crtc_color_test_only_commit(fd, crtc_id, invalid_lut_blob,
	    ctm_blob, 0, &saved_errno);
	if (ret == 0) {
		check(false,
		    "atomic TEST_ONLY rejects invalid CRTC LUT size");
	} else {
		check(saved_errno == EINVAL,
		    "atomic TEST_ONLY rejects invalid CRTC LUT size with EINVAL");
	}

out:
	destroy_property_blob(fd, ctm_blob,
	    "DESTROY_BLOB succeeds for identity CTM");
	destroy_property_blob(fd, invalid_lut_blob,
	    "DESTROY_BLOB succeeds for invalid-size identity LUT");
	destroy_property_blob(fd, lut256_blob,
	    "DESTROY_BLOB succeeds for legacy 256-entry identity LUT");
	destroy_property_blob(fd, lut1024_blob,
	    "DESTROY_BLOB succeeds for 1024-entry identity LUT");
}

static bool
check_crtc_blob_property_equals(int fd, uint32_t crtc_id, const char *name,
    uint32_t expected, const char *what)
{
	uint64_t value;

	if (!get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    name, &value, "active CRTC color runtime probe"))
		return false;
	check(value == expected, what);
	return value == expected;
}

/*
 * check_atomic_crtc_color_runtime_contract()
 *
 * Ownership:
 *   Owns temporary LUT/CTM blobs until the restore commit has been attempted.
 *   The active CRTC state borrows blob IDs through atomic KMS; the original
 *   blob IDs are read-only snapshots and are restored without taking ownership.
 *
 * Lifetime:
 *   Mutates the active CRTC by installing identity degamma, CTM, and gamma
 *   blobs, then restores the exact original blob IDs.  The committed identity
 *   blobs must be visible through GETPROPERTY and must drive the display color
 *   tail before the blobs are released.
 *
 * Threading:
 *   Single-threaded userspace probe.  The driver serializes both commits under
 *   normal modeset locks; state counters are sampled only after each commit
 *   returns and the KMS tail has reached its completion point.
 */
static void
check_atomic_crtc_color_runtime_contract(int fd, uint32_t crtc_id)
{
	struct color_counter_snapshot before;
	struct color_counter_snapshot after_commit;
	struct color_counter_snapshot after_restore;
	uint64_t old_degamma_blob;
	uint64_t old_ctm_blob;
	uint64_t old_gamma_blob;
	uint32_t lut1024_blob = 0;
	uint32_t ctm_blob = 0;
	bool committed = false;
	bool restored = false;
	int saved_errno = 0;
	int ret;

	if (!get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "DEGAMMA_LUT", &old_degamma_blob,
	    "active CRTC color runtime probe") ||
	    !get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "CTM", &old_ctm_blob, "active CRTC color runtime probe") ||
	    !get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "GAMMA_LUT", &old_gamma_blob,
	    "active CRTC color runtime probe"))
		return;
	check(old_degamma_blob <= UINT32_MAX,
	    "active CRTC DEGAMMA_LUT blob id fits uint32_t");
	check(old_ctm_blob <= UINT32_MAX,
	    "active CRTC CTM blob id fits uint32_t");
	check(old_gamma_blob <= UINT32_MAX,
	    "active CRTC GAMMA_LUT blob id fits uint32_t");
	if (old_degamma_blob > UINT32_MAX || old_ctm_blob > UINT32_MAX ||
	    old_gamma_blob > UINT32_MAX)
		return;

	if (!read_color_counter_snapshot(&before, "CRTC color runtime probe"))
		return;
	if (!create_identity_lut_blob(fd, 1024, &lut1024_blob,
	    "CREATE_BLOB succeeds for runtime 1024-entry identity LUT"))
		goto out;
	if (!create_identity_ctm_blob(fd, &ctm_blob,
	    "CREATE_BLOB succeeds for runtime identity CTM"))
		goto out;

	ret = atomic_crtc_color_commit(fd, crtc_id, lut1024_blob, ctm_blob,
	    lut1024_blob, &saved_errno);
	if (ret != 0) {
		printf("    CRTC color runtime commit errno=%d\n",
		    saved_errno);
		check(false, "atomic CRTC color runtime commit succeeds");
		goto out;
	}
	committed = true;
	check(true, "atomic CRTC color runtime commit succeeds");
	check_crtc_blob_property_equals(fd, crtc_id, "DEGAMMA_LUT",
	    lut1024_blob, "atomic CRTC color runtime DEGAMMA_LUT is applied");
	check_crtc_blob_property_equals(fd, crtc_id, "CTM", ctm_blob,
	    "atomic CRTC color runtime CTM is applied");
	check_crtc_blob_property_equals(fd, crtc_id, "GAMMA_LUT",
	    lut1024_blob, "atomic CRTC color runtime GAMMA_LUT is applied");

	if (read_color_counter_snapshot(&after_commit,
	    "CRTC color runtime commit")) {
		check(after_commit.commit_error_count ==
		    before.commit_error_count,
		    "atomic CRTC color runtime commit does not increment commit_error_count");
		check(after_commit.color_degamma_lut_count >
		    before.color_degamma_lut_count,
		    "atomic CRTC color runtime commit programs degamma LUT");
		check(after_commit.color_ctm_count > before.color_ctm_count,
		    "atomic CRTC color runtime commit programs CTM");
		check(after_commit.color_gamma_lut_count >
		    before.color_gamma_lut_count,
		    "atomic CRTC color runtime commit programs gamma LUT");
		check(after_commit.atomic_tail_active == 0 &&
		    after_commit.atomic_tail_stage == 0,
		    "atomic CRTC color runtime commit leaves tail idle");
		check(after_commit.display_audit_pending_valid == 0,
		    "atomic CRTC color runtime commit leaves no pending display audit");
	}

	ret = atomic_crtc_color_commit(fd, crtc_id,
	    (uint32_t)old_degamma_blob, (uint32_t)old_ctm_blob,
	    (uint32_t)old_gamma_blob, &saved_errno);
	if (ret != 0) {
		printf("    CRTC color runtime restore errno=%d\n",
		    saved_errno);
		check(false, "atomic CRTC color runtime restore commit succeeds");
		goto out;
	}
	restored = true;
	check(true, "atomic CRTC color runtime restore commit succeeds");
	check_crtc_blob_property_equals(fd, crtc_id, "DEGAMMA_LUT",
	    (uint32_t)old_degamma_blob,
	    "atomic CRTC color runtime DEGAMMA_LUT is restored");
	check_crtc_blob_property_equals(fd, crtc_id, "CTM",
	    (uint32_t)old_ctm_blob,
	    "atomic CRTC color runtime CTM is restored");
	check_crtc_blob_property_equals(fd, crtc_id, "GAMMA_LUT",
	    (uint32_t)old_gamma_blob,
	    "atomic CRTC color runtime GAMMA_LUT is restored");

	if (read_color_counter_snapshot(&after_restore,
	    "CRTC color runtime restore")) {
		check(after_restore.commit_error_count ==
		    before.commit_error_count,
		    "atomic CRTC color runtime restore does not increment commit_error_count");
		check(after_restore.atomic_tail_active == 0 &&
		    after_restore.atomic_tail_stage == 0,
		    "atomic CRTC color runtime restore leaves tail idle");
		check(after_restore.display_audit_pending_valid == 0,
		    "atomic CRTC color runtime restore leaves no pending display audit");
	}

out:
	if (committed && !restored) {
		printf("    CRTC color runtime probe could not restore original "
		    "state; leaving failure visible to caller\n");
	}
	destroy_property_blob(fd, ctm_blob,
	    "DESTROY_BLOB succeeds for runtime identity CTM");
	destroy_property_blob(fd, lut1024_blob,
	    "DESTROY_BLOB succeeds for runtime 1024-entry identity LUT");
}

static void
dump_properties(int fd, uint32_t object_id, uint32_t object_type,
    const char *object_name)
{
	drmModeObjectPropertiesPtr props;

	props = drmModeObjectGetProperties(fd, object_id, object_type);
	if (props == NULL) {
		printf("%s properties: unavailable errno=%d\n", object_name,
		    errno);
		failures++;
		return;
	}

	printf("%s properties: count=%u\n", object_name, props->count_props);
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop;

		prop = drmModeGetProperty(fd, props->props[i]);
		if (prop == NULL) {
			printf("    property id=%u unavailable value=%llu\n",
			    props->props[i],
			    (unsigned long long)props->prop_values[i]);
			failures++;
			continue;
		}
		printf("    %s id=%u type=%s value=%llu\n", prop->name,
		    prop->prop_id, property_type_name(prop->flags),
		    (unsigned long long)props->prop_values[i]);
		for (int j = 0; j < prop->count_enums; j++) {
			if (prop->enums[j].value == props->prop_values[i]) {
				printf("        enum=%s\n", prop->enums[j].name);
				break;
			}
		}
		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
}

/*
 * check_disconnected_connector_contract()
 *
 * Ownership:
 *   Borrows the libdrm connector snapshot and the DRM fd.  Property lookups
 *   allocate temporary libdrm objects and release them before return.
 *
 * Lifetime:
 *   Valid for the current MODE_GETCONNECTOR snapshot only.  The helper does not
 *   keep EDID blobs, connector properties, or modes alive after it returns.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  It does not issue hotplug events or mutate
 *   connector state.
 */
static void
check_disconnected_connector_contract(int fd, drmModeConnector *connector,
    const char *name)
{
	check(connector->count_modes == 0,
	    "disconnected connector exposes no modes");
	check(connector->encoder_id == 0,
	    "disconnected connector has no current encoder");
	check_property_value_is_zero(fd, connector->connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "EDID", name,
	    "disconnected connector EDID is 0");
	check_property_value_is_zero(fd, connector->connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", name,
	    "disconnected connector CRTC_ID is 0");
}

/*
 * check_connected_connector_edid_contract()
 *
 * Ownership:
 *   Borrows the libdrm connector snapshot and DRM fd.  The helper owns each
 *   temporary drmModePropertyPtr and drmModePropertyBlobPtr returned by libdrm,
 *   and releases them before return.
 *
 * Lifetime:
 *   Valid for the current MODE_GETCONNECTOR snapshot only.  The EDID blob ID is
 *   read and consumed immediately; no pointer into the blob survives this call.
 *
 * Threading:
 *   Single-threaded read-only KMS UAPI probe.  It does not mutate connector
 *   state or issue hotplug events, so concurrent hotplug is only reflected in a
 *   later probe run.
 */
static void
check_connected_connector_edid_contract(int fd,
    const drmModeConnector *connector, const char *name)
{
	static const uint8_t edid_header[8] =
	    { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	drmModePropertyBlobPtr blob;
	drmModePropertyPtr prop;
	const uint8_t *edid;
	uint64_t blob_id = 0;
	size_t expected_length;
	bool block_aligned;
	bool checksums_ok = true;
	bool has_base_block;
	bool header_ok;
	bool is_blob;
	char text[192];

	prop = get_property_by_name(fd, connector->connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "EDID", &blob_id);
	snprintf(text, sizeof(text), "%s has property EDID", name);
	check(prop != NULL, text);
	if (prop == NULL)
		return;

	is_blob = (prop->flags & DRM_MODE_PROP_BLOB) != 0;
	check(is_blob, "connected connector EDID property is blob");
	drmModeFreeProperty(prop);
	if (!is_blob)
		return;

	check(blob_id != 0, "connected connector EDID blob is non-zero");
	if (blob_id == 0 || blob_id > UINT32_MAX)
		return;

	blob = drmModeGetPropertyBlob(fd, (uint32_t)blob_id);
	check(blob != NULL, "connected connector EDID blob is readable");
	if (blob == NULL)
		return;

	check(blob->data != NULL, "connected connector EDID blob has data");
	if (blob->data == NULL) {
		drmModeFreePropertyBlob(blob);
		return;
	}

	edid = (const uint8_t *)blob->data;
	has_base_block = blob->length >= 128;
	block_aligned = (blob->length % 128) == 0;
	check(has_base_block, "connected connector EDID blob has base block");
	check(block_aligned,
	    "connected connector EDID blob length is block aligned");

	header_ok = has_base_block &&
	    memcmp(edid, edid_header, sizeof(edid_header)) == 0;
	check(header_ok, "connected connector EDID base header is valid");

	if (has_base_block) {
		expected_length = ((size_t)edid[126] + 1) * 128;
		check(blob->length == expected_length,
		    "connected connector EDID extension count matches blob length");
	} else {
		check(false,
		    "connected connector EDID extension count matches blob length");
	}

	if (has_base_block && block_aligned) {
		for (size_t offset = 0; offset < blob->length; offset += 128) {
			uint8_t sum = 0;

			for (size_t i = 0; i < 128; i++)
				sum = (uint8_t)(sum + edid[offset + i]);
			if (sum != 0) {
				checksums_ok = false;
				break;
			}
		}
		check(checksums_ok,
		    "connected connector EDID block checksums are valid");
	} else {
		check(false,
		    "connected connector EDID block checksums are valid");
	}

	drmModeFreePropertyBlob(blob);
}

static bool
connector_allows_encoder_type(uint32_t connector_type, uint32_t encoder_type)
{
	switch (connector_type) {
	case DRM_MODE_CONNECTOR_VGA:
		return encoder_type == DRM_MODE_ENCODER_DAC;
	case DRM_MODE_CONNECTOR_DVII:
	case DRM_MODE_CONNECTOR_DVID:
	case DRM_MODE_CONNECTOR_HDMIA:
	case DRM_MODE_CONNECTOR_HDMIB:
		return encoder_type == DRM_MODE_ENCODER_TMDS;
	case DRM_MODE_CONNECTOR_DisplayPort:
		/*
		 * DRM has no separate physical SST DP encoder type.  Nouveau and
		 * drm_encoder.h expose SOR-backed DVI/HDMI/SST-DP as TMDS; only
		 * MST virtual connectors use the special DPMST encoder type.
		 */
		return encoder_type == DRM_MODE_ENCODER_TMDS ||
		    encoder_type == DRM_MODE_ENCODER_DPMST;
	case DRM_MODE_CONNECTOR_eDP:
		return encoder_type == DRM_MODE_ENCODER_TMDS;
	case DRM_MODE_CONNECTOR_LVDS:
		return encoder_type == DRM_MODE_ENCODER_LVDS;
	case DRM_MODE_CONNECTOR_TV:
	case DRM_MODE_CONNECTOR_Composite:
	case DRM_MODE_CONNECTOR_SVIDEO:
	case DRM_MODE_CONNECTOR_Component:
	case DRM_MODE_CONNECTOR_9PinDIN:
		return encoder_type == DRM_MODE_ENCODER_TVDAC;
	case DRM_MODE_CONNECTOR_VIRTUAL:
		return encoder_type == DRM_MODE_ENCODER_VIRTUAL;
	default:
		return encoder_type != DRM_MODE_ENCODER_NONE;
	}
}

/*
 * check_connector_encoder_type_contract()
 *
 * Ownership:
 *   Borrows connector encoder IDs from libdrm's connector snapshot and reads
 *   matching encoder snapshots.  It owns no DRM object references beyond each
 *   temporary drmModeEncoderPtr, which is freed before return.
 *
 * Lifetime:
 *   Performs read-only KMS UAPI validation.  It must not create modeset state,
 *   blobs, framebuffers, or connector references.
 *
 * Threading:
 *   Single-threaded userspace probe.  The KMS objects are read without driver
 *   private locks; hotplug races may change future snapshots but not this one.
 */
static void
check_connector_encoder_type_contract(int fd, const drmModeConnector *connector)
{
	drmModeEncoderPtr encoder;

	for (int i = 0; i < connector->count_encoders; i++) {
		encoder = drmModeGetEncoder(fd, connector->encoders[i]);
		check(encoder != NULL,
		    "connector attached encoder is readable for type contract");
		if (encoder == NULL)
			continue;
		printf("    connector encoder %u type=%u\n",
		    encoder->encoder_id, encoder->encoder_type);
		check(connector_allows_encoder_type(connector->connector_type,
		    encoder->encoder_type),
		    "connector encoder type matches connector type");
		if (connector->encoder_id == encoder->encoder_id) {
			check(connector_allows_encoder_type(
			    connector->connector_type, encoder->encoder_type),
			    "connector current encoder type matches connector type");
		}
		drmModeFreeEncoder(encoder);
	}
}

/*
 * check_connected_connector_route_contract()
 *
 * Ownership:
 *   Borrows the connector snapshot and DRM resource snapshot.  The helper
 *   reads the current encoder snapshot and connector properties through libdrm,
 *   releasing every temporary object before return.
 *
 * Lifetime:
 *   Valid only for the current MODE_GETCONNECTOR/MODE_GETENCODER snapshot.
 *   A later modeset or hotplug may legitimately change the active route.
 *
 * Threading:
 *   Single-threaded read-only KMS UAPI validation.  It never issues an atomic
 *   commit or takes driver-private locks.
 */
static void
check_connected_connector_route_contract(int fd,
    const drmModeConnector *connector, const drmModeRes *resources,
    const char *name)
{
	drmModeEncoderPtr encoder;
	uint64_t connector_crtc_id = 0;

	if (connector->encoder_id == 0)
		return;

	if (!get_property_value_checked(fd, connector->connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &connector_crtc_id, name))
		return;

	check(connector_crtc_id != 0,
	    "connected connector CRTC_ID is non-zero");
	check(id_in_list(resources->crtcs, resources->count_crtcs,
	    (uint32_t)connector_crtc_id),
	    "connected connector CRTC_ID is present in resources");

	encoder = drmModeGetEncoder(fd, connector->encoder_id);
	check(encoder != NULL,
	    "connected connector current encoder is readable");
	if (encoder == NULL)
		return;

	check(encoder->crtc_id != 0,
	    "connected connector current encoder has current CRTC");
	check(connector_crtc_id == encoder->crtc_id,
	    "connected connector CRTC_ID matches current encoder CRTC");
	drmModeFreeEncoder(encoder);
}

static void
check_connector(int fd, drmModeConnector *connector,
    const drmModeRes *resources, bool expect_no_connected,
    int *connected_count)
{
	char name[64];
	char text[128];

	snprintf(name, sizeof(name), "connector %u", connector->connector_id);
	printf("%s type=%u status=%s modes=%d encoders=%d\n", name,
	    connector->connector_type,
	    connector_status_name(connector->connection),
	    connector->count_modes, connector->count_encoders);
	for (int m = 0; m < connector->count_modes && m < 8; m++) {
		printf("    mode %ux%u@%u %s\n",
		    connector->modes[m].hdisplay,
		    connector->modes[m].vdisplay,
		    connector->modes[m].vrefresh,
		    connector->modes[m].name);
	}
	dump_properties(fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    name);
	snprintf(text, sizeof(text), "%s is not writeback", name);
	check(connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK, text);
	check(connector->count_encoders > 0, "connector has at least one encoder");
	check_unique_ids(connector->encoders, connector->count_encoders,
	    "connector encoder ids are unique");
	for (int i = 0; i < connector->count_encoders; i++) {
		check(id_in_list(resources->encoders, resources->count_encoders,
		    connector->encoders[i]),
		    "connector encoder id is present in resources");
	}
	check_connector_encoder_type_contract(fd, connector);
	if (connector->connection == DRM_MODE_CONNECTED) {
		(*connected_count)++;
		if (expect_no_connected)
			check(false,
			    "no connected connector exposed when requested");
		check(connector->count_modes > 0,
		    "connected connector exposes at least one mode");
		check(connector->encoder_id != 0,
		    "connected connector has current encoder");
		if (connector->encoder_id != 0) {
			check(id_in_list(connector->encoders,
			    connector->count_encoders, connector->encoder_id),
			    "connected connector current encoder is attached");
			check_connected_connector_edid_contract(fd, connector,
			    name);
			check_connected_connector_route_contract(fd, connector,
			    resources, name);
		}
	} else if (connector->connection == DRM_MODE_DISCONNECTED) {
		check_disconnected_connector_contract(fd, connector, name);
	}
	check_connector_property_contract(fd, connector->connector_id,
	    connector->connector_type, name);
}

static void
check_encoder(int fd, uint32_t encoder_id, const drmModeRes *resources)
{
	drmModeEncoderPtr encoder;
	uint32_t crtc_mask;

	crtc_mask = resources->count_crtcs >= 32 ?
	    UINT32_MAX : ((1u << resources->count_crtcs) - 1u);
	encoder = drmModeGetEncoder(fd, encoder_id);
	check(encoder != NULL, "encoder is readable");
	if (encoder == NULL)
		return;

	printf("encoder %u type=%u crtc=%u possible_crtcs=0x%x possible_clones=0x%x\n",
	    encoder->encoder_id, encoder->encoder_type, encoder->crtc_id,
	    encoder->possible_crtcs, encoder->possible_clones);
	check(encoder->encoder_type != DRM_MODE_ENCODER_NONE,
	    "encoder type is not NONE");
	check(encoder->possible_crtcs != 0,
	    "encoder possible_crtcs is non-empty");
	check((encoder->possible_crtcs & ~crtc_mask) == 0,
	    "encoder possible_crtcs fits resources CRTC mask");
	if (encoder->crtc_id != 0) {
		int crtc_index = -1;
		bool crtc_present;

		crtc_present = id_index_in_list(resources->crtcs,
		    resources->count_crtcs, encoder->crtc_id, &crtc_index);
		check(crtc_present, "encoder current CRTC is present");
		if (crtc_present) {
			check(crtc_index >= 0 && crtc_index < 32,
			    "encoder current CRTC index fits possible_crtcs mask width");
			if (crtc_index >= 0 && crtc_index < 32) {
				check((encoder->possible_crtcs &
				    (1u << crtc_index)) != 0,
				    "encoder current CRTC is allowed by possible_crtcs");
			}
		}
	}

	drmModeFreeEncoder(encoder);
}

static void
check_encoders(int fd, const drmModeRes *resources)
{
	check(resources->count_crtcs > 0 && resources->count_crtcs <= 32,
	    "CRTC count is valid for encoder masks");
	for (int i = 0; i < resources->count_encoders; i++)
		check_encoder(fd, resources->encoders[i], resources);
}

static void
check_crtc_route_state_contract(int fd, uint32_t crtc_id, const char *name);

static void
check_crtc(int fd, uint32_t crtc_id)
{
	char name[64];

	snprintf(name, sizeof(name), "crtc %u", crtc_id);
	dump_properties(fd, crtc_id, DRM_MODE_OBJECT_CRTC, name);
	check_crtc_route_state_contract(fd, crtc_id, name);
	check_crtc_color_property_contract(fd, crtc_id, name);
	check_crtc_sync_property_contract(fd, crtc_id, name);
	check_crtc_unsupported_extension_contract(fd, crtc_id, name);
	check_atomic_crtc_color_contract(fd, crtc_id);
}

static bool
modeinfo_timings_match(const drmModeModeInfo *a, const drmModeModeInfo *b)
{
	return a->clock == b->clock &&
	    a->hdisplay == b->hdisplay &&
	    a->hsync_start == b->hsync_start &&
	    a->hsync_end == b->hsync_end &&
	    a->htotal == b->htotal &&
	    a->hskew == b->hskew &&
	    a->vdisplay == b->vdisplay &&
	    a->vsync_start == b->vsync_start &&
	    a->vsync_end == b->vsync_end &&
	    a->vtotal == b->vtotal &&
	    a->vscan == b->vscan;
}

/*
 * check_active_crtc_mode_blob_contract()
 *
 * Ownership:
 *   Borrows the CRTC object ID and legacy CRTC snapshot from the caller.  The
 *   MODE_ID property blob is owned by this helper and freed before return.
 *
 * Lifetime:
 *   Valid only while the current KMS snapshot is stable.  The blob data is
 *   consumed immediately; no pointer into it survives this call.
 *
 * Threading:
 *   Single-threaded read-only KMS UAPI validation.  It never creates modeset
 *   state or takes driver-private locks.
 */
static void
check_active_crtc_mode_blob_contract(int fd, uint32_t mode_id,
    const drmModeCrtc *crtc)
{
	drmModePropertyBlobPtr blob;
	const drmModeModeInfo *mode;

	blob = drmModeGetPropertyBlob(fd, mode_id);
	check(blob != NULL, "active CRTC MODE_ID blob is readable");
	if (blob == NULL)
		return;

	check(blob->data != NULL, "active CRTC MODE_ID blob has data");
	if (blob->data == NULL) {
		drmModeFreePropertyBlob(blob);
		return;
	}

	check(blob->length == sizeof(*mode),
	    "active CRTC MODE_ID blob has modeinfo size");
	if (blob->length != sizeof(*mode)) {
		drmModeFreePropertyBlob(blob);
		return;
	}

	mode = (const drmModeModeInfo *)blob->data;
	check(mode->hdisplay == crtc->width && mode->vdisplay == crtc->height,
	    "active CRTC MODE_ID blob matches legacy mode size");
	check(modeinfo_timings_match(mode, &crtc->mode),
	    "active CRTC MODE_ID blob matches legacy mode timings");
	check(mode->vrefresh == crtc->mode.vrefresh,
	    "active CRTC MODE_ID blob matches legacy mode refresh");
	check(mode->flags == crtc->mode.flags,
	    "active CRTC MODE_ID blob matches legacy mode flags");
	check(strncmp(mode->name, crtc->mode.name, DRM_DISPLAY_MODE_LEN) == 0,
	    "active CRTC MODE_ID blob matches legacy mode name");

	drmModeFreePropertyBlob(blob);
}

static bool
get_legacy_gamma_raw(int fd, uint32_t crtc_id, uint32_t gamma_size,
    uint16_t *red, uint16_t *green, uint16_t *blue, int *saved_errno_out)
{
	struct drm_mode_crtc_lut lut;
	int saved_errno;
	int ret;

	memset(&lut, 0, sizeof(lut));
	lut.crtc_id = crtc_id;
	lut.gamma_size = gamma_size;
	lut.red = (uintptr_t)red;
	lut.green = (uintptr_t)green;
	lut.blue = (uintptr_t)blue;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GETGAMMA, &lut);
	saved_errno = errno;
	if (saved_errno_out != NULL)
		*saved_errno_out = saved_errno;
	return ret == 0;
}

/*
 * check_legacy_getgamma_contract()
 *
 * Ownership:
 *   Borrows the active CRTC ID and allocates temporary userspace gamma arrays.
 *   No kernel object, blob, or GEM handle is created or retained.
 *
 * Lifetime:
 *   The copied gamma values are consumed only to prove the ioctl copies into
 *   caller-owned storage.  The display gamma state is never modified.
 *
 * Threading:
 *   Single-threaded read-only KMS UAPI probe.  The secondary fd check proves
 *   GETGAMMA is readable without DRM master authority.
 */
static void
check_legacy_getgamma_contract(int fd, uint32_t crtc_id,
    const drmModeCrtc *crtc)
{
	uint16_t *red = NULL;
	uint16_t *green = NULL;
	uint16_t *blue = NULL;
	uint32_t gamma_size = (uint32_t)crtc->gamma_size;
	int secondary_fd = -1;
	int saved_errno;

	check(gamma_size > 0, "legacy GETGAMMA active CRTC gamma size is non-zero");
	if (gamma_size == 0)
		return;

	red = calloc(gamma_size, sizeof(*red));
	green = calloc(gamma_size, sizeof(*green));
	blue = calloc(gamma_size, sizeof(*blue));
	check(red != NULL && green != NULL && blue != NULL,
	    "legacy GETGAMMA allocates owner readback ramps");
	if (red == NULL || green == NULL || blue == NULL)
		goto out;

	saved_errno = 0;
	check(get_legacy_gamma_raw(fd, crtc_id, gamma_size, red, green, blue,
	    &saved_errno), "legacy GETGAMMA owner read succeeds");
	if (saved_errno != 0)
		printf("    legacy GETGAMMA owner errno=%d\n", saved_errno);

	saved_errno = 0;
	check(!get_legacy_gamma_raw(fd, crtc_id, gamma_size + 1, red, green,
	    blue, &saved_errno), "legacy GETGAMMA rejects wrong gamma size");
	check(saved_errno == EINVAL,
	    "legacy GETGAMMA wrong gamma size fails with EINVAL");

	errno = 0;
	secondary_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	saved_errno = errno;
	check(secondary_fd >= 0,
	    "legacy GETGAMMA opens secondary card fd");
	if (secondary_fd < 0) {
		printf("    legacy GETGAMMA secondary open errno=%d\n",
		    saved_errno);
		goto out;
	}

	memset(red, 0, gamma_size * sizeof(*red));
	memset(green, 0, gamma_size * sizeof(*green));
	memset(blue, 0, gamma_size * sizeof(*blue));
	saved_errno = 0;
	check(get_legacy_gamma_raw(secondary_fd, crtc_id, gamma_size, red,
	    green, blue, &saved_errno),
	    "legacy GETGAMMA non-master read succeeds");
	if (saved_errno != 0)
		printf("    legacy GETGAMMA non-master errno=%d\n",
		    saved_errno);
	check(close(secondary_fd) == 0,
	    "legacy GETGAMMA closes secondary card fd");

out:
	free(blue);
	free(green);
	free(red);
}

/*
 * check_crtc_route_state_contract()
 *
 * Ownership:
 *   Borrows the DRM fd and CRTC object ID from the caller.  The temporary CRTC
 *   snapshot and any MODE_ID blob opened by this function are released before
 *   return.
 *
 * Lifetime:
 *   Valid only for the current KMS snapshot.  A later modeset, DPMS change, or
 *   lastclose restore may legitimately change ACTIVE, MODE_ID, buffer_id, and
 *   mode geometry.
 *
 * Threading:
 *   Single-threaded read-only KMS UAPI validation.  It does not submit modeset
 *   work and does not take driver-private locks.
 */
static void
check_crtc_route_state_contract(int fd, uint32_t crtc_id, const char *name)
{
	drmModeCrtcPtr crtc;
	uint64_t active = 0;
	uint64_t mode_id = 0;
	bool active_enabled;

	crtc = drmModeGetCrtc(fd, crtc_id);
	check(crtc != NULL, "CRTC state is readable");
	if (crtc == NULL)
		return;

	if (!get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "ACTIVE", &active, name) ||
	    !get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "MODE_ID", &mode_id, name)) {
		drmModeFreeCrtc(crtc);
		return;
	}

	active_enabled = active != 0;
	check(active_enabled == (mode_id != 0),
	    "CRTC ACTIVE and MODE_ID enable state match");
	check(active_enabled == (crtc->mode_valid != 0),
	    "CRTC ACTIVE matches legacy mode_valid");

	if (active_enabled) {
		check(crtc->buffer_id != 0, "active CRTC has framebuffer");
		check(crtc->width > 0 && crtc->height > 0,
		    "active CRTC has non-zero size");
		check(mode_id <= UINT32_MAX,
		    "active CRTC MODE_ID fits property blob id");
		if (mode_id <= UINT32_MAX)
			check_active_crtc_mode_blob_contract(fd,
			    (uint32_t)mode_id, crtc);
		check_legacy_getgamma_contract(fd, crtc_id, crtc);
	} else {
		check(crtc->buffer_id == 0, "inactive CRTC has no framebuffer");
	}

	drmModeFreeCrtc(crtc);
}

static bool
find_active_crtc(int fd, const drmModeRes *resources, uint32_t *crtc_id_out,
    uint32_t *crtc_index_out)
{
	for (int i = 0; i < resources->count_crtcs; i++) {
		drmModePropertyPtr prop;
		uint64_t active = 0;

		prop = get_property_by_name(fd, resources->crtcs[i],
		    DRM_MODE_OBJECT_CRTC, "ACTIVE", &active);
		if (prop == NULL)
			continue;
		drmModeFreeProperty(prop);
		if (active != 0) {
			*crtc_id_out = resources->crtcs[i];
			*crtc_index_out = (uint32_t)i;
			return true;
		}
	}
	return false;
}

static bool
wait_vblank_type_for_crtc_index(uint32_t crtc_index,
    drmVBlankSeqType *type_out)
{
	if (crtc_index >= 32)
		return false;
	*type_out = DRM_VBLANK_RELATIVE |
	    (crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT);
	return true;
}

/*
 * check_vblank_sequence_runtime_contract()
 *
 * Ownership:
 *   Borrows the DRM fd and active CRTC identity from the caller.  It does not
 *   retain the CRTC object ID or any event pointer after return.
 *
 * Lifetime:
 *   Runs against the currently active scanout head.  It does not change
 *   modes, framebuffers, planes, connector routing, or cursor state.
 *
 * Threading:
 *   Single-threaded userspace probe.  The kernel owns vblank references and
 *   pending event state until each ioctl or queued event completes.
 */
static void
check_vblank_sequence_runtime_contract(int fd, uint32_t crtc_id,
    uint32_t crtc_index)
{
	struct raw_vblank_event raw_vblank;
	struct sequence_event_state event_state;
	struct sequence_event_state invalid_event_state;
	struct drm_crtc_queue_sequence invalid_queue;
	drmVBlank vblank;
	drmVBlankSeqType vblank_type;
	uint64_t sequence_before = 0;
	uint64_t sequence_after = 0;
	uint64_t sequence_ns = 0;
	uint64_t queued_sequence = 0;
	uint64_t user_data;
	int saved_errno = 0;
	int ret;

	errno = 0;
	ret = drmCrtcGetSequence(fd, crtc_id, &sequence_before,
	    &sequence_ns);
	if (ret != 0) {
		printf("    drmCrtcGetSequence errno=%d\n", errno);
		check(false, "drmCrtcGetSequence succeeds on active CRTC");
		return;
	}
	check(true, "drmCrtcGetSequence succeeds on active CRTC");
	printf("    active crtc %u sequence=%llu ns=%llu\n", crtc_id,
	    (unsigned long long)sequence_before,
	    (unsigned long long)sequence_ns);

	if (!wait_vblank_type_for_crtc_index(crtc_index, &vblank_type)) {
		check(false, "active CRTC index fits legacy WAIT_VBLANK UAPI");
		return;
	}
	check(true, "active CRTC index fits legacy WAIT_VBLANK UAPI");

	memset(&vblank, 0, sizeof(vblank));
	vblank.request.type = vblank_type;
	vblank.request.sequence = 1;
	errno = 0;
	ret = drmWaitVBlank(fd, &vblank);
	if (ret != 0) {
		printf("    drmWaitVBlank errno=%d\n", errno);
		check(false, "drmWaitVBlank relative wait succeeds on active CRTC");
		return;
	}
	check(true, "drmWaitVBlank relative wait succeeds on active CRTC");
	printf("    wait_vblank sequence=%u time=%ld.%06ld\n",
	    vblank.reply.sequence, vblank.reply.tval_sec,
	    vblank.reply.tval_usec);

	errno = 0;
	ret = drmCrtcGetSequence(fd, crtc_id, &sequence_after,
	    &sequence_ns);
	if (ret != 0) {
		printf("    drmCrtcGetSequence after wait errno=%d\n", errno);
		check(false, "drmCrtcGetSequence succeeds after WAIT_VBLANK");
		return;
	}
	check(true, "drmCrtcGetSequence succeeds after WAIT_VBLANK");
	check(sequence_after != sequence_before,
	    "WAIT_VBLANK advances active CRTC sequence");

	memset(&raw_vblank, 0, sizeof(raw_vblank));
	user_data = 0x4e564b4d56424c4bULL;
	memset(&vblank, 0, sizeof(vblank));
	vblank.request.type = vblank_type | DRM_VBLANK_EVENT;
	vblank.request.sequence = 1;
	vblank.request.signal = (unsigned long)user_data;
	errno = 0;
	ret = drmWaitVBlank(fd, &vblank);
	if (ret != 0) {
		printf("    drmWaitVBlank event errno=%d\n", errno);
		check(false, "drmWaitVBlank event queues active CRTC event");
		return;
	}
	check(true, "drmWaitVBlank event queues active CRTC event");

	ret = read_raw_vblank_event(fd, &raw_vblank, 2000, &saved_errno);
	if (ret != 1) {
		printf("    raw vblank event wait ret=%d errno=%d\n", ret,
		    saved_errno);
		check(false, "drmWaitVBlank event arrives");
		return;
	}
	check(true, "drmWaitVBlank event arrives");
	check(raw_vblank.event.user_data == user_data,
	    "drmWaitVBlank event preserves user_data");
	check(raw_vblank.event.crtc_id == crtc_id,
	    "drmWaitVBlank event reports active CRTC id");
	check(raw_vblank.event.sequence >= vblank.reply.sequence,
	    "drmWaitVBlank event reaches queued sequence");
	printf("    raw vblank event sequence=%u crtc_id=%u\n",
	    raw_vblank.event.sequence, raw_vblank.event.crtc_id);

	memset(&invalid_event_state, 0, sizeof(invalid_event_state));
	memset(&invalid_queue, 0, sizeof(invalid_queue));
	invalid_queue.crtc_id = crtc_id;
	invalid_queue.flags = DRM_CRTC_SEQUENCE_RELATIVE | 0x80000000u;
	invalid_queue.sequence = 1;
	invalid_queue.user_data = (uint64_t)(uintptr_t)&invalid_event_state;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &invalid_queue);
	saved_errno = errno;
	check(ret != 0, "drmCrtcQueueSequence rejects unknown flags");
	check(saved_errno == EINVAL,
	    "drmCrtcQueueSequence unknown flags fail with EINVAL");
	if (ret == 0) {
		(void)wait_sequence_event(fd, &invalid_event_state, 1000,
		    &saved_errno);
	}

	memset(&event_state, 0, sizeof(event_state));
	user_data = (uint64_t)(uintptr_t)&event_state;
	errno = 0;
	ret = drmCrtcQueueSequence(fd, crtc_id, DRM_CRTC_SEQUENCE_RELATIVE,
	    1, &queued_sequence, user_data);
	if (ret != 0) {
		printf("    drmCrtcQueueSequence errno=%d\n", errno);
		check(false, "drmCrtcQueueSequence queues active CRTC event");
		return;
	}
	check(true, "drmCrtcQueueSequence queues active CRTC event");
	printf("    queued crtc sequence=%llu\n",
	    (unsigned long long)queued_sequence);

	ret = wait_sequence_event(fd, &event_state, 2000, &saved_errno);
	if (ret != 1) {
		printf("    sequence event wait ret=%d errno=%d\n", ret,
		    saved_errno);
		check(false, "drmCrtcQueueSequence event arrives");
		return;
	}
	check(true, "drmCrtcQueueSequence event arrives");
	check(event_state.count == 1,
	    "drmCrtcQueueSequence delivers exactly one event");
	check(event_state.user_data == user_data,
	    "drmCrtcQueueSequence preserves user_data");
	check(event_state.sequence >= queued_sequence,
	    "drmCrtcQueueSequence event reaches queued sequence");
	printf("    sequence event sequence=%llu ns=%llu\n",
	    (unsigned long long)event_state.sequence,
	    (unsigned long long)event_state.ns);
}

static void
check_drm_lease_vblank_sequence_contract(int lease_fd, uint32_t crtc_id)
{
	int failures_before;

	failures_before = failures;
	check_vblank_sequence_runtime_contract(lease_fd, crtc_id, 0);
	check(failures == failures_before,
	    "DRM lease vblank sequence accepts leased CRTC");
}

static void
check_drm_lease_empty_vblank_rejects(int lease_fd, uint32_t crtc_id)
{
	drmVBlank vblank;
	drmVBlankSeqType vblank_type;
	uint64_t queued_sequence = 0;
	uint64_t sequence = 0;
	uint64_t sequence_ns = 0;
	int saved_errno;
	int ret;

	errno = 0;
	ret = drmCrtcGetSequence(lease_fd, crtc_id, &sequence, &sequence_ns);
	saved_errno = errno;
	check(ret != 0, "DRM empty lease CRTC_GET_SEQUENCE is rejected");
	check(saved_errno == ENOENT,
	    "DRM empty lease CRTC_GET_SEQUENCE fails with ENOENT");

	errno = 0;
	ret = drmCrtcQueueSequence(lease_fd, crtc_id,
	    DRM_CRTC_SEQUENCE_RELATIVE, 1, &queued_sequence, 0);
	saved_errno = errno;
	check(ret != 0, "DRM empty lease CRTC_QUEUE_SEQUENCE is rejected");
	check(saved_errno == ENOENT,
	    "DRM empty lease CRTC_QUEUE_SEQUENCE fails with ENOENT");

	if (!wait_vblank_type_for_crtc_index(0, &vblank_type)) {
		check(false, "DRM empty lease WAIT_VBLANK builds CRTC index 0");
		return;
	}
	check(true, "DRM empty lease WAIT_VBLANK builds CRTC index 0");

	memset(&vblank, 0, sizeof(vblank));
	vblank.request.type = vblank_type;
	vblank.request.sequence = 0;
	errno = 0;
	ret = drmWaitVBlank(lease_fd, &vblank);
	saved_errno = errno;
	check(ret != 0, "DRM empty lease WAIT_VBLANK is rejected");
	check(saved_errno == EINVAL,
	    "DRM empty lease WAIT_VBLANK fails with EINVAL");
}

static bool
find_active_connector_for_crtc(int fd, const drmModeRes *resources,
    uint32_t crtc_id, uint32_t *connector_id_out)
{
	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnectorPtr connector;
		drmModePropertyPtr prop;
		uint64_t connector_crtc = 0;
		bool found = false;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector == NULL)
			continue;

		prop = get_property_by_name(fd, connector->connector_id,
		    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &connector_crtc);
		if (prop != NULL) {
			drmModeFreeProperty(prop);
			found = connector_crtc == crtc_id;
		} else if (connector->encoder_id != 0) {
			drmModeEncoderPtr encoder;

			encoder = drmModeGetEncoder(fd, connector->encoder_id);
			if (encoder != NULL) {
				found = encoder->crtc_id == crtc_id;
				drmModeFreeEncoder(encoder);
			}
		}

		if (found) {
			*connector_id_out = connector->connector_id;
			drmModeFreeConnector(connector);
			return true;
		}
		drmModeFreeConnector(connector);
	}
	return false;
}

static bool
get_crtc_size(int fd, uint32_t crtc_id, uint32_t *width_out,
    uint32_t *height_out)
{
	drmModeCrtcPtr crtc;
	bool ok;

	crtc = drmModeGetCrtc(fd, crtc_id);
	if (crtc == NULL)
		return false;
	ok = crtc->mode_valid && crtc->width > 0 && crtc->height > 0;
	if (ok) {
		*width_out = (uint32_t)crtc->width;
		*height_out = (uint32_t)crtc->height;
	}
	drmModeFreeCrtc(crtc);
	return ok;
}

static bool
range_valid(size_t offset, size_t count, size_t elem_size, size_t total)
{
	if (offset > total)
		return false;
	if (elem_size != 0 && count > (SIZE_MAX - offset) / elem_size)
		return false;
	return offset + count * elem_size <= total;
}

static bool
modifier_has_format(const struct drm_format_modifier *modifier,
    uint32_t format_index)
{
	uint32_t bit;

	if (format_index < modifier->offset)
		return false;
	bit = format_index - modifier->offset;
	if (bit >= 64)
		return false;
	return (modifier->formats & (1ULL << bit)) != 0;
}

static int
find_format_index(const uint32_t *formats, uint32_t count_formats,
    uint32_t format)
{
	for (uint32_t i = 0; i < count_formats; i++) {
		if (formats[i] == format)
			return (int)i;
	}
	return -1;
}

static bool
format_has_linear_modifier(const uint32_t *formats,
    const struct drm_format_modifier *modifiers, uint32_t count_formats,
    uint32_t count_modifiers, uint32_t format)
{
	int index;

	index = find_format_index(formats, count_formats, format);
	if (index < 0)
		return false;
	for (uint32_t i = 0; i < count_modifiers; i++) {
		if (modifiers[i].modifier == DRM_FORMAT_MOD_LINEAR &&
		    modifier_has_format(&modifiers[i], (uint32_t)index))
			return true;
	}
	return false;
}

static bool
format_has_nvidia_modifier(const uint32_t *formats,
    const struct drm_format_modifier *modifiers, uint32_t count_formats,
    uint32_t count_modifiers, uint32_t format)
{
	int index;

	index = find_format_index(formats, count_formats, format);
	if (index < 0)
		return false;
	for (uint32_t i = 0; i < count_modifiers; i++) {
		if ((modifiers[i].modifier >> 56) == DRM_FORMAT_MOD_VENDOR_NVIDIA &&
		    modifier_has_format(&modifiers[i], (uint32_t)index))
			return true;
	}
	return false;
}

static bool
format_has_nonlinear_modifier(const uint32_t *formats,
    const struct drm_format_modifier *modifiers, uint32_t count_formats,
    uint32_t count_modifiers, uint32_t format)
{
	int index;

	index = find_format_index(formats, count_formats, format);
	if (index < 0)
		return false;
	for (uint32_t i = 0; i < count_modifiers; i++) {
		if (modifiers[i].modifier != DRM_FORMAT_MOD_LINEAR &&
		    modifier_has_format(&modifiers[i], (uint32_t)index))
			return true;
	}
	return false;
}

static bool
find_first_nvidia_modifier(const struct drm_format_modifier *modifiers,
    uint32_t count_modifiers, uint64_t *modifier_out)
{
	for (uint32_t i = 0; i < count_modifiers; i++) {
		if ((modifiers[i].modifier >> 56) ==
		    DRM_FORMAT_MOD_VENDOR_NVIDIA) {
			*modifier_out = modifiers[i].modifier;
			return true;
		}
	}
	return false;
}

static bool
create_dumb_buffer_for(int fd, uint32_t width, uint32_t height, uint32_t bpp,
    uint32_t *handle_out, uint32_t *pitch_out, const char *what)
{
	struct drm_mode_create_dumb create;
	bool ok;

	memset(&create, 0, sizeof(create));
	create.width = width;
	create.height = height;
	create.bpp = bpp;

	ok = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0;
	check(ok, what);
	if (!ok)
		return false;

	*handle_out = create.handle;
	*pitch_out = create.pitch;
	return true;
}

static void
destroy_dumb_buffer_for(int fd, uint32_t handle, const char *what)
{
	struct drm_mode_destroy_dumb destroy;

	if (handle == 0)
		return;
	memset(&destroy, 0, sizeof(destroy));
	destroy.handle = handle;
	check(drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) == 0, what);
}

static void
close_gem_handle_for(int fd, uint32_t handle, const char *what)
{
	struct drm_gem_close close_args;

	if (handle == 0)
		return;
	memset(&close_args, 0, sizeof(close_args));
	close_args.handle = handle;
	check(drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_args) == 0, what);
}

static bool
create_dumb_buffer(int fd, uint32_t width, uint32_t height, uint32_t bpp,
    uint32_t *handle_out, uint32_t *pitch_out)
{
	return create_dumb_buffer_for(fd, width, height, bpp, handle_out,
	    pitch_out, "CREATE_DUMB succeeds for ADDFB2 negative probe");
}

static void
destroy_dumb_buffer(int fd, uint32_t handle)
{
	destroy_dumb_buffer_for(fd, handle,
	    "DESTROY_DUMB succeeds for ADDFB2 negative probe");
}

static void
check_create_dumb_error(int fd, uint32_t width, uint32_t height, uint32_t bpp,
    uint32_t flags, int expected_errno, const char *what,
    const char *errno_what)
{
	struct drm_mode_create_dumb create;
	int saved_errno;
	int ret;

	memset(&create, 0, sizeof(create));
	create.width = width;
	create.height = height;
	create.bpp = bpp;
	create.flags = flags;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	saved_errno = errno;
	check(ret != 0, what);
	check(saved_errno == expected_errno, errno_what);
	if (ret == 0)
		destroy_dumb_buffer_for(fd, create.handle,
		    "destroy unexpected dumb buffer from negative create probe");
}

static void
check_map_dumb_error(int fd, uint32_t handle, int expected_errno,
    const char *what, const char *errno_what)
{
	struct drm_mode_map_dumb map;
	int saved_errno;
	int ret;

	memset(&map, 0, sizeof(map));
	map.handle = handle;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	saved_errno = errno;
	check(ret != 0, what);
	check(saved_errno == expected_errno, errno_what);
}

static void
check_destroy_dumb_error(int fd, uint32_t handle, int expected_errno,
    const char *what, const char *errno_what)
{
	struct drm_mode_destroy_dumb destroy;
	int saved_errno;
	int ret;

	memset(&destroy, 0, sizeof(destroy));
	destroy.handle = handle;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	saved_errno = errno;
	check(ret != 0, what);
	check(saved_errno == expected_errno, errno_what);
}

/*
 * check_dumb_buffer_lifetime_contract()
 *
 * Ownership:
 *   Creates one dumb GEM handle owned by the caller's DRM file.  A second card
 *   fd only reuses the numeric handle value to prove GEM handles are per-file
 *   and must not gain mmap or destroy authority.
 *
 * Lifetime:
 *   The temporary dumb BO is never attached to an FB or plane.  Owner destroy
 *   releases the handle before return; later map/destroy operations on the same
 *   numeric handle must fail with ENOENT.
 *
 * Threading:
 *   Single-threaded KMS/GEM UAPI probe.  No mmap pointer, framebuffer, or
 *   display state outlives this helper.
 */
static void
check_dumb_buffer_lifetime_contract(int fd)
{
	struct drm_mode_create_dumb create;
	struct drm_mode_map_dumb map;
	uint64_t min_size;
	uint32_t handle = 0;
	int secondary_fd = -1;
	int saved_errno;
	int ret;

	check_create_dumb_error(fd, 0, 16, 32, 0, EINVAL,
	    "CREATE_DUMB rejects zero width",
	    "CREATE_DUMB zero width fails with EINVAL");
	check_create_dumb_error(fd, 16, 0, 32, 0, EINVAL,
	    "CREATE_DUMB rejects zero height",
	    "CREATE_DUMB zero height fails with EINVAL");
	check_create_dumb_error(fd, 16, 16, 0, 0, EINVAL,
	    "CREATE_DUMB rejects zero bpp",
	    "CREATE_DUMB zero bpp fails with EINVAL");
	check_create_dumb_error(fd, 16, 16, 32, 1, EINVAL,
	    "CREATE_DUMB rejects unknown flags",
	    "CREATE_DUMB unknown flags fail with EINVAL");

	memset(&create, 0, sizeof(create));
	create.width = 17;
	create.height = 9;
	create.bpp = 32;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	saved_errno = errno;
	check(ret == 0, "CREATE_DUMB lifecycle probe succeeds");
	if (ret != 0) {
		printf("    CREATE_DUMB lifecycle errno=%d\n", saved_errno);
		return;
	}
	handle = create.handle;
	check(create.handle != 0, "CREATE_DUMB returns non-zero handle");
	check(create.pitch >= create.width * 4,
	    "CREATE_DUMB pitch covers requested width");
	min_size = (uint64_t)create.pitch * create.height;
	check(create.size >= min_size,
	    "CREATE_DUMB size covers requested pitch and height");

	memset(&map, 0, sizeof(map));
	map.handle = handle;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	saved_errno = errno;
	check(ret == 0, "MAP_DUMB owner handle succeeds");
	if (ret != 0)
		printf("    MAP_DUMB owner errno=%d\n", saved_errno);

	secondary_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	check(secondary_fd >= 0,
	    "DUMB buffer lifecycle opens secondary card fd");
	if (secondary_fd >= 0) {
		check_map_dumb_error(secondary_fd, handle, ENOENT,
		    "MAP_DUMB foreign fd handle is denied",
		    "MAP_DUMB foreign fd handle fails with ENOENT");
		check_destroy_dumb_error(secondary_fd, handle, ENOENT,
		    "DESTROY_DUMB foreign fd handle is denied",
		    "DESTROY_DUMB foreign fd handle fails with ENOENT");
		check(close(secondary_fd) == 0,
		    "DUMB buffer lifecycle closes secondary card fd");
	}

	destroy_dumb_buffer_for(fd, handle,
	    "DESTROY_DUMB owner handle succeeds");
	handle = 0;

	check_map_dumb_error(fd, create.handle, ENOENT,
	    "MAP_DUMB destroyed handle is rejected",
	    "MAP_DUMB destroyed handle fails with ENOENT");
	check_destroy_dumb_error(fd, create.handle, ENOENT,
	    "DESTROY_DUMB destroyed handle is rejected",
	    "DESTROY_DUMB destroyed handle fails with ENOENT");
}

static bool
read_drm_state_text(char **text_out)
{
	char *text;
	size_t length;

	*text_out = NULL;
	length = 0;
	if (sysctlbyname("dev.drm.0.state", NULL, &length, NULL, 0) != 0 ||
	    length == 0)
		return false;

	text = calloc(1, length + 1);
	if (text == NULL)
		return false;
	if (sysctlbyname("dev.drm.0.state", text, &length, NULL, 0) != 0) {
		free(text);
		return false;
	}
	text[length] = '\0';
	*text_out = text;
	return true;
}

static bool
state_counter_from_text(const char *text, const char *key,
    uint64_t *value_out)
{
	const char *line;
	size_t key_length;

	key_length = strlen(key);
	for (line = text; line != NULL && *line != '\0';) {
		const char *next_line;
		const char *value;
		char *end;

		while (*line == ' ' || *line == '\t')
			line++;
		next_line = strchr(line, '\n');
		if (strncmp(line, key, key_length) == 0) {
			value = line + key_length;
			while (*value == ' ' || *value == '\t')
				value++;
			if (*value == '=') {
				value++;
				while (*value == ' ' || *value == '\t')
					value++;
				errno = 0;
				*value_out = strtoull(value, &end, 0);
				return end != value && errno == 0;
			}
		}
		if (next_line == NULL)
			break;
		line = next_line + 1;
	}
	return false;
}

static bool
state_string_from_text(const char *text, const char *key, char *value_out,
    size_t value_size)
{
	const char *line;
	size_t key_length;

	if (value_size == 0)
		return false;

	key_length = strlen(key);
	for (line = text; line != NULL && *line != '\0';) {
		const char *next_line;
		const char *value;
		size_t length;

		while (*line == ' ' || *line == '\t')
			line++;
		next_line = strchr(line, '\n');
		if (strncmp(line, key, key_length) == 0) {
			value = line + key_length;
			while (*value == ' ' || *value == '\t')
				value++;
			if (*value != '=')
				return false;
			value++;
			while (*value == ' ' || *value == '\t')
				value++;
			length = next_line != NULL ?
			    (size_t)(next_line - value) : strlen(value);
			while (length > 0 &&
			    (value[length - 1] == ' ' ||
			    value[length - 1] == '\t' ||
			    value[length - 1] == '\r'))
				length--;
			if (length >= value_size)
				length = value_size - 1;
			memcpy(value_out, value, length);
			value_out[length] = '\0';
			return true;
		}
		if (next_line == NULL)
			break;
		line = next_line + 1;
	}
	return false;
}

static bool
read_connector_fill_modes_count(uint64_t *count_out, const char *stage)
{
	char text[192];
	char *state;
	bool ok;

	ok = read_drm_state_text(&state);
	snprintf(text, sizeof(text), "DRM state is readable before %s", stage);
	check(ok, text);
	if (!ok)
		return false;

	ok = state_counter_from_text(state, "connector_fill_modes_count",
	    count_out);
	snprintf(text, sizeof(text),
	    "connector fill_modes counter is present before %s", stage);
	check(ok, text);
	free(state);
	return ok;
}

static bool
read_modeset_counter_snapshot(struct modeset_counter_snapshot *snapshot,
    const char *stage)
{
	char text[192];
	char *state;
	bool ok;

	ok = read_drm_state_text(&state);
	snprintf(text, sizeof(text), "DRM state is readable before %s", stage);
	check(ok, text);
	if (!ok)
		return false;

	memset(snapshot, 0, sizeof(*snapshot));
	ok = state_counter_from_text(state, "atomic_tail_disable_op_count",
	    &snapshot->atomic_tail_disable_op_count) &&
	    state_counter_from_text(state, "atomic_tail_enable_op_count",
	    &snapshot->atomic_tail_enable_op_count) &&
	    state_counter_from_text(state, "atomic_tail_last_disable_op_count",
	    &snapshot->atomic_tail_last_disable_op_count) &&
	    state_counter_from_text(state, "atomic_tail_last_enable_op_count",
	    &snapshot->atomic_tail_last_enable_op_count) &&
	    state_counter_from_text(state, "atomic_tail_last_disable_heads",
	    &snapshot->atomic_tail_last_disable_heads) &&
	    state_counter_from_text(state, "atomic_tail_last_enable_heads",
	    &snapshot->atomic_tail_last_enable_heads) &&
	    state_counter_from_text(state, "atomic_tail_last_new_active_heads",
	    &snapshot->atomic_tail_last_new_active_heads) &&
	    state_counter_from_text(state, "atomic_disable_vblank_off_count",
	    &snapshot->atomic_disable_vblank_off_count) &&
	    state_counter_from_text(state, "atomic_disable_vblank_keep_count",
	    &snapshot->atomic_disable_vblank_keep_count) &&
	    state_counter_from_text(state, "commit_error_count",
	    &snapshot->commit_error_count) &&
	    state_counter_from_text(state, "atomic_tail_active",
	    &snapshot->atomic_tail_active) &&
	    state_counter_from_text(state, "atomic_tail_stage",
	    &snapshot->atomic_tail_stage) &&
	    state_counter_from_text(state, "display_audit_pending_valid",
	    &snapshot->display_audit_pending_valid) &&
	    state_string_from_text(state, "display_audit_current_op",
	    snapshot->display_audit_current_op,
	    sizeof(snapshot->display_audit_current_op));
	if (ok && snapshot->display_audit_pending_valid != 0)
		ok = state_string_from_text(state, "display_audit_pending_op",
		    snapshot->display_audit_pending_op,
		    sizeof(snapshot->display_audit_pending_op));
	snprintf(text, sizeof(text), "modeset counters are present before %s",
	    stage);
	check(ok, text);
	free(state);
	return ok;
}

static bool
read_color_counter_snapshot(struct color_counter_snapshot *snapshot,
    const char *stage)
{
	char text[160];
	char *state;
	bool ok;

	ok = read_drm_state_text(&state);
	snprintf(text, sizeof(text), "DRM state is readable before %s", stage);
	check(ok, text);
	if (!ok)
		return false;

	memset(snapshot, 0, sizeof(*snapshot));
	ok = state_counter_from_text(state, "atomic_tail_color_op_count",
	    &snapshot->atomic_tail_color_op_count) &&
	    state_counter_from_text(state, "atomic_tail_last_color_op_count",
	    &snapshot->atomic_tail_last_color_op_count) &&
	    state_counter_from_text(state, "color_degamma_lut_count",
	    &snapshot->color_degamma_lut_count) &&
	    state_counter_from_text(state, "color_ctm_count",
	    &snapshot->color_ctm_count) &&
	    state_counter_from_text(state, "color_gamma_lut_count",
	    &snapshot->color_gamma_lut_count) &&
	    state_counter_from_text(state, "commit_error_count",
	    &snapshot->commit_error_count) &&
	    state_counter_from_text(state, "atomic_tail_active",
	    &snapshot->atomic_tail_active) &&
	    state_counter_from_text(state, "atomic_tail_stage",
	    &snapshot->atomic_tail_stage) &&
	    state_counter_from_text(state, "display_audit_pending_valid",
	    &snapshot->display_audit_pending_valid);
	snprintf(text, sizeof(text), "color counters are present before %s",
	    stage);
	check(ok, text);
	free(state);
	return ok;
}

static bool
read_cursor_counter_snapshot(struct cursor_counter_snapshot *snapshot,
    const char *stage, uint32_t head)
{
	char text[160];
	char key[64];
	char *state;
	bool ok;

	ok = read_drm_state_text(&state);
	snprintf(text, sizeof(text), "DRM state is readable before %s", stage);
	check(ok, text);
	if (!ok)
		return false;

	memset(snapshot, 0, sizeof(*snapshot));
	ok = state_counter_from_text(state, "plane_update_count",
	    &snapshot->plane_update_count) &&
	    state_counter_from_text(state, "cursor_update_count",
	    &snapshot->cursor_update_count) &&
	    state_counter_from_text(state, "cursor_async_update_count",
	    &snapshot->cursor_async_update_count) &&
	    state_counter_from_text(state, "cursor_disable_count",
	    &snapshot->cursor_disable_count) &&
	    state_counter_from_text(state, "cursor_error_count",
	    &snapshot->cursor_error_count) &&
	    state_counter_from_text(state, "cursor_pin_count",
	    &snapshot->cursor_pin_count) &&
	    state_counter_from_text(state, "cursor_unpin_count",
	    &snapshot->cursor_unpin_count) &&
	    state_counter_from_text(state, "atomic_last_legacy_cursor_update",
	    &snapshot->atomic_last_legacy_cursor_update) &&
	    state_counter_from_text(state, "atomic_last_async_update",
	    &snapshot->atomic_last_async_update);
	snprintf(key, sizeof(key), "head[%u]_cursor_enabled", head);
	ok = ok && state_counter_from_text(state, key,
	    &snapshot->head_cursor_enabled);
	snprintf(key, sizeof(key), "head[%u]_cursor_fb", head);
	ok = ok && state_counter_from_text(state, key, &snapshot->head_cursor_fb);
	snprintf(key, sizeof(key), "head[%u]_cursor_bo", head);
	ok = ok && state_counter_from_text(state, key, &snapshot->head_cursor_bo);
	snprintf(text, sizeof(text), "cursor counters are present before %s",
	    stage);
	check(ok, text);
	free(state);
	return ok;
}

static bool
read_pageflip_counter_snapshot(struct pageflip_counter_snapshot *snapshot,
    const char *stage)
{
	char text[160];
	char *state;
	bool ok;

	ok = read_drm_state_text(&state);
	snprintf(text, sizeof(text), "DRM state is readable before %s", stage);
	check(ok, text);
	if (!ok)
		return false;

	memset(snapshot, 0, sizeof(*snapshot));
	ok = state_counter_from_text(state, "page_flip_count",
	    &snapshot->page_flip_count) &&
	    state_counter_from_text(state, "page_flip_event_count",
	    &snapshot->page_flip_event_count) &&
	    state_counter_from_text(state, "page_flip_reject_count",
	    &snapshot->page_flip_reject_count) &&
	    state_counter_from_text(state, "page_flip_error_count",
	    &snapshot->page_flip_error_count) &&
	    state_counter_from_text(state, "commit_error_count",
	    &snapshot->commit_error_count) &&
	    state_counter_from_text(state, "atomic_tail_active",
	    &snapshot->atomic_tail_active) &&
	    state_counter_from_text(state, "atomic_tail_stage",
	    &snapshot->atomic_tail_stage) &&
	    state_counter_from_text(state, "display_audit_pending_valid",
	    &snapshot->display_audit_pending_valid);
	snprintf(text, sizeof(text), "pageflip counters are present before %s",
	    stage);
	check(ok, text);
	free(state);
	return ok;
}

static bool
clear_dumb_buffer(int fd, uint32_t handle, uint32_t pitch, uint32_t height,
    const char *what)
{
	struct drm_mode_map_dumb map;
	size_t length;
	void *data;
	bool ok;

	memset(&map, 0, sizeof(map));
	map.handle = handle;
	ok = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0;
	check(ok, what);
	if (!ok)
		return false;

	length = (size_t)pitch * height;
	data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    (off_t)map.offset);
	ok = data != MAP_FAILED;
	check(ok, "mmap succeeds for dumb buffer probe");
	if (!ok)
		return false;

	memset(data, 0, length);
	check(munmap(data, length) == 0, "munmap succeeds for dumb buffer probe");
	return true;
}

static bool
add_linear_framebuffer(int fd, uint32_t width, uint32_t height,
    uint32_t format, uint32_t handle, uint32_t pitch, uint32_t *fb_id_out,
    const char *what)
{
	uint32_t handles[4] = { 0 };
	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint64_t modifiers[4] = { DRM_FORMAT_MOD_LINEAR };
	bool ok;

	handles[0] = handle;
	pitches[0] = pitch;
	ok = drmModeAddFB2WithModifiers(fd, width, height, format, handles,
	    pitches, offsets, modifiers, fb_id_out, DRM_MODE_FB_MODIFIERS) == 0;
	check(ok, what);
	return ok;
}

static void
remove_framebuffer(int fd, uint32_t fb_id, const char *what)
{
	if (fb_id == 0)
		return;
	check(drmModeRmFB(fd, fb_id) == 0, what);
}

/*
 * check_same_device_prime_framebuffer_contract()
 *
 * Ownership:
 *   Owns one temporary render-node dumb BO, one dma-buf fd exported from that
 *   BO, one imported GEM handle on the master KMS fd, and one framebuffer made
 *   from the imported handle.  All handles and fds are released before return.
 *
 * Lifetime:
 *   The imported KMS handle must remain valid while the framebuffer exists, even
 *   if the exporting render fd is a different DRM file.  The framebuffer is
 *   metadata-only: it is never attached to a plane or scanned out by this probe.
 *
 * Threading:
 *   Single-threaded userspace probe.  It exercises PRIME same-device
 *   export/import, GEM handle installation, ADDFB2 metadata, and cleanup paths
 *   without starting a display transaction.
 */
static void
check_same_device_prime_framebuffer_contract(int kms_fd)
{
	drmModeFB2Ptr fb2 = NULL;
	struct drm_prime_handle prime_args;
	uint32_t render_handle = 0;
	uint32_t imported_handle = 0;
	uint32_t pitch = 0;
	uint32_t fb_id = 0;
	int render_fd = -1;
	int prime_fd = -1;
	int saved_errno;
	int ret;

	errno = 0;
	render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	saved_errno = errno;
	check(render_fd >= 0, "PRIME render node opens for framebuffer roundtrip");
	if (render_fd < 0) {
		printf("    render node errno=%d\n", saved_errno);
		return;
	}

	if (!create_dumb_buffer_for(render_fd, 64, 64, 32, &render_handle,
	    &pitch, "CREATE_DUMB succeeds on render fd for PRIME framebuffer probe"))
		goto out_close_render;
	if (!clear_dumb_buffer(render_fd, render_handle, pitch, 64,
	    "MAP_DUMB succeeds on render fd for PRIME framebuffer probe"))
		goto out_destroy_render_bo;

	memset(&prime_args, 0, sizeof(prime_args));
	prime_args.handle = render_handle;
	prime_args.flags = 0x80000000u;
	prime_args.fd = -1;
	errno = 0;
	ret = drmIoctl(render_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_args);
	saved_errno = errno;
	check(ret != 0, "PRIME_HANDLE_TO_FD rejects unknown flags");
	check(saved_errno == EINVAL,
	    "PRIME_HANDLE_TO_FD unknown flags fail with EINVAL");
	check(prime_args.fd < 0,
	    "PRIME_HANDLE_TO_FD unknown flags return no dma-buf fd");
	if (ret == 0 || saved_errno != EINVAL || prime_args.fd >= 0) {
		printf("    PRIME_HANDLE_TO_FD unknown flags ret=%d errno=%d fd=%d\n",
		    ret, saved_errno, prime_args.fd);
		if (prime_args.fd >= 0)
			check(close(prime_args.fd) == 0,
			    "close succeeds for unexpected PRIME dma-buf fd");
	}

	errno = 0;
	ret = drmPrimeHandleToFD(render_fd, render_handle, DRM_CLOEXEC, &prime_fd);
	saved_errno = errno;
	check(ret == 0 && prime_fd >= 0,
	    "PRIME_HANDLE_TO_FD exports render dumb BO");
	if (ret != 0 || prime_fd < 0) {
		printf("    PRIME_HANDLE_TO_FD errno=%d\n", saved_errno);
		goto out_destroy_render_bo;
	}

	errno = 0;
	ret = drmPrimeFDToHandle(kms_fd, prime_fd, &imported_handle);
	saved_errno = errno;
	check(ret == 0 && imported_handle != 0,
	    "PRIME_FD_TO_HANDLE imports render BO on KMS fd");
	if (ret != 0 || imported_handle == 0) {
		printf("    PRIME_FD_TO_HANDLE errno=%d\n", saved_errno);
		goto out_close_prime_fd;
	}

	if (!add_linear_framebuffer(kms_fd, 64, 64, DRM_FORMAT_XRGB8888,
	    imported_handle, pitch, &fb_id,
	    "ADDFB2 accepts PRIME-imported XRGB8888 framebuffer"))
		goto out_close_imported;

	errno = 0;
	fb2 = drmModeGetFB2(kms_fd, fb_id);
	saved_errno = errno;
	check(fb2 != NULL, "GETFB2 succeeds for PRIME-imported framebuffer");
	if (fb2 != NULL) {
		check(fb2->pixel_format == DRM_FORMAT_XRGB8888,
		    "PRIME-imported framebuffer reports XRGB8888 format");
		check((fb2->flags & DRM_MODE_FB_MODIFIERS) != 0,
		    "PRIME-imported framebuffer reports modifier flag");
		check(fb2->modifier == DRM_FORMAT_MOD_LINEAR,
		    "PRIME-imported framebuffer reports linear modifier");
		check(fb2->pitches[0] == pitch,
		    "PRIME-imported framebuffer reports exported pitch");
		check(fb2->handles[0] != 0,
		    "PRIME-imported framebuffer returns a GEM handle to master");
		close_fb2_handles(kms_fd, fb2,
		    "GEM_CLOSE succeeds for PRIME-imported GETFB2 handle");
		drmModeFreeFB2(fb2);
	} else {
		printf("    PRIME-imported GETFB2 errno=%d\n", saved_errno);
	}

out_close_imported:
	remove_framebuffer(kms_fd, fb_id,
	    "RMFB succeeds for PRIME-imported framebuffer probe");
	close_gem_handle_for(kms_fd, imported_handle,
	    "GEM_CLOSE succeeds for PRIME-imported KMS handle");
out_close_prime_fd:
	if (prime_fd >= 0)
		check(close(prime_fd) == 0,
		    "close succeeds for PRIME framebuffer dma-buf fd");
out_destroy_render_bo:
	destroy_dumb_buffer_for(render_fd, render_handle,
	    "DESTROY_DUMB succeeds for PRIME framebuffer render BO");
out_close_render:
	check(close(render_fd) == 0,
	    "close succeeds for PRIME framebuffer render fd");
}

static void
close_fb2_handles(int fd, const drmModeFB2 *fb2, const char *what)
{
	uint32_t closed[4] = { 0 };
	uint32_t closed_count = 0;

	for (uint32_t i = 0; i < 4; i++) {
		bool duplicate = false;

		if (fb2->handles[i] == 0)
			continue;
		for (uint32_t j = 0; j < closed_count; j++) {
			if (closed[j] == fb2->handles[i]) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;
		close_gem_handle_for(fd, fb2->handles[i], what);
		closed[closed_count++] = fb2->handles[i];
	}
}

/*
 * check_getfb_non_master_metadata_contract()
 *
 * Ownership:
 *   Borrows the master-owned framebuffer ID from the caller.  Owns one
 *   temporary non-master DRM fd and closes it before return.  The kernel must
 *   not return GEM handles to this fd unless it is current master or root; if a
 *   buggy kernel does return handles, this probe closes them before returning.
 *
 * Lifetime:
 *   The framebuffer must stay alive on the caller's master fd for the duration
 *   of the metadata query.  The second fd does not retain the framebuffer or any
 *   GEM handle after return.
 *
 * Threading:
 *   Single-threaded userspace probe.  It exercises only KMS metadata lookup and
 *   must not start a display transaction.
 */
static void
check_getfb_non_master_metadata_contract(uint32_t fb_id, uint32_t width,
    uint32_t height, uint32_t format, uint32_t pitch, uint64_t modifier)
{
	drmModeFBPtr fb = NULL;
	drmModeFB2Ptr fb2 = NULL;
	int fd;
	int saved_errno;

	if (geteuid() == 0) {
		printf("SKIP GETFB/GETFB2 non-master handle privacy probe as root\n");
		return;
	}

	errno = 0;
	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	saved_errno = errno;
	check(fd >= 0, "GETFB non-master opens secondary card fd");
	if (fd < 0) {
		printf("    secondary card fd errno=%d\n", saved_errno);
		return;
	}

	errno = 0;
	fb = drmModeGetFB(fd, fb_id);
	saved_errno = errno;
	check(fb != NULL, "GETFB non-master reads framebuffer metadata");
	if (fb != NULL) {
		check(fb->fb_id == fb_id,
		    "GETFB non-master returns requested FB_ID");
		check(fb->width == width,
		    "GETFB non-master reports framebuffer width");
		check(fb->height == height,
		    "GETFB non-master reports framebuffer height");
		check(fb->pitch == pitch,
		    "GETFB non-master reports framebuffer pitch");
		check(fb->handle == 0,
		    "GETFB non-master returns no GEM handle");
		if (fb->handle != 0)
			close_gem_handle_for(fd, fb->handle,
			    "GEM_CLOSE succeeds for unexpected non-master GETFB handle");
		drmModeFreeFB(fb);
	} else {
		printf("    GETFB non-master errno=%d\n", saved_errno);
	}

	errno = 0;
	fb2 = drmModeGetFB2(fd, fb_id);
	saved_errno = errno;
	check(fb2 != NULL, "GETFB2 non-master reads framebuffer metadata");
	if (fb2 != NULL) {
		check(fb2->fb_id == fb_id,
		    "GETFB2 non-master returns requested FB_ID");
		check(fb2->width == width,
		    "GETFB2 non-master reports framebuffer width");
		check(fb2->height == height,
		    "GETFB2 non-master reports framebuffer height");
		check(fb2->pixel_format == format,
		    "GETFB2 non-master reports framebuffer format");
		check((fb2->flags & DRM_MODE_FB_MODIFIERS) != 0,
		    "GETFB2 non-master reports modifier flag");
		check(fb2->modifier == modifier,
		    "GETFB2 non-master reports framebuffer modifier");
		check(fb2->pitches[0] == pitch,
		    "GETFB2 non-master reports framebuffer pitch");
		check(fb2->offsets[0] == 0,
		    "GETFB2 non-master reports zero plane offset");
		for (uint32_t i = 0; i < 4; i++) {
			check(fb2->handles[i] == 0,
			    "GETFB2 non-master returns no GEM handles");
		}
		close_fb2_handles(fd, fb2,
		    "GEM_CLOSE succeeds for unexpected non-master GETFB2 handle");
		drmModeFreeFB2(fb2);
	} else {
		printf("    GETFB2 non-master errno=%d\n", saved_errno);
	}

	check(close(fd) == 0, "GETFB non-master closes secondary card fd");
}

/*
 * check_dirtyfb_non_master_denied()
 *
 * Ownership:
 *   Borrows a master-owned framebuffer ID from the caller.  Owns one
 *   temporary secondary DRM fd and closes it before return.
 *
 * Lifetime:
 *   The framebuffer must stay alive on the caller's master fd while this probe
 *   runs.  The secondary fd does not retain the framebuffer or any GEM handle.
 *
 * Threading:
 *   Single-threaded userspace probe.  DIRTYFB is a frontbuffer update
 *   acknowledgement and must be rejected by the DRM_MASTER gate before the
 *   driver framebuffer dirty callback can observe the request.
 */
static void
check_dirtyfb_non_master_denied(uint32_t fb_id, uint32_t width,
    uint32_t height)
{
	drmModeClip clip;
	int fd;
	int saved_errno;
	int ret;

	memset(&clip, 0, sizeof(clip));
	clip.x1 = 0;
	clip.y1 = 0;
	clip.x2 = (uint16_t)width;
	clip.y2 = (uint16_t)height;

	errno = 0;
	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	saved_errno = errno;
	check(fd >= 0, "DIRTYFB non-master opens secondary card fd");
	if (fd < 0) {
		printf("    secondary card fd errno=%d\n", saved_errno);
		return;
	}

	errno = 0;
	ret = drmModeDirtyFB(fd, fb_id, &clip, 1);
	saved_errno = errno;
	check(ret != 0, "DIRTYFB non-master is denied");
	if (ret == 0) {
		printf("    DIRTYFB non-master unexpectedly succeeded\n");
	} else {
		printf("    DIRTYFB non-master errno=%d\n", saved_errno);
		check(saved_errno == EACCES,
		    "DIRTYFB non-master fails with EACCES");
	}

	check(close(fd) == 0, "DIRTYFB non-master closes secondary card fd");
}

/*
 * check_addfb2_rejects()
 *
 * Ownership:
 *   Borrows the DRM fd and the GEM handles embedded in request.  If a buggy
 *   kernel accepts the request and creates an FB, the helper removes that FB
 *   before returning.
 *
 * Lifetime:
 *   The request is copied by value so the caller's template remains reusable
 *   across negative probes.
 *
 * Threading:
 *   Single-threaded framebuffer UAPI probe.  These requests must fail in
 *   common DRM validation before nvkm fb_create or display programming runs.
 */
static void
check_addfb2_rejects(int fd, struct drm_mode_fb_cmd2 request,
    int expected_errno, const char *what, const char *errno_what)
{
	int saved_errno;
	int ret;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_ADDFB2, &request);
	saved_errno = errno;
	check(ret != 0, what);
	check(saved_errno == expected_errno, errno_what);
	if (ret == 0 && request.fb_id != 0)
		remove_framebuffer(fd, request.fb_id,
		    "RMFB succeeds for unexpected ADDFB2 negative probe");
	else if (ret != 0 && saved_errno != expected_errno)
		printf("    ADDFB2 ret=%d errno=%d expected=%d\n", ret,
		    saved_errno, expected_errno);
}

/*
 * check_non_master_display_mutation_contract()
 *
 * Ownership:
 *   Borrows the current master DRM fd, mode resource snapshot, active CRTC ID,
 *   and active primary plane ID.  Owns one temporary secondary card fd and
 *   closes it before return.
 *
 * Lifetime:
 *   The active CRTC mode, connector properties, and primary plane state must
 *   remain valid while the helper runs.  The rejected probes reuse current
 *   state or no-op values, so even an erroneous success would not
 *   intentionally program a new visible state.
 *
 * Threading:
 *   Single-threaded userspace probe.  The secondary fd must not be current
 *   master, and mutating KMS ioctls must be rejected by the DRM_MASTER gate
 *   before nvkm display state changes.
 */
static void
check_non_master_display_mutation_contract(int master_fd,
    const drmModeRes *resources, uint32_t crtc_id, uint32_t plane_id,
    const char *object_name)
{
	struct atomic_plane_snapshot snapshot;
	drmModeCrtcPtr crtc;
	drmModeModeInfo saved_mode;
	struct drm_mode_cursor cursor;
	struct drm_mode_cursor2 cursor2;
	struct drm_mode_mode_cmd mode_cmd;
	struct drm_mode_obj_set_property obj_set_property;
	uint32_t connector_id = 0;
	uint32_t dpms_property_id = 0;
	uint64_t connector_dpms = 0;
	uint16_t *gamma_red = NULL;
	uint16_t *gamma_green = NULL;
	uint16_t *gamma_blue = NULL;
	int secondary_fd;
	int saved_errno;
	int gamma_size;
	int crtc_x;
	int crtc_y;
	int ret;

	if (!find_active_connector_for_crtc(master_fd, resources, crtc_id,
	    &connector_id)) {
		check(false,
		    "active connector is available for non-master display mutation probe");
		return;
	}
	check(true,
	    "active connector is available for non-master display mutation probe");

	if (!get_plane_snapshot(master_fd, plane_id, &snapshot, object_name))
		return;
	check(snapshot.fb_id != 0,
	    "active primary plane has framebuffer for non-master display mutation probe");
	check(snapshot.crtc_id == crtc_id,
	    "active primary plane is attached to active CRTC for non-master display mutation probe");
	if (snapshot.fb_id == 0 || snapshot.crtc_id != crtc_id)
		return;

	crtc = drmModeGetCrtc(master_fd, crtc_id);
	check(crtc != NULL,
	    "active CRTC is readable for non-master display mutation probe");
	if (crtc == NULL)
		return;
	check(crtc->mode_valid,
	    "active CRTC has a mode for non-master display mutation probe");
	if (!crtc->mode_valid) {
		drmModeFreeCrtc(crtc);
		return;
	}
	saved_mode = crtc->mode;
	gamma_size = crtc->gamma_size;
	crtc_x = crtc->x;
	crtc_y = crtc->y;
	drmModeFreeCrtc(crtc);
	check(gamma_size > 0,
	    "active CRTC has gamma ramp for non-master display mutation probe");
	if (gamma_size <= 0)
		return;

	if (!get_property_id(master_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "DPMS", &dpms_property_id) ||
	    !get_property_value_checked(master_fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "DPMS", &connector_dpms,
	    "non-master display mutation connector"))
		return;

	errno = 0;
	secondary_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	saved_errno = errno;
	check(secondary_fd >= 0,
	    "non-master display mutation opens secondary card fd");
	if (secondary_fd < 0) {
		printf("    secondary card fd errno=%d\n", saved_errno);
		return;
	}

	errno = 0;
	ret = drmModeSetCrtc(secondary_fd, crtc_id, snapshot.fb_id, crtc_x,
	    crtc_y, &connector_id, 1, &saved_mode);
	saved_errno = errno;
	check(ret != 0, "non-master legacy SetCrtc is denied");
	if (ret == 0) {
		printf("    non-master legacy SetCrtc unexpectedly succeeded\n");
	} else {
		printf("    non-master legacy SetCrtc errno=%d\n",
		    saved_errno);
		check(saved_errno == EACCES,
		    "non-master legacy SetCrtc fails with EACCES");
	}

	errno = 0;
	ret = drmModePageFlip(secondary_fd, crtc_id, snapshot.fb_id,
	    DRM_MODE_PAGE_FLIP_EVENT, NULL);
	saved_errno = errno;
	check(ret != 0, "non-master legacy pageflip is denied");
	if (ret == 0) {
		printf("    non-master legacy pageflip unexpectedly succeeded\n");
	} else {
		printf("    non-master legacy pageflip errno=%d\n",
		    saved_errno);
		check(saved_errno == EACCES,
		    "non-master legacy pageflip fails with EACCES");
	}

	errno = 0;
	ret = drmModeSetPlane(secondary_fd, plane_id, crtc_id,
	    (uint32_t)snapshot.fb_id, 0, (int32_t)snapshot.crtc_x,
	    (int32_t)snapshot.crtc_y, (uint32_t)snapshot.crtc_w,
	    (uint32_t)snapshot.crtc_h, (uint32_t)snapshot.src_x,
	    (uint32_t)snapshot.src_y, (uint32_t)snapshot.src_w,
	    (uint32_t)snapshot.src_h);
	saved_errno = errno;
	check(ret != 0, "non-master legacy SetPlane is denied");
	if (ret == 0) {
		printf("    non-master legacy SetPlane unexpectedly succeeded\n");
	} else {
		printf("    non-master legacy SetPlane errno=%d\n",
		    saved_errno);
		check(saved_errno == EACCES,
		    "non-master legacy SetPlane fails with EACCES");
	}

	memset(&mode_cmd, 0, sizeof(mode_cmd));
	mode_cmd.connector_id = connector_id;
	errno = 0;
	ret = drmIoctl(secondary_fd, DRM_IOCTL_MODE_ATTACHMODE, &mode_cmd);
	saved_errno = errno;
	check(ret != 0, "non-master legacy AttachMode is denied");
	if (ret == 0) {
		printf("    non-master legacy AttachMode unexpectedly succeeded\n");
	} else {
		printf("    non-master legacy AttachMode errno=%d\n",
		    saved_errno);
		check(saved_errno == EACCES,
		    "non-master legacy AttachMode fails with EACCES");
	}

	errno = 0;
	ret = drmIoctl(secondary_fd, DRM_IOCTL_MODE_DETACHMODE, &mode_cmd);
	saved_errno = errno;
	check(ret != 0, "non-master legacy DetachMode is denied");
	if (ret == 0) {
		printf("    non-master legacy DetachMode unexpectedly succeeded\n");
	} else {
		printf("    non-master legacy DetachMode errno=%d\n",
		    saved_errno);
		check(saved_errno == EACCES,
		    "non-master legacy DetachMode fails with EACCES");
	}

	memset(&cursor, 0, sizeof(cursor));
	cursor.flags = DRM_MODE_CURSOR_MOVE;
	cursor.crtc_id = crtc_id;
	errno = 0;
	ret = drmIoctl(secondary_fd, DRM_IOCTL_MODE_CURSOR, &cursor);
	saved_errno = errno;
	check(ret != 0, "non-master legacy cursor MOVE is denied");
	if (ret == 0) {
		printf("    non-master legacy cursor MOVE unexpectedly succeeded\n");
	} else {
		printf("    non-master legacy cursor MOVE errno=%d\n",
		    saved_errno);
		check(saved_errno == EACCES,
		    "non-master legacy cursor MOVE fails with EACCES");
	}

	memset(&cursor2, 0, sizeof(cursor2));
	cursor2.flags = DRM_MODE_CURSOR_MOVE;
	cursor2.crtc_id = crtc_id;
	errno = 0;
	ret = drmIoctl(secondary_fd, DRM_IOCTL_MODE_CURSOR2, &cursor2);
	saved_errno = errno;
	check(ret != 0, "non-master legacy cursor2 MOVE is denied");
	if (ret == 0) {
		printf("    non-master legacy cursor2 MOVE unexpectedly succeeded\n");
	} else {
		printf("    non-master legacy cursor2 MOVE errno=%d\n",
		    saved_errno);
		check(saved_errno == EACCES,
		    "non-master legacy cursor2 MOVE fails with EACCES");
	}

	gamma_red = calloc((size_t)gamma_size, sizeof(*gamma_red));
	gamma_green = calloc((size_t)gamma_size, sizeof(*gamma_green));
	gamma_blue = calloc((size_t)gamma_size, sizeof(*gamma_blue));
	check(gamma_red != NULL && gamma_green != NULL && gamma_blue != NULL,
	    "non-master legacy SetGamma allocates identity ramp");
	if (gamma_red != NULL && gamma_green != NULL && gamma_blue != NULL) {
		for (int i = 0; i < gamma_size; i++) {
			uint16_t value;

			value = gamma_size == 1 ? 0 :
			    (uint16_t)((uint64_t)i * 65535u /
			    (uint64_t)(gamma_size - 1));
			gamma_red[i] = value;
			gamma_green[i] = value;
			gamma_blue[i] = value;
		}

		errno = 0;
		ret = drmModeCrtcSetGamma(secondary_fd, crtc_id,
		    (uint32_t)gamma_size, gamma_red, gamma_green, gamma_blue);
		saved_errno = errno;
		check(ret != 0, "non-master legacy SetGamma is denied");
		if (ret == 0) {
			printf("    non-master legacy SetGamma unexpectedly succeeded\n");
		} else {
			printf("    non-master legacy SetGamma errno=%d\n",
			    saved_errno);
			check(saved_errno == EACCES,
			    "non-master legacy SetGamma fails with EACCES");
		}
	}

	errno = 0;
	ret = drmModeConnectorSetProperty(secondary_fd, connector_id,
	    dpms_property_id, connector_dpms);
	saved_errno = errno;
	check(ret != 0, "non-master connector SetProperty is denied");
	if (ret == 0) {
		printf("    non-master connector SetProperty unexpectedly succeeded\n");
	} else {
		printf("    non-master connector SetProperty errno=%d\n",
		    saved_errno);
		check(saved_errno == EACCES,
		    "non-master connector SetProperty fails with EACCES");
	}

	memset(&obj_set_property, 0, sizeof(obj_set_property));
	obj_set_property.value = connector_dpms;
	obj_set_property.prop_id = dpms_property_id;
	obj_set_property.obj_id = connector_id;
	obj_set_property.obj_type = DRM_MODE_OBJECT_CONNECTOR;
	errno = 0;
	ret = drmIoctl(secondary_fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY,
	    &obj_set_property);
	saved_errno = errno;
	check(ret != 0, "non-master object SetProperty is denied");
	if (ret == 0) {
		printf("    non-master object SetProperty unexpectedly succeeded\n");
	} else {
		printf("    non-master object SetProperty errno=%d\n",
		    saved_errno);
		check(saved_errno == EACCES,
		    "non-master object SetProperty fails with EACCES");
	}

	{
		drmModeAtomicReqPtr req;

		req = drmModeAtomicAlloc();
		check(req != NULL,
		    "non-master atomic TEST_ONLY allocates request");
		if (req != NULL) {
			if (!atomic_add_plane_property(master_fd, req,
			    plane_id, "FB_ID", snapshot.fb_id) ||
			    !atomic_add_plane_property(master_fd, req,
			    plane_id, "CRTC_ID", snapshot.crtc_id) ||
			    !atomic_add_plane_property(master_fd, req,
			    plane_id, "CRTC_X", snapshot.crtc_x) ||
			    !atomic_add_plane_property(master_fd, req,
			    plane_id, "CRTC_Y", snapshot.crtc_y) ||
			    !atomic_add_plane_property(master_fd, req,
			    plane_id, "CRTC_W", snapshot.crtc_w) ||
			    !atomic_add_plane_property(master_fd, req,
			    plane_id, "CRTC_H", snapshot.crtc_h) ||
			    !atomic_add_plane_property(master_fd, req,
			    plane_id, "SRC_X", snapshot.src_x) ||
			    !atomic_add_plane_property(master_fd, req,
			    plane_id, "SRC_Y", snapshot.src_y) ||
			    !atomic_add_plane_property(master_fd, req,
			    plane_id, "SRC_W", snapshot.src_w) ||
			    !atomic_add_plane_property(master_fd, req,
			    plane_id, "SRC_H", snapshot.src_h)) {
				check(false,
				    "non-master atomic TEST_ONLY request describes current primary plane");
			} else {
				check(true,
				    "non-master atomic TEST_ONLY request describes current primary plane");
				errno = 0;
				ret = drmModeAtomicCommit(secondary_fd, req,
				    DRM_MODE_ATOMIC_TEST_ONLY |
				    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
				saved_errno = errno;
				check(ret != 0,
				    "non-master atomic TEST_ONLY commit is denied");
				if (ret == 0) {
					printf("    non-master atomic TEST_ONLY unexpectedly succeeded\n");
				} else {
					printf("    non-master atomic TEST_ONLY errno=%d\n",
					    saved_errno);
					check(saved_errno == EACCES,
					    "non-master atomic TEST_ONLY commit fails with EACCES");
				}
			}
			drmModeAtomicFree(req);
		}
	}

	free(gamma_blue);
	free(gamma_green);
	free(gamma_red);
	check(close(secondary_fd) == 0,
	    "non-master display mutation closes secondary card fd");
}

/*
 * check_framebuffer_uapi_contract()
 *
 * Ownership:
 *   Owns one temporary dumb BO and framebuffer.  GETFB/GETFB2 return fresh GEM
 *   handles owned by this drm file; the probe closes each returned handle
 *   before removing the framebuffer and destroying the original dumb handle.
 *
 * Lifetime:
 *   The temporary framebuffer is never attached to a plane.  DIRTYFB is tested
 *   only as a KMS frontbuffer update acknowledgement on the framebuffer object.
 *
 * Threading:
 *   Single-threaded userspace probe.  The driver must not start an atomic
 *   display transaction, queue a vblank event, or leave pending display audit
 *   state for these metadata/no-op framebuffer ioctls.
 */
static void
check_framebuffer_uapi_contract(int fd)
{
	struct pageflip_counter_snapshot before;
	struct pageflip_counter_snapshot after;
	struct drm_mode_fb_dirty_cmd dirty;
	struct drm_mode_fb_cmd2 addfb2;
	drmModeClip clip;
	drmModeFBPtr fb = NULL;
	drmModeFB2Ptr fb2 = NULL;
	uint32_t handle = 0;
	uint32_t pitch = 0;
	uint32_t fb_id = 0;
	uint32_t closefb_id = 0;
	uint32_t legacy_fb24_id = 0;
	uint32_t legacy_fb30_id = 0;
	uint32_t invalid_legacy_fb_id = 0;
	int saved_errno;
	int ret;

	if (!read_pageflip_counter_snapshot(&before, "framebuffer UAPI probe"))
		return;
	if (!create_dumb_buffer_for(fd, 64, 64, 32, &handle, &pitch,
	    "CREATE_DUMB succeeds for framebuffer UAPI probe"))
		return;
	if (!clear_dumb_buffer(fd, handle, pitch, 64,
	    "MAP_DUMB succeeds for framebuffer UAPI probe"))
		goto out_destroy_bo;

	memset(&addfb2, 0, sizeof(addfb2));
	addfb2.width = 64;
	addfb2.height = 64;
	addfb2.pixel_format = DRM_FORMAT_XRGB8888;
	addfb2.handles[0] = handle;
	addfb2.pitches[0] = pitch;
	addfb2.flags = DRM_MODE_FB_MODIFIERS;
	addfb2.modifier[0] = DRM_FORMAT_MOD_LINEAR;

	addfb2.width = 0;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects zero width",
	    "ADDFB2 zero width fails with EINVAL");
	addfb2.width = 64;

	addfb2.pixel_format = 0xffffffffu;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects unknown pixel format",
	    "ADDFB2 unknown pixel format fails with EINVAL");
	addfb2.pixel_format = DRM_FORMAT_XRGB8888;

	addfb2.flags = DRM_MODE_FB_MODIFIERS | 0x80000000u;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects unknown framebuffer flags",
	    "ADDFB2 unknown framebuffer flags fail with EINVAL");
	addfb2.flags = DRM_MODE_FB_MODIFIERS;

	addfb2.handles[0] = 0;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects missing plane handle",
	    "ADDFB2 missing plane handle fails with EINVAL");
	addfb2.handles[0] = handle;

	addfb2.pitches[0] = 4;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects undersized pitch",
	    "ADDFB2 undersized pitch fails with EINVAL");
	addfb2.pitches[0] = pitch;

	addfb2.flags = 0;
	addfb2.modifier[0] = DRM_FORMAT_MOD_INVALID;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects modifier without modifier flag",
	    "ADDFB2 modifier without flag fails with EINVAL");
	addfb2.flags = DRM_MODE_FB_MODIFIERS;
	addfb2.modifier[0] = DRM_FORMAT_MOD_LINEAR;

	addfb2.handles[1] = handle;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects unused plane handle",
	    "ADDFB2 unused plane handle fails with EINVAL");
	addfb2.handles[1] = 0;

	addfb2.pitches[1] = pitch;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects unused plane pitch",
	    "ADDFB2 unused plane pitch fails with EINVAL");
	addfb2.pitches[1] = 0;

	addfb2.offsets[1] = 4;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects unused plane offset",
	    "ADDFB2 unused plane offset fails with EINVAL");
	addfb2.offsets[1] = 0;

	addfb2.modifier[1] = DRM_FORMAT_MOD_INVALID;
	check_addfb2_rejects(fd, addfb2, EINVAL,
	    "ADDFB2 rejects unused plane modifier",
	    "ADDFB2 unused plane modifier fails with EINVAL");
	addfb2.modifier[1] = 0;

	if (!add_linear_framebuffer(fd, 64, 64, DRM_FORMAT_XRGB8888, handle,
	    pitch, &fb_id,
	    "ADDFB2 accepts XRGB8888 linear framebuffer UAPI probe"))
		goto out_destroy_bo;
	check_same_device_prime_framebuffer_contract(fd);

	errno = 0;
	fb = drmModeGetFB(fd, fb_id);
	saved_errno = errno;
	check(fb != NULL, "GETFB succeeds for owned framebuffer");
	if (fb != NULL) {
		check(fb->fb_id == fb_id, "GETFB returns requested FB_ID");
		check(fb->width == 64, "GETFB reports framebuffer width");
		check(fb->height == 64, "GETFB reports framebuffer height");
		check(fb->pitch == pitch, "GETFB reports framebuffer pitch");
		check(fb->bpp == 32, "GETFB reports framebuffer bpp");
		check(fb->depth == 24, "GETFB reports XRGB8888 depth");
		check(fb->handle != 0, "GETFB returns a GEM handle to master");
		close_gem_handle_for(fd, fb->handle,
		    "GEM_CLOSE succeeds for GETFB returned handle");
		drmModeFreeFB(fb);
	} else {
		printf("    GETFB errno=%d\n", saved_errno);
	}

	errno = 0;
	fb2 = drmModeGetFB2(fd, fb_id);
	saved_errno = errno;
	check(fb2 != NULL, "GETFB2 succeeds for owned framebuffer");
	if (fb2 != NULL) {
		check(fb2->fb_id == fb_id, "GETFB2 returns requested FB_ID");
		check(fb2->width == 64, "GETFB2 reports framebuffer width");
		check(fb2->height == 64, "GETFB2 reports framebuffer height");
		check(fb2->pixel_format == DRM_FORMAT_XRGB8888,
		    "GETFB2 reports XRGB8888 format");
		check((fb2->flags & DRM_MODE_FB_MODIFIERS) != 0,
		    "GETFB2 reports modifier flag");
		check(fb2->modifier == DRM_FORMAT_MOD_LINEAR,
		    "GETFB2 reports linear modifier");
		check(fb2->handles[0] != 0,
		    "GETFB2 returns a GEM handle to master");
		check(fb2->pitches[0] == pitch,
		    "GETFB2 reports framebuffer pitch");
		check(fb2->offsets[0] == 0,
		    "GETFB2 reports zero plane offset");
		for (uint32_t i = 1; i < 4; i++)
			check(fb2->handles[i] == 0,
			    "GETFB2 has no extra plane handles for XRGB8888");
		close_fb2_handles(fd, fb2,
		    "GEM_CLOSE succeeds for GETFB2 returned handle");
		drmModeFreeFB2(fb2);
		fb2 = NULL;
	} else {
		printf("    GETFB2 errno=%d\n", saved_errno);
	}
	check_getfb_non_master_metadata_contract(fb_id, 64, 64,
	    DRM_FORMAT_XRGB8888, pitch, DRM_FORMAT_MOD_LINEAR);

	if (add_linear_framebuffer(fd, 64, 64, DRM_FORMAT_XRGB8888, handle,
	    pitch, &closefb_id,
	    "ADDFB2 accepts XRGB8888 linear CLOSEFB probe")) {
		struct drm_mode_closefb closefb;

		memset(&closefb, 0, sizeof(closefb));
		closefb.fb_id = closefb_id;
		closefb.pad = 1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_MODE_CLOSEFB, &closefb);
		saved_errno = errno;
		check(ret != 0, "CLOSEFB rejects non-zero pad");
		check(saved_errno == EINVAL,
		    "CLOSEFB non-zero pad fails with EINVAL");

		errno = 0;
		ret = drmModeCloseFB(fd, closefb_id);
		saved_errno = errno;
		check(ret == 0, "CLOSEFB succeeds for framebuffer UAPI probe");
		if (ret == 0) {
			errno = 0;
			ret = drmModeCloseFB(fd, closefb_id);
			saved_errno = errno;
			check(ret != 0, "CLOSEFB second close fails");
			check(saved_errno == ENOENT,
			    "CLOSEFB second close fails with ENOENT");

			errno = 0;
			ret = drmModeRmFB(fd, closefb_id);
			saved_errno = errno;
			check(ret != 0, "RMFB after CLOSEFB fails");
			check(saved_errno == ENOENT,
			    "RMFB after CLOSEFB fails with ENOENT");
			closefb_id = 0;
		} else {
			printf("    CLOSEFB errno=%d\n", saved_errno);
		}
	}

	errno = 0;
	ret = drmModeAddFB(fd, 64, 64, 24, 32, pitch, handle,
	    &legacy_fb24_id);
	saved_errno = errno;
	check(ret == 0,
	    "legacy ADDFB accepts depth 24 XRGB8888 dumb framebuffer");
	if (ret == 0) {
		errno = 0;
		fb = drmModeGetFB(fd, legacy_fb24_id);
		saved_errno = errno;
		check(fb != NULL,
		    "legacy ADDFB depth 24 GETFB succeeds");
		if (fb != NULL) {
			check(fb->fb_id == legacy_fb24_id,
			    "legacy ADDFB depth 24 GETFB returns requested FB_ID");
			check(fb->width == 64,
			    "legacy ADDFB depth 24 GETFB reports framebuffer width");
			check(fb->height == 64,
			    "legacy ADDFB depth 24 GETFB reports framebuffer height");
			check(fb->pitch == pitch,
			    "legacy ADDFB depth 24 GETFB reports framebuffer pitch");
			check(fb->bpp == 32,
			    "legacy ADDFB depth 24 GETFB reports framebuffer bpp");
			check(fb->depth == 24,
			    "legacy ADDFB depth 24 GETFB reports framebuffer depth");
			close_gem_handle_for(fd, fb->handle,
			    "GEM_CLOSE succeeds for legacy ADDFB depth 24 GETFB handle");
			drmModeFreeFB(fb);
			fb = NULL;
		} else {
			printf("    legacy ADDFB depth 24 GETFB errno=%d\n",
			    saved_errno);
		}
		errno = 0;
		fb2 = drmModeGetFB2(fd, legacy_fb24_id);
		saved_errno = errno;
		check(fb2 != NULL,
		    "legacy ADDFB depth 24 GETFB2 succeeds");
		if (fb2 != NULL) {
			check(fb2->pixel_format == DRM_FORMAT_XRGB8888,
			    "legacy ADDFB depth 24 GETFB2 reports XRGB8888 format");
			check(fb2->handles[0] != 0,
			    "legacy ADDFB depth 24 GETFB2 returns a GEM handle");
			close_fb2_handles(fd, fb2,
			    "GEM_CLOSE succeeds for legacy ADDFB depth 24 GETFB2 handle");
			drmModeFreeFB2(fb2);
			fb2 = NULL;
		} else {
			printf("    legacy ADDFB depth 24 GETFB2 errno=%d\n",
			    saved_errno);
		}
		remove_framebuffer(fd, legacy_fb24_id,
		    "RMFB succeeds for legacy ADDFB depth 24 probe");
		legacy_fb24_id = 0;
	} else {
		printf("    legacy ADDFB depth 24 errno=%d\n",
		    saved_errno);
	}

	errno = 0;
	ret = drmModeAddFB(fd, 64, 64, 30, 32, pitch, handle,
	    &legacy_fb30_id);
	saved_errno = errno;
	check(ret == 0, "legacy ADDFB accepts depth 30 dumb framebuffer");
	if (ret == 0) {
		errno = 0;
		fb = drmModeGetFB(fd, legacy_fb30_id);
		saved_errno = errno;
		check(fb != NULL,
		    "legacy ADDFB depth 30 GETFB succeeds");
		if (fb != NULL) {
			check(fb->fb_id == legacy_fb30_id,
			    "legacy ADDFB depth 30 GETFB returns requested FB_ID");
			check(fb->width == 64,
			    "legacy ADDFB depth 30 GETFB reports framebuffer width");
			check(fb->height == 64,
			    "legacy ADDFB depth 30 GETFB reports framebuffer height");
			check(fb->pitch == pitch,
			    "legacy ADDFB depth 30 GETFB reports framebuffer pitch");
			check(fb->bpp == 32,
			    "legacy ADDFB depth 30 GETFB reports framebuffer bpp");
			check(fb->depth == 30,
			    "legacy ADDFB depth 30 GETFB reports framebuffer depth");
			close_gem_handle_for(fd, fb->handle,
			    "GEM_CLOSE succeeds for legacy ADDFB depth 30 GETFB handle");
			drmModeFreeFB(fb);
			fb = NULL;
		} else {
			printf("    legacy ADDFB depth 30 GETFB errno=%d\n",
			    saved_errno);
		}
		errno = 0;
		fb2 = drmModeGetFB2(fd, legacy_fb30_id);
		saved_errno = errno;
		check(fb2 != NULL,
		    "legacy ADDFB depth 30 GETFB2 succeeds");
		if (fb2 != NULL) {
			check(fb2->pixel_format == DRM_FORMAT_XBGR2101010,
			    "legacy ADDFB depth 30 GETFB2 reports XBGR2101010 format");
			check(fb2->handles[0] != 0,
			    "legacy ADDFB depth 30 GETFB2 returns a GEM handle");
			close_fb2_handles(fd, fb2,
			    "GEM_CLOSE succeeds for legacy ADDFB depth 30 GETFB2 handle");
			drmModeFreeFB2(fb2);
			fb2 = NULL;
		} else {
			printf("    legacy ADDFB depth 30 GETFB2 errno=%d\n",
			    saved_errno);
		}
		remove_framebuffer(fd, legacy_fb30_id,
		    "RMFB succeeds for legacy ADDFB depth 30 probe");
		legacy_fb30_id = 0;
	} else {
		printf("    legacy ADDFB depth 30 errno=%d\n",
		    saved_errno);
	}

	errno = 0;
	ret = drmModeAddFB(fd, 64, 64, 31, 32, pitch, handle,
	    &invalid_legacy_fb_id);
	saved_errno = errno;
	check(ret != 0, "legacy ADDFB rejects invalid depth 31");
	check(saved_errno == EINVAL,
	    "legacy ADDFB invalid depth fails with EINVAL");
	if (invalid_legacy_fb_id != 0) {
		remove_framebuffer(fd, invalid_legacy_fb_id,
		    "RMFB succeeds for unexpected legacy ADDFB invalid-depth probe");
		invalid_legacy_fb_id = 0;
	}

	memset(&clip, 0, sizeof(clip));
	clip.x1 = 0;
	clip.y1 = 0;
	clip.x2 = 64;
	clip.y2 = 64;
	ret = drmModeDirtyFB(fd, fb_id, &clip, 1);
	check(ret == 0, "DIRTYFB accepts one framebuffer damage clip");

	ret = drmModeDirtyFB(fd, fb_id, NULL, 0);
	check(ret == 0, "DIRTYFB accepts full-frame no-clip damage");
	check_dirtyfb_non_master_denied(fb_id, 64, 64);

	memset(&dirty, 0, sizeof(dirty));
	dirty.fb_id = fb_id;
	dirty.num_clips = 1;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
	saved_errno = errno;
	check(ret != 0, "DIRTYFB rejects missing clip pointer");
	check(saved_errno == EINVAL,
	    "DIRTYFB missing clip pointer fails with EINVAL");

	memset(&dirty, 0, sizeof(dirty));
	dirty.fb_id = fb_id;
	dirty.flags = DRM_MODE_FB_DIRTY_ANNOTATE_COPY;
	dirty.num_clips = 1;
	dirty.clips_ptr = (uint64_t)(uintptr_t)&clip;
	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
	saved_errno = errno;
	check(ret != 0, "DIRTYFB rejects odd COPY clip count");
	check(saved_errno == EINVAL,
	    "DIRTYFB odd COPY clip count fails with EINVAL");

	if (read_pageflip_counter_snapshot(&after, "framebuffer UAPI probe")) {
		check(after.commit_error_count == before.commit_error_count,
		    "framebuffer UAPI probe does not increment commit_error_count");
		check(after.atomic_tail_active == 0 &&
		    after.atomic_tail_stage == 0,
		    "framebuffer UAPI probe leaves no active tail transaction");
		check(after.display_audit_pending_valid == 0,
		    "framebuffer UAPI probe leaves no pending display audit");
	}

	remove_framebuffer(fd, invalid_legacy_fb_id,
	    "RMFB succeeds for unexpected legacy ADDFB invalid-depth probe");
	remove_framebuffer(fd, legacy_fb30_id,
	    "RMFB succeeds for legacy ADDFB depth 30 probe");
	remove_framebuffer(fd, legacy_fb24_id,
	    "RMFB succeeds for legacy ADDFB depth 24 probe");
	remove_framebuffer(fd, closefb_id,
	    "RMFB succeeds for leftover CLOSEFB probe");
	remove_framebuffer(fd, fb_id,
	    "RMFB succeeds for framebuffer UAPI probe");
out_destroy_bo:
	destroy_dumb_buffer_for(fd, handle,
	    "DESTROY_DUMB succeeds for framebuffer UAPI probe");
}

static bool
atomic_add_plane_property(int fd, drmModeAtomicReqPtr req, uint32_t plane_id,
    const char *name, uint64_t value)
{
	uint32_t property_id = 0;

	if (!get_property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, name,
	    &property_id)) {
		printf("plane %u property %s unavailable\n", plane_id, name);
		return false;
	}
	return drmModeAtomicAddProperty(req, plane_id, property_id, value) >= 0;
}

static bool
atomic_add_connector_property(int fd, drmModeAtomicReqPtr req,
    uint32_t connector_id, const char *name, uint64_t value)
{
	uint32_t property_id = 0;

	if (!get_property_id(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, name,
	    &property_id)) {
		printf("connector %u property %s unavailable\n",
		    connector_id, name);
		return false;
	}
	return drmModeAtomicAddProperty(req, connector_id, property_id,
	    value) >= 0;
}

static bool
mode_timing_is_sane(const drmModeModeInfo *mode)
{
	return mode->clock > 0 &&
	    mode->hdisplay > 0 &&
	    mode->hsync_start >= mode->hdisplay &&
	    mode->hsync_end >= mode->hsync_start &&
	    mode->htotal >= mode->hsync_end &&
	    mode->vdisplay > 0 &&
	    mode->vsync_start >= mode->vdisplay &&
	    mode->vsync_end >= mode->vsync_start &&
	    mode->vtotal >= mode->vsync_end &&
	    mode->name[0] != '\0';
}

static bool
find_primary_plane_for_crtc_index(drmModePlaneResPtr plane_resources,
    int fd, int crtc_index, uint32_t *plane_id_out)
{
	drmModePlanePtr plane;
	uint32_t crtc_bit;
	bool found = false;

	if (crtc_index < 0 || crtc_index >= 32)
		return false;
	crtc_bit = 1u << crtc_index;

	for (uint32_t i = 0; i < plane_resources->count_planes; i++) {
		plane = drmModeGetPlane(fd, plane_resources->planes[i]);
		if (plane == NULL)
			continue;
		if (get_plane_type(fd, plane->plane_id) ==
		    DRM_PLANE_TYPE_PRIMARY &&
		    (plane->possible_crtcs & crtc_bit) != 0) {
			*plane_id_out = plane->plane_id;
			found = true;
			drmModeFreePlane(plane);
			break;
		}
		drmModeFreePlane(plane);
	}

	return found;
}

static bool
drm_lease_get_objects(int fd, uint32_t *objects, uint32_t object_capacity,
    uint32_t *object_count_out, const char *what)
{
	struct drm_mode_get_lease get_lease;
	int saved_errno;
	int ret;

	memset(&get_lease, 0, sizeof(get_lease));
	get_lease.count_objects = object_capacity;
	get_lease.objects_ptr = (uintptr_t)objects;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GET_LEASE, &get_lease);
	saved_errno = errno;
	check(ret == 0, what);
	if (ret != 0) {
		printf("    GET_LEASE errno=%d\n", saved_errno);
		return false;
	}

	*object_count_out = get_lease.count_objects;
	return true;
}

static bool
drm_lease_create_flags(int fd, const uint32_t *object_ids,
    uint32_t object_count, uint32_t flags, int *lease_fd_out,
    uint32_t *lessee_id_out, const char *what)
{
	struct drm_mode_create_lease create_lease;
	int saved_errno;
	int ret;

	memset(&create_lease, 0, sizeof(create_lease));
	create_lease.object_ids = (uintptr_t)object_ids;
	create_lease.object_count = object_count;
	create_lease.flags = flags;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_LEASE, &create_lease);
	saved_errno = errno;
	check(ret == 0, what);
	if (ret != 0) {
		printf("    CREATE_LEASE errno=%d\n", saved_errno);
		return false;
	}

	check(create_lease.lessee_id != 0, "DRM lease returns lessee id");
	check(create_lease.fd > 0, "DRM lease returns lease fd");
	*lease_fd_out = (int)create_lease.fd;
	*lessee_id_out = create_lease.lessee_id;
	return true;
}

static bool
drm_lease_create(int fd, const uint32_t *object_ids, uint32_t object_count,
    int *lease_fd_out, uint32_t *lessee_id_out, const char *what)
{
	return drm_lease_create_flags(fd, object_ids, object_count, O_CLOEXEC,
	    lease_fd_out, lessee_id_out, what);
}

static void
drm_lease_create_error(int fd, const uint32_t *object_ids,
    uint32_t object_count, int expected_errno, const char *what)
{
	struct drm_mode_create_lease create_lease;
	int saved_errno;
	int ret;

	memset(&create_lease, 0, sizeof(create_lease));
	create_lease.object_ids = (uintptr_t)object_ids;
	create_lease.object_count = object_count;
	create_lease.flags = O_CLOEXEC;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_LEASE, &create_lease);
	saved_errno = errno;
	check(ret != 0 && saved_errno == expected_errno, what);
	if (ret == 0) {
		printf("    CREATE_LEASE unexpectedly returned fd=%u lessee=%u\n",
		    create_lease.fd, create_lease.lessee_id);
		close((int)create_lease.fd);
	} else if (saved_errno != expected_errno) {
		printf("    CREATE_LEASE errno=%d expected=%d\n", saved_errno,
		    expected_errno);
	}
}

static bool
drm_lease_list_contains(int fd, uint32_t lessee_id)
{
	struct drm_mode_list_lessees list_lessees;
	uint32_t lessees[16];
	const uint32_t sentinel = 0xfeedbee0u;
	bool found = false;
	int saved_errno;
	int ret;

	memset(&list_lessees, 0, sizeof(list_lessees));
	for (uint32_t i = 0; i < (uint32_t)(sizeof(lessees) /
	    sizeof(lessees[0])); i++)
		lessees[i] = sentinel;
	list_lessees.count_lessees = (uint32_t)(sizeof(lessees) /
	    sizeof(lessees[0]));
	list_lessees.lessees_ptr = (uintptr_t)lessees;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_LIST_LESSEES, &list_lessees);
	saved_errno = errno;
	check(ret == 0, "DRM lease LIST_LESSEES succeeds");
	if (ret != 0) {
		printf("    LIST_LESSEES errno=%d\n", saved_errno);
		return false;
	}

	if (list_lessees.count_lessees < (uint32_t)(sizeof(lessees) /
	    sizeof(lessees[0]))) {
		check(lessees[list_lessees.count_lessees] == sentinel,
		    "DRM lease LIST_LESSEES uses u32 lessee id stride");
	}

	for (uint32_t i = 0; i < list_lessees.count_lessees &&
	    i < (uint32_t)(sizeof(lessees) / sizeof(lessees[0])); i++) {
		if (lessees[i] == lessee_id)
			found = true;
	}
	return found;
}

static void
drm_lease_list_empty(int fd, const char *what)
{
	struct drm_mode_list_lessees list_lessees;
	uint32_t lessees[4];
	int saved_errno;
	int ret;

	memset(&list_lessees, 0, sizeof(list_lessees));
	memset(lessees, 0, sizeof(lessees));
	list_lessees.count_lessees = (uint32_t)(sizeof(lessees) /
	    sizeof(lessees[0]));
	list_lessees.lessees_ptr = (uintptr_t)lessees;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_LIST_LESSEES, &list_lessees);
	saved_errno = errno;
	check(ret == 0, "DRM lease lessee LIST_LESSEES succeeds");
	if (ret != 0)
		printf("    LIST_LESSEES errno=%d\n", saved_errno);
	check(ret == 0 && list_lessees.count_lessees == 0, what);
}

static void
drm_lease_list_pad_error(int fd)
{
	struct drm_mode_list_lessees list_lessees;
	int saved_errno;
	int ret;

	memset(&list_lessees, 0, sizeof(list_lessees));
	list_lessees.pad = 1;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_LIST_LESSEES, &list_lessees);
	saved_errno = errno;
	check(ret != 0 && saved_errno == EINVAL,
	    "DRM lease LIST_LESSEES rejects non-zero pad");
	if (ret == 0 || saved_errno != EINVAL)
		printf("    LIST_LESSEES pad ret=%d errno=%d\n", ret,
		    saved_errno);
}

static void
drm_lease_get_pad_error(int fd)
{
	struct drm_mode_get_lease get_lease;
	int saved_errno;
	int ret;

	memset(&get_lease, 0, sizeof(get_lease));
	get_lease.pad = 1;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_GET_LEASE, &get_lease);
	saved_errno = errno;
	check(ret != 0 && saved_errno == EINVAL,
	    "DRM lease GET_LEASE rejects non-zero pad");
	if (ret == 0 || saved_errno != EINVAL)
		printf("    GET_LEASE pad ret=%d errno=%d\n", ret,
		    saved_errno);
}

/*
 * drm_lease_revoke_id()
 *
 * Ownership:
 *   Borrows the owner DRM master fd.  The target lessee fd, if still open, is
 *   not closed here; only the lease object set is revoked through the owner.
 *
 * Lifetime:
 *   Valid while the lessee id still names a live lessee master.  Linux KMS
 *   keeps that identity until fd close, so a second revoke of an already empty
 *   live lessee must still find the id and succeed.
 *
 * Threading:
 *   Single-threaded smoke helper.  Kernel-side lease tree mutation is
 *   serialized by common DRM locks.
 */
static bool
drm_lease_revoke_id(int fd, uint32_t lessee_id, const char *what)
{
	struct drm_mode_revoke_lease revoke_lease;
	int saved_errno;
	int ret;

	memset(&revoke_lease, 0, sizeof(revoke_lease));
	revoke_lease.lessee_id = lessee_id;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_REVOKE_LEASE, &revoke_lease);
	saved_errno = errno;
	check(ret == 0, what);
	if (ret != 0)
		printf("    REVOKE_LEASE errno=%d\n", saved_errno);
	return ret == 0;
}

/*
 * drm_lease_revoke_error()
 *
 * Ownership:
 *   Borrows a DRM master fd and does not take ownership of any lessee fd.  It
 *   only probes the REVOKE_LEASE lookup and permission result for a lessee id.
 *
 * Lifetime:
 *   Valid for both live lessee permission probes and post-close/destroy
 *   lifetime probes.
 *
 * Threading:
 *   Single-threaded smoke helper; the kernel serializes owner idr lookup under
 *   the common DRM lease lock.
 */
static void
drm_lease_revoke_error(int fd, uint32_t lessee_id, int expected_errno,
    const char *what)
{
	struct drm_mode_revoke_lease revoke_lease;
	int saved_errno;
	int ret;

	memset(&revoke_lease, 0, sizeof(revoke_lease));
	revoke_lease.lessee_id = lessee_id;

	errno = 0;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_REVOKE_LEASE, &revoke_lease);
	saved_errno = errno;
	check(ret != 0 && saved_errno == expected_errno, what);
	if (ret == 0 || saved_errno != expected_errno)
		printf("    REVOKE_LEASE ret=%d errno=%d expected=%d\n",
		    ret, saved_errno, expected_errno);
}

static void
check_drm_lease_atomic_unleased_connector(int owner_fd, int lease_fd,
    uint32_t unleased_connector_id, uint32_t crtc_id)
{
	drmModeAtomicReqPtr req;
	int saved_errno;
	int ret;

	req = drmModeAtomicAlloc();
	check(req != NULL,
	    "DRM lease atomic unleased connector allocates request");
	if (req == NULL)
		return;

	if (!atomic_add_connector_property(owner_fd, req,
	    unleased_connector_id, "CRTC_ID", crtc_id)) {
		check(false,
		    "DRM lease atomic unleased connector request is constructed");
		drmModeAtomicFree(req);
		return;
	}
	check(true, "DRM lease atomic unleased connector request is constructed");

	errno = 0;
	ret = drmModeAtomicCommit(lease_fd, req,
	    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	saved_errno = errno;
	check(ret != 0,
	    "DRM lease atomic TEST_ONLY with unleased connector is rejected");
	check(saved_errno == ENOENT,
	    "DRM lease atomic unleased connector fails with ENOENT");
	if (ret == 0 || saved_errno != ENOENT)
		printf("    lease atomic unleased connector ret=%d errno=%d\n",
		    ret, saved_errno);

	drmModeAtomicFree(req);
}

static void
check_drm_lease_non_universal_planes(int fd, uint32_t connector_id,
    uint32_t crtc_id, uint32_t primary_plane_id)
{
	drmModePlaneResPtr lease_planes;
	uint32_t get_ids[32];
	uint32_t get_count = 0;
	uint32_t lease_ids[2];
	uint32_t lessee_id = 0;
	int lease_fd = -1;

	check(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 0) == 0,
	    "DRM non-universal lease owner disables ATOMIC client cap");
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 0) == 0,
	    "DRM non-universal lease owner disables UNIVERSAL_PLANES");

	lease_ids[0] = connector_id;
	lease_ids[1] = crtc_id;
	if (drm_lease_create(fd, lease_ids,
	    (uint32_t)(sizeof(lease_ids) / sizeof(lease_ids[0])), &lease_fd,
	    &lessee_id, "DRM non-universal lease CREATE_LEASE succeeds")) {
		check(drmSetClientCap(lease_fd,
		    DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0,
		    "DRM non-universal lease fd accepts UNIVERSAL_PLANES");
		lease_planes = drmModeGetPlaneResources(lease_fd);
		check(lease_planes != NULL,
		    "DRM non-universal lease fd plane resources are readable");
		if (lease_planes != NULL) {
			check(id_in_list(lease_planes->planes,
			    (int)lease_planes->count_planes, primary_plane_id),
			    "DRM non-universal lease exposes implicit primary plane");
			drmModeFreePlaneResources(lease_planes);
		}
		if (drm_lease_get_objects(lease_fd, get_ids,
		    (uint32_t)(sizeof(get_ids) / sizeof(get_ids[0])),
		    &get_count, "DRM non-universal lease GET_LEASE succeeds")) {
			check(id_in_list(get_ids, (int)get_count,
			    primary_plane_id),
			    "DRM non-universal lease GET_LEASE returns implicit primary plane");
		}
		check(close(lease_fd) == 0,
		    "close non-universal DRM lease fd succeeds");
	}

	check(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0,
	    "DRM non-universal lease owner restores ATOMIC client cap");
	check(drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0,
	    "DRM non-universal lease owner restores UNIVERSAL_PLANES");
}

static void
check_drm_lease_atomic_test_only(int lease_fd, uint32_t connector_id,
    uint32_t crtc_id, uint32_t plane_id)
{
	struct atomic_plane_snapshot snapshot;
	drmModeAtomicReqPtr req;
	drmModeCrtcPtr crtc;
	uint32_t mode_blob = 0;
	int saved_errno;
	int ret;

	crtc = drmModeGetCrtc(lease_fd, crtc_id);
	check(crtc != NULL, "DRM lease active CRTC is readable for atomic TEST_ONLY");
	if (crtc == NULL)
		return;
	check(crtc->mode_valid, "DRM lease active CRTC has mode for atomic TEST_ONLY");
	if (!crtc->mode_valid) {
		drmModeFreeCrtc(crtc);
		return;
	}

	if (!get_plane_snapshot(lease_fd, plane_id, &snapshot,
	    "DRM lease primary plane")) {
		drmModeFreeCrtc(crtc);
		return;
	}
	check(snapshot.fb_id != 0, "DRM lease primary plane has framebuffer");
	check(snapshot.crtc_id == crtc_id,
	    "DRM lease primary plane is attached to leased CRTC");

	ret = drmModeCreatePropertyBlob(lease_fd, &crtc->mode,
	    sizeof(crtc->mode), &mode_blob);
	check(ret == 0, "DRM lease creates MODE_ID blob for atomic TEST_ONLY");
	if (ret != 0) {
		printf("    CREATE_BLOB errno=%d\n", errno);
		drmModeFreeCrtc(crtc);
		return;
	}

	req = drmModeAtomicAlloc();
	check(req != NULL, "DRM lease atomic TEST_ONLY allocates request");
	if (req != NULL) {
		if (!atomic_add_crtc_property(lease_fd, req, crtc_id,
		    "MODE_ID", mode_blob) ||
		    !atomic_add_crtc_property(lease_fd, req, crtc_id,
		    "ACTIVE", 1) ||
		    !atomic_add_connector_property(lease_fd, req,
		    connector_id, "CRTC_ID", crtc_id) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "FB_ID", snapshot.fb_id) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "CRTC_ID", snapshot.crtc_id) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "CRTC_X", snapshot.crtc_x) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "CRTC_Y", snapshot.crtc_y) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "CRTC_W", snapshot.crtc_w) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "CRTC_H", snapshot.crtc_h) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "SRC_X", snapshot.src_x) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "SRC_Y", snapshot.src_y) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "SRC_W", snapshot.src_w) ||
		    !atomic_add_plane_property(lease_fd, req, plane_id,
		    "SRC_H", snapshot.src_h)) {
			check(false,
			    "DRM lease atomic TEST_ONLY request describes leased state");
		} else {
			check(true,
			    "DRM lease atomic TEST_ONLY request describes leased state");
			errno = 0;
			ret = drmModeAtomicCommit(lease_fd, req,
			    DRM_MODE_ATOMIC_TEST_ONLY |
			    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
			saved_errno = errno;
			check(ret == 0,
			    "DRM lease atomic TEST_ONLY on leased objects succeeds");
			if (ret != 0)
				printf("    lease atomic TEST_ONLY errno=%d\n",
				    saved_errno);
		}
		drmModeAtomicFree(req);
	}

	check(drmModeDestroyPropertyBlob(lease_fd, mode_blob) == 0,
	    "DRM lease destroys MODE_ID blob for atomic TEST_ONLY");
	drmModeFreeCrtc(crtc);
}

static void
check_drm_lease_encoder_filter(int lease_fd, uint32_t connector_id,
    uint32_t crtc_id)
{
	drmModeConnectorPtr connector;
	drmModeEncoderPtr encoder;

	connector = drmModeGetConnector(lease_fd, connector_id);
	check(connector != NULL,
	    "DRM lease connector is readable for encoder filter");
	if (connector == NULL)
		return;

	check(connector->count_encoders > 0,
	    "DRM lease connector exposes encoder list");
	check(connector->encoder_id != 0,
	    "DRM lease connector has current encoder");
	if (connector->encoder_id == 0) {
		drmModeFreeConnector(connector);
		return;
	}
	check(id_in_list(connector->encoders, connector->count_encoders,
	    connector->encoder_id),
	    "DRM lease current encoder is attached to connector");

	encoder = drmModeGetEncoder(lease_fd, connector->encoder_id);
	check(encoder != NULL,
	    "DRM lease current encoder is readable");
	if (encoder != NULL) {
		check(encoder->crtc_id == crtc_id,
		    "DRM lease current encoder points at leased CRTC");
		check(encoder->possible_crtcs != 0,
		    "DRM lease current encoder possible_crtcs is non-empty");
		check((encoder->possible_crtcs & ~1u) == 0,
		    "DRM lease current encoder possible_crtcs is lease-relative");
		check((encoder->possible_crtcs & 1u) != 0,
		    "DRM lease current encoder allows leased CRTC index");
		drmModeFreeEncoder(encoder);
	}

	drmModeFreeConnector(connector);
}

static void
check_drm_lease_plane_filter(int lease_fd, uint32_t plane_id,
    uint32_t crtc_id)
{
	drmModePlanePtr plane;

	plane = drmModeGetPlane(lease_fd, plane_id);
	check(plane != NULL,
	    "DRM lease primary plane is readable for CRTC filter");
	if (plane == NULL)
		return;

	check(plane->crtc_id == crtc_id,
	    "DRM lease primary plane GETPLANE points at leased CRTC");
	check(plane->possible_crtcs != 0,
	    "DRM lease primary plane possible_crtcs is non-empty");
	check((plane->possible_crtcs & ~1u) == 0,
	    "DRM lease primary plane possible_crtcs is lease-relative");
	check((plane->possible_crtcs & 1u) != 0,
	    "DRM lease primary plane allows leased CRTC index");

	drmModeFreePlane(plane);
}

/*
 * check_drm_lease_contract()
 *
 * Ownership:
 *   Borrows the owner DRM master fd and the current resources snapshot.  Any
 *   lease fd, libdrm resource snapshot, plane snapshot, and temporary property
 *   blob created here is closed or released before return.
 *
 * Lifetime:
 *   The probe is valid only while the current active connector/CRTC/primary
 *   plane route remains stable.  It revokes its own lease before returning so
 *   later display probes keep the same owner-visible object graph.
 *
 * Threading:
 *   Single-threaded KMS UAPI probe.  Kernel-side lease tree updates are
 *   serialized by common DRM locks; this userspace test does not share fds
 *   with another thread.
 */
static void
check_drm_lease_contract(int fd, const drmModeRes *resources,
    bool expect_no_connected)
{
	drmModePlaneResPtr plane_resources;
	drmModePlaneResPtr lease_planes;
	drmModePlaneResPtr owner_planes;
	drmModeConnectorPtr lease_connector;
	drmModeRes *lease_resources;
	drmModeRes *revoked_resources;
	uint32_t active_crtc_id = 0;
	uint32_t active_crtc_index = 0;
	uint32_t bad_ids[3];
	uint32_t connector_id = 0;
	uint32_t duplicate_ids[4];
	uint32_t lease_ids[3];
	uint32_t empty_get_ids[4];
	uint32_t empty_get_count = 0;
	uint32_t lessee_get_ids[32];
	uint32_t lessee_get_count = 0;
	uint32_t empty_lessee_id = 0;
	uint32_t lessee_id = 0;
	uint32_t missing_connector_ids[2];
	uint32_t missing_plane_ids[2];
	uint32_t object_count = 0;
	uint32_t owner_get_ids[4096];
	uint32_t owner_get_count = 0;
	uint32_t owner_get_visible_count = 0;
	uint32_t primary_plane_id = 0;
	uint32_t second_lessee_id = 0;
	uint32_t third_lessee_id = 0;
	uint32_t unleased_connector_id = 0;
	int lease_fd = -1;
	int empty_lease_fd = -1;
	int second_lease_fd = -1;
	int third_lease_fd = -1;
	int fd_flags;
	int saved_errno;

	if (expect_no_connected) {
		check(true,
		    "DRM lease skipped because no connected connector is expected");
		return;
	}

	check(find_active_crtc(fd, resources, &active_crtc_id,
	    &active_crtc_index), "DRM lease found active CRTC");
	if (active_crtc_id == 0)
		return;
	check(find_active_connector_for_crtc(fd, resources, active_crtc_id,
	    &connector_id), "DRM lease found active connector");
	if (connector_id == 0)
		return;

	plane_resources = drmModeGetPlaneResources(fd);
	check(plane_resources != NULL, "DRM lease reads owner plane resources");
	if (plane_resources == NULL)
		return;
	check(find_primary_plane_for_crtc_index(plane_resources, fd,
	    (int)active_crtc_index, &primary_plane_id),
	    "DRM lease found primary plane for active CRTC");
	drmModeFreePlaneResources(plane_resources);
	if (primary_plane_id == 0)
		return;

	for (int i = 0; i < resources->count_connectors; i++) {
		if (resources->connectors[i] != connector_id) {
			unleased_connector_id = resources->connectors[i];
			break;
		}
	}
	check(unleased_connector_id != 0,
	    "DRM lease has unleased connector for visibility probe");

	if (drm_lease_create_flags(fd, NULL, 0, O_CLOEXEC | O_NONBLOCK,
	    &empty_lease_fd, &empty_lessee_id,
	    "DRM empty lease CREATE_LEASE with O_NONBLOCK succeeds")) {
		check(empty_lessee_id != 0,
		    "DRM empty lease returns lessee id");
		fd_flags = fcntl(empty_lease_fd, F_GETFL);
		check(fd_flags >= 0,
		    "DRM empty lease fd flags are readable");
		check((fd_flags & O_NONBLOCK) != 0,
		    "DRM empty lease fd preserves O_NONBLOCK");
		lease_resources = drmModeGetResources(empty_lease_fd);
		check(lease_resources != NULL,
		    "DRM empty lease fd resources are readable");
		if (lease_resources != NULL) {
			check(lease_resources->count_connectors == 0,
			    "DRM empty lease fd exposes no connector");
			check(lease_resources->count_crtcs == 0,
			    "DRM empty lease fd exposes no CRTC");
			drmModeFreeResources(lease_resources);
		}
		lease_planes = drmModeGetPlaneResources(empty_lease_fd);
		check(lease_planes != NULL,
		    "DRM empty lease fd plane resources are readable");
		if (lease_planes != NULL) {
			check(lease_planes->count_planes == 0,
			    "DRM empty lease fd exposes no plane");
			drmModeFreePlaneResources(lease_planes);
		}
		if (drm_lease_get_objects(empty_lease_fd, empty_get_ids,
		    (uint32_t)(sizeof(empty_get_ids) /
		    sizeof(empty_get_ids[0])), &empty_get_count,
		    "DRM empty lease GET_LEASE succeeds")) {
			check(empty_get_count == 0,
			    "DRM empty lease GET_LEASE returns no objects");
		}
		check_drm_lease_empty_vblank_rejects(empty_lease_fd,
		    active_crtc_id);
		check(close(empty_lease_fd) == 0,
		    "close empty DRM lease fd succeeds");
	}

	check_drm_lease_non_universal_planes(fd, connector_id,
	    active_crtc_id, primary_plane_id);

	lease_ids[0] = connector_id;
	lease_ids[1] = active_crtc_id;
	lease_ids[2] = primary_plane_id;
	object_count = (uint32_t)(sizeof(lease_ids) / sizeof(lease_ids[0]));

	if (!drm_lease_create(fd, lease_ids, object_count, &lease_fd,
	    &lessee_id, "DRM lease CREATE_LEASE succeeds"))
		return;
	check(drmSetClientCap(lease_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0,
	    "DRM lease fd accepts UNIVERSAL_PLANES");
	check(drmSetClientCap(lease_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0,
	    "DRM lease fd accepts ATOMIC");

	lease_resources = drmModeGetResources(lease_fd);
	check(lease_resources != NULL, "DRM lease fd resources are readable");
	if (lease_resources != NULL) {
		check(lease_resources->count_connectors == 1,
		    "DRM lease fd exposes one connector");
		check(lease_resources->count_crtcs == 1,
		    "DRM lease fd exposes one CRTC");
		check(id_in_list(lease_resources->connectors,
		    lease_resources->count_connectors, connector_id),
		    "DRM lease fd exposes leased connector");
		check(id_in_list(lease_resources->crtcs,
		    lease_resources->count_crtcs, active_crtc_id),
		    "DRM lease fd exposes leased CRTC");
		check(lease_resources->count_encoders >= 1,
		    "DRM lease fd exposes encoder resources");
		lease_connector = drmModeGetConnector(lease_fd, connector_id);
		check(lease_connector != NULL,
		    "DRM lease fd connector is readable for encoder resources");
		if (lease_connector != NULL) {
			check(lease_connector->encoder_id != 0,
			    "DRM lease fd connector has current encoder for resources");
			if (lease_connector->encoder_id != 0) {
				check(id_in_list(lease_resources->encoders,
				    lease_resources->count_encoders,
				    lease_connector->encoder_id),
				    "DRM lease fd encoder resources include current encoder");
			}
			drmModeFreeConnector(lease_connector);
		}
		drmModeFreeResources(lease_resources);
	}

	lease_planes = drmModeGetPlaneResources(lease_fd);
	check(lease_planes != NULL, "DRM lease fd plane resources are readable");
	if (lease_planes != NULL) {
		check(lease_planes->count_planes == 1,
		    "DRM lease fd exposes one plane");
		check(id_in_list(lease_planes->planes,
		    (int)lease_planes->count_planes, primary_plane_id),
		    "DRM lease fd exposes leased primary plane");
		drmModeFreePlaneResources(lease_planes);
	}

	if (unleased_connector_id != 0) {
		drmModeConnector *connector;

		errno = 0;
		connector = drmModeGetConnector(lease_fd,
		    unleased_connector_id);
		saved_errno = errno;
		check(connector == NULL,
		    "DRM lease hides unleased connector");
		check(saved_errno == ENOENT,
		    "DRM lease unleased connector lookup fails with ENOENT");
		if (connector != NULL)
			drmModeFreeConnector(connector);
	}

	if (drm_lease_get_objects(fd, owner_get_ids,
	    (uint32_t)(sizeof(owner_get_ids) / sizeof(owner_get_ids[0])),
	    &owner_get_count, "DRM lease owner GET_LEASE succeeds")) {
		uint32_t owner_expected_count;

		owner_get_visible_count = owner_get_count <=
		    (uint32_t)(sizeof(owner_get_ids) / sizeof(owner_get_ids[0])) ?
		    owner_get_count :
		    (uint32_t)(sizeof(owner_get_ids) / sizeof(owner_get_ids[0]));
		check(owner_get_count <= (uint32_t)(sizeof(owner_get_ids) /
		    sizeof(owner_get_ids[0])),
		    "DRM lease owner GET_LEASE fits probe buffer");
		owner_planes = drmModeGetPlaneResources(fd);
		check(owner_planes != NULL,
		    "DRM lease owner plane resources are readable for GET_LEASE");
		owner_expected_count = (uint32_t)(resources->count_connectors +
		    resources->count_crtcs + resources->count_encoders);
		if (owner_planes != NULL)
			owner_expected_count += owner_planes->count_planes;
		check(owner_get_count >= owner_expected_count,
		    "DRM lease owner GET_LEASE returns full mode object set");
		for (int i = 0; i < resources->count_connectors; i++) {
			check(id_in_list(owner_get_ids,
			    (int)owner_get_visible_count,
			    resources->connectors[i]),
			    "DRM lease owner GET_LEASE returns every connector");
		}
		for (int i = 0; i < resources->count_crtcs; i++) {
			check(id_in_list(owner_get_ids,
			    (int)owner_get_visible_count, resources->crtcs[i]),
			    "DRM lease owner GET_LEASE returns every CRTC");
		}
		for (int i = 0; i < resources->count_encoders; i++) {
			check(id_in_list(owner_get_ids,
			    (int)owner_get_visible_count,
			    resources->encoders[i]),
			    "DRM lease owner GET_LEASE returns every encoder");
		}
		if (owner_planes != NULL) {
			for (uint32_t i = 0; i < owner_planes->count_planes; i++) {
				check(id_in_list(owner_get_ids,
				    (int)owner_get_visible_count,
				    owner_planes->planes[i]),
				    "DRM lease owner GET_LEASE returns every plane");
			}
			drmModeFreePlaneResources(owner_planes);
		}
	}

	if (drm_lease_get_objects(lease_fd, lessee_get_ids,
	    (uint32_t)(sizeof(lessee_get_ids) / sizeof(lessee_get_ids[0])),
	    &lessee_get_count, "DRM lease lessee GET_LEASE succeeds")) {
		check(lessee_get_count == object_count,
		    "DRM lease lessee GET_LEASE returns exact object count");
		check(id_in_list(lessee_get_ids, (int)lessee_get_count,
		    connector_id), "DRM lease lessee GET_LEASE returns connector");
		check(id_in_list(lessee_get_ids, (int)lessee_get_count,
		    active_crtc_id), "DRM lease lessee GET_LEASE returns CRTC");
		check(id_in_list(lessee_get_ids, (int)lessee_get_count,
		    primary_plane_id),
		    "DRM lease lessee GET_LEASE returns primary plane");
	}

	check(drm_lease_list_contains(fd, lessee_id),
	    "DRM lease LIST_LESSEES returns lessee");
	drm_lease_list_empty(lease_fd,
	    "DRM lease lessee LIST_LESSEES returns empty list");
	drm_lease_list_pad_error(fd);
	drm_lease_get_pad_error(fd);
	drm_lease_create_error(lease_fd, lease_ids, object_count, EINVAL,
	    "DRM lease lessee cannot create sub-lease");
	drm_lease_revoke_error(lease_fd, lessee_id, EACCES,
	    "DRM lease lessee REVOKE_LEASE fails with EACCES");
	check_drm_lease_encoder_filter(lease_fd, connector_id, active_crtc_id);
	check_drm_lease_plane_filter(lease_fd, primary_plane_id, active_crtc_id);
	check_drm_lease_atomic_test_only(lease_fd, connector_id,
	    active_crtc_id, primary_plane_id);
	check_drm_lease_vblank_sequence_contract(lease_fd, active_crtc_id);
	if (unleased_connector_id != 0)
		check_drm_lease_atomic_unleased_connector(fd, lease_fd,
		    unleased_connector_id, active_crtc_id);

	drm_lease_create_error(fd, lease_ids, object_count, EBUSY,
	    "DRM lease duplicate live object is rejected with EBUSY");

	duplicate_ids[0] = connector_id;
	duplicate_ids[1] = connector_id;
	duplicate_ids[2] = active_crtc_id;
	duplicate_ids[3] = primary_plane_id;
	drm_lease_create_error(fd, duplicate_ids,
	    (uint32_t)(sizeof(duplicate_ids) / sizeof(duplicate_ids[0])),
	    EEXIST, "DRM lease duplicate request object is rejected with EEXIST");

	bad_ids[0] = 0x7ffffffeu;
	bad_ids[1] = active_crtc_id;
	bad_ids[2] = primary_plane_id;
	drm_lease_create_error(fd, bad_ids,
	    (uint32_t)(sizeof(bad_ids) / sizeof(bad_ids[0])), ENOENT,
	    "DRM lease bad object id is rejected with ENOENT");

	drm_lease_revoke_id(fd, lessee_id, "DRM lease REVOKE_LEASE succeeds");
	check(!drm_lease_list_contains(fd, lessee_id),
	    "DRM lease LIST_LESSEES hides revoked lessee");
	drm_lease_revoke_id(fd, lessee_id,
	    "DRM lease second REVOKE_LEASE on revoked lessee succeeds");

	revoked_resources = drmModeGetResources(lease_fd);
	check(revoked_resources != NULL,
	    "DRM lease revoked fd resources are readable");
	if (revoked_resources != NULL) {
		check(revoked_resources->count_crtcs == 0,
		    "DRM lease fd has no CRTC after revoke");
		check(revoked_resources->count_connectors == 0,
		    "DRM lease fd has no connector after revoke");
		drmModeFreeResources(revoked_resources);
	}
	lease_planes = drmModeGetPlaneResources(lease_fd);
	check(lease_planes != NULL,
	    "DRM lease revoked fd plane resources are readable");
	if (lease_planes != NULL) {
		check(lease_planes->count_planes == 0,
		    "DRM lease fd has no plane after revoke");
		drmModeFreePlaneResources(lease_planes);
	}

	missing_connector_ids[0] = active_crtc_id;
	missing_connector_ids[1] = primary_plane_id;
	drm_lease_create_error(fd, missing_connector_ids,
	    (uint32_t)(sizeof(missing_connector_ids) /
	    sizeof(missing_connector_ids[0])), EINVAL,
	    "DRM lease missing connector is rejected with EINVAL");

	missing_plane_ids[0] = connector_id;
	missing_plane_ids[1] = active_crtc_id;
	drm_lease_create_error(fd, missing_plane_ids,
	    (uint32_t)(sizeof(missing_plane_ids) /
	    sizeof(missing_plane_ids[0])), EINVAL,
	    "DRM lease missing plane is rejected with EINVAL");

	if (drm_lease_create(fd, lease_ids, object_count, &second_lease_fd,
	    &second_lessee_id,
	    "DRM lease object can be re-leased after revoke")) {
		check(second_lessee_id != lessee_id,
		    "DRM lease re-lease returns a fresh lessee id");
		check(close(second_lease_fd) == 0,
		    "close second DRM lease fd succeeds");
		check(!drm_lease_list_contains(fd, second_lessee_id),
		    "DRM lease LIST_LESSEES hides closed second lessee");
		drm_lease_revoke_error(fd, second_lessee_id, ENOENT,
		    "DRM lease closed second lessee revoke fails with ENOENT");
		if (drm_lease_create(fd, lease_ids, object_count,
		    &third_lease_fd, &third_lessee_id,
		    "DRM lease object can be re-leased after close")) {
			check(third_lessee_id != 0,
			    "DRM lease close re-lease returns lessee id");
			check(close(third_lease_fd) == 0,
			    "close third DRM lease fd succeeds");
			check(!drm_lease_list_contains(fd, third_lessee_id),
			    "DRM lease LIST_LESSEES hides closed third lessee");
			drm_lease_revoke_error(fd, third_lessee_id, ENOENT,
			    "DRM lease closed third lessee revoke fails with ENOENT");
		}
	}

	check(close(lease_fd) == 0, "close DRM lease fd succeeds");
	drm_lease_revoke_error(fd, lessee_id, ENOENT,
	    "DRM lease closed revoked lessee revoke fails with ENOENT");
}

static int
atomic_connected_mode_test_only_commit(int fd, uint32_t connector_id,
    uint32_t crtc_id, uint32_t plane_id, uint32_t fb_id,
    const drmModeModeInfo *mode, int *saved_errno)
{
	drmModeAtomicReqPtr req;
	uint32_t mode_blob = 0;
	int ret;

	ret = drmModeCreatePropertyBlob(fd, mode, sizeof(*mode), &mode_blob);
	if (ret != 0) {
		*saved_errno = errno;
		return -1;
	}

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		destroy_property_blob(fd, mode_blob,
		    "destroy MODE_ID blob for failed connected mode TEST_ONLY probe");
		return -1;
	}

	if (!atomic_add_crtc_property(fd, req, crtc_id, "MODE_ID",
	    mode_blob) ||
	    !atomic_add_crtc_property(fd, req, crtc_id, "ACTIVE", 1) ||
	    !atomic_add_connector_property(fd, req, connector_id, "CRTC_ID",
	    crtc_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "FB_ID", fb_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_ID",
	    crtc_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_X", 0) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_Y", 0) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_W",
	    mode->hdisplay) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_H",
	    mode->vdisplay) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_X", 0) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_Y", 0) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_W",
	    (uint64_t)mode->hdisplay << 16) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_H",
	    (uint64_t)mode->vdisplay << 16)) {
		drmModeAtomicFree(req);
		destroy_property_blob(fd, mode_blob,
		    "destroy MODE_ID blob for failed connected mode TEST_ONLY probe");
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req,
	    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	destroy_property_blob(fd, mode_blob,
	    "destroy MODE_ID blob for connected mode TEST_ONLY probe");
	return ret;
}

/*
 * check_connected_mode_list_atomic_contract()
 *
 * Ownership:
 *   Borrows the DRM resource snapshot and reads connector, encoder, and plane
 *   snapshots through libdrm.  The probe owns one temporary dumb BO/FB per
 *   connected connector and one MODE_ID blob per mode; every temporary object
 *   is destroyed before return.
 *
 * Lifetime:
 *   Uses TEST_ONLY atomic commits only.  It must not program display hardware,
 *   change the visible framebuffer, or retain connector/plane state after the
 *   current probe.
 *
 * Threading:
 *   Single-threaded KMS UAPI validation.  The kernel evaluates each TEST_ONLY
 *   request under normal modeset locks while userspace owns no driver-private
 *   lock.
 */
static void
check_connected_mode_list_atomic_contract(int fd, const drmModeRes *resources,
    bool expect_no_connected)
{
	drmModePlaneResPtr plane_resources;

	if (expect_no_connected) {
		printf("SKIP connected mode TEST_ONLY probe by NVKM_DRMTEST_EXPECT_NO_CONNECTED\n");
		return;
	}

	plane_resources = drmModeGetPlaneResources(fd);
	check(plane_resources != NULL,
	    "plane resources readable for connected mode TEST_ONLY probe");
	if (plane_resources == NULL)
		return;

	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnectorPtr connector;
		drmModeEncoderPtr encoder;
		uint32_t max_width = 0;
		uint32_t max_height = 0;
		uint32_t handle = 0;
		uint32_t pitch = 0;
		uint32_t fb_id = 0;
		uint32_t plane_id = 0;
		int crtc_index = -1;
		bool all_modes_sane = true;
		bool all_modes_passed = true;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector == NULL)
			continue;
		if (connector->connection != DRM_MODE_CONNECTED) {
			drmModeFreeConnector(connector);
			continue;
		}
		check(connector->count_modes > 0,
		    "connected connector exposes TEST_ONLY-checkable modes");
		if (connector->count_modes <= 0 || connector->encoder_id == 0) {
			drmModeFreeConnector(connector);
			continue;
		}

		for (int m = 0; m < connector->count_modes; m++) {
			const drmModeModeInfo *mode = &connector->modes[m];

			if (!mode_timing_is_sane(mode))
				all_modes_sane = false;
			if (mode->hdisplay > max_width)
				max_width = mode->hdisplay;
			if (mode->vdisplay > max_height)
				max_height = mode->vdisplay;
		}
		check(all_modes_sane,
		    "connected connector mode list has sane timings");
		if (!all_modes_sane || max_width == 0 || max_height == 0) {
			drmModeFreeConnector(connector);
			continue;
		}

		encoder = drmModeGetEncoder(fd, connector->encoder_id);
		check(encoder != NULL,
		    "connected connector encoder readable for mode TEST_ONLY probe");
		if (encoder == NULL) {
			drmModeFreeConnector(connector);
			continue;
		}
		if (!id_index_in_list(resources->crtcs, resources->count_crtcs,
		    encoder->crtc_id, &crtc_index)) {
			check(false,
			    "connected connector CRTC present for mode TEST_ONLY probe");
			drmModeFreeEncoder(encoder);
			drmModeFreeConnector(connector);
			continue;
		}
		check(true,
		    "connected connector CRTC present for mode TEST_ONLY probe");
		if (!find_primary_plane_for_crtc_index(plane_resources, fd,
		    crtc_index, &plane_id)) {
			check(false,
			    "connected connector primary plane available for mode TEST_ONLY probe");
			drmModeFreeEncoder(encoder);
			drmModeFreeConnector(connector);
			continue;
		}
		check(true,
		    "connected connector primary plane available for mode TEST_ONLY probe");

		if (!create_dumb_buffer_for(fd, max_width, max_height, 32,
		    &handle, &pitch,
		    "CREATE_DUMB succeeds for connected mode TEST_ONLY framebuffer")) {
			drmModeFreeEncoder(encoder);
			drmModeFreeConnector(connector);
			continue;
		}
		if (!add_linear_framebuffer(fd, max_width, max_height,
		    DRM_FORMAT_XRGB8888, handle, pitch, &fb_id,
		    "ADDFB2 accepts connected mode TEST_ONLY framebuffer")) {
			destroy_dumb_buffer_for(fd, handle,
			    "DESTROY_DUMB succeeds for connected mode TEST_ONLY framebuffer");
			drmModeFreeEncoder(encoder);
			drmModeFreeConnector(connector);
			continue;
		}

		for (int m = 0; m < connector->count_modes; m++) {
			int saved_errno = 0;
			int ret;

			printf("    TEST_ONLY mode %s %ux%u@%u\n",
			    connector->modes[m].name,
			    connector->modes[m].hdisplay,
			    connector->modes[m].vdisplay,
			    connector->modes[m].vrefresh);
			ret = atomic_connected_mode_test_only_commit(fd,
			    connector->connector_id, encoder->crtc_id, plane_id,
			    fb_id, &connector->modes[m], &saved_errno);
			if (ret != 0) {
				printf("    connected mode TEST_ONLY errno=%d\n",
				    saved_errno);
				all_modes_passed = false;
			}
		}
		check(all_modes_passed,
		    "connected connector mode list passes atomic TEST_ONLY");

		remove_framebuffer(fd, fb_id,
		    "RMFB succeeds for connected mode TEST_ONLY framebuffer");
		destroy_dumb_buffer_for(fd, handle,
		    "DESTROY_DUMB succeeds for connected mode TEST_ONLY framebuffer");
		drmModeFreeEncoder(encoder);
		drmModeFreeConnector(connector);
	}

	drmModeFreePlaneResources(plane_resources);
}

static int
atomic_connector_scaler_commit(int fd, uint32_t connector_id,
    uint32_t crtc_id, uint64_t scaling_mode, uint64_t underscan,
    uint64_t underscan_hborder, uint64_t underscan_vborder, int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	if (!atomic_add_connector_property(fd, req, connector_id, "CRTC_ID",
	    crtc_id) ||
	    !atomic_add_connector_property(fd, req, connector_id,
	    "scaling mode", scaling_mode) ||
	    !atomic_add_connector_property(fd, req, connector_id, "underscan",
	    underscan) ||
	    !atomic_add_connector_property(fd, req, connector_id,
	    "underscan hborder", underscan_hborder) ||
	    !atomic_add_connector_property(fd, req, connector_id,
	    "underscan vborder", underscan_vborder)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

static bool
check_connector_scaler_property_value(int fd, uint32_t connector_id,
    const char *name, uint64_t expected, const char *object_name,
    const char *label)
{
	uint64_t value = 0;

	if (!get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, name, &value, object_name))
		return false;
	check(value == expected, label);
	return value == expected;
}

static int
legacy_connector_set_property(int fd, uint32_t connector_id, const char *name,
    uint64_t value, int *saved_errno)
{
	uint32_t property_id = 0;
	int ret;

	if (!get_property_id(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, name,
	    &property_id)) {
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeConnectorSetProperty(fd, connector_id, property_id, value);
	*saved_errno = errno;
	return ret;
}

/*
 * check_legacy_dpms_runtime_contract()
 *
 * Ownership:
 *   Borrows the active connector and CRTC IDs from KMS state.  The helper owns
 *   no framebuffer, mode blob, connector state, or GEM reference.
 *
 * Lifetime:
 *   Performs one legacy MODE_OBJ_SETPROPERTY(DPMS=OFF) call followed by a
 *   DPMS=ON restore.  The display may blank briefly while OFF is live.  If the
 *   OFF call succeeds and the normal restore fails, a best-effort second ON
 *   restore is issued before return.
 *
 * Threading:
 *   Single-threaded console probe.  It must run without an X/Wayland DRM
 *   master.  The DRM atomic helper serializes DPMS remapping with modeset
 *   locks; this function does not take driver-private locks.
 */
static void
check_legacy_dpms_runtime_contract(int fd, const drmModeRes *resources,
    uint32_t crtc_id, uint32_t crtc_index)
{
	struct modeset_counter_snapshot before;
	struct modeset_counter_snapshot after_off;
	struct modeset_counter_snapshot after_on;
	uint32_t connector_id = 0;
	uint64_t saved_crtc_id = 0;
	uint64_t saved_dpms = 0;
	uint64_t active = 0;
	uint64_t head_mask;
	char object_name[64];
	int saved_errno = 0;
	int ret;
	bool dpms_off = false;
	bool restored = false;

	head_mask = crtc_index >= 64 ? 0 : (1ULL << crtc_index);
	check(head_mask != 0, "active CRTC index fits DPMS head mask");
	if (head_mask == 0)
		return;

	if (!find_active_connector_for_crtc(fd, resources, crtc_id,
	    &connector_id)) {
		check(false, "active connector is available for legacy DPMS probe");
		return;
	}
	check(true, "active connector is available for legacy DPMS probe");
	snprintf(object_name, sizeof(object_name), "connector %u",
	    connector_id);

	if (!get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &saved_crtc_id,
	    object_name) ||
	    !get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "DPMS", &saved_dpms, object_name))
		return;
	check(saved_crtc_id == crtc_id,
	    "active connector is attached to active CRTC for legacy DPMS probe");
	check(saved_dpms == DRM_MODE_DPMS_ON,
	    "active connector DPMS starts On for legacy DPMS probe");
	if (saved_crtc_id != crtc_id || saved_dpms != DRM_MODE_DPMS_ON)
		return;

	if (!read_modeset_counter_snapshot(&before, "legacy DPMS probe"))
		return;

	ret = legacy_connector_set_property(fd, connector_id, "DPMS",
	    DRM_MODE_DPMS_OFF, &saved_errno);
	if (ret != 0) {
		printf("    legacy DPMS OFF errno=%d\n", saved_errno);
		check(false, "legacy DPMS OFF commit succeeds");
		return;
	}
	dpms_off = true;
	check(true, "legacy DPMS OFF commit succeeds");
	check_connector_scaler_property_value(fd, connector_id, "DPMS",
	    DRM_MODE_DPMS_OFF, object_name,
	    "legacy DPMS OFF updates connector DPMS property");
	if (get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "ACTIVE", &active, "DPMS-off CRTC"))
		check(active == 0, "legacy DPMS OFF marks CRTC inactive");
	if (read_modeset_counter_snapshot(&after_off, "legacy DPMS OFF")) {
		check(after_off.atomic_tail_disable_op_count >
		    before.atomic_tail_disable_op_count,
		    "legacy DPMS OFF increments tail disable op count");
		check((after_off.atomic_tail_last_disable_heads &
		    head_mask) != 0,
		    "legacy DPMS OFF records disabled head");
		check(after_off.commit_error_count == before.commit_error_count,
		    "legacy DPMS OFF does not increment commit_error_count");
		check(after_off.atomic_tail_active == 0 &&
		    after_off.atomic_tail_stage == 0,
		    "legacy DPMS OFF leaves no active tail transaction");
		check(after_off.display_audit_pending_valid == 0,
		    "legacy DPMS OFF leaves no pending display audit");
	}

	ret = legacy_connector_set_property(fd, connector_id, "DPMS",
	    DRM_MODE_DPMS_ON, &saved_errno);
	if (ret != 0) {
		printf("    legacy DPMS ON restore errno=%d\n", saved_errno);
		check(false, "legacy DPMS ON restore succeeds");
		goto out_restore;
	}
	dpms_off = false;
	restored = true;
	check(true, "legacy DPMS ON restore succeeds");
	check_connector_scaler_property_value(fd, connector_id, "DPMS",
	    DRM_MODE_DPMS_ON, object_name,
	    "legacy DPMS ON restores connector DPMS property");
	active = 0;
	if (get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "ACTIVE", &active, "DPMS-restored CRTC"))
		check(active != 0, "legacy DPMS ON marks CRTC active");
	if (read_modeset_counter_snapshot(&after_on, "legacy DPMS ON restore")) {
		check(after_on.atomic_tail_enable_op_count >
		    before.atomic_tail_enable_op_count,
		    "legacy DPMS ON increments tail enable op count");
		check((after_on.atomic_tail_last_enable_heads &
		    head_mask) != 0,
		    "legacy DPMS ON records enabled head");
		check(after_on.commit_error_count == before.commit_error_count,
		    "legacy DPMS ON does not increment commit_error_count");
		check(after_on.atomic_tail_active == 0 &&
		    after_on.atomic_tail_stage == 0,
		    "legacy DPMS ON leaves no active tail transaction");
		check(after_on.display_audit_pending_valid == 0,
		    "legacy DPMS ON leaves no pending display audit");
	}

out_restore:
	if (dpms_off && !restored) {
		printf("    attempting best-effort legacy DPMS ON cleanup\n");
		ret = legacy_connector_set_property(fd, connector_id, "DPMS",
		    DRM_MODE_DPMS_ON, &saved_errno);
		check(ret == 0, "legacy DPMS cleanup restore succeeds");
	}
}

/*
 * check_legacy_setcrtc_runtime_contract()
 *
 * Ownership:
 *   Borrows the active CRTC, connector, and primary plane IDs from KMS state.
 *   The probe owns one temporary dumb BO/FB used as the restore scanout target.
 *   On successful restore the FB remains referenced by KMS until fd close; on
 *   failure it is removed before the BO is destroyed.
 *
 * Lifetime:
 *   Performs one legacy SetCrtc disable call followed by one legacy SetCrtc
 *   restore call with the saved mode.  The display may blank briefly while the
 *   disable is live.  The restore path does not reuse the old console FB, whose
 *   lifetime is outside this probe.
 *
 * Threading:
 *   Single-threaded console probe.  It must run without an X/Wayland DRM
 *   master.  The DRM legacy modeset helper serializes the translated atomic
 *   commits with normal modeset locks.
 */
static void
check_legacy_setcrtc_runtime_contract(int fd, const drmModeRes *resources,
    uint32_t crtc_id, uint32_t crtc_index, uint32_t plane_id,
    const char *object_name)
{
	struct atomic_plane_snapshot snapshot;
	struct atomic_plane_snapshot restored_snapshot;
	struct modeset_counter_snapshot before;
	struct modeset_counter_snapshot after_disable;
	struct modeset_counter_snapshot after_restore;
	drmModeCrtcPtr crtc;
	drmModeModeInfo saved_mode;
	uint32_t connector_id = 0;
	uint32_t restore_handle = 0;
	uint32_t restore_pitch = 0;
	uint32_t restore_fb = 0;
	uint64_t active = 0;
	uint64_t connector_crtc = 0;
	uint64_t head_mask;
	int saved_errno = 0;
	int ret;
	int crtc_x;
	int crtc_y;
	bool disabled = false;
	bool restored = false;
	bool have_after_disable = false;

	head_mask = crtc_index >= 64 ? 0 : (1ULL << crtc_index);
	check(head_mask != 0, "active CRTC index fits legacy SetCrtc head mask");
	if (head_mask == 0)
		return;

	if (!find_active_connector_for_crtc(fd, resources, crtc_id,
	    &connector_id)) {
		check(false,
		    "active connector is available for legacy SetCrtc probe");
		return;
	}
	check(true, "active connector is available for legacy SetCrtc probe");

	if (!get_plane_snapshot(fd, plane_id, &snapshot, object_name))
		return;
	check(snapshot.fb_id != 0,
	    "active primary plane has framebuffer for legacy SetCrtc probe");
	check(snapshot.crtc_id == crtc_id,
	    "active primary plane is attached to active CRTC for legacy SetCrtc probe");
	if (snapshot.fb_id == 0 || snapshot.crtc_id != crtc_id)
		return;

	crtc = drmModeGetCrtc(fd, crtc_id);
	check(crtc != NULL, "active CRTC is readable for legacy SetCrtc probe");
	if (crtc == NULL)
		return;
	check(crtc->mode_valid, "active CRTC has a mode for legacy SetCrtc probe");
	if (!crtc->mode_valid) {
		drmModeFreeCrtc(crtc);
		return;
	}
	saved_mode = crtc->mode;
	crtc_x = crtc->x;
	crtc_y = crtc->y;
	drmModeFreeCrtc(crtc);

	if (!read_modeset_counter_snapshot(&before, "legacy SetCrtc probe"))
		return;

	if (!create_dumb_buffer_for(fd, saved_mode.hdisplay,
	    saved_mode.vdisplay, 32, &restore_handle, &restore_pitch,
	    "CREATE_DUMB succeeds for legacy SetCrtc restore probe"))
		return;
	if (!clear_dumb_buffer(fd, restore_handle, restore_pitch,
	    saved_mode.vdisplay, "clear legacy SetCrtc restore framebuffer"))
		goto out_destroy_restore_bo;
	if (!add_linear_framebuffer(fd, saved_mode.hdisplay,
	    saved_mode.vdisplay, DRM_FORMAT_XRGB8888, restore_handle,
	    restore_pitch, &restore_fb,
	    "ADDFB2 accepts XRGB8888 linear legacy SetCrtc restore probe"))
		goto out_destroy_restore_bo;

	errno = 0;
	ret = drmModeSetCrtc(fd, crtc_id, 0, 0, 0, NULL, 0, NULL);
	saved_errno = errno;
	if (ret != 0) {
		printf("    legacy SetCrtc disable errno=%d\n", saved_errno);
		check(false, "legacy SetCrtc disable succeeds");
		goto out_remove_restore_fb;
	}
	disabled = true;
	check(true, "legacy SetCrtc disable succeeds");

	if (get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "ACTIVE", &active, "legacy SetCrtc disabled CRTC"))
		check(active == 0, "legacy SetCrtc disable marks CRTC inactive");
	if (get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &connector_crtc,
	    "legacy SetCrtc disabled connector"))
		check(connector_crtc == 0,
		    "legacy SetCrtc disable detaches connector CRTC_ID");
	have_after_disable = read_modeset_counter_snapshot(&after_disable,
	    "legacy SetCrtc disable");
	if (have_after_disable) {
		check(after_disable.atomic_tail_disable_op_count >
		    before.atomic_tail_disable_op_count,
		    "legacy SetCrtc disable increments tail disable op count");
		check((after_disable.atomic_tail_last_disable_heads &
		    head_mask) != 0,
		    "legacy SetCrtc disable records disabled head");
		check(after_disable.commit_error_count == before.commit_error_count,
		    "legacy SetCrtc disable does not increment commit_error_count");
		check(after_disable.atomic_tail_active == 0 &&
		    after_disable.atomic_tail_stage == 0,
		    "legacy SetCrtc disable leaves no active tail transaction");
		check(after_disable.display_audit_pending_valid == 0,
		    "legacy SetCrtc disable leaves no pending display audit");
	}

	errno = 0;
	ret = drmModeSetCrtc(fd, crtc_id, restore_fb, crtc_x, crtc_y,
	    &connector_id, 1, &saved_mode);
	saved_errno = errno;
	if (ret != 0) {
		printf("    legacy SetCrtc restore errno=%d\n", saved_errno);
		check(false, "legacy SetCrtc restore succeeds");
		goto out_restore;
	}
	restored = true;
	check(true, "legacy SetCrtc restore succeeds");

	active = 0;
	if (get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "ACTIVE", &active, "legacy SetCrtc restored CRTC"))
		check(active != 0, "legacy SetCrtc restore marks CRTC active");
	connector_crtc = 0;
	if (get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &connector_crtc,
	    "legacy SetCrtc restored connector"))
		check(connector_crtc == crtc_id,
		    "legacy SetCrtc restore attaches connector CRTC_ID");
	if (get_plane_snapshot(fd, plane_id, &restored_snapshot, object_name)) {
		check(restored_snapshot.fb_id == restore_fb,
		    "legacy SetCrtc restore restores primary FB_ID");
		check(restored_snapshot.crtc_id == snapshot.crtc_id,
		    "legacy SetCrtc restore restores primary CRTC_ID");
	}
	if (read_modeset_counter_snapshot(&after_restore,
	    "legacy SetCrtc restore")) {
		if (have_after_disable) {
			check(after_restore.atomic_tail_enable_op_count >
			    after_disable.atomic_tail_enable_op_count,
			    "legacy SetCrtc restore increments tail enable op count");
		}
		check((after_restore.atomic_tail_last_enable_heads &
		    head_mask) != 0,
		    "legacy SetCrtc restore records enabled head");
		check(after_restore.commit_error_count == before.commit_error_count,
		    "legacy SetCrtc restore does not increment commit_error_count");
		check(after_restore.atomic_tail_active == 0 &&
		    after_restore.atomic_tail_stage == 0,
		    "legacy SetCrtc restore leaves no active tail transaction");
		check(after_restore.display_audit_pending_valid == 0,
		    "legacy SetCrtc restore leaves no pending display audit");
	}

out_restore:
	if (!restored && disabled) {
		printf("    attempting best-effort legacy SetCrtc restore\n");
		ret = drmModeSetCrtc(fd, crtc_id, restore_fb, crtc_x, crtc_y,
		    &connector_id, 1, &saved_mode);
		check(ret == 0, "legacy SetCrtc cleanup restore succeeds");
		restored = ret == 0;
	}
	if (restored) {
		printf("    legacy SetCrtc restore probe fb=%u handle=%u kept until fd close\n",
		    restore_fb, restore_handle);
		return;
	}

out_remove_restore_fb:
	remove_framebuffer(fd, restore_fb,
	    "RMFB succeeds for failed legacy SetCrtc restore probe");
out_destroy_restore_bo:
	destroy_dumb_buffer_for(fd, restore_handle,
	    "DESTROY_DUMB succeeds for failed legacy SetCrtc restore probe");
}

/*
 * check_atomic_connector_scaler_runtime_contract()
 *
 * Ownership:
 *   Borrows the active connector and CRTC IDs from KMS state.  No framebuffer,
 *   blob, or GEM object ownership is transferred.
 *
 * Lifetime:
 *   Performs one real connector-property atomic commit with underscan enabled,
 *   then restores the exact property values observed before the probe.  The
 *   visible console may shrink briefly while the test commit is live.
 *
 * Threading:
 *   Single-threaded console probe.  It must run without an X/Wayland DRM
 *   master.  The kernel serializes connector state changes with the normal
 *   atomic modeset locks.
 */
static void
check_atomic_connector_scaler_runtime_contract(int fd,
    const drmModeRes *resources, uint32_t crtc_id)
{
	struct modeset_counter_snapshot before;
	struct modeset_counter_snapshot after_scaling;
	struct modeset_counter_snapshot after_scaling_restore;
	struct modeset_counter_snapshot after_underscan;
	struct modeset_counter_snapshot after_restore;
	uint32_t connector_id = 0;
	uint64_t saved_crtc_id = 0;
	uint64_t saved_scaling = 0;
	uint64_t saved_underscan = 0;
	uint64_t saved_hborder = 0;
	uint64_t saved_vborder = 0;
	uint64_t probe_scaling = DRM_MODE_SCALE_CENTER;
	char object_name[64];
	int saved_errno = 0;
	int ret;
	bool committed = false;
	bool restored = false;

	if (!find_active_connector_for_crtc(fd, resources, crtc_id,
	    &connector_id)) {
		check(false, "active connector is available for scaler runtime probe");
		return;
	}
	check(true, "active connector is available for scaler runtime probe");
	snprintf(object_name, sizeof(object_name), "connector %u",
	    connector_id);

	if (!has_property(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "underscan")) {
		printf("SKIP %s underscan runtime probe: property unavailable\n",
		    object_name);
		return;
	}

	if (!get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &saved_crtc_id,
	    object_name) ||
	    !get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "scaling mode", &saved_scaling,
	    object_name) ||
	    !get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "underscan", &saved_underscan,
	    object_name) ||
	    !get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "underscan hborder", &saved_hborder,
	    object_name) ||
	    !get_property_value_checked(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "underscan vborder", &saved_vborder,
	    object_name))
		return;

	check(saved_crtc_id == crtc_id,
	    "active connector is attached to active CRTC for scaler runtime probe");
	if (saved_crtc_id != crtc_id)
		return;
	if (saved_scaling == DRM_MODE_SCALE_CENTER)
		probe_scaling = DRM_MODE_SCALE_ASPECT;

	if (!read_modeset_counter_snapshot(&before,
	    "connector scaler runtime probe"))
		return;

	ret = atomic_connector_scaler_commit(fd, connector_id, crtc_id,
	    probe_scaling, saved_underscan, saved_hborder, saved_vborder,
	    &saved_errno);
	if (ret != 0) {
		printf("    connector scaler probe commit errno=%d\n",
		    saved_errno);
		check(false, "atomic connector scaling-only probe commit succeeds");
		return;
	}
	committed = true;
	check(true, "atomic connector scaling-only probe commit succeeds");

	check_connector_scaler_property_value(fd, connector_id,
	    "scaling mode", probe_scaling, object_name,
	    "atomic connector scaling-only probe updates scaling mode");
	check_connector_scaler_property_value(fd, connector_id,
	    "underscan", saved_underscan, object_name,
	    "atomic connector scaling-only probe preserves underscan");

	if (read_modeset_counter_snapshot(&after_scaling,
	    "connector scaling-only probe")) {
		check(after_scaling.commit_error_count == before.commit_error_count,
		    "atomic connector scaling-only probe does not increment commit_error_count");
		check(after_scaling.atomic_tail_active == 0 &&
		    after_scaling.atomic_tail_stage == 0,
		    "atomic connector scaling-only probe leaves no active tail transaction");
		check(after_scaling.display_audit_pending_valid == 0,
		    "atomic connector scaling-only probe leaves no pending display audit");
	}

	ret = atomic_connector_scaler_commit(fd, connector_id, crtc_id,
	    saved_scaling, saved_underscan, saved_hborder, saved_vborder,
	    &saved_errno);
	if (ret != 0) {
		printf("    connector scaling-only restore commit errno=%d\n",
		    saved_errno);
		check(false, "atomic connector scaling-only restore commit succeeds");
		goto out_cleanup;
	}
	committed = false;
	check(true, "atomic connector scaling-only restore commit succeeds");
	check_connector_scaler_property_value(fd, connector_id,
	    "scaling mode", saved_scaling, object_name,
	    "atomic connector scaling-only restore restores scaling mode");

	if (read_modeset_counter_snapshot(&after_scaling_restore,
	    "connector scaling-only restore")) {
		check(after_scaling_restore.commit_error_count ==
		    before.commit_error_count,
		    "atomic connector scaling-only restore does not increment commit_error_count");
		check(after_scaling_restore.atomic_tail_active == 0 &&
		    after_scaling_restore.atomic_tail_stage == 0,
		    "atomic connector scaling-only restore leaves no active tail transaction");
		check(after_scaling_restore.display_audit_pending_valid == 0,
		    "atomic connector scaling-only restore leaves no pending display audit");
	}

	ret = atomic_connector_scaler_commit(fd, connector_id, crtc_id,
	    saved_scaling, NVKM_DRMTEST_UNDERSCAN_ON, 64, 36,
	    &saved_errno);
	if (ret != 0) {
		printf("    connector underscan-only probe commit errno=%d\n",
		    saved_errno);
		check(false, "atomic connector underscan-only probe commit succeeds");
		return;
	}
	committed = true;
	check(true, "atomic connector underscan-only probe commit succeeds");
	check_connector_scaler_property_value(fd, connector_id,
	    "scaling mode", saved_scaling, object_name,
	    "atomic connector underscan-only probe preserves scaling mode");
	check_connector_scaler_property_value(fd, connector_id,
	    "underscan", NVKM_DRMTEST_UNDERSCAN_ON, object_name,
	    "atomic connector underscan-only probe updates underscan");
	check_connector_scaler_property_value(fd, connector_id,
	    "underscan hborder", 64, object_name,
	    "atomic connector underscan-only probe updates underscan hborder");
	check_connector_scaler_property_value(fd, connector_id,
	    "underscan vborder", 36, object_name,
	    "atomic connector underscan-only probe updates underscan vborder");

	if (read_modeset_counter_snapshot(&after_underscan,
	    "connector underscan-only probe")) {
		check(after_underscan.commit_error_count == before.commit_error_count,
		    "atomic connector underscan-only probe does not increment commit_error_count");
		check(after_underscan.atomic_tail_active == 0 &&
		    after_underscan.atomic_tail_stage == 0,
		    "atomic connector underscan-only probe leaves no active tail transaction");
		check(after_underscan.display_audit_pending_valid == 0,
		    "atomic connector underscan-only probe leaves no pending display audit");
	}

	ret = atomic_connector_scaler_commit(fd, connector_id, crtc_id,
	    saved_scaling, saved_underscan, saved_hborder, saved_vborder,
	    &saved_errno);
	if (ret != 0) {
		printf("    connector underscan restore commit errno=%d\n",
		    saved_errno);
		check(false, "atomic connector scaler restore commit succeeds");
		goto out_cleanup;
	}
	check(true, "atomic connector scaler restore commit succeeds");
	committed = false;
	restored = true;

	check_connector_scaler_property_value(fd, connector_id,
	    "scaling mode", saved_scaling, object_name,
	    "atomic connector scaler restore restores scaling mode");
	check_connector_scaler_property_value(fd, connector_id,
	    "underscan", saved_underscan, object_name,
	    "atomic connector scaler restore restores underscan");
	check_connector_scaler_property_value(fd, connector_id,
	    "underscan hborder", saved_hborder, object_name,
	    "atomic connector scaler restore restores underscan hborder");
	check_connector_scaler_property_value(fd, connector_id,
	    "underscan vborder", saved_vborder, object_name,
	    "atomic connector scaler restore restores underscan vborder");

	if (restored && read_modeset_counter_snapshot(&after_restore,
	    "connector scaler restore completion")) {
		check(after_restore.commit_error_count == before.commit_error_count,
		    "atomic connector scaler restore does not increment commit_error_count");
		check(after_restore.atomic_tail_active == 0 &&
		    after_restore.atomic_tail_stage == 0,
		    "atomic connector scaler restore leaves no active tail transaction");
		check(after_restore.display_audit_pending_valid == 0,
		    "atomic connector scaler restore leaves no pending display audit");
	}

out_cleanup:
	if (committed) {
		(void)atomic_connector_scaler_commit(fd, connector_id, crtc_id,
		    saved_scaling, saved_underscan, saved_hborder,
		    saved_vborder, &saved_errno);
	}
}

static int
atomic_cursor_test_only_commit(int fd, uint32_t plane_id, uint32_t crtc_id,
    uint32_t fb_id, uint32_t size, int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	if (!atomic_add_plane_property(fd, req, plane_id, "FB_ID", fb_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_ID", crtc_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_X", 0) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_Y", 0) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_W", size) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_H", size) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_X", 0) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_Y", 0) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_W",
	    (uint64_t)size << 16) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_H",
	    (uint64_t)size << 16)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req,
	    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

static int
atomic_primary_test_only_commit(int fd, uint32_t plane_id, uint32_t crtc_id,
    uint32_t fb_id, uint32_t crtc_x, uint32_t crtc_y, uint32_t crtc_w,
    uint32_t crtc_h, uint64_t src_x, uint64_t src_y, uint64_t src_w,
    uint64_t src_h, int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	if (!atomic_add_plane_property(fd, req, plane_id, "FB_ID", fb_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_ID", crtc_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_X", crtc_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_Y", crtc_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_W", crtc_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_H", crtc_h) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_X", src_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_Y", src_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_W", src_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_H", src_h)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req,
	    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

static int
atomic_modeset_disable_commit(int fd, uint32_t crtc_id, uint32_t connector_id,
    uint32_t plane_id, int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	if (!atomic_add_plane_property(fd, req, plane_id, "FB_ID", 0) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_ID", 0) ||
	    !atomic_add_connector_property(fd, req, connector_id, "CRTC_ID",
	    0) ||
	    !atomic_add_crtc_property(fd, req, crtc_id, "ACTIVE", 0) ||
	    !atomic_add_crtc_property(fd, req, crtc_id, "MODE_ID", 0)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

static int
atomic_modeset_restore_commit(int fd, uint32_t crtc_id,
    uint32_t connector_id, uint32_t plane_id,
    const struct atomic_plane_snapshot *snapshot, uint32_t mode_blob,
    int *saved_errno)
{
	drmModeAtomicReqPtr req;
	int ret;

	req = drmModeAtomicAlloc();
	if (req == NULL) {
		*saved_errno = errno;
		return -1;
	}

	if (!atomic_add_crtc_property(fd, req, crtc_id, "MODE_ID",
	    mode_blob) ||
	    !atomic_add_crtc_property(fd, req, crtc_id, "ACTIVE", 1) ||
	    !atomic_add_connector_property(fd, req, connector_id, "CRTC_ID",
	    crtc_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "FB_ID",
	    snapshot->fb_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_ID",
	    snapshot->crtc_id) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_X",
	    snapshot->crtc_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_Y",
	    snapshot->crtc_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_W",
	    snapshot->crtc_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "CRTC_H",
	    snapshot->crtc_h) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_X",
	    snapshot->src_x) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_Y",
	    snapshot->src_y) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_W",
	    snapshot->src_w) ||
	    !atomic_add_plane_property(fd, req, plane_id, "SRC_H",
	    snapshot->src_h)) {
		drmModeAtomicFree(req);
		*saved_errno = EINVAL;
		return -1;
	}

	errno = 0;
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	*saved_errno = errno;
	drmModeAtomicFree(req);
	return ret;
}

/*
 * check_atomic_modeset_disable_restore_contract()
 *
 * Ownership:
 *   Borrows the active CRTC, connector, and primary plane IDs from KMS state.
 *   The probe owns a temporary MODE_ID blob and a temporary dumb BO/FB used as
 *   the restore scanout target.  The MODE_ID blob is destroyed after restore;
 *   a successfully restored FB stays referenced by KMS until fd close.
 *
 * Lifetime:
 *   Performs one real modeset-disable commit followed by one restore commit.
 *   The display may blank briefly, but the mode and primary-plane tuple are
 *   restored before return using the probe-owned FB.  If atomic restore fails
 *   after a successful disable, a legacy SetCrtc call is attempted only as
 *   cleanup fallback.
 *
 * Threading:
 *   Single-threaded console probe.  It must run without an X/Wayland DRM
 *   master.  The kernel serializes both commits with normal modeset locks.
 */
static void
check_atomic_modeset_disable_restore_contract(int fd,
    const drmModeRes *resources, uint32_t crtc_id, uint32_t crtc_index,
    uint32_t plane_id, const char *object_name)
{
	struct atomic_plane_snapshot snapshot;
	struct atomic_plane_snapshot restore_snapshot;
	struct atomic_plane_snapshot restored_snapshot;
	struct modeset_counter_snapshot before;
	struct modeset_counter_snapshot after_disable;
	struct modeset_counter_snapshot after_disable_delay;
	struct modeset_counter_snapshot after_restore;
	drmModeCrtcPtr crtc;
	drmModeModeInfo saved_mode;
	const char *delay_env;
	uint32_t connector_id = 0;
	uint32_t mode_blob = 0;
	uint32_t restore_handle = 0;
	uint32_t restore_pitch = 0;
	uint32_t restore_fb = 0;
	uint64_t active = 0;
	uint64_t head_mask;
	int saved_errno = 0;
	int ret;
	int crtc_x;
	int crtc_y;
	bool disabled = false;
	bool restored = false;
	bool have_after_disable = false;

	head_mask = crtc_index >= 64 ? 0 : (1ULL << crtc_index);
	check(head_mask != 0, "active CRTC index fits modeset head mask");
	if (head_mask == 0)
		return;

	if (!find_active_connector_for_crtc(fd, resources, crtc_id,
	    &connector_id)) {
		check(false, "active connector is available for modeset disable probe");
		return;
	}
	check(true, "active connector is available for modeset disable probe");

	if (!get_plane_snapshot(fd, plane_id, &snapshot, object_name))
		return;
	check(snapshot.fb_id != 0,
	    "active primary plane has framebuffer for modeset disable probe");
	check(snapshot.crtc_id == crtc_id,
	    "active primary plane is attached to active CRTC for modeset disable probe");
	if (snapshot.fb_id == 0 || snapshot.crtc_id != crtc_id)
		return;

	crtc = drmModeGetCrtc(fd, crtc_id);
	check(crtc != NULL, "active CRTC is readable for modeset restore probe");
	if (crtc == NULL)
		return;
	check(crtc->mode_valid, "active CRTC has a mode for restore probe");
	if (!crtc->mode_valid) {
		drmModeFreeCrtc(crtc);
		return;
	}
	saved_mode = crtc->mode;
	crtc_x = crtc->x;
	crtc_y = crtc->y;
	drmModeFreeCrtc(crtc);

	if (!read_modeset_counter_snapshot(&before, "modeset disable probe"))
		return;

	ret = drmModeCreatePropertyBlob(fd, &saved_mode, sizeof(saved_mode),
	    &mode_blob);
	check(ret == 0, "create MODE_ID blob for modeset restore probe");
	if (ret != 0)
		return;

	if (!create_dumb_buffer_for(fd, saved_mode.hdisplay,
	    saved_mode.vdisplay, 32, &restore_handle, &restore_pitch,
	    "CREATE_DUMB succeeds for modeset restore probe"))
		goto out_destroy_blob;
	if (!clear_dumb_buffer(fd, restore_handle, restore_pitch,
	    saved_mode.vdisplay, "clear modeset restore probe framebuffer"))
		goto out_destroy_restore_bo;
	if (!add_linear_framebuffer(fd, saved_mode.hdisplay,
	    saved_mode.vdisplay, DRM_FORMAT_XRGB8888, restore_handle,
	    restore_pitch, &restore_fb,
	    "ADDFB2 accepts XRGB8888 linear modeset restore probe"))
		goto out_destroy_restore_bo;
	restore_snapshot = snapshot;
	restore_snapshot.fb_id = restore_fb;

	ret = atomic_modeset_disable_commit(fd, crtc_id, connector_id,
	    plane_id, &saved_errno);
	if (ret != 0) {
		printf("    modeset disable commit errno=%d\n", saved_errno);
		check(false, "atomic modeset disable commit succeeds");
		goto out_remove_restore_fb;
	}
	disabled = true;
	check(true, "atomic modeset disable commit succeeds");

	if (get_property_value_checked(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "ACTIVE", &active, "disabled CRTC"))
		check(active == 0, "atomic modeset disable marks CRTC inactive");
	have_after_disable = read_modeset_counter_snapshot(&after_disable,
	    "modeset restore probe");
	if (have_after_disable) {
		check(after_disable.atomic_tail_disable_op_count >
		    before.atomic_tail_disable_op_count,
		    "atomic modeset disable increments tail disable op count");
		check(after_disable.atomic_tail_last_disable_op_count > 0,
		    "atomic modeset disable records last disable op count");
		check((after_disable.atomic_tail_last_disable_heads &
		    head_mask) != 0,
		    "atomic modeset disable records disabled head");
		check((after_disable.atomic_tail_last_new_active_heads &
		    head_mask) == 0,
		    "atomic modeset disable clears active head bit");
		check(after_disable.atomic_disable_vblank_off_count >
		    before.atomic_disable_vblank_off_count,
		    "atomic modeset disable turns vblank off");
		check(after_disable.atomic_disable_vblank_keep_count ==
		    before.atomic_disable_vblank_keep_count,
		    "atomic modeset disable does not keep vblank active");
		check(after_disable.commit_error_count ==
		    before.commit_error_count,
		    "atomic modeset disable does not increment commit_error_count");
		check(after_disable.atomic_tail_active == 0 &&
		    after_disable.atomic_tail_stage == 0,
		    "atomic modeset disable leaves no active tail transaction");
		check(after_disable.display_audit_pending_valid == 0,
		    "atomic modeset disable leaves no pending display audit");
	}

	delay_env = getenv("NVKM_DRMTEST_MODESET_DELAY_MS");
	if (delay_env != NULL && delay_env[0] != '\0') {
		long delay_ms;

		delay_ms = strtol(delay_env, NULL, 10);
		if (delay_ms > 0) {
			printf("    delaying modeset restore by %ld ms\n",
			    delay_ms);
			usleep((useconds_t)delay_ms * 1000);
			(void)read_modeset_counter_snapshot(
			    &after_disable_delay,
			    "modeset disable delayed probe");
		}
	}

	ret = atomic_modeset_restore_commit(fd, crtc_id, connector_id,
	    plane_id, &restore_snapshot, mode_blob, &saved_errno);
	if (ret != 0) {
		printf("    modeset restore commit errno=%d\n", saved_errno);
		check(false, "atomic modeset restore commit succeeds");
	} else {
		restored = true;
		check(true, "atomic modeset restore commit succeeds");
	}

	if (!restored && disabled) {
		printf("    attempting legacy SetCrtc cleanup restore\n");
		ret = drmModeSetCrtc(fd, crtc_id, restore_fb,
		    crtc_x, crtc_y, &connector_id, 1, &saved_mode);
		check(ret == 0, "legacy SetCrtc cleanup restore succeeds");
		restored = ret == 0;
	}

	if (restored) {
		active = 0;
		if (get_property_value_checked(fd, crtc_id,
		    DRM_MODE_OBJECT_CRTC, "ACTIVE", &active,
		    "restored CRTC"))
			check(active != 0,
			    "atomic modeset restore marks CRTC active");
		if (get_plane_snapshot(fd, plane_id, &restored_snapshot,
		    object_name)) {
			check(restored_snapshot.fb_id == restore_fb,
			    "atomic modeset restore restores primary FB_ID");
			check(restored_snapshot.crtc_id == snapshot.crtc_id,
			    "atomic modeset restore restores primary CRTC_ID");
		}
		if (read_modeset_counter_snapshot(&after_restore,
		    "modeset restore completion")) {
			if (have_after_disable) {
				check(after_restore.atomic_tail_enable_op_count >
				    after_disable.atomic_tail_enable_op_count,
				    "atomic modeset restore increments tail enable op count");
			}
			check(after_restore.atomic_tail_last_enable_op_count > 0,
			    "atomic modeset restore records last enable op count");
			check((after_restore.atomic_tail_last_enable_heads &
			    head_mask) != 0,
			    "atomic modeset restore records enabled head");
			check(after_restore.commit_error_count ==
			    before.commit_error_count,
			    "atomic modeset restore does not increment commit_error_count");
			check(after_restore.atomic_tail_active == 0 &&
			    after_restore.atomic_tail_stage == 0,
			    "atomic modeset restore leaves no active tail transaction");
			check(after_restore.display_audit_pending_valid == 0,
			    "atomic modeset restore leaves no pending display audit");
			check(strcmp(after_restore.display_audit_current_op,
			    "atomic_enable") == 0,
			    "atomic modeset restore publishes atomic_enable audit");
		}
	}

	if (restored) {
		printf("    restore probe fb=%u handle=%u kept until fd close\n",
		    restore_fb, restore_handle);
		goto out_destroy_blob;
	}

out_remove_restore_fb:
	remove_framebuffer(fd, restore_fb,
	    "RMFB succeeds for failed modeset restore probe");
out_destroy_restore_bo:
	destroy_dumb_buffer_for(fd, restore_handle,
	    "DESTROY_DUMB succeeds for failed modeset restore probe");
out_destroy_blob:
	check(drmModeDestroyPropertyBlob(fd, mode_blob) == 0,
	    "destroy MODE_ID blob for modeset restore probe");
}

/*
 * check_atomic_primary_panning_contract()
 *
 * Ownership:
 *   Owns the temporary dumb BO handle and framebuffer ID.  The kernel borrows
 *   the FB ID only for each TEST_ONLY atomic request.
 *
 * Lifetime:
 *   No KMS state is committed.  The positive request proves integer source
 *   panning is accepted on the selected primary plane; the negative requests
 *   isolate the no-fractional-source and full-CRTC-destination gates.
 *
 * Threading:
 *   Single-threaded userspace probe.  The driver evaluates atomic_check under
 *   normal modeset locks, and TEST_ONLY must not program display hardware.
 */
static void
check_atomic_primary_panning_contract(int fd, uint32_t plane_id,
    uint32_t crtc_id, uint32_t crtc_width, uint32_t crtc_height)
{
	uint32_t handle = 0;
	uint32_t pitch = 0;
	uint32_t fb_id = 0;
	uint32_t fb_width;
	int saved_errno;
	int ret;

	fb_width = crtc_width + 64;
	if (!create_dumb_buffer_for(fd, fb_width, crtc_height, 32, &handle,
	    &pitch,
	    "CREATE_DUMB succeeds for primary panning TEST_ONLY probe"))
		goto out;
	if (!add_linear_framebuffer(fd, fb_width, crtc_height,
	    DRM_FORMAT_XRGB8888, handle, pitch, &fb_id,
	    "ADDFB2 accepts XRGB8888 linear primary panning probe"))
		goto out;

	ret = atomic_primary_test_only_commit(fd, plane_id, crtc_id, fb_id,
	    0, 0, crtc_width, crtc_height, (uint64_t)64 << 16, 0,
	    (uint64_t)crtc_width << 16, (uint64_t)crtc_height << 16,
	    &saved_errno);
	check(ret == 0,
	    "atomic TEST_ONLY accepts integer primary source panning");

	ret = atomic_primary_test_only_commit(fd, plane_id, crtc_id, fb_id,
	    0, 0, crtc_width, crtc_height, ((uint64_t)64 << 16) | 1, 0,
	    (uint64_t)crtc_width << 16, (uint64_t)crtc_height << 16,
	    &saved_errno);
	if (ret == 0) {
		check(false, "atomic TEST_ONLY rejects fractional primary source");
	} else {
		(void)saved_errno;
		check(true, "atomic TEST_ONLY rejects fractional primary source");
	}

	ret = atomic_primary_test_only_commit(fd, plane_id, crtc_id, fb_id,
	    1, 0, crtc_width, crtc_height, (uint64_t)64 << 16, 0,
	    (uint64_t)crtc_width << 16, (uint64_t)crtc_height << 16,
	    &saved_errno);
	if (ret == 0) {
		check(false,
		    "atomic TEST_ONLY rejects shifted primary destination");
	} else {
		check(saved_errno == EINVAL,
		    "atomic TEST_ONLY rejects shifted primary destination with EINVAL");
	}

out:
	remove_framebuffer(fd, fb_id,
	    "RMFB succeeds for primary panning TEST_ONLY probe");
	destroy_dumb_buffer_for(fd, handle,
	    "DESTROY_DUMB succeeds for primary panning TEST_ONLY probe");
}

/*
 * check_atomic_cursor_test_only_contract()
 *
 * Ownership:
 *   Owns the temporary dumb BO handles and framebuffer IDs it creates, and
 *   releases both before returning.  The kernel only borrows the FB IDs for
 *   the duration of each TEST_ONLY atomic request.
 *
 * Lifetime:
 *   No KMS state is committed.  The positive ARGB8888 request proves the
 *   selected cursor plane/CRTC/property tuple is otherwise valid; the RGB565
 *   request then isolates the plane format/modifier rejection path.
 *
 * Threading:
 *   Single-threaded userspace probe.  The driver runs atomic_check under its
 *   normal modeset locks, and TEST_ONLY must not program display hardware.
 */
static void
check_atomic_cursor_test_only_contract(int fd, uint32_t plane_id,
    uint32_t crtc_id)
{
	uint32_t argb_handle = 0;
	uint32_t rgb565_handle = 0;
	uint32_t argb_pitch = 0;
	uint32_t rgb565_pitch = 0;
	uint32_t argb_fb = 0;
	uint32_t rgb565_fb = 0;
	int saved_errno;
	int ret;

	if (!create_dumb_buffer_for(fd, 64, 64, 32, &argb_handle,
	    &argb_pitch,
	    "CREATE_DUMB succeeds for cursor atomic positive probe"))
		goto out;
	if (!add_linear_framebuffer(fd, 64, 64, DRM_FORMAT_ARGB8888,
	    argb_handle, argb_pitch, &argb_fb,
	    "ADDFB2 accepts ARGB8888 linear cursor probe"))
		goto out;
	ret = atomic_cursor_test_only_commit(fd, plane_id, crtc_id, argb_fb,
	    64, &saved_errno);
	check(ret == 0, "atomic TEST_ONLY accepts ARGB8888 linear cursor");

	if (!create_dumb_buffer_for(fd, 64, 64, 16, &rgb565_handle,
	    &rgb565_pitch,
	    "CREATE_DUMB succeeds for cursor atomic negative probe"))
		goto out;
	if (!add_linear_framebuffer(fd, 64, 64, DRM_FORMAT_RGB565,
	    rgb565_handle, rgb565_pitch, &rgb565_fb,
	    "ADDFB2 accepts RGB565 linear atomic negative probe"))
		goto out;
	ret = atomic_cursor_test_only_commit(fd, plane_id, crtc_id, rgb565_fb,
	    64, &saved_errno);
	if (ret == 0) {
		check(false, "atomic TEST_ONLY rejects RGB565 linear cursor");
	} else {
		check(saved_errno == EINVAL,
		    "atomic TEST_ONLY rejects RGB565 linear cursor with EINVAL");
	}

out:
	remove_framebuffer(fd, rgb565_fb,
	    "RMFB succeeds for cursor atomic negative probe");
	remove_framebuffer(fd, argb_fb,
	    "RMFB succeeds for cursor atomic positive probe");
	destroy_dumb_buffer_for(fd, rgb565_handle,
	    "DESTROY_DUMB succeeds for cursor atomic negative probe");
	destroy_dumb_buffer_for(fd, argb_handle,
	    "DESTROY_DUMB succeeds for cursor atomic positive probe");
}

/*
 * check_legacy_cursor_runtime_contract()
 *
 * Ownership:
 *   Owns one temporary dumb cursor BO while the legacy cursor ioctl path
 *   borrows its handle.  The probe hides the cursor before destroying the BO,
 *   and it never publishes a framebuffer ID to userspace.
 *
 * Lifetime:
 *   Installs a transparent 64x64 ARGB cursor, moves it through the legacy
 *   MOVE ioctl, and hides it again before returning.  The enabled interval is
 *   intentionally short and visually transparent; KMS state must be restored
 *   even when a later check fails.
 *
 * Threading:
 *   Single-threaded userspace probe.  The kernel serializes legacy cursor
 *   update/disable through normal modeset locks; the MOVE operation must use
 *   the driver's atomic_async_update hook rather than a primary plane update.
 */
static void
check_legacy_cursor_runtime_contract(int fd, uint32_t crtc_id,
    uint32_t crtc_index)
{
	struct cursor_counter_snapshot before;
	struct cursor_counter_snapshot after_enable;
	struct cursor_counter_snapshot after_move;
	struct cursor_counter_snapshot after_hide;
	uint64_t pin_delta;
	uint64_t unpin_delta;
	uint32_t handle = 0;
	uint32_t pitch = 0;
	bool cursor_visible = false;
	bool have_after_enable = false;
	bool have_after_move = false;
	int saved_errno;
	int ret;

	if (!read_cursor_counter_snapshot(&before, "legacy cursor probe",
	    crtc_index))
		return;

	if (!create_dumb_buffer_for(fd, 64, 64, 32, &handle, &pitch,
	    "CREATE_DUMB succeeds for legacy cursor runtime probe"))
		goto out;
	if (!clear_dumb_buffer(fd, handle, pitch, 64,
	    "MAP_DUMB succeeds for transparent cursor probe"))
		goto out;

	errno = 0;
	ret = drmModeSetCursor2(fd, crtc_id, handle, 64, 64, 0, 0);
	saved_errno = errno;
	if (ret != 0) {
		printf("    drmModeSetCursor2 errno=%d\n", saved_errno);
		check(false, "legacy cursor SetCursor2 enables cursor image");
		goto out;
	}
	cursor_visible = true;
	check(true, "legacy cursor SetCursor2 enables cursor image");

	if (read_cursor_counter_snapshot(&after_enable,
	    "legacy cursor enable", crtc_index)) {
		have_after_enable = true;
		check(after_enable.cursor_update_count >
		    before.cursor_update_count,
		    "legacy cursor SetCursor2 increments cursor_update_count");
		check(after_enable.cursor_error_count ==
		    before.cursor_error_count,
		    "legacy cursor SetCursor2 does not increment cursor_error_count");
		check(after_enable.head_cursor_enabled == 1,
		    "legacy cursor SetCursor2 marks head cursor enabled");
		check(after_enable.head_cursor_fb != 0,
		    "legacy cursor SetCursor2 publishes cursor framebuffer");
		check(after_enable.head_cursor_bo != 0,
		    "legacy cursor SetCursor2 publishes cursor BO");
	}

	errno = 0;
	ret = drmModeMoveCursor(fd, crtc_id, 8, 8);
	saved_errno = errno;
	if (ret != 0) {
		printf("    drmModeMoveCursor errno=%d\n", saved_errno);
		check(false, "legacy cursor MOVE succeeds");
		goto hide;
	}
	check(true, "legacy cursor MOVE succeeds");

	if (read_cursor_counter_snapshot(&after_move, "legacy cursor move",
	    crtc_index)) {
		have_after_move = true;
		check(after_move.cursor_async_update_count > (have_after_enable ?
		    after_enable.cursor_async_update_count :
		    before.cursor_async_update_count),
		    "legacy cursor MOVE increments cursor_async_update_count");
		check(!have_after_enable || after_move.cursor_update_count ==
		    after_enable.cursor_update_count,
		    "legacy cursor MOVE does not reprogram cursor image");
		check(!have_after_enable || after_move.cursor_disable_count ==
		    after_enable.cursor_disable_count,
		    "legacy cursor MOVE does not disable cursor");
		check(after_move.cursor_error_count == before.cursor_error_count,
		    "legacy cursor MOVE does not increment cursor_error_count");
		check(!have_after_enable || after_move.plane_update_count ==
		    after_enable.plane_update_count,
		    "legacy cursor MOVE does not update primary plane");
		check(after_move.atomic_last_legacy_cursor_update == 1,
		    "legacy cursor MOVE sets legacy cursor atomic summary bit");
		check(after_move.atomic_last_async_update == 1,
		    "legacy cursor MOVE sets async atomic summary bit");
		check(after_move.head_cursor_enabled == 1,
		    "legacy cursor MOVE keeps head cursor enabled");
		check(!have_after_enable || after_move.head_cursor_fb ==
		    after_enable.head_cursor_fb,
		    "legacy cursor MOVE keeps cursor framebuffer");
		check(!have_after_enable || after_move.head_cursor_bo ==
		    after_enable.head_cursor_bo,
		    "legacy cursor MOVE keeps cursor BO");
	}

hide:
	errno = 0;
	ret = drmModeSetCursor2(fd, crtc_id, 0, 0, 0, 0, 0);
	saved_errno = errno;
	if (ret != 0) {
		printf("    hide drmModeSetCursor2 errno=%d\n", saved_errno);
		check(false, "legacy cursor SetCursor2 hides cursor image");
		goto out;
	}
	cursor_visible = false;
	check(true, "legacy cursor SetCursor2 hides cursor image");

	if (read_cursor_counter_snapshot(&after_hide, "legacy cursor hide",
	    crtc_index)) {
		check(after_hide.cursor_disable_count >
		    before.cursor_disable_count,
		    "legacy cursor hide increments cursor_disable_count");
		check(after_hide.cursor_error_count == before.cursor_error_count,
		    "legacy cursor hide does not increment cursor_error_count");
		check(after_hide.cursor_pin_count >= before.cursor_pin_count,
		    "legacy cursor pin counter is monotonic");
		check(after_hide.cursor_unpin_count >= before.cursor_unpin_count,
		    "legacy cursor unpin counter is monotonic");
		pin_delta = after_hide.cursor_pin_count >=
		    before.cursor_pin_count ?
		    after_hide.cursor_pin_count - before.cursor_pin_count : 0;
		unpin_delta = after_hide.cursor_unpin_count >=
		    before.cursor_unpin_count ?
		    after_hide.cursor_unpin_count - before.cursor_unpin_count :
		    0;
		printf("    legacy cursor pin_delta=%llu unpin_delta=%llu\n",
		    (unsigned long long)pin_delta,
		    (unsigned long long)unpin_delta);
		check(pin_delta == unpin_delta,
		    "legacy cursor probe balances cursor pin/unpin");
		check(!have_after_move ||
		    after_hide.cursor_async_update_count >=
		    after_move.cursor_async_update_count,
		    "legacy cursor async counter remains monotonic after hide");
		check(after_hide.head_cursor_enabled == 0,
		    "legacy cursor hide marks head cursor disabled");
		check(after_hide.head_cursor_fb == 0,
		    "legacy cursor hide clears cursor framebuffer");
		check(after_hide.head_cursor_bo == 0,
		    "legacy cursor hide clears cursor BO");
	}

out:
	if (cursor_visible) {
		errno = 0;
		ret = drmModeSetCursor2(fd, crtc_id, 0, 0, 0, 0, 0);
		saved_errno = errno;
		if (ret != 0)
			printf("    cleanup hide cursor errno=%d\n",
			    saved_errno);
		check(ret == 0, "legacy cursor cleanup hide succeeds");
	}
	destroy_dumb_buffer_for(fd, handle,
	    "DESTROY_DUMB succeeds for legacy cursor runtime probe");
}

static void
check_addfb2_rejects_rgb565_nvidia(int fd,
    const struct drm_format_modifier *modifiers, uint32_t count_modifiers)
{
	uint32_t handles[4] = { 0 };
	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint64_t modifier[4] = { 0 };
	uint32_t fb_id = 0;
	uint64_t nvidia_modifier = 0;
	int saved_errno;
	int ret;

	check(find_first_nvidia_modifier(modifiers, count_modifiers,
	    &nvidia_modifier), "primary IN_FORMATS has NVIDIA modifier for ADDFB2 negative probe");
	if (nvidia_modifier == 0)
		return;

	if (!create_dumb_buffer(fd, 64, 64, 16, &handles[0], &pitches[0]))
		return;
	modifier[0] = nvidia_modifier;

	errno = 0;
	ret = drmModeAddFB2WithModifiers(fd, 64, 64, DRM_FORMAT_RGB565,
	    handles, pitches, offsets, modifier, &fb_id,
	    DRM_MODE_FB_MODIFIERS);
	saved_errno = errno;
	if (ret == 0) {
		check(false, "ADDFB2 rejects RGB565 NVIDIA blocklinear");
		(void)drmModeRmFB(fd, fb_id);
	} else {
		check(saved_errno == EINVAL,
		    "ADDFB2 rejects RGB565 NVIDIA blocklinear with EINVAL");
	}

	destroy_dumb_buffer(fd, handles[0]);
}

static bool
check_in_formats_blob_shape(const char *name, drmModePlanePtr plane,
    const struct drm_format_modifier_blob *blob, uint32_t length,
    const uint32_t **formats_out,
    const struct drm_format_modifier **modifiers_out)
{
	const uint32_t *formats;
	const struct drm_format_modifier *modifiers;
	bool ok;

	ok = blob->version == FORMAT_BLOB_CURRENT;
	check(ok, "IN_FORMATS blob version is current");
	if (!ok)
		return false;

	ok = blob->count_formats > 0 && blob->count_modifiers > 0;
	check(ok, "IN_FORMATS blob is non-empty");
	if (!ok)
		return false;

	ok = range_valid(blob->formats_offset, blob->count_formats,
	    sizeof(uint32_t), length) &&
	    range_valid(blob->modifiers_offset, blob->count_modifiers,
	    sizeof(struct drm_format_modifier), length);
	check(ok, "IN_FORMATS blob ranges are valid");
	if (!ok)
		return false;

	formats = (const uint32_t *)((const char *)blob + blob->formats_offset);
	modifiers = (const struct drm_format_modifier *)
	    ((const char *)blob + blob->modifiers_offset);

	printf("%s IN_FORMATS formats=%u modifiers=%u\n", name,
	    blob->count_formats, blob->count_modifiers);
	for (uint32_t i = 0; i < blob->count_formats; i++) {
		printf("    format[%u]=0x%08x %s\n", i, formats[i],
		    fourcc_name(formats[i]));
	}
	for (uint32_t i = 0; i < blob->count_modifiers; i++) {
		printf("    modifier[%u]=0x%016llx %s offset=%u mask=0x%016llx\n",
		    i, (unsigned long long)modifiers[i].modifier,
		    modifier_name(modifiers[i].modifier), modifiers[i].offset,
		    (unsigned long long)modifiers[i].formats);
	}

	ok = blob->count_formats == plane->count_formats;
	check(ok, "IN_FORMATS format count matches GETPLANE");
	if (!ok)
		return false;
	for (uint32_t i = 0; i < plane->count_formats; i++) {
		ok = formats[i] == plane->formats[i];
		check(ok, "IN_FORMATS format order matches GETPLANE");
		if (!ok)
			return false;
	}

	for (uint32_t i = 0; i < blob->count_modifiers; i++) {
		bool has_any = false;

		for (uint32_t j = 0; j < blob->count_formats; j++) {
			if (modifier_has_format(&modifiers[i], j)) {
				has_any = true;
				break;
			}
		}
		check(has_any, "IN_FORMATS modifier references a format");
		if (!has_any)
			return false;
	}

	*formats_out = formats;
	*modifiers_out = modifiers;
	return true;
}

static void
check_in_formats(int fd, drmModePlanePtr plane, int plane_type,
    const char *name)
{
	const struct drm_format_modifier_blob *blob;
	const struct drm_format_modifier *modifiers;
	const uint32_t *formats;
	drmModePropertyBlobPtr property_blob;
	drmModePropertyPtr prop;
	uint64_t blob_id = 0;
	bool ok;

	prop = get_property_by_name(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE,
	    "IN_FORMATS", &blob_id);
	check(prop != NULL, "plane has IN_FORMATS property");
	if (prop == NULL)
		return;
	check((prop->flags & DRM_MODE_PROP_BLOB) != 0,
	    "IN_FORMATS property is a blob");
	drmModeFreeProperty(prop);

	check(blob_id != 0, "IN_FORMATS blob id is non-zero");
	if (blob_id == 0)
		return;

	property_blob = drmModeGetPropertyBlob(fd, (uint32_t)blob_id);
	check(property_blob != NULL, "IN_FORMATS blob is readable");
	if (property_blob == NULL)
		return;

	ok = property_blob->length >= sizeof(*blob);
	check(ok, "IN_FORMATS blob header is present");
	if (!ok) {
		drmModeFreePropertyBlob(property_blob);
		return;
	}

	blob = (const struct drm_format_modifier_blob *)property_blob->data;
	if (!check_in_formats_blob_shape(name, plane, blob,
	    property_blob->length, &formats, &modifiers)) {
		drmModeFreePropertyBlob(property_blob);
		return;
	}

	if (plane_type == DRM_PLANE_TYPE_PRIMARY) {
		check(format_has_linear_modifier(formats, modifiers,
		    blob->count_formats, blob->count_modifiers,
		    DRM_FORMAT_XRGB8888),
		    "primary IN_FORMATS has XRGB8888 linear");
		check(format_has_linear_modifier(formats, modifiers,
		    blob->count_formats, blob->count_modifiers,
		    DRM_FORMAT_ARGB8888),
		    "primary IN_FORMATS has ARGB8888 linear");
		check(format_has_linear_modifier(formats, modifiers,
		    blob->count_formats, blob->count_modifiers,
		    DRM_FORMAT_RGB565),
		    "primary IN_FORMATS has RGB565 linear");
		check(format_has_nvidia_modifier(formats, modifiers,
		    blob->count_formats, blob->count_modifiers,
		    DRM_FORMAT_XRGB8888),
		    "primary IN_FORMATS has XRGB8888 NVIDIA blocklinear");
		check(format_has_nvidia_modifier(formats, modifiers,
		    blob->count_formats, blob->count_modifiers,
		    DRM_FORMAT_ARGB8888),
		    "primary IN_FORMATS has ARGB8888 NVIDIA blocklinear");
		check(!format_has_nvidia_modifier(formats, modifiers,
		    blob->count_formats, blob->count_modifiers,
		    DRM_FORMAT_RGB565),
		    "primary IN_FORMATS excludes RGB565 NVIDIA blocklinear");
		check_addfb2_rejects_rgb565_nvidia(fd, modifiers,
		    blob->count_modifiers);
	} else if (plane_type == DRM_PLANE_TYPE_CURSOR) {
		check(plane->count_formats == 1 &&
		    plane->formats[0] == DRM_FORMAT_ARGB8888,
		    "cursor GETPLANE exposes only ARGB8888");
		check(format_has_linear_modifier(formats, modifiers,
		    blob->count_formats, blob->count_modifiers,
		    DRM_FORMAT_ARGB8888),
		    "cursor IN_FORMATS has ARGB8888 linear");
		check(!format_has_nonlinear_modifier(formats, modifiers,
		    blob->count_formats, blob->count_modifiers,
		    DRM_FORMAT_ARGB8888),
		    "cursor IN_FORMATS excludes non-linear ARGB8888");
	}

	drmModeFreePropertyBlob(property_blob);
}

/*
 * check_plane_route_state_contract()
 *
 * Ownership:
 *   Borrows the plane and mode resources snapshots.  The helper reads plane
 *   properties through libdrm and does not retain references after return.
 *
 * Lifetime:
 *   Valid only for the current plane/resources snapshot.  A later atomic commit
 *   may legitimately change CRTC_ID, FB_ID, or possible_crtcs.
 *
 * Threading:
 *   Single-threaded read-only KMS UAPI validation.  It never submits modeset
 *   work or takes driver-private locks.
 */
static void
check_plane_route_state_contract(int fd, const drmModePlane *plane,
    const drmModeRes *mode_resources, const char *name)
{
	uint64_t crtc_id = 0;
	uint64_t fb_id = 0;
	int crtc_index = -1;
	bool crtc_present;

	if (!get_property_value_checked(fd, plane->plane_id,
	    DRM_MODE_OBJECT_PLANE, "CRTC_ID", &crtc_id, name))
		return;
	if (!get_property_value_checked(fd, plane->plane_id,
	    DRM_MODE_OBJECT_PLANE, "FB_ID", &fb_id, name))
		return;

	check((crtc_id == 0) == (fb_id == 0),
	    "plane FB_ID and CRTC_ID enable state match");
	if (crtc_id == 0)
		return;

	crtc_present = id_index_in_list(mode_resources->crtcs,
	    mode_resources->count_crtcs, (uint32_t)crtc_id, &crtc_index);
	check(crtc_present, "plane current CRTC is present");
	if (!crtc_present)
		return;

	check(crtc_index >= 0 && crtc_index < 32,
	    "plane current CRTC index fits possible_crtcs mask width");
	if (crtc_index >= 0 && crtc_index < 32) {
		check((plane->possible_crtcs & (1u << crtc_index)) != 0,
		    "plane current CRTC is allowed by possible_crtcs");
	}
}

static void
check_planes(int fd, const drmModeRes *mode_resources)
{
	drmModePlaneResPtr resources;
	uint32_t active_crtc_id = 0;
	uint32_t active_crtc_index = 0;
	uint32_t active_crtc_width = 0;
	uint32_t active_crtc_height = 0;
	bool primary_for_crtc[32] = { false };
	bool cursor_for_crtc[32] = { false };
	bool primary_panning_probe_done = false;
	bool cursor_probe_done = false;
	bool track_plane_topology;
	bool have_active_crtc;
	bool have_active_crtc_size;
	bool skip_cursor;
	bool metadata_only;

	resources = drmModeGetPlaneResources(fd);
	check(resources != NULL, "plane resources available");
	if (resources == NULL)
		return;
	skip_cursor = getenv("NVKM_DRMTEST_SKIP_CURSOR") != NULL;
	metadata_only = getenv("NVKM_DRMTEST_METADATA_ONLY") != NULL;
	track_plane_topology = mode_resources->count_crtcs > 0 &&
	    mode_resources->count_crtcs <= 32;
	check(track_plane_topology,
	    "CRTC count is valid for plane topology masks");

	if (metadata_only) {
		have_active_crtc = false;
		have_active_crtc_size = false;
		printf("SKIP active CRTC runtime probes by NVKM_DRMTEST_METADATA_ONLY\n");
	} else {
		have_active_crtc = find_active_crtc(fd, mode_resources,
		    &active_crtc_id, &active_crtc_index);
		check(have_active_crtc,
		    "active CRTC available for atomic TEST_ONLY probe");
		have_active_crtc_size = have_active_crtc &&
		    get_crtc_size(fd, active_crtc_id, &active_crtc_width,
		    &active_crtc_height);
		check(have_active_crtc_size,
		    "active CRTC mode size available for primary panning probe");
		if (have_active_crtc) {
			check_vblank_sequence_runtime_contract(fd,
			    active_crtc_id, active_crtc_index);
			check_atomic_crtc_color_runtime_contract(fd,
			    active_crtc_id);
		}
	}

	printf("planes: count=%u\n", resources->count_planes);
	check(resources->count_planes > 0, "at least one KMS plane exposed");
	if (resources->count_planes <= INT_MAX) {
		check_unique_ids(resources->planes, (int)resources->count_planes,
		    "plane resource ids are unique");
	} else {
		check(false, "plane resource ids are unique");
	}
	for (uint32_t i = 0; i < resources->count_planes; i++) {
		drmModePlanePtr plane;
		char name[64];
		int plane_type;

		plane = drmModeGetPlane(fd, resources->planes[i]);
		if (plane == NULL) {
			printf("plane %u unavailable errno=%d\n",
			    resources->planes[i], errno);
			failures++;
			continue;
		}
		snprintf(name, sizeof(name), "plane %u", plane->plane_id);
		printf("%s possible_crtcs=0x%x formats=%u\n", name,
		    plane->possible_crtcs, plane->count_formats);
		dump_properties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE,
		    name);
		require_property(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE,
		    "type", name);
		check_plane_sync_property_contract(fd, plane->plane_id, name);
		plane_type = get_plane_type(fd, plane->plane_id);
		check(plane_type >= 0, "plane type is readable");
		check_in_formats(fd, plane, plane_type, name);
		check_plane_route_state_contract(fd, plane, mode_resources,
		    name);
		if (track_plane_topology) {
			for (int c = 0; c < mode_resources->count_crtcs; c++) {
				if ((plane->possible_crtcs & (1u << c)) == 0)
					continue;
				if (plane_type == DRM_PLANE_TYPE_PRIMARY)
					primary_for_crtc[c] = true;
				else if (plane_type == DRM_PLANE_TYPE_CURSOR)
					cursor_for_crtc[c] = true;
			}
		}
		if (have_active_crtc_size && !primary_panning_probe_done &&
		    plane_type == DRM_PLANE_TYPE_PRIMARY &&
		    (plane->possible_crtcs & (1u << active_crtc_index)) != 0) {
			check_atomic_primary_panning_contract(fd,
			    plane->plane_id, active_crtc_id,
			    active_crtc_width, active_crtc_height);
			check_atomic_out_fence_runtime_contract(fd,
			    active_crtc_id, plane->plane_id, name);
			check_atomic_in_fence_runtime_contract(fd,
			    active_crtc_id, plane->plane_id, name);
			check_legacy_pageflip_runtime_contract(fd,
			    active_crtc_id, plane->plane_id,
			    active_crtc_width, active_crtc_height, name);
			check_non_master_display_mutation_contract(fd,
			    mode_resources, active_crtc_id, plane->plane_id,
			    name);
			check_atomic_connector_scaler_runtime_contract(fd,
			    mode_resources, active_crtc_id);
			check_legacy_dpms_runtime_contract(fd,
			    mode_resources, active_crtc_id,
			    active_crtc_index);
			check_legacy_setcrtc_runtime_contract(fd,
			    mode_resources, active_crtc_id,
			    active_crtc_index, plane->plane_id, name);
			check_atomic_modeset_disable_restore_contract(fd,
			    mode_resources, active_crtc_id,
			    active_crtc_index, plane->plane_id, name);
			primary_panning_probe_done = true;
		}
		if (!skip_cursor && have_active_crtc && !cursor_probe_done &&
		    plane_type == DRM_PLANE_TYPE_CURSOR &&
		    (plane->possible_crtcs & (1u << active_crtc_index)) != 0) {
			check_atomic_cursor_test_only_contract(fd,
			    plane->plane_id, active_crtc_id);
			check_legacy_cursor_runtime_contract(fd,
			    active_crtc_id, active_crtc_index);
			cursor_probe_done = true;
		}
		drmModeFreePlane(plane);
	}

	if (track_plane_topology) {
		bool all_primary = true;
		bool all_cursor = true;

		for (int c = 0; c < mode_resources->count_crtcs; c++) {
			all_primary = all_primary && primary_for_crtc[c];
			all_cursor = all_cursor && cursor_for_crtc[c];
		}
		check(all_primary, "every CRTC has a primary plane");
		check(all_cursor, "every CRTC has a cursor plane");
	}

	if (metadata_only) {
		printf("SKIP primary/cursor runtime probes by NVKM_DRMTEST_METADATA_ONLY\n");
	} else {
		check(primary_panning_probe_done,
		    "primary plane supports active CRTC for panning TEST_ONLY probe");
	}
	if (metadata_only) {
		/* Cursor runtime needs an active CRTC; metadata was checked above. */
	} else if (skip_cursor)
		printf("SKIP cursor plane runtime probe by NVKM_DRMTEST_SKIP_CURSOR\n");
	else
		check(cursor_probe_done,
		    "cursor plane supports active CRTC for atomic TEST_ONLY probe");

	drmModeFreePlaneResources(resources);
}

static int
run_syncobj_transfer_only(void)
{
	const char *path;
	int fd;

	path = getenv("NVKM_DRMTEST_SYNC_NODE");
	if (path == NULL || path[0] == '\0')
		path = "/dev/dri/renderD128";

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("open %s failed errno=%d\n", path, errno);
		return 1;
	}

	printf("sync-only node=%s\n", path);
	check_syncobj_transfer_contract(fd);
	check(close(fd) == 0, "close succeeds for sync-only DRM fd");
	return failures == 0 ? 0 : 1;
}

int
main(void)
{
	drmModeRes *resources;
	bool deprecated_mode_noop_done = false;
	bool expect_no_connected;
	uint32_t connected_connector_id = 0;
	int connected_count = 0;
	int fd;

	if (getenv("NVKM_DRMTEST_SYNC_ONLY") != NULL)
		return run_syncobj_transfer_only();

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open card0");
		return 1;
	}
	check_client_cap_value_error(fd, DRM_CLIENT_CAP_STEREO_3D, 2,
	    EINVAL, "STEREO_3D");
	check_legacy_plane_visibility_without_universal_cap(fd);
	check_client_cap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES,
	    "UNIVERSAL_PLANES");
	check_client_cap_value_error(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 2,
	    EINVAL, "UNIVERSAL_PLANES");
	check_client_cap(fd, DRM_CLIENT_CAP_ASPECT_RATIO, "ASPECT_RATIO");
	check_client_cap_value_error(fd, DRM_CLIENT_CAP_ASPECT_RATIO, 2,
	    EINVAL, "ASPECT_RATIO");
	check_atomic_properties_hidden_without_client_cap(fd);
	check_client_cap_error(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS,
	    EINVAL, "WRITEBACK_CONNECTORS before ATOMIC");
	check_client_cap_value_error(fd, DRM_CLIENT_CAP_ATOMIC, 2,
	    EINVAL, "ATOMIC");
	check_client_cap(fd, DRM_CLIENT_CAP_ATOMIC, "ATOMIC");
	check_atomic_ioctl_flag_contract(fd);
	check_cursor_ioctl_flag_contract(fd);
	check_wait_vblank_flag_contract(fd);
	check_pageflip_ioctl_flag_contract(fd);
	check_property_read_error_contract(fd);
	check_property_set_error_contract(fd);
	check_resource_lookup_error_contract(fd);
	check_property_blob_lifetime_contract(fd);
	check_dumb_buffer_lifetime_contract(fd);
	check_client_cap_value_error(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS,
	    2, EINVAL, "WRITEBACK_CONNECTORS");
	check_client_cap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS,
	    "WRITEBACK_CONNECTORS");
	check_client_cap_error(fd, DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT,
	    EOPNOTSUPP, "CURSOR_PLANE_HOTSPOT");
	check_syncobj_transfer_contract(fd);

	resources = drmModeGetResources(fd);
	if (resources == NULL) {
		printf("drmModeGetResources failed errno=%d\n", errno);
		close(fd);
		return 1;
	}
	expect_no_connected = getenv("NVKM_DRMTEST_EXPECT_NO_CONNECTED") != NULL;

	printf("resources: connectors=%d crtcs=%d encoders=%d\n",
	    resources->count_connectors, resources->count_crtcs,
	    resources->count_encoders);
	check(resources->count_connectors > 0, "at least one connector exposed");
	check(resources->count_crtcs > 0, "at least one CRTC exposed");
	check(resources->count_encoders > 0, "at least one encoder exposed");
	check_unique_ids(resources->connectors, resources->count_connectors,
	    "connector resource ids are unique");
	check_unique_ids(resources->crtcs, resources->count_crtcs,
	    "CRTC resource ids are unique");
	check_unique_ids(resources->encoders, resources->count_encoders,
	    "encoder resource ids are unique");
	check_mode_config_contract(fd, resources);
	check_framebuffer_uapi_contract(fd);
	check_encoders(fd, resources);

	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector == NULL) {
			printf("connector %u unavailable errno=%d\n",
			    resources->connectors[i], errno);
			failures++;
			continue;
		}
		check_connector(fd, connector, resources, expect_no_connected,
		    &connected_count);
		if (connector->connection == DRM_MODE_CONNECTED &&
		    connected_connector_id == 0)
			connected_connector_id = connector->connector_id;
		if (!deprecated_mode_noop_done &&
		    connector->connection == DRM_MODE_CONNECTED) {
			check_deprecated_master_mode_ioctl_contract(fd,
			    connector->connector_id);
			deprecated_mode_noop_done = true;
		}
		drmModeFreeConnector(connector);
	}
	if (expect_no_connected)
		check(connected_count == 0,
		    "no connected connector exposed when requested");
	else {
		check(connected_count > 0,
		    "at least one connected connector exposed");
		check(deprecated_mode_noop_done,
		    "connected connector supports deprecated master mode no-op probe");
		check(connected_connector_id != 0,
		    "connected connector supports GETCONNECTOR reprobe probe");
		if (connected_connector_id != 0)
			check_getconnector_reprobe_master_contract(fd,
			    connected_connector_id);
	}
	check_connected_mode_list_atomic_contract(fd, resources,
	    expect_no_connected);
	check_drm_lease_contract(fd, resources, expect_no_connected);

	for (int i = 0; i < resources->count_crtcs; i++)
		check_crtc(fd, resources->crtcs[i]);

	check_planes(fd, resources);
	drmModeFreeResources(resources);
	close(fd);

	return failures == 0 ? 0 : 1;
}
