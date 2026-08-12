/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs filesystem module entry point.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include "vmmfs.h"

#define VMMFS_ROOT_MODE	0555

static int vmmfs_mount(struct mount *, char *, caddr_t, struct ucred *);
static int vmmfs_unmount(struct mount *, int);
static int vmmfs_root(struct mount *, struct vnode **);
static int vmmfs_statfs(struct mount *, struct statfs *, struct ucred *);
static int vmmfs_root_access(struct vop_access_args *);
static int vmmfs_root_getattr(struct vop_getattr_args *);
static int vmmfs_root_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_root_nmkdir(struct vop_nmkdir_args *);
static int vmmfs_root_nresolve(struct vop_nresolve_args *);
static int vmmfs_root_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_root_readdir(struct vop_readdir_args *);
static int vmmfs_root_reclaim(struct vop_reclaim_args *);

static struct vop_ops vmmfs_root_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_root_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_root_getattr,
	.vop_getattr_lite = vmmfs_root_getattr_lite,
	.vop_open = vop_stdopen,
	.vop_nmkdir = vmmfs_root_nmkdir,
	.vop_nresolve = vmmfs_root_nresolve,
	.vop_nrmdir = vmmfs_root_nrmdir,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_root_readdir,
	.vop_reclaim = vmmfs_root_reclaim,
};

static struct vfsops vmmfs_vfsops = {
	.vfs_flags = 0,
	.vfs_mount = vmmfs_mount,
	.vfs_unmount = vmmfs_unmount,
	.vfs_root = vmmfs_root,
	.vfs_statfs = vmmfs_statfs,
};

static int
vmmfs_mount(struct mount *mount, char *path, caddr_t data,
    struct ucred *cred)
{
	struct vmmfs_domain *domain;
	size_t size;
	int error;

	(void)data;
	if ((mount->mnt_flag & MNT_UPDATE) != 0)
		return (EOPNOTSUPP);

	error = vmmfs_domain_create(mount, &domain);
	if (error != 0)
		return (error);
	mount->mnt_flag |= MNT_LOCAL;
	mount->mnt_kern_flag |= MNTK_NOSTKMNT | MNTK_ALL_MPSAFE;
	mount->mnt_data = (qaddr_t)domain;
	vfs_getnewfsid(mount);

	size = sizeof("vmmfs") - 1;
	bcopy("vmmfs", mount->mnt_stat.f_mntfromname, size);
	bzero(mount->mnt_stat.f_mntfromname + size, MNAMELEN - size);
	bzero(mount->mnt_stat.f_mntonname,
	    sizeof(mount->mnt_stat.f_mntonname));
	error = copyinstr(path, mount->mnt_stat.f_mntonname,
	    sizeof(mount->mnt_stat.f_mntonname) - 1, &size);
	if (error != 0) {
		mount->mnt_data = NULL;
		vmmfs_domain_destroy(domain);
		return (error);
	}
	vfs_add_vnodeops(mount, &vmmfs_root_vops, &mount->mnt_vn_norm_ops);
	vfs_add_vnodeops(mount, &vmmfs_machine_vops, &domain->machine_vops);
	return (vmmfs_statfs(mount, &mount->mnt_stat, cred));
}

static int
vmmfs_unmount(struct mount *mount, int flags)
{
	struct vmmfs_domain *domain;
	int error;

	domain = (struct vmmfs_domain *)mount->mnt_data;
	if (domain == NULL)
		return (ENXIO);
	lwkt_gettoken(&domain->token);
	if (!RB_EMPTY(&domain->machines)) {
		lwkt_reltoken(&domain->token);
		return (EBUSY);
	}
	lwkt_reltoken(&domain->token);
	error = vflush(mount, 0, (flags & MNT_FORCE) ? FORCECLOSE : 0);
	if (error != 0)
		return (error);
	vfs_rm_vnodeops(mount, NULL, &domain->machine_vops);
	vfs_rm_vnodeops(mount, NULL, &mount->mnt_vn_norm_ops);
	mount->mnt_data = NULL;
	vmmfs_domain_destroy(domain);
	return (0);
}

static int
vmmfs_root(struct mount *mount, struct vnode **vnode)
{
	struct vmmfs_domain *domain;

	domain = (struct vmmfs_domain *)mount->mnt_data;
	if (domain == NULL)
		return (ENXIO);
	*vnode = domain->as_vnode(domain);
	return (*vnode != NULL ? 0 : ENOMEM);
}

static int
vmmfs_statfs(struct mount *mount, struct statfs *statfs,
    struct ucred *cred)
{
	(void)cred;

	statfs->f_bsize = PAGE_SIZE;
	statfs->f_iosize = PAGE_SIZE;
	statfs->f_blocks = 1;
	statfs->f_bfree = 0;
	statfs->f_bavail = 0;
	statfs->f_files = 1;
	statfs->f_ffree = 0;
	if (statfs != &mount->mnt_stat) {
		statfs->f_type = mount->mnt_vfc->vfc_typenum;
		bcopy(&mount->mnt_stat.f_fsid, &statfs->f_fsid,
		    sizeof(statfs->f_fsid));
		bcopy(mount->mnt_stat.f_mntfromname, statfs->f_mntfromname,
		    MNAMELEN);
	}
	return (0);
}

static int
vmmfs_root_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_ROOT_MODE, 0));
}

static int
vmmfs_root_getattr(struct vop_getattr_args *ap)
{
	struct vattr *vattr;

	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_ROOT_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = VMMFS_ROOT_INO;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
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
	return (0);
}

static int
vmmfs_root_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_domain *domain;
	struct vmmfs_item item;
	struct uio *uio;
	off_t offset;
	uint64_t index;
	int error;
	int stop;

	if (ap->a_vp->v_type != VDIR)
		return (ENOTDIR);
	domain = ap->a_vp->v_data;
	if (domain == NULL)
		return (ENXIO);
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
		stop = vop_write_dirent(&error, uio, VMMFS_ROOT_INO, DT_DIR, 1,
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
	index = offset - 2;
	while (!stop) {
		error = domain->read_item(domain, index, &item);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0)
			break;
		stop = vop_write_dirent(&error, uio, item.id, DT_DIR,
		    (uint16_t)strlen(item.name), item.name);
		if (!stop) {
			offset++;
			index++;
		}
	}
	uio->uio_offset = offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !stop && error == 0;
	return (error);
}

static int
vmmfs_root_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_domain *domain;
	struct vmmfs_machine *machine;
	struct vmmfs_item item;
	struct vnode *vnode;
	struct namecache *ncp;
	uint64_t index;
	int error;

	domain = ap->a_dvp->v_data;
	if (domain == NULL)
		return (ENXIO);
	ncp = ap->a_nch->ncp;
	machine = NULL;
	for (index = 0;; index++) {
		error = domain->read_item(domain, index, &item);
		if (error != 0)
			break;
		if (ncp->nc_nlen != strlen(item.name))
			continue;
		if (strncmp(ncp->nc_name, item.name, ncp->nc_nlen) == 0) {
			machine = item.machine;
			break;
		}
	}
	if (machine == NULL) {
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	vnode = machine->as_vnode(machine);
	if (vnode == NULL)
		return (ENOMEM);
	vn_unlock(vnode);
	cache_setvp(ap->a_nch, vnode);
	vrele(vnode);
	return (0);
}

static int
vmmfs_root_nmkdir(struct vop_nmkdir_args *ap)
{
	struct vmmfs_domain *domain;
	struct vmmfs_machine *machine;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	domain = ap->a_dvp->v_data;
	if (domain == NULL)
		return (ENXIO);
	if (ap->a_vap->va_type != VDIR)
		return (EINVAL);
	ncp = ap->a_nch->ncp;
	error = domain->create_item(domain, ncp->nc_name, ncp->nc_nlen,
	    &machine);
	if (error != 0)
		return (error);
	vnode = machine->as_vnode(machine);
	if (vnode == NULL) {
		(void)domain->remove_item(domain, machine);
		vmmfs_machine_destroy(machine);
		return (ENOMEM);
	}
	*ap->a_vpp = vnode;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
	return (0);
}

static int
vmmfs_root_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vmmfs_domain *domain;
	struct vnode *vnode;
	struct vmmfs_machine *machine;
	int error;

	domain = ap->a_dvp->v_data;
	if (domain == NULL)
		return (ENXIO);
	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vnode);
	if (error != 0)
		return (error);
	vn_unlock(vnode);
	if (vnode->v_type != VDIR) {
		vrele(vnode);
		return (ENOTDIR);
	}
	machine = vnode->v_data;
	if (machine == NULL) {
		vrele(vnode);
		return (ENOENT);
	}
	error = domain->remove_item(domain, machine);
	if (error == 0)
		cache_inval_vp(vnode, CINV_DESTROY | CINV_CHILDREN);
	vrele(vnode);
	return (error);
}

static int
vmmfs_root_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_domain *domain;

	domain = ap->a_vp->v_data;
	if (domain != NULL) {
		lwkt_gettoken(&domain->token);
		if (domain->vnode == ap->a_vp)
			domain->vnode = NULL;
		lwkt_reltoken(&domain->token);
	}
	ap->a_vp->v_data = NULL;
	return (0);
}

VFS_SET(vmmfs_vfsops, vmmfs, VFCF_SYNTHETIC | VFCF_MPSAFE);
MODULE_VERSION(vmmfs, 1);
MODULE_DEPEND(vmmfs, vmm, 1, 1, 1);
