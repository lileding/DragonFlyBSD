/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (C) Red Hat, Inc., 2009-2011
 */

#ifndef _DEV_VIRTUAL_VIRTIO_CONSOLE_VIRTIO_CONSOLE_H_
#define _DEV_VIRTUAL_VIRTIO_CONSOLE_VIRTIO_CONSOLE_H_

/* VirtIO console feature bits. */
#define VIRTIO_CONSOLE_F_SIZE		0x01
#define VIRTIO_CONSOLE_F_MULTIPORT	0x02
#define VIRTIO_CONSOLE_F_EMERG_WRITE	0x04

struct virtio_console_config {
	uint16_t cols;
	uint16_t rows;
	uint32_t max_nr_ports;
	uint32_t emerg_wr;
} __packed;

struct virtio_console_control {
	uint32_t id;
	uint16_t event;
	uint16_t value;
} __packed;

#define VIRTIO_CONSOLE_DEVICE_READY 0
#define VIRTIO_CONSOLE_PORT_ADD 1
#define VIRTIO_CONSOLE_PORT_REMOVE 2
#define VIRTIO_CONSOLE_PORT_READY 3
#define VIRTIO_CONSOLE_CONSOLE_PORT 4
#define VIRTIO_CONSOLE_PORT_OPEN 6

#endif /* _DEV_VIRTUAL_VIRTIO_CONSOLE_VIRTIO_CONSOLE_H_ */
