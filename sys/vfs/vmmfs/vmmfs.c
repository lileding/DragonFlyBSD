/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs filesystem module entry point.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_node.h"
#include "vmmfs_boot.h"
#include "vmmfs_events.h"
#include "vmmfs_loader.h"
#include "vmmfs_machine.h"
#include "vmmfs_memory.h"
#include "vmmfs_root.h"
#include "vmmfs_stopped.h"
#include "vmmfs_vcpu.h"
#include "vmmfs_machine_id.h"
#include "vmmfs_pciroot.h"
#include "vmmfs_pcislot.h"
#include "vmmfs_pcislot_config.h"
#include "vmmfs_pcislot_descriptor.h"
#include "vmmfs_pcislot_resource.h"
#include "vmmfs_pcislot_events.h"
#include "vmmfs_serialport.h"
#include "vmmfs_serialroot.h"

extern struct vop_ops vmmfs_root_vops;
extern struct vop_ops vmmfs_machine_vops;
extern struct vop_ops vmmfs_machine_id_vops;
extern struct vop_ops vmmfs_vcpu_vops;
extern struct vop_ops vmmfs_memory_vops;
extern struct vop_ops vmmfs_loader_vops;
extern struct vop_ops vmmfs_boot_vops;
extern struct vop_ops vmmfs_stopped_vops;
extern struct vop_ops vmmfs_events_vops;
extern struct vop_ops vmmfs_serialroot_vops;
extern struct vop_ops vmmfs_serialport_vops;
extern struct vop_ops vmmfs_pciroot_vops;
extern struct vop_ops vmmfs_pcislot_vops;
extern struct vop_ops vmmfs_pcislot_descriptor_vops;
extern struct vop_ops vmmfs_pcislot_config_vops;
extern struct vop_ops vmmfs_pcislot_resource_vops;
extern struct vop_ops vmmfs_pcislot_events_vops;

#include "vmmfs_launch.h"

static int vmmfs_mount(struct mount *, char *, caddr_t, struct ucred *);
static int vmmfs_ncreate(struct vop_ncreate_args *);
static int vmmfs_nmkdir(struct vop_nmkdir_args *);
static int vmmfs_nremove(struct vop_nremove_args *);
static int vmmfs_nresolve(struct vop_nresolve_args *);
static int vmmfs_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_vfs_uninit(struct vfsconf *);
static int vmmfs_unmount(struct mount *, int);
static int vmmfs_statfs(struct mount *, struct statfs *, struct ucred *);
static int vmmfs_root_vfs(struct mount *, struct vnode **);

/*
 * DragonFly dispatches namespace VOPs through mnt_vn_use_ops rather than the
 * parent vnode's v_ops.  Keep this mount vector as a thin trampoline so each
 * vmmfs object still owns the namespace methods in its own VOP vector.
 */
static struct vop_ops vmmfs_namespace_vops = {
	.vop_default = vop_defaultop,
	.vop_ncreate = vmmfs_ncreate,
	.vop_nmkdir = vmmfs_nmkdir,
	.vop_nremove = vmmfs_nremove,
	.vop_nresolve = vmmfs_nresolve,
	.vop_nrmdir = vmmfs_nrmdir,
};

static struct vfsops vmmfs_vfsops = {
	.vfs_flags = 0,
	.vfs_mount = vmmfs_mount,
	.vfs_unmount = vmmfs_unmount,
	.vfs_root = vmmfs_root_vfs,
	.vfs_statfs = vmmfs_statfs,
	.vfs_uninit = vmmfs_vfs_uninit,
};

static int
vmmfs_ncreate(struct vop_ncreate_args *ap)
{
	return ((*ap->a_dvp->v_ops)->vop_ncreate(ap));
}

static int
vmmfs_vfs_uninit(struct vfsconf *configuration)
{
	(void)configuration;
	return (vmmfs_root_module_fini());
}

static int
vmmfs_nmkdir(struct vop_nmkdir_args *ap)
{
	return ((*ap->a_dvp->v_ops)->vop_nmkdir(ap));
}

static int
vmmfs_nremove(struct vop_nremove_args *ap)
{
	return ((*ap->a_dvp->v_ops)->vop_nremove(ap));
}

static int
vmmfs_nresolve(struct vop_nresolve_args *ap)
{
	return ((*ap->a_dvp->v_ops)->vop_nresolve(ap));
}

static int
vmmfs_nrmdir(struct vop_nrmdir_args *ap)
{
	return ((*ap->a_dvp->v_ops)->vop_nrmdir(ap));
}

static int
vmmfs_root_vfs(struct mount *mount, struct vnode **vnode)
{
	struct vmmfs_mount *state;
	struct vnode *vp;
	int error;

	state = (struct vmmfs_mount *)mount->mnt_data;
	if (state == NULL)
		return (ENXIO);
	vp = (state->root != NULL ? state->root->vnode : NULL);
	if (vp == NULL)
		return (ENOENT);
	vhold(vp);
	error = vget(vp, LK_EXCLUSIVE | LK_RETRY);
	vdrop(vp);
	if (error != 0)
		return (error);
	*vnode = vp;
	return (0);
}

static int
vmmfs_mount(struct mount *mount, char *path, caddr_t data,
	struct ucred *cred)
{
	struct vmmfs_mount *state;
	struct vmmfs_node *root;
	size_t size;
	int error;

	(void)data;
	if ((mount->mnt_flag & MNT_UPDATE) != 0)
		return (EOPNOTSUPP);

	state = kmalloc(sizeof(*state), M_VMMFS, M_WAITOK | M_ZERO);
	state->mount = mount;
	mount->mnt_flag |= MNT_LOCAL;
	mount->mnt_kern_flag |= MNTK_NOSTKMNT | MNTK_ALL_MPSAFE;
	mount->mnt_data = (qaddr_t)state;
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
		goto fail;
	}
	vfs_add_vnodeops(mount, &vmmfs_namespace_vops,
	    &mount->mnt_vn_norm_ops);
	vfs_add_vnodeops(mount, &vmmfs_root_vops, &state->root_vops);
	vfs_add_vnodeops(mount, &vmmfs_machine_vops,
	    &state->machine_vops);
	vfs_add_vnodeops(mount, &vmmfs_machine_id_vops,
	    &state->machine_id_vops);
	vfs_add_vnodeops(mount, &vmmfs_vcpu_vops, &state->vcpu_vops);
	vfs_add_vnodeops(mount, &vmmfs_memory_vops, &state->memory_vops);
	vfs_add_vnodeops(mount, &vmmfs_loader_vops, &state->loader_vops);
	vfs_add_vnodeops(mount, &vmmfs_boot_vops, &state->boot_vops);
	vfs_add_vnodeops(mount, &vmmfs_launch_vops, &state->launch_vops);
	vfs_add_vnodeops(mount, &vmmfs_stopped_vops,
	    &state->stopped_vops);
	vfs_add_vnodeops(mount, &vmmfs_events_vops,
	    &state->events_vops);
	vfs_add_vnodeops(mount, &vmmfs_serialroot_vops,
	    &state->serialroot_vops);
	vfs_add_vnodeops(mount, &vmmfs_serialport_vops,
	    &state->serialport_vops);
	vfs_add_vnodeops(mount, &vmmfs_pciroot_vops,
	    &state->pciroot_vops);
	vfs_add_vnodeops(mount, &vmmfs_pcislot_vops,
	    &state->pcislot_vops);
	vfs_add_vnodeops(mount, &vmmfs_pcislot_descriptor_vops,
	    &state->pcislot_descriptor_vops);
	vfs_add_vnodeops(mount, &vmmfs_pcislot_config_vops,
	    &state->pcislot_config_vops);
	vfs_add_vnodeops(mount, &vmmfs_pcislot_resource_vops,
	    &state->pcislot_resource_vops);
	vfs_add_vnodeops(mount, &vmmfs_pcislot_events_vops,
	    &state->pcislot_events_vops);
	error = vmmfs_root_create(mount, &root);
	if (error != 0) {
		vfs_rm_vnodeops(mount, NULL, &state->launch_vops);
		vfs_rm_vnodeops(mount, NULL, &state->boot_vops);
		vfs_rm_vnodeops(mount, NULL, &state->pcislot_events_vops);
		vfs_rm_vnodeops(mount, NULL, &state->pcislot_resource_vops);
		vfs_rm_vnodeops(mount, NULL, &state->pcislot_config_vops);
		vfs_rm_vnodeops(mount, NULL, &state->pcislot_descriptor_vops);
		vfs_rm_vnodeops(mount, NULL, &state->pcislot_vops);
		vfs_rm_vnodeops(mount, NULL, &state->pciroot_vops);
		vfs_rm_vnodeops(mount, NULL, &state->serialport_vops);
		vfs_rm_vnodeops(mount, NULL, &state->serialroot_vops);
		vfs_rm_vnodeops(mount, NULL, &state->events_vops);
		vfs_rm_vnodeops(mount, NULL, &state->stopped_vops);
		vfs_rm_vnodeops(mount, NULL, &state->loader_vops);
		vfs_rm_vnodeops(mount, NULL, &state->memory_vops);
		vfs_rm_vnodeops(mount, NULL, &state->vcpu_vops);
		vfs_rm_vnodeops(mount, NULL, &state->machine_id_vops);
		vfs_rm_vnodeops(mount, NULL, &state->machine_vops);
		vfs_rm_vnodeops(mount, NULL, &state->root_vops);
		vfs_rm_vnodeops(mount, NULL, &mount->mnt_vn_norm_ops);
		mount->mnt_data = NULL;
		goto fail;
	}
	state->root = root;
	return (vmmfs_statfs(mount, &mount->mnt_stat, cred));

fail:
	kfree(state, M_VMMFS);
	return (error);
}

static int
vmmfs_unmount(struct mount *mount, int flags)
{
	struct vmmfs_mount *state;
	struct vnode *root_vnode;
	int error;

	state = (struct vmmfs_mount *)mount->mnt_data;
	if (state == NULL)
		return (ENXIO);
	root_vnode = (state->root != NULL ? state->root->vnode : NULL);
	if (root_vnode == NULL)
		return (ENXIO);
	error = vmmfs_vnode_deactivate(root_vnode);
	if (error != 0)
		return (error);
	/* root_create() retains the filesystem's base root-vnode reference. */
	error = vflush(mount, 1, (flags & MNT_FORCE) ? FORCECLOSE : 0);
	if (error != 0) {
		struct vmmfs_node *root = root_vnode->v_data;

		/* Root has no children or private teardown to roll back. */
		(void)lockmgr(&root->lock, LK_EXCLUSIVE);
		root->dead = false;
		(void)lockmgr(&root->lock, LK_RELEASE);
		return (error);
	}
	state->root = NULL;
	vfs_rm_vnodeops(mount, NULL, &state->pcislot_events_vops);
	vfs_rm_vnodeops(mount, NULL, &state->pcislot_resource_vops);
	vfs_rm_vnodeops(mount, NULL, &state->pcislot_config_vops);
	vfs_rm_vnodeops(mount, NULL, &state->pcislot_descriptor_vops);
	vfs_rm_vnodeops(mount, NULL, &state->pcislot_vops);
	vfs_rm_vnodeops(mount, NULL, &state->pciroot_vops);
	vfs_rm_vnodeops(mount, NULL, &state->serialport_vops);
	vfs_rm_vnodeops(mount, NULL, &state->serialroot_vops);
	vfs_rm_vnodeops(mount, NULL, &state->stopped_vops);
	vfs_rm_vnodeops(mount, NULL, &state->events_vops);
	vfs_rm_vnodeops(mount, NULL, &state->loader_vops);
	vfs_rm_vnodeops(mount, NULL, &state->launch_vops);
	vfs_rm_vnodeops(mount, NULL, &state->boot_vops);
	vfs_rm_vnodeops(mount, NULL, &state->memory_vops);
	vfs_rm_vnodeops(mount, NULL, &state->vcpu_vops);
	vfs_rm_vnodeops(mount, NULL, &state->machine_id_vops);
	vfs_rm_vnodeops(mount, NULL, &state->machine_vops);
	vfs_rm_vnodeops(mount, NULL, &state->root_vops);
	vfs_rm_vnodeops(mount, NULL, &mount->mnt_vn_norm_ops);
	mount->mnt_data = NULL;
	kfree(state, M_VMMFS);
	return (0);
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

VFS_SET(vmmfs_vfsops, vmmfs, VFCF_SYNTHETIC | VFCF_MPSAFE);
MODULE_VERSION(vmmfs, 1);
MODULE_DEPEND(vmmfs, vmm, 1, 1, 1);
