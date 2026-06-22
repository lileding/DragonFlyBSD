#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
		require_property(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE,
		    "IN_FORMATS", name);
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
