/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Vnode and identity lifecycle for one vmmfs machine directory.
 */
#include <sys/param.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "vmmfs_machine.h"

#define VMMFS_MACHINE_TAG VT_UNUSED6
#define VMMFS_MACHINE_INO 2
#define VMMFS_MACHINE_MODE 0555

MALLOC_DECLARE(M_VMMFS);

static int vmmfs_machine_getattr(struct vop_getattr_args *);
static int vmmfs_machine_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_machine_readdir(struct vop_readdir_args *);
static int vmmfs_machine_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_machine_access(struct vop_access_args *);
static int vmmfs_machine_reclaim(struct vop_reclaim_args *);

struct vop_ops vmmfs_machine_vops = {
	.vop_default = vop_defaultop,
	.vop_open = vop_stdopen,
	.vop_close = vop_stdclose,
	.vop_nlookupdotdot = vmmfs_machine_nlookupdotdot,
	.vop_access = vmmfs_machine_access,
	.vop_getattr = vmmfs_machine_getattr,
	.vop_getattr_lite = vmmfs_machine_getattr_lite,
	.vop_readdir = vmmfs_machine_readdir,
	.vop_reclaim = vmmfs_machine_reclaim,
};

int
vmmfs_machine_name_cmp(struct vmmfs_machine_name *left,
    struct vmmfs_machine_name *right)
{
	return strcmp(left->name, right->name);
}

RB_GENERATE(vmmfs_machine_tree, vmmfs_machine_name, entry,
	vmmfs_machine_name_cmp);

int
vmmfs_machine_create(struct mount *mount, struct vnode *parent,
    struct vop_ops **machine_vops, const char *name, int name_len,
    struct vmmfs_machine **machinep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_machine_name *name_entry;
	struct vnode *vnode;
	int error;

	if (name_len <= 0 || name_len > VMMFS_MACHINE_NAME_MAX)
		return EINVAL;
	KKASSERT(machine_vops != NULL);

	machine = kmalloc(sizeof(*machine), M_VMMFS, M_WAITOK | M_ZERO);
	name_entry = kmalloc(sizeof(*name_entry), M_VMMFS, M_WAITOK | M_ZERO);
	machine->mount = mount;
	machine->parent = parent;
	name_entry->machine = machine;
	bcopy(name, name_entry->name, name_len);
	name_entry->name[name_len] = '\0';
	machine->name_entry = name_entry;

	error = getnewvnode(VMMFS_MACHINE_TAG, mount, &vnode, VLKTIMEOUT,
	    LK_CANRECURSE);
	if (error != 0) {
		kfree(name_entry, M_VMMFS);
		kfree(machine, M_VMMFS);
		return error;
	}
	vnode->v_ops = machine_vops;
	vnode->v_type = VDIR;
	vnode->v_data = machine;
	machine->vnode = vnode;
	vx_downgrade(vnode);
	*machinep = machine;
	return 0;
}

void
vmmfs_machine_free(struct vmmfs_machine *machine)
{
	struct vmmfs_machine_name *name_entry;
	struct vnode *vnode;

	name_entry = machine->name_entry;
	machine->name_entry = NULL;
	vnode = machine->vnode;
	if (vnode != NULL) {
		machine->vnode = NULL;
		vnode->v_data = NULL;
		vn_gone(vnode);
		vrele(vnode);
	}
	kfree(name_entry, M_VMMFS);
	kfree(machine, M_VMMFS);
}

static int
vmmfs_machine_getattr(struct vop_getattr_args *ap)
{
	struct vattr *vattr;

	vattr = ap->a_vap;
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_MACHINE_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = VMMFS_MACHINE_INO;
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
	return 0;
}

static int
vmmfs_machine_readdir(struct vop_readdir_args *ap)
{
	struct uio *uio;
	off_t offset;
	int error;
	int full;

	uio = ap->a_uio;
	if (ap->a_vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0)
		return EINVAL;
	offset = uio->uio_offset;
	error = 0;
	full = 0;
	if (offset == 0) {
		if (vop_write_dirent(&error, uio, VMMFS_MACHINE_INO, DT_DIR, 1,
		    ".")) {
			full = 1;
			goto done;
		}
		offset = 1;
	}
	if (offset == 1) {
		if (vop_write_dirent(&error, uio, VMMFS_MACHINE_INO, DT_DIR, 2,
		    "..")) {
			full = 1;
			goto done;
		}
		offset = 2;
	}
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
vmmfs_machine_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_machine *machine;

	if (ap->a_dvp->v_type != VDIR)
		return ENOTDIR;
	machine = ap->a_dvp->v_data;
	if (machine == NULL || machine->parent == NULL)
		return ENOENT;
	vref(machine->parent);
	*ap->a_vpp = machine->parent;
	return 0;
}

static int
vmmfs_machine_access(struct vop_access_args *ap)
{
	return vop_helper_access(ap, 0, 0, VMMFS_MACHINE_MODE, 0);
}

static int
vmmfs_machine_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_machine *machine;

	machine = ap->a_vp->v_data;
	if (machine != NULL) {
		machine->vnode = NULL;
		ap->a_vp->v_data = NULL;
		kfree(machine->name_entry, M_VMMFS);
		kfree(machine, M_VMMFS);
	}
	return 0;
}
