#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
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

static void
require_property(int fd, uint32_t object_id, uint32_t object_type,
    const char *name, const char *object_name)
{
	char text[160];

	snprintf(text, sizeof(text), "%s has property %s", object_name, name);
	check(has_property(fd, object_id, object_type, name), text);
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
check_connector(int fd, drmModeConnector *connector)
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
	require_property(fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "link-status", name);
	require_property(fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "max bpc", name);
	require_property(fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "dithering mode", name);
	require_property(fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR,
	    "dithering depth", name);
}

static void
check_crtc(int fd, uint32_t crtc_id)
{
	char name[64];

	snprintf(name, sizeof(name), "crtc %u", crtc_id);
	dump_properties(fd, crtc_id, DRM_MODE_OBJECT_CRTC, name);
	require_property(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "GAMMA_LUT",
	    name);
	require_property(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "GAMMA_LUT_SIZE",
	    name);
	require_property(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "DEGAMMA_LUT",
	    name);
	require_property(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
	    "DEGAMMA_LUT_SIZE", name);
	require_property(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "CTM", name);
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
check_planes(int fd)
{
	drmModePlaneResPtr resources;

	resources = drmModeGetPlaneResources(fd);
	check(resources != NULL, "plane resources available");
	if (resources == NULL)
		return;

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
		plane_type = get_plane_type(fd, plane->plane_id);
		check(plane_type >= 0, "plane type is readable");
		check_in_formats(fd, plane, plane_type, name);
		drmModeFreePlane(plane);
	}

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

	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector == NULL) {
			printf("connector %u unavailable errno=%d\n",
			    resources->connectors[i], errno);
			failures++;
			continue;
		}
		check_connector(fd, connector);
		drmModeFreeConnector(connector);
	}

	for (int i = 0; i < resources->count_crtcs; i++)
		check_crtc(fd, resources->crtcs[i]);

	check_planes(fd);
	drmModeFreeResources(resources);
	close(fd);

	return failures == 0 ? 0 : 1;
}
