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

static int failures;

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
	check_drm_cap(fd, DRM_CAP_ASYNC_PAGE_FLIP, 0,
	    "ASYNC_PAGE_FLIP");
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
    const char *object_name)
{
	check_enum_property_default(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "link-status", object_name, "Good");
	check_enum_property_default(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "dithering mode", object_name, "auto");
	check_enum_property_default(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "dithering depth", object_name, "auto");
	check_range_property_value(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR, "max bpc", object_name, 8, 8, 8);
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

static void
check_connector(int fd, drmModeConnector *connector,
    const drmModeRes *resources)
{
	char name[64];

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
	check(connector->count_encoders > 0, "connector has at least one encoder");
	for (int i = 0; i < connector->count_encoders; i++) {
		check(id_in_list(resources->encoders, resources->count_encoders,
		    connector->encoders[i]),
		    "connector encoder id is present in resources");
	}
	if (connector->connection == DRM_MODE_CONNECTED) {
		check(connector->count_modes > 0,
		    "connected connector exposes at least one mode");
		check(connector->encoder_id != 0,
		    "connected connector has current encoder");
		if (connector->encoder_id != 0) {
			check(id_in_list(connector->encoders,
			    connector->count_encoders, connector->encoder_id),
			    "connected connector current encoder is attached");
		}
	}
	check_connector_property_contract(fd, connector->connector_id, name);
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
		check(id_in_list(resources->crtcs, resources->count_crtcs,
		    encoder->crtc_id), "encoder current CRTC is present");
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
check_crtc(int fd, uint32_t crtc_id)
{
	char name[64];

	snprintf(name, sizeof(name), "crtc %u", crtc_id);
	dump_properties(fd, crtc_id, DRM_MODE_OBJECT_CRTC, name);
	check_crtc_color_property_contract(fd, crtc_id, name);
	check_crtc_sync_property_contract(fd, crtc_id, name);
	check_atomic_crtc_color_contract(fd, crtc_id);
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

static void
check_planes(int fd, const drmModeRes *mode_resources)
{
	drmModePlaneResPtr resources;
	uint32_t active_crtc_id = 0;
	uint32_t active_crtc_index = 0;
	uint32_t active_crtc_width = 0;
	uint32_t active_crtc_height = 0;
	bool primary_panning_probe_done = false;
	bool cursor_probe_done = false;
	bool have_active_crtc;
	bool have_active_crtc_size;

	resources = drmModeGetPlaneResources(fd);
	check(resources != NULL, "plane resources available");
	if (resources == NULL)
		return;

	have_active_crtc = find_active_crtc(fd, mode_resources,
	    &active_crtc_id, &active_crtc_index);
	check(have_active_crtc, "active CRTC available for atomic TEST_ONLY probe");
	have_active_crtc_size = have_active_crtc &&
	    get_crtc_size(fd, active_crtc_id, &active_crtc_width,
	    &active_crtc_height);
	check(have_active_crtc_size,
	    "active CRTC mode size available for primary panning probe");

	printf("planes: count=%u\n", resources->count_planes);
	check(resources->count_planes > 0, "at least one KMS plane exposed");
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
		if (have_active_crtc_size && !primary_panning_probe_done &&
		    plane_type == DRM_PLANE_TYPE_PRIMARY &&
		    (plane->possible_crtcs & (1u << active_crtc_index)) != 0) {
			check_atomic_primary_panning_contract(fd,
			    plane->plane_id, active_crtc_id,
			    active_crtc_width, active_crtc_height);
			primary_panning_probe_done = true;
		}
		if (have_active_crtc && !cursor_probe_done &&
		    plane_type == DRM_PLANE_TYPE_CURSOR &&
		    (plane->possible_crtcs & (1u << active_crtc_index)) != 0) {
			check_atomic_cursor_test_only_contract(fd,
			    plane->plane_id, active_crtc_id);
			cursor_probe_done = true;
		}
		drmModeFreePlane(plane);
	}

	check(primary_panning_probe_done,
	    "primary plane supports active CRTC for panning TEST_ONLY probe");
	check(cursor_probe_done,
	    "cursor plane supports active CRTC for atomic TEST_ONLY probe");

	drmModeFreePlaneResources(resources);
}

int
main(void)
{
	drmModeRes *resources;
	int fd;

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open card0");
		return 1;
	}
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
		printf("WARN universal planes client cap failed errno=%d\n",
		    errno);
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
		printf("WARN atomic client cap failed errno=%d\n", errno);

	resources = drmModeGetResources(fd);
	if (resources == NULL) {
		printf("drmModeGetResources failed errno=%d\n", errno);
		close(fd);
		return 1;
	}

	printf("resources: connectors=%d crtcs=%d encoders=%d\n",
	    resources->count_connectors, resources->count_crtcs,
	    resources->count_encoders);
	check(resources->count_connectors > 0, "at least one connector exposed");
	check(resources->count_crtcs > 0, "at least one CRTC exposed");
	check(resources->count_encoders > 0, "at least one encoder exposed");
	check_mode_config_contract(fd, resources);
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
		check_connector(fd, connector, resources);
		drmModeFreeConnector(connector);
	}

	for (int i = 0; i < resources->count_crtcs; i++)
		check_crtc(fd, resources->crtcs[i]);

	check_planes(fd, resources);
	drmModeFreeResources(resources);
	close(fd);

	return failures == 0 ? 0 : 1;
}
