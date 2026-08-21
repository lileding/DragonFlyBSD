/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs 16550A serial-port object.
 */
#ifndef VMMFS_SERIALPORT_H
#define VMMFS_SERIALPORT_H

#include <sys/queue.h>
#include <sys/thread.h>
#include <sys/tree.h>
#include <sys/types.h>

#include <dev/virtual/vmm/vmm.h>

struct cdev;
struct tty;
struct vnode;
struct vop_ops;
struct vmmfs_serialroot;

#define VMMFS_SERIALPORT_FIFO_SIZE 1024
#define VMMFS_SERIALPORT_OUTPUT_SIZE (64 * 1024)

struct vmmfs_serialport {
	RB_ENTRY(vmmfs_serialport) entry;
	struct vmmfs_serialroot *serialroot;
	struct vnode *vnode;
	ino_t inode;
	char name[sizeof("com4")];
	uint8_t number;
	uint16_t base;
	uint32_t gsi;
	struct cdev *dev;
	struct tty *tty;
	struct thread *thread;
	struct lwkt_token token;
	vmm_machine_t machine;
	vmm_io_t read_io;
	vmm_io_t write_io;
	volatile u_int open;
	unsigned int opening_count;
	bool stopping;
	bool destroying;
	bool thread_exited;
	uint8_t dll;
	uint8_t dlm;
	uint8_t ier;
	uint8_t fcr;
	uint8_t lcr;
	uint8_t mcr;
	uint8_t scr;
	bool thre_pending;
	bool lsr_overrun;
	bool irq_asserted;
	size_t input_start;
	size_t input_length;
	char input[VMMFS_SERIALPORT_FIFO_SIZE];
	size_t output_start;
	size_t output_length;
	char *output;
};

RB_PROTOTYPE(vmmfs_serialport_tree, vmmfs_serialport, entry,
	vmmfs_serialport_compare);

extern struct vop_ops vmmfs_serialport_vops;

int vmmfs_serialport_compare(struct vmmfs_serialport *,
	struct vmmfs_serialport *);
int vmmfs_serialport_create(struct vmmfs_serialroot *, const char *, size_t,
	struct vmmfs_serialport **);
int vmmfs_serialport_destroy(struct vmmfs_serialport *);
int vmmfs_serialport_start(struct vmmfs_serialport *, vmm_machine_t);
int vmmfs_serialport_stop(struct vmmfs_serialport *);

#endif /* VMMFS_SERIALPORT_H */
