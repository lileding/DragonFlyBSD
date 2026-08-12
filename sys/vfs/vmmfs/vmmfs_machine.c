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

static struct vnode *vmmfs_machine_vnode(struct vmmfs_machine *);
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
vmmfs_machine_create(struct vmmfs_domain *domain, const char *name,
	size_t namelen)
{
	struct vmmfs_machine *machine;

	if (namelen == 0 || namelen > NAME_MAX)
		return (NULL);
	machine = kmalloc(sizeof(*machine), M_VMMFS, M_WAITOK | M_ZERO);
	machine->domain = domain;
	machine->as_vnode = vmmfs_machine_vnode;
	bcopy(name, machine->name, namelen);
	machine->name[namelen] = '\0';
	lwkt_token_init(&machine->spec_token, "vmmfsmachine");
	return (machine);
}

void
vmmfs_machine_destroy(struct vmmfs_machine *machine)
{
	KKASSERT(machine->domain == NULL);
	KKASSERT(machine->vnode == NULL);
	lwkt_token_uninit(&machine->spec_token);
	kfree(machine, M_VMMFS);
}

static struct vnode *
vmmfs_machine_vnode(struct vmmfs_machine *machine)
{
	struct vmmfs_domain *domain;
	struct vnode *vnode;
	int detached;
	int error;

retry:
	lwkt_gettoken(&machine->spec_token);
	vnode = machine->vnode;
	if (vnode != NULL) {
		vhold(vnode);
		lwkt_reltoken(&machine->spec_token);
		error = vget(vnode, LK_EXCLUSIVE | LK_RETRY);
		vdrop(vnode);
		if (error == 0)
			return (vnode);
		if (error != ENOENT)
			return (NULL);
		goto retry;
	}
	domain = machine->domain;
	if (domain == NULL) {
		lwkt_reltoken(&machine->spec_token);
		return (NULL);
	}
	lwkt_reltoken(&machine->spec_token);

	error = getnewvnode(VT_SYNTH, domain->mount, &vnode, 0, 0);
	if (error != 0)
		return (NULL);

	lwkt_gettoken(&machine->spec_token);
	detached = machine->domain == NULL;
	if (machine->vnode != NULL || detached) {
		vnode->v_type = VBAD;
		vx_put(vnode);
		lwkt_reltoken(&machine->spec_token);
		if (detached)
			return (NULL);
		goto retry;
	}
	vnode->v_data = machine;
	vnode->v_ops = &domain->machine_vops;
	vnode->v_type = VDIR;
	machine->vnode = vnode;
	lwkt_reltoken(&machine->spec_token);
	vx_downgrade(vnode);
	return (vnode);
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
	struct vmmfs_domain *domain;
	struct vnode *vnode;

	machine = ap->a_dvp->v_data;
	domain = (struct vmmfs_domain *)ap->a_dvp->v_mount->mnt_data;
	if (machine == NULL || domain == NULL)
		return (ENOENT);
	lwkt_gettoken(&machine->spec_token);
	if (machine->domain != domain) {
		lwkt_reltoken(&machine->spec_token);
		return (ENOENT);
	}
	lwkt_reltoken(&machine->spec_token);
	vnode = domain->as_vnode(domain);
	if (vnode == NULL)
		return (ENOMEM);
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
	lwkt_gettoken(&machine->spec_token);
	if (machine->vnode == ap->a_vp)
		machine->vnode = NULL;
	destroy = machine->domain == NULL;
	lwkt_reltoken(&machine->spec_token);
	ap->a_vp->v_data = NULL;
	if (destroy)
		vmmfs_machine_destroy(machine);
	return (0);
}
