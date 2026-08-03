/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "virtiod.h"

static int virtiod_config_parse_line(char *, struct virtiod_device *);
static int virtiod_config_slot_valid(const char *);
static int virtiod_config_set_parameter(struct virtiod_device *, char *);

int
virtiod_config_load(FILE *stream, struct virtiod_config *config)
{
	char *line;
	size_t capacity;
	size_t line_capacity;
	ssize_t line_size;
	int error;

	if (stream == NULL || config == NULL)
		return EINVAL;
	memset(config, 0, sizeof(*config));
	line = NULL;
	capacity = 0;
	line_capacity = 0;
	error = 0;
	while ((line_size = getline(&line, &line_capacity, stream)) >= 0) {
		struct virtiod_device device;
		char *text;

		while (line_size > 0 && isspace((unsigned char)line[line_size - 1]))
			line[--line_size] = '\0';
		text = line;
		while (isspace((unsigned char)*text))
			text++;
		if (*text == '\0' || *text == '#')
			continue;
		error = virtiod_config_parse_line(text, &device);
		if (error != 0)
			break;
		for (size_t i = 0; i < config->mut_count; i++) {
			if (strcmp(config->own_mut_devices[i].imm_slot_path,
			    device.imm_slot_path) == 0) {
				error = EEXIST;
				break;
			}
			if (device.imm_type == VIRTIOD_DEVICE_VSOCK &&
			    config->own_mut_devices[i].imm_type ==
			    VIRTIOD_DEVICE_VSOCK) {
				error = EEXIST;
				break;
			}
		}
		if (error != 0)
			break;
		if (config->mut_count == capacity) {
			struct virtiod_device *devices;

			capacity = capacity == 0 ? 4 : capacity * 2;
			devices = realloc(config->own_mut_devices,
			    capacity * sizeof(*devices));
			if (devices == NULL) {
				error = ENOMEM;
				break;
			}
			config->own_mut_devices = devices;
		}
		config->own_mut_devices[config->mut_count++] = device;
	}
	if (ferror(stream) != 0 && error == 0)
		error = errno == 0 ? EIO : errno;
	free(line);
	if (error != 0 || config->mut_count == 0) {
		if (error == 0)
			error = EINVAL;
		virtiod_config_fini(config);
	}
	return error;
}

void
virtiod_config_fini(struct virtiod_config *config)
{

	if (config == NULL)
		return;
	free(config->own_mut_devices);
	memset(config, 0, sizeof(*config));
}

static int
virtiod_config_parse_line(char *text, struct virtiod_device *device)
{
	char *fields[4];
	char *cursor;
	unsigned int count;
	int error;

	memset(device, 0, sizeof(*device));
	cursor = text;
	for (count = 0; count < 4; count++) {
		fields[count] = strsep(&cursor, " \t");
		while (fields[count] != NULL && *fields[count] == '\0')
			fields[count] = strsep(&cursor, " \t");
		if (fields[count] == NULL)
			break;
	}
	if (count != 3 || cursor != NULL || fields[0] == NULL ||
	    fields[1] == NULL || fields[2] == NULL)
		return EINVAL;
	if (strcmp(fields[0], "blk") == 0)
		device->imm_type = VIRTIOD_DEVICE_BLK;
	else if (strcmp(fields[0], "net") == 0)
		device->imm_type = VIRTIOD_DEVICE_NET;
	else if (strcmp(fields[0], "vsock") == 0)
		device->imm_type = VIRTIOD_DEVICE_VSOCK;
	else
		return EINVAL;
	if (!virtiod_config_slot_valid(fields[1]) ||
	    strlcpy(device->imm_slot_path, fields[1],
	    sizeof(device->imm_slot_path)) >= sizeof(device->imm_slot_path))
		return EINVAL;
	device->imm_queue_count = 1;
	error = virtiod_config_set_parameter(device, fields[2]);
	if (error != 0)
		return error;
	if (device->imm_type == VIRTIOD_DEVICE_NET &&
	    device->imm_queue_count != 1)
		return EOPNOTSUPP;
	if (device->imm_type == VIRTIOD_DEVICE_VSOCK &&
	    device->imm_queue_count != 1)
		return EOPNOTSUPP;
	return 0;
}

static int
virtiod_config_slot_valid(const char *path)
{
	char copy[VIRTIOD_PATH_MAX];
	char *cursor;
	char *component;
	unsigned int count;

	if (path == NULL || path[0] == '/' || strlen(path) >= sizeof(copy))
		return 0;
	strlcpy(copy, path, sizeof(copy));
	cursor = copy;
	count = 0;
	while ((component = strsep(&cursor, "/")) != NULL) {
		if (*component == '\0' || strcmp(component, ".") == 0 ||
		    strcmp(component, "..") == 0)
			return 0;
		if ((count == 1 && strcmp(component, "devices") != 0) || ++count > 3)
			return 0;
	}
	return count == 3;
}

static int
virtiod_config_set_parameter(struct virtiod_device *device, char *parameters)
{
	char *cursor;
	char *parameter;
	int has_path;
	int has_tap;
	int has_mac;
	int has_cid;
	int has_queues;

	has_path = 0;
	has_tap = 0;
	has_mac = 0;
	has_cid = 0;
	has_queues = 0;
	cursor = parameters;
	while ((parameter = strsep(&cursor, ",")) != NULL) {
		char *value;

		if (*parameter == '\0' || (value = strchr(parameter, '=')) == NULL)
			return EINVAL;
		*value++ = '\0';
		if (strcmp(parameter, "path") == 0 &&
		    device->imm_type == VIRTIOD_DEVICE_BLK && !has_path) {
			if (*value == '\0' || strlcpy(device->imm_path, value,
			    sizeof(device->imm_path)) >= sizeof(device->imm_path))
				return EINVAL;
			has_path = 1;
		} else if (strcmp(parameter, "tap") == 0 &&
		    device->imm_type == VIRTIOD_DEVICE_NET && !has_tap) {
			if (*value == '\0' || strlcpy(device->imm_path, value,
			    sizeof(device->imm_path)) >= sizeof(device->imm_path))
				return EINVAL;
			has_tap = 1;
		} else if (strcmp(parameter, "mac") == 0 &&
		    device->imm_type == VIRTIOD_DEVICE_NET && !has_mac) {
			if (strlcpy(device->imm_mac, value, sizeof(device->imm_mac)) >=
			    sizeof(device->imm_mac))
				return EINVAL;
			has_mac = 1;
		} else if (strcmp(parameter, "cid") == 0 &&
		    device->imm_type == VIRTIOD_DEVICE_VSOCK && !has_cid) {
			char *end;
			unsigned long long cid;

			errno = 0;
			cid = strtoull(value, &end, 10);
			if (*value == '\0' || *end != '\0' || errno != 0 ||
			    cid <= 2)
				return EINVAL;
			device->imm_guest_cid = (uint64_t)cid;
			has_cid = 1;
		} else if (strcmp(parameter, "queues") == 0 && !has_queues) {
			char *end;
			unsigned long count;

			errno = 0;
			count = strtoul(value, &end, 10);
			if (*value == '\0' || *end != '\0' || errno != 0 ||
			    count == 0 || count > VIRTIOD_MAX_QUEUES || count > UINT_MAX)
				return EINVAL;
			device->imm_queue_count = (unsigned int)count;
			has_queues = 1;
		} else {
			return EINVAL;
		}
	}
	if (device->imm_type == VIRTIOD_DEVICE_BLK)
		return has_path ? 0 : EINVAL;
	if (device->imm_type == VIRTIOD_DEVICE_NET)
		return has_tap && has_mac ? 0 : EINVAL;
	return has_cid ? 0 : EINVAL;
}
