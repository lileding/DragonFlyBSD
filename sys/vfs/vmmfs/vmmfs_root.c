/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The vmmfs root is a name-indexed machine registry.  It owns no VM state;
 * each tree entry owns exactly one machine vnode and the vnode owns the
 * corresponding struct vmmfs_machine until reclaim.
 */
#include <sys/param.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/stat.h>
#include <sys/tree.h>
#include <sys/uio.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include "vmmfs_machine.h"
#include "vmmfs_root.h"

#define VMMFS_ROOT_TAG VT_UNUSED7
#define VMMFS_ROOT_INO 1
#define VMMFS_ROOT_MODE 0555

MALLOC_DECLARE(M_VMMFS);

struct vmmfs_root_mount {
	struct mount *mount;
	struct lock lock;
	struct vnode *root;
	struct vop_ops *machine_vops;
	struct vmmfs_machine_tree machines;
};

static int vmmfs_root_getattr(struct vop_getattr_args *);
static int vmmfs_root_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_root_readdir(struct vop_readdir_args *);
static int vmmfs_root_nresolve(struct vop_nresolve_args *);
static int vmmfs_root_nmkdir(struct vop_nmkdir_args *);
static int vmmfs_root_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_root_nrename(struct vop_nrename_args *);
static int vmmfs_root_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_root_access(struct vop_access_args *);
static int vmmfs_root_reclaim(struct vop_reclaim_args *);

struct vop_ops vmmfs_root_vops = {
	.vop_default = vop_defaultop,
	.vop_open = vop_stdopen,
	.vop_close = vop_stdclose,
	.vop_nresolve = vmmfs_root_nresolve,
	.vop_nmkdir = vmmfs_root_nmkdir,
	.vop_nrmdir = vmmfs_root_nrmdir,
	.vop_nrename = vmmfs_root_nrename,
	.vop_nlookupdotdot = vmmfs_root_nlookupdotdot,
	.vop_access = vmmfs_root_access,
	.vop_getattr = vmmfs_root_getattr,
	.vop_getattr_lite = vmmfs_root_getattr_lite,
	.vop_readdir = vmmfs_root_readdir,
	.vop_reclaim = vmmfs_root_reclaim,
};

static struct vmmfs_machine *
vmmfs_root_find(struct vmmfs_root_mount *root_mount, const char *name,
    int name_len)
{
	struct vmmfs_machine_name *name_entry;
	int comparison;
	int machine_len;

	name_entry = RB_ROOT(&root_mount->machines);
	while (name_entry != NULL) {
		machine_len = strlen(name_entry->name);
		comparison = strncmp(name, name_entry->name,
		    (name_len < machine_len) ? name_len : machine_len);
		if (comparison == 0) {
			if (name_len < machine_len)
				comparison = -1;
			else if (name_len > machine_len)
				comparison = 1;
		}
		if (comparison < 0)
			name_entry = RB_LEFT(name_entry, entry);
		else if (comparison > 0)
			name_entry = RB_RIGHT(name_entry, entry);
		else
			return name_entry->machine;
	}
	return NULL;
}

int
vmmfs_root_mount(struct mount *mount)
{
	struct vmmfs_root_mount *root_mount;

	root_mount = kmalloc(sizeof(*root_mount), M_VMMFS, M_WAITOK | M_ZERO);
	root_mount->mount = mount;
	lockinit(&root_mount->lock, "vmmfs root", 0, 0);
	RB_INIT(&root_mount->machines);
	vfs_add_vnodeops(mount, &vmmfs_machine_vops,
	    &root_mount->machine_vops);
	mount->mnt_data = (qaddr_t)root_mount;
	return 0;
}

int
vmmfs_root_unmount(struct mount *mount, int flags)
{
	struct vmmfs_root_mount *root_mount;
	int error;
	int vflush_flags;

	root_mount = (struct vmmfs_root_mount *)mount->mnt_data;
	if (root_mount == NULL)
		return 0;
	vflush_flags = (flags & MNT_FORCE) != 0 ? FORCECLOSE : 0;
	error = vflush(mount, 0, vflush_flags);
	if (error != 0)
		return error;
	if (!RB_EMPTY(&root_mount->machines))
		return EBUSY;
	vfs_rm_vnodeops(mount, NULL, &root_mount->machine_vops);
	lockuninit(&root_mount->lock);
	mount->mnt_data = NULL;
	kfree(root_mount, M_VMMFS);
	return 0;
}

int
vmmfs_root_vnode(struct mount *mount, struct vnode **vnode)
{
	struct vmmfs_root_mount *root_mount;
	struct vnode *root;
	int error;

	root_mount = (struct vmmfs_root_mount *)mount->mnt_data;
	if (root_mount == NULL)
		return ENXIO;
	lockmgr(&root_mount->lock, LK_EXCLUSIVE);
	root = root_mount->root;
	if (root != NULL) {
		vhold(root);
		lockmgr(&root_mount->lock, LK_RELEASE);
		error = vget(root, LK_EXCLUSIVE | LK_RETRY);
		vdrop(root);
		if (error != 0)
			return error;
		*vnode = root;
		return 0;
	}
	error = getnewvnode(VMMFS_ROOT_TAG, mount, &root, VLKTIMEOUT,
	    LK_CANRECURSE);
	if (error != 0) {
		lockmgr(&root_mount->lock, LK_RELEASE);
		return error;
	}
	root->v_type = VDIR;
	root->v_flag |= VROOT;
	root->v_data = root_mount;
	root_mount->root = root;
	lockmgr(&root_mount->lock, LK_RELEASE);
	vx_downgrade(root);
	*vnode = root;
	return 0;
}

static int
vmmfs_root_getattr(struct vop_getattr_args *ap)
{
	struct vattr *vattr;

	vattr = ap->a_vap;
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_ROOT_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = VMMFS_ROOT_INO;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_atime.tv_sec = 0;
	vattr->va_atime.tv_nsec = 0;
	vattr->va_mtime = vattr->va_atime;
	vattr->va_ctime = vattr->va_atime;
	vattr->va_gen = 1;
	vattr->va_flags = 0;
	vattr->va_bytes = 0;
	vattr->va_filerev = 0;
	return 0;
}

static int
vmmfs_root_getattr_lite(struct vop_getattr_lite_args *ap)
{
	struct vattr_lite *vattr;

	vattr = ap->a_lvap;
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_ROOT_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_size = 0;
	vattr->va_flags = 0;
	return 0;
}

static int
vmmfs_root_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_root_mount *root_mount;
	struct vmmfs_machine_name *name_entry;
	struct uio *uio;
	off_t offset;
	int error;
	int full;
	int index;
	int skipped;

	uio = ap->a_uio;
	if (ap->a_vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0)
		return EINVAL;
	offset = uio->uio_offset;
	error = 0;
	full = 0;
	skipped = 0;
	if (offset == 0) {
		if (vop_write_dirent(&error, uio, VMMFS_ROOT_INO, DT_DIR, 1,
		    ".")) {
			full = 1;
			goto done;
		}
		offset = 1;
	}
	if (offset == 1) {
		if (vop_write_dirent(&error, uio, VMMFS_ROOT_INO, DT_DIR, 2,
		    "..")) {
			full = 1;
			goto done;
		}
		offset = 2;
	}

	root_mount = (struct vmmfs_root_mount *)ap->a_vp->v_mount->mnt_data;
	index = (int)offset - 2;
	lockmgr(&root_mount->lock, LK_SHARED);
	RB_FOREACH(name_entry, vmmfs_machine_tree, &root_mount->machines) {
		if (skipped++ < index)
			continue;
		if (vop_write_dirent(&error, uio, VMMFS_MACHINE_INO, DT_DIR,
		    (uint16_t)strlen(name_entry->name), name_entry->name)) {
			full = 1;
			break;
		}
		offset++;
	}
	if (skipped <= index)
		offset = 2 + skipped;
	lockmgr(&root_mount->lock, LK_RELEASE);
done:
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !full;
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	return error;
}

static int
vmmfs_root_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_root_mount *root_mount;
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	struct namecache *ncp;
	int error;

	if ((ap->a_dvp->v_flag & VROOT) == 0) {
		cache_setvp(ap->a_nch, NULL);
		return ENOENT;
	}
	root_mount = (struct vmmfs_root_mount *)ap->a_dvp->v_mount->mnt_data;
	ncp = ap->a_nch->ncp;
	lockmgr(&root_mount->lock, LK_SHARED);
	machine = vmmfs_root_find(root_mount, ncp->nc_name, ncp->nc_nlen);
	if (machine == NULL) {
		lockmgr(&root_mount->lock, LK_RELEASE);
		cache_setvp(ap->a_nch, NULL);
		return ENOENT;
	}
	vnode = machine->vnode;
	vhold(vnode);
	lockmgr(&root_mount->lock, LK_RELEASE);
	error = vget(vnode, LK_EXCLUSIVE | LK_RETRY);
	vdrop(vnode);
	if (error != 0)
		return error;
	vn_unlock(vnode);
	cache_setvp(ap->a_nch, vnode);
	vrele(vnode);
	return 0;
}

static int
vmmfs_root_nmkdir(struct vop_nmkdir_args *ap)
{
	struct vmmfs_root_mount *root_mount;
	struct vmmfs_machine *machine;
	struct vmmfs_machine_name *name_entry;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	if ((ap->a_dvp->v_flag & VROOT) == 0)
		return EOPNOTSUPP;
	ncp = ap->a_nch->ncp;
	if (ncp->nc_nlen <= 0 || ncp->nc_nlen > VMMFS_MACHINE_NAME_MAX)
		return ENAMETOOLONG;
	root_mount = (struct vmmfs_root_mount *)ap->a_dvp->v_mount->mnt_data;
	lockmgr(&root_mount->lock, LK_EXCLUSIVE);
	if (vmmfs_root_find(root_mount, ncp->nc_name, ncp->nc_nlen) != NULL) {
		lockmgr(&root_mount->lock, LK_RELEASE);
		return EEXIST;
	}
	lockmgr(&root_mount->lock, LK_RELEASE);

	error = vmmfs_machine_create(ap->a_dvp->v_mount, ap->a_dvp,
	    &root_mount->machine_vops, ncp->nc_name, ncp->nc_nlen, &machine);
	if (error != 0)
		return error;
	lockmgr(&root_mount->lock, LK_EXCLUSIVE);
	if (vmmfs_root_find(root_mount, ncp->nc_name, ncp->nc_nlen) != NULL) {
		lockmgr(&root_mount->lock, LK_RELEASE);
		vmmfs_machine_free(machine);
		return EEXIST;
	}
	name_entry = machine->name_entry;
	RB_INSERT(vmmfs_machine_tree, &root_mount->machines, name_entry);
	lockmgr(&root_mount->lock, LK_RELEASE);
	vnode = machine->vnode;
	*ap->a_vpp = vnode;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
	return 0;
}

static int
vmmfs_root_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vmmfs_root_mount *root_mount;
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	struct namecache *ncp;
	int error;

	if ((ap->a_dvp->v_flag & VROOT) == 0)
		return EOPNOTSUPP;
	ncp = ap->a_nch->ncp;
	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vnode);
	if (error != 0)
		return error;
	vn_unlock(vnode);
	root_mount = (struct vmmfs_root_mount *)ap->a_dvp->v_mount->mnt_data;
	lockmgr(&root_mount->lock, LK_EXCLUSIVE);
	machine = vmmfs_root_find(root_mount, ncp->nc_name, ncp->nc_nlen);
	if (machine == NULL || machine->vnode != vnode) {
		lockmgr(&root_mount->lock, LK_RELEASE);
		vrele(vnode);
		return ENOENT;
	}
	RB_REMOVE(vmmfs_machine_tree, &root_mount->machines,
	    machine->name_entry);
	lockmgr(&root_mount->lock, LK_RELEASE);
	cache_inval_vp(vnode, CINV_DESTROY | CINV_CHILDREN);
	vn_gone(vnode);
	vrele(vnode);
	return 0;
}

static int
vmmfs_root_nrename(struct vop_nrename_args *ap)
{
	struct vmmfs_root_mount *root_mount;
	struct vmmfs_machine *machine;
	struct vnode *source_vnode;
	struct vnode *target_vnode;
	struct namecache *source_ncp;
	struct namecache *target_ncp;
	int error;

	if ((ap->a_fdvp->v_flag & VROOT) == 0 ||
	    (ap->a_tdvp->v_flag & VROOT) == 0)
		return EOPNOTSUPP;
	if (ap->a_fdvp != ap->a_tdvp ||
	    ap->a_fdvp->v_mount != ap->a_tdvp->v_mount)
		return EXDEV;
	source_ncp = ap->a_fnch->ncp;
	target_ncp = ap->a_tnch->ncp;
	if (target_ncp->nc_nlen <= 0 ||
	    target_ncp->nc_nlen > VMMFS_MACHINE_NAME_MAX)
		return ENAMETOOLONG;
	if (source_ncp->nc_nlen == target_ncp->nc_nlen &&
	    bcmp(source_ncp->nc_name, target_ncp->nc_name,
	    source_ncp->nc_nlen) == 0)
		return 0;

	error = cache_vget(ap->a_fnch, ap->a_cred, LK_SHARED, &source_vnode);
	if (error != 0)
		return error;
	vn_unlock(source_vnode);
	error = cache_vget(ap->a_tnch, ap->a_cred, LK_SHARED, &target_vnode);
	if (error == 0) {
		vn_unlock(target_vnode);
		vrele(target_vnode);
		return EEXIST;
	}
	if (error != ENOENT) {
		vrele(source_vnode);
		return error;
	}

	root_mount = (struct vmmfs_root_mount *)ap->a_fdvp->v_mount->mnt_data;
	lockmgr(&root_mount->lock, LK_EXCLUSIVE);
	machine = vmmfs_root_find(root_mount, source_ncp->nc_name,
	    source_ncp->nc_nlen);
	if (machine == NULL || machine->vnode != source_vnode) {
		lockmgr(&root_mount->lock, LK_RELEASE);
		vrele(source_vnode);
		return ENOENT;
	}
	if (vmmfs_root_find(root_mount, target_ncp->nc_name,
	    target_ncp->nc_nlen) != NULL) {
		lockmgr(&root_mount->lock, LK_RELEASE);
		vrele(source_vnode);
		return EEXIST;
	}
	RB_REMOVE(vmmfs_machine_tree, &root_mount->machines,
	    machine->name_entry);
	bcopy(target_ncp->nc_name, machine->name_entry->name,
	    target_ncp->nc_nlen);
	machine->name_entry->name[target_ncp->nc_nlen] = '\0';
	RB_INSERT(vmmfs_machine_tree, &root_mount->machines,
	    machine->name_entry);
	lockmgr(&root_mount->lock, LK_RELEASE);
	cache_rename(ap->a_fnch, ap->a_tnch);
	vrele(source_vnode);
	return 0;
}

static int
vmmfs_root_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	if (ap->a_dvp->v_type != VDIR)
		return ENOTDIR;
	vref(ap->a_dvp);
	*ap->a_vpp = ap->a_dvp;
	return 0;
}

static int
vmmfs_root_access(struct vop_access_args *ap)
{
	return vop_helper_access(ap, 0, 0, VMMFS_ROOT_MODE, 0);
}

static int
vmmfs_root_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_root_mount *root_mount;

	root_mount = ap->a_vp->v_data;
	if (root_mount != NULL) {
		lockmgr(&root_mount->lock, LK_EXCLUSIVE);
		if (root_mount->root == ap->a_vp)
			root_mount->root = NULL;
		lockmgr(&root_mount->lock, LK_RELEASE);
	}
	ap->a_vp->v_data = NULL;
	return 0;
}
