/*-
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <sys/socket.h>
#include <sys/un.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "virtiod.h"

struct virtiod_worker {
	const struct virtiod_device *borrow_imm_device;
	int mut_error;
	pthread_t own_thread;
};

static void virtiod_usage(void);
static int virtiod_control_fd(const char *);
static void *virtiod_worker_main(void *);

int
main(int argc, char **argv)
{
	struct virtiod_config config;
	struct virtiod_worker *workers;
	FILE *stream;
	const char *config_path;
	const char *control;
	unsigned int i;
	int ch;
	int error;
	int status;

	config_path = "-";
	control = NULL;
	while ((ch = getopt(argc, argv, "c:dhvs:")) != -1) {
		switch (ch) {
		case 'c':
			config_path = optarg;
			break;
		case 's':
			control = optarg;
			break;
		case 'v':
			puts("virtiod 1");
			return 0;
		case 'h':
			virtiod_usage();
			return 0;
		case 'd':
			errno = ENOTSUP;
			err(1, "daemon mode is not implemented");
		default:
			virtiod_usage();
			return 1;
		}
	}
	if (optind != argc) {
		virtiod_usage();
		return 1;
	}
	if (control != NULL && (error = virtiod_control_fd(control)) != 0) {
		errno = error;
		err(1, "control endpoint %s", control);
	}
	if (strcmp(config_path, "-") == 0)
		stream = stdin;
	else
		stream = fopen(config_path, "r");
	if (stream == NULL)
		err(1, "open configuration %s", config_path);
	error = virtiod_config_load(stream, &config);
	if (stream != stdin)
		(void)fclose(stream);
	if (error != 0) {
		errno = error;
		err(1, "parse configuration");
	}
	workers = calloc(config.mut_count, sizeof(*workers));
	if (workers == NULL)
		err(1, "allocate workers");
	for (i = 0; i < config.mut_count; i++) {
		workers[i].borrow_imm_device = &config.own_mut_devices[i];
		error = pthread_create(&workers[i].own_thread, NULL,
		    virtiod_worker_main, &workers[i]);
		if (error != 0) {
			errno = error;
			err(1, "create provider worker");
		}
	}
	status = 0;
	for (i = 0; i < config.mut_count; i++) {
		(void)pthread_join(workers[i].own_thread, NULL);
		if (workers[i].mut_error != 0) {
			warnx("provider %s stopped: %s",
			    workers[i].borrow_imm_device->imm_slot_path,
			    strerror(workers[i].mut_error));
			status = 1;
		}
	}
	free(workers);
	virtiod_config_fini(&config);
	return status;
}

static void
virtiod_usage(void)
{

	fprintf(stderr, "usage: virtiod [-d] [-c CONFIG] [-s CONTROL]\n");
}

static int
virtiod_control_fd(const char *control)
{
	struct sockaddr_un address;
	socklen_t length;
	int fd;
	int type;

	if (strncmp(control, "fd:", 3) != 0)
		return ENOTSUP;
	fd = atoi(control + 3);
	if (fd < 0)
		return EINVAL;
	length = sizeof(type);
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 ||
	    type != SOCK_SEQPACKET)
		return ENOTSOCK;
	length = sizeof(address);
	if (getpeername(fd, (struct sockaddr *)&address, &length) != 0 ||
	    address.sun_family != AF_UNIX)
		return EPROTOTYPE;
	return 0;
}

static void *
virtiod_worker_main(void *argument)
{
	struct virtiod_worker *worker;

	worker = argument;
	if (worker->borrow_imm_device->imm_type == VIRTIOD_DEVICE_BLK)
		worker->mut_error = virtiod_blk_run(worker->borrow_imm_device);
	else
		worker->mut_error = virtiod_net_run(worker->borrow_imm_device);
	return NULL;
}
