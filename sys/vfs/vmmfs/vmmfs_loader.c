/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs machine vCPU declaration node.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"

#define VMMFS_LOADER_MODE 0644

static int vmmfs_loader_access(struct vop_access_args *);
static int vmmfs_loader_getattr(struct vop_getattr_args *);
static int vmmfs_loader_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_loader_open(struct vop_open_args *);
static int vmmfs_loader_read(struct vop_read_args *);
static int vmmfs_loader_setattr(struct vop_setattr_args *);
static int vmmfs_loader_write(struct vop_write_args *);
static int vmmfs_loader_reclaim(struct vop_reclaim_args *);

struct vop_ops vmmfs_loader_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_loader_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_loader_getattr,
	.vop_getattr_lite = vmmfs_loader_getattr_lite,
	.vop_open = vmmfs_loader_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_loader_read,
	.vop_reclaim = vmmfs_loader_reclaim,
	.vop_setattr = vmmfs_loader_setattr,
	.vop_write = vmmfs_loader_write,
};

static int
vmmfs_loader_load(struct vmmfs_loader *loader, char *buffer, size_t capacity, size_t *length)
{
	int result;
	result = ksnprintf(buffer, capacity, "%s\n",
	    loader->machine->spec.loader.path);
	if (result < 0 || (size_t)result >= capacity)
		return (EOVERFLOW);
	*length = (size_t)result;
	return (0);
}

static int
vmmfs_loader_store(struct vmmfs_loader *loader, const char *buffer, size_t length)
{
	if (length == 0)
		return (EINVAL);
	if (buffer[length - 1] == 10)
		--length;
	if (length == 0 || length >= MAXPATHLEN)
		return (ENAMETOOLONG);
	lwkt_gettoken(&loader->machine->token);
	if (!loader->machine->stopped.expect_stopped ||
	    loader->machine->machine != NULL) {
		lwkt_reltoken(&loader->machine->token);
		return (EBUSY);
	}
	bcopy(buffer, loader->machine->spec.loader.path, length);
	loader->machine->spec.loader.path[length] = 0;
	lwkt_reltoken(&loader->machine->token);
	return (0);
}

int
vmmfs_loader_create(struct vmmfs_machine *machine, struct vmmfs_loader *loader)
{
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	bzero(loader, sizeof(*loader));
	loader->machine = machine;
	state = (struct vmmfs_mount *)machine->root->mount->mnt_data;
    loader->inode = atomic_fetchadd_int(&state->next_inode, 1);
	if (state->loader_vops == NULL)
		return (ENXIO);

	error = getnewvnode(VT_SYNTH, machine->root->mount, &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_data = loader;
	vnode->v_ops = &state->loader_vops;
	vnode->v_type = VREG;
	loader->vnode = vnode;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_loader_destroy(struct vmmfs_loader *loader)
{
	struct vnode *vnode;

	if (loader == NULL)
		return (EINVAL);

	vnode = loader->vnode;
	if (vnode != NULL) {
		vx_get(vnode);
		vgone_vxlocked(vnode);
		vx_put(vnode);
		vrele(vnode);
	}
	KKASSERT(loader->vnode == NULL);
	loader->machine = NULL;
	return (0);
}

static int
vmmfs_loader_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_LOADER_MODE, 0));
}

static int
vmmfs_loader_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_loader *loader;
	struct vattr *vattr;
	char buffer[32];
	size_t length;
	int error;

	loader = ap->a_vp->v_data;
	if (loader == NULL)
		return (ENOENT);
	error = vmmfs_loader_load(loader, buffer, sizeof(buffer), &length);
	if (error != 0)
		return (error);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_LOADER_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = loader->inode;
	vattr->va_size = length;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = length;
	return (0);
}

static int
vmmfs_loader_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_LOADER_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_loader_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
}

static int
vmmfs_loader_read(struct vop_read_args *ap)
{
	struct vmmfs_loader *loader;
	struct uio *uio;
	char buffer[32];
	size_t length;
	off_t offset;
	int error;

	loader = ap->a_vp->v_data;
	if (loader == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	error = vmmfs_loader_load(loader, buffer, sizeof(buffer), &length);
	if (error != 0)
		return (error);
	offset = uio->uio_offset;
	if ((size_t)offset >= length)
		return (0);
	return (uiomove(buffer + offset, length - (size_t)offset, uio));
}

static int
vmmfs_loader_setattr(struct vop_setattr_args *ap)
{
	/* Accept the O_TRUNC size update performed before a control write. */
	(void)ap;
	return (0);
}

static int
vmmfs_loader_write(struct vop_write_args *ap)
{
	struct vmmfs_loader *loader;
	struct uio *uio;
	char buffer[32];
	size_t length;
	int error;

	loader = ap->a_vp->v_data;
	if (loader == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset != 0 || uio->uio_resid == 0 ||
	    (size_t)uio->uio_resid >= sizeof(buffer))
		return (EINVAL);
	length = (size_t)uio->uio_resid;
	error = uiomove(buffer, length, uio);
	if (error != 0)
		return (error);
	return (vmmfs_loader_store(loader, buffer, length));
}

static int
vmmfs_loader_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_loader *loader;

	loader = ap->a_vp->v_data;
	if (loader != NULL && loader->vnode == ap->a_vp)
		loader->vnode = NULL;
	ap->a_vp->v_data = NULL;
	return (0);
}
