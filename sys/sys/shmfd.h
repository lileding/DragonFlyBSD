/*
 * Copyright (c) 2026 The DragonFly Project.  All rights reserved.
 */

#ifndef _SYS_SHMFD_H_
#define _SYS_SHMFD_H_

#include <sys/types.h>

#ifdef _KERNEL

#include <sys/lock.h>
#include <vm/vm.h>

struct file;
struct thread;

/*
 * A caller-owned file-mapping facade for a VM object.
 *
 * The caller embeds this structure in its own export object, places its
 * address in file->f_data, and supplies its own fileops.  shmfd_close()
 * releases only this structure's VM-object reference; it never frees the
 * enclosing allocation.
 */
struct shmfd {
	struct lock	shmfd_lock;
	vm_object_t	shmfd_object;
	off_t		shmfd_size;
};

int shmfd_init(struct shmfd *, vm_object_t, off_t);
int shmfd_mmap(struct file *, vm_map_t, vm_offset_t *, vm_size_t,
	vm_prot_t, vm_prot_t, int, vm_ooffset_t, struct thread *);
void shmfd_close(struct shmfd *);
int shmfd_revoke(struct shmfd *);

#endif /* _KERNEL */

#endif /* _SYS_SHMFD_H_ */
