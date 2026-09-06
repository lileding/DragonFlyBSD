/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs PCI descriptor authorization capability.
 */
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/sysmsg.h>
#include <sys/uio.h>
#include <sys/spinlock2.h>

#include <machine/atomic.h>

#include "vmmfs.h"
#include "vmmfs_parent.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_auth.h"

struct vmmfs_pcislot_auth {
	struct vmmfs_pcislot *slot;
	ino_t slot_inode;
	uint64_t generation;
	volatile u_int valid;
	volatile u_int references;
};

static int vmmfs_pcislot_auth_readwrite(struct file *, struct uio *,
	struct ucred *, int);
static int vmmfs_pcislot_auth_ioctl(struct file *, u_long, caddr_t,
	struct ucred *, struct sysmsg *);
static int vmmfs_pcislot_auth_kqfilter(struct file *, struct knote *);
static int vmmfs_pcislot_auth_stat(struct file *, struct stat *,
	struct ucred *);
static int vmmfs_pcislot_auth_close(struct file *);
static int vmmfs_pcislot_auth_seek(struct file *, off_t, int, off_t *);
static void vmmfs_pcislot_auth_hold(struct vmmfs_pcislot_auth *);
static void vmmfs_pcislot_auth_put(struct vmmfs_pcislot_auth *);

static struct fileops vmmfs_pcislot_auth_fileops = {
	.fo_read = vmmfs_pcislot_auth_readwrite,
	.fo_write = vmmfs_pcislot_auth_readwrite,
	.fo_ioctl = vmmfs_pcislot_auth_ioctl,
	.fo_kqfilter = vmmfs_pcislot_auth_kqfilter,
	.fo_stat = vmmfs_pcislot_auth_stat,
	.fo_close = vmmfs_pcislot_auth_close,
	.fo_shutdown = nofo_shutdown,
	.fo_seek = vmmfs_pcislot_auth_seek,
};

int
vmmfs_pcislot_auth_create(struct vmmfs_pcislot *slot, uint64_t generation,
	struct vmmfs_pcislot_auth **authp)
{
	struct vmmfs_pcislot_auth *auth;
	struct file *file;
	int fd;
	int error;

	if (slot == NULL || generation == 0 || authp == NULL)
		return (EINVAL);
	*authp = NULL;
	auth = kmalloc(sizeof(*auth), M_VMMFS, M_WAITOK | M_ZERO);
	auth->slot = slot;
	vmmfs_node_hold(&slot->node);
	auth->slot_inode = slot->node.inode;
	auth->generation = generation;
	auth->valid = 1;
	auth->references = 1;
	error = falloc(NULL, &file, NULL);
	if (error != 0)
		goto fail;
	vmmfs_pcislot_auth_hold(auth);
	file->f_type = DTYPE_DMABUF;
	file->f_flag = FREAD;
	file->f_ops = &vmmfs_pcislot_auth_fileops;
	file->f_data = auth;
	error = fdalloc(curproc, 0, &fd);
	if (error != 0) {
		fdrop(file);
		goto fail;
	}
	fsetfd(curproc->p_fd, file, fd);
	fdrop(file);
	*authp = auth;
	return (0);

fail:
	vmmfs_pcislot_auth_put(auth);
	return (error);
}

void
vmmfs_pcislot_auth_revoke(struct vmmfs_pcislot_auth *auth)
{
	if (auth == NULL)
		return;
	atomic_store_rel_int(&auth->valid, 0);
	vmmfs_pcislot_auth_put(auth);
}

int
vmmfs_pcislot_auth_check(struct vmmfs_pcislot *slot)
{
	struct filedesc *filedesc;
	struct file *file;
	struct vmmfs_pcislot_auth *auth;
	int last;
	int fd;

	if (slot == NULL || curproc == NULL || curproc->p_fd == NULL)
		return (EACCES);
	filedesc = curproc->p_fd;
	spin_lock_shared(&filedesc->fd_spin);
	last = filedesc->fd_lastfile;
	spin_unlock_shared(&filedesc->fd_spin);
	for (fd = 0; fd <= last; ++fd) {
		file = holdfp_fdp(filedesc, fd, -1);
		if (file == NULL)
			continue;
		if (file->f_ops == &vmmfs_pcislot_auth_fileops) {
			auth = file->f_data;
			if (auth != NULL && atomic_load_acq_int(&auth->valid) != 0 &&
			    auth->slot == slot && auth->slot_inode == slot->node.inode &&
			    auth->generation == slot->descriptor.generation) {
				fdrop(file);
				return (0);
			}
		}
		fdrop(file);
	}
	return (EACCES);
}

static int
vmmfs_pcislot_auth_readwrite(struct file *file, struct uio *uio,
	struct ucred *cred, int flags)
{
	(void)file;
	(void)uio;
	(void)cred;
	(void)flags;
	return (EBADF);
}

static int
vmmfs_pcislot_auth_ioctl(struct file *file, u_long command, caddr_t data,
	struct ucred *cred, struct sysmsg *message)
{
	(void)file;
	(void)command;
	(void)data;
	(void)cred;
	(void)message;
	return (EOPNOTSUPP);
}

static int
vmmfs_pcislot_auth_kqfilter(struct file *file, struct knote *knote)
{
	(void)file;
	(void)knote;
	return (EOPNOTSUPP);
}

static int
vmmfs_pcislot_auth_stat(struct file *file, struct stat *status,
	struct ucred *cred)
{
	struct vmmfs_pcislot_auth *auth;

	(void)cred;
	if (file == NULL || status == NULL)
		return (EINVAL);
	auth = file->f_data;
	if (auth == NULL)
		return (EBADF);
	bzero(status, sizeof(*status));
	status->st_nlink = 1;
	status->st_mode = S_IFREG | 0400;
	status->st_uid = 0;
	status->st_gid = 0;
	status->st_ino = auth->slot_inode;
	status->st_gen = (uint32_t)auth->generation;
	return (0);
}

static int
vmmfs_pcislot_auth_close(struct file *file)
{
	struct vmmfs_pcislot_auth *auth;

	if (file == NULL)
		return (0);
	auth = file->f_data;
	file->f_data = NULL;
	if (auth != NULL)
		vmmfs_pcislot_auth_put(auth);
	return (0);
}

static int
vmmfs_pcislot_auth_seek(struct file *file, off_t offset, int whence,
	off_t *result)
{
	(void)file;
	(void)offset;
	(void)whence;
	(void)result;
	return (ESPIPE);
}

static void
vmmfs_pcislot_auth_hold(struct vmmfs_pcislot_auth *auth)
{
	atomic_add_int(&auth->references, 1);
}

static void
vmmfs_pcislot_auth_put(struct vmmfs_pcislot_auth *auth)
{
	struct vmmfs_pcislot *slot;

	if (atomic_fetchadd_int(&auth->references, -1) != 1)
		return;
	slot = auth->slot;
	kfree(auth, M_VMMFS);
	vmmfs_node_put(&slot->node);
}
