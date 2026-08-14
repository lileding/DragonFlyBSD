/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs machine directory object.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs.h"

#define VMMFS_MACHINE_MODE 0555

static int vmmfs_machine_access(struct vop_access_args *);
static int vmmfs_machine_getattr(struct vop_getattr_args *);
static int vmmfs_machine_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_machine_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_machine_nresolve(struct vop_nresolve_args *);
static int vmmfs_machine_readdir(struct vop_readdir_args *);
static int vmmfs_machine_reclaim(struct vop_reclaim_args *);

struct vop_ops vmmfs_machine_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_machine_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_machine_getattr,
	.vop_getattr_lite = vmmfs_machine_getattr_lite,
	.vop_nlookupdotdot = vmmfs_machine_nlookupdotdot,
	.vop_nresolve = vmmfs_machine_nresolve,
	.vop_open = vop_stdopen,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_machine_readdir,
	.vop_reclaim = vmmfs_machine_reclaim,
};

int
vmmfs_machine_compare(struct vmmfs_machine *left,
	struct vmmfs_machine *right)
{
	return (strcmp(left->name, right->name));
}

RB_GENERATE(vmmfs_machine_tree, vmmfs_machine, entry, vmmfs_machine_compare);

struct vmmfs_machine *
vmmfs_machine_create(struct vmmfs_root *root, const char *name,
	size_t namelen)
{
	struct vmmfs_machine *machine;
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	if (namelen == 0 || namelen > NAME_MAX)
		return (NULL);
	machine = kmalloc(sizeof(*machine), M_VMMFS, M_WAITOK | M_ZERO);
	machine->root = root;
	state = (struct vmmfs_mount *)root->mount->mnt_data;
	machine->vops = state->machine_vops;
	bcopy(name, machine->name, namelen);
	machine->name[namelen] = '\0';
	lwkt_token_init(&machine->token, "vmmfsmachine");
	error = getnewvnode(VT_SYNTH, root->mount, &vnode, 0, 0);
	if (error != 0) {
		lwkt_token_uninit(&machine->token);
		kfree(machine, M_VMMFS);
		return (NULL);
	}
	vnode->v_data = machine;
	vnode->v_ops = &state->machine_vops;
	vnode->v_type = VDIR;
	machine->vnode = vnode;
	vx_downgrade(vnode);
	return (machine);
}

int
vmmfs_machine_destroy(struct vmmfs_machine *machine)
{
	KKASSERT(machine->root == NULL);
	return (0);
}

void
vmmfs_machine_free(struct vmmfs_machine *machine)
{
	KKASSERT(machine->root == NULL);
	KKASSERT(machine->vnode == NULL);
	lwkt_token_uninit(&machine->token);
	kfree(machine, M_VMMFS);
}

static int
vmmfs_machine_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_MACHINE_MODE, 0));
}

static int
vmmfs_machine_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_machine *machine;
	struct vattr *vattr;

	machine = ap->a_vp->v_data;
	if (machine == NULL)
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_MACHINE_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = machine->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
}

static int
vmmfs_machine_getattr_lite(struct vop_getattr_lite_args *ap)
{
	struct vattr_lite *vattr;

	vattr = ap->a_lvap;
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_MACHINE_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_size = 0;
	vattr->va_flags = 0;
	return (0);
}

static int
vmmfs_machine_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_machine *machine;
	struct vmmfs_root *root;
	struct vnode *vnode;

	machine = ap->a_dvp->v_data;
	root = ((struct vmmfs_mount *)ap->a_dvp->v_mount->mnt_data)->root;
	if (machine == NULL || root == NULL)
		return (ENOENT);
	lwkt_gettoken(&machine->token);
	if (machine->root != root) {
		lwkt_reltoken(&machine->token);
		return (ENOENT);
	}
	lwkt_reltoken(&machine->token);
	lwkt_gettoken(&root->token);
	vnode = root->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&root->token);
	if (vnode == NULL)
		return (ENOENT);
	if (vget(vnode, LK_EXCLUSIVE | LK_RETRY) != 0) {
		vdrop(vnode);
		return (ENOENT);
	}
	vdrop(vnode);
	*ap->a_vpp = vnode;
	vn_unlock(vnode);
	return (0);
}

static int
vmmfs_machine_nresolve(struct vop_nresolve_args *ap)
{
	cache_setvp(ap->a_nch, NULL);
	return (ENOENT);
}

static int
vmmfs_machine_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_machine *machine;
	struct uio *uio;
	off_t offset;
	int error;
	int stop;

	machine = ap->a_vp->v_data;
	if (machine == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	offset = uio->uio_offset;
	error = 0;
	stop = 0;
	if (offset == 0) {
		stop = vop_write_dirent(&error, uio, machine->inode, DT_DIR, 1,
		    ".");
		if (!stop)
			offset = 1;
	}
	if (!stop && offset == 1) {
		stop = vop_write_dirent(&error, uio, VMMFS_ROOT_INO, DT_DIR, 2,
		    "..");
		if (!stop)
			offset = 2;
	}
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop;
	return (error);
}

static int
vmmfs_machine_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_machine *machine;
	int destroy;

	machine = ap->a_vp->v_data;
	if (machine == NULL)
		return (0);
	lwkt_gettoken(&machine->token);
	if (machine->vnode == ap->a_vp)
		machine->vnode = NULL;
	destroy = machine->root == NULL;
	lwkt_reltoken(&machine->token);
	ap->a_vp->v_data = NULL;
	if (destroy)
		vmmfs_machine_free(machine);
	return (0);
}
