/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs vnode operations: the vop_ops handlers + table.  The data model,
 * registry, vnode allocation, and per-open buffers live in vmmfs.c; vmmfs.h is
 * the shared interface.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namecache.h>
#include <sys/nlookup.h>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/queue.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmmfs.h"
#include "vmm_node_if.h"

static int
vmmnode_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child = NULL;
	struct vnode *vp = NULL;
	int error;

	if (dnode->vn_type == VMMFS_NROOT) {
		if (ncp->nc_nlen == 8 && bcmp(ncp->nc_name, "machines", 8) == 0)
			child = &vmp->vm_machines;
		else if (ncp->nc_nlen == 7 &&
		    bcmp(ncp->nc_name, "devices", 7) == 0)
			child = &vmp->vm_devroot;
	} else if (dnode->vn_type == VMMFS_NMACHINES) {
		if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0) {
			child = &vmp->vm_host;
		} else {
			struct vmmfs_machine *m;

			lockmgr(&vmp->vm_lock, LK_SHARED);
			m = vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen);
			if (m != NULL)
				child = &m->node;
			lockmgr(&vmp->vm_lock, LK_RELEASE);
		}
	} else if (dnode->vn_type == VMMFS_NHOST) {
		if (ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "devices", 7) == 0)
			child = &vmp->vm_host_devices;
	} else if (dnode->vn_type == VMMFS_NMACHINE) {
		struct vmmfs_machine *m = dnode->vn_machine;
		int i;

		if (ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "devices", 7) == 0) {
			child = &m->vn_devices;
		} else {
			for (i = 0; i < VMMFS_NCFG; i++) {
				if (!vmmfs_cfg_present(m, i))
					continue;
				if ((int)strlen(vmmfs_cfg_name[i]) ==
				    ncp->nc_nlen &&
				    bcmp(vmmfs_cfg_name[i], ncp->nc_name,
				    ncp->nc_nlen) == 0) {
					child = &m->cfg[i];
					break;
				}
			}
		}
	} else if (dnode->vn_type == VMMFS_NDEVICES) {
		struct vmmfs_device *d;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		d = vmmfs_find_device(vmp, dnode->vn_owner, ncp->nc_name,
		    ncp->nc_nlen);
		if (d != NULL)
			child = &d->node;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
	} else if (dnode->vn_type == VMMFS_NDEVROOT) {
		struct vmmfs_device *d;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		d = vmmfs_find_device_any(vmp, ncp->nc_name, ncp->nc_nlen);
		if (d != NULL)
			child = &d->link;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
	}

	if (child == NULL) {
		cache_setvp(ap->a_nch, NULL);
		return ENOENT;
	}

	error = vmmfs_alloc_vp(dvp->v_mount, child, LK_EXCLUSIVE | LK_RETRY,
	    &vp);
	if (error)
		return error;

	vn_unlock(vp);
	cache_setvp(ap->a_nch, vp);
	vrele(vp);
	return 0;
}

static int
vmmnode_nlookupdotdot(struct vmmfs_node *dnode, struct vop_nlookupdotdot_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	int error;

	*vpp = NULL;

	error = VOP_ACCESS(dvp, VEXEC, ap->a_cred);
	if (error)
		return error;

	if (dnode->vn_parent != NULL) {
		error = vmmfs_alloc_vp(dvp->v_mount, dnode->vn_parent,
		    LK_EXCLUSIVE | LK_RETRY, vpp);
		if (*vpp != NULL)
			vn_unlock(*vpp);
	}

	return (*vpp == NULL) ? ENOENT : 0;
}

/*
 * `mkdir machines/<name>` creates a machine: always stopped, empty config.
 * The user then writes vcpu/mem/loader and `rm stopped` to start.
 */
static int
vmmnode_nmkdir(struct vmmfs_node *dnode, struct vop_nmkdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	if (dnode->vn_type != VMMFS_NMACHINES)
		return EPERM;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen > VMMFS_NAME_MAX)
		return ENAMETOOLONG;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0)
		return EEXIST;	/* host is reserved */

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen) != NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EEXIST;
	}
	m = vmmfs_alloc_slot(vmp);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return ENOSPC;
	}
	bcopy(ncp->nc_name, m->name, ncp->nc_nlen);
	m->name[ncp->nc_nlen] = '\0';
	vmm_machine_init(&m->state);
	m->in_use = 1;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	error = vmmfs_alloc_vp(dvp->v_mount, &m->node, LK_EXCLUSIVE | LK_RETRY,
	    &vp);
	if (error) {
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		m->in_use = 0;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return error;
	}

	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	return 0;
}

/*
 * `rmdir machines/<name>` removes a stopped machine (source 1): it deletes
 * regardless of leases.  The Rust state is freed lazily (vmmfs_alloc_slot) so
 * any still-open fds keep working until reclaimed.
 */
static int
vmmnode_nrmdir(struct vmmfs_node *dnode, struct vop_nrmdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	if (dnode->vn_type != VMMFS_NMACHINES)
		return EINVAL;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0)
		return EPERM;	/* host is not removable */

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m = vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	if (!vmm_machine_is_stopped(&m->state)) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return EBUSY;
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	vmmfs_machine_mark_deleted(vmp, m);
	cache_inval_vp(vp, CINV_DESTROY | CINV_CHILDREN);
	vrele(vp);
	return 0;
}

/*
 * Create "stopped" under a machine directory: an atomic, idempotent request to
 * stop the machine.  `echo apic > stopped` opens with O_CREAT.
 */
static int
vmmnode_ncreate(struct vmmfs_node *dnode, struct vop_ncreate_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	if (dnode->vn_type != VMMFS_NMACHINE)
		return EPERM;
	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;

	m = dnode->vn_machine;
	vmm_machine_stop(&m->state, 0);

	error = vmmfs_alloc_vp(dvp->v_mount, &m->cfg[VMMFS_CFG_STOPPED],
	    LK_EXCLUSIVE | LK_RETRY, &vp);
	if (error)
		return error;

	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	return 0;
}

/*
 * `rm <name>/devices/<dev>` unbinds a device.  A host device returns to the
 * host pool; a user backend is deleted (unloaded).  Removing from host/devices/
 * itself is refused (the host pool is fixed).
 */
static int
vmmfs_nremove_device(struct vop_nremove_args *ap, struct vmmfs_node *dnode)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_dvp->v_mount);
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_device *d;
	struct vnode *vp;
	int error;

	if (dnode->vn_owner == VMMFS_OWNER_HOST)
		return EPERM;

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	d = vmmfs_find_device(vmp, dnode->vn_owner, ncp->nc_name, ncp->nc_nlen);
	if (d == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	if (d->is_host)
		d->owner = VMMFS_OWNER_HOST;	/* unbind: back to host pool */
	else
		d->in_use = 0;			/* backend: unload */
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	cache_unlink(ap->a_nch);
	vrele(vp);
	return 0;
}

/*
 * `rm machines/<name>/stopped` is an atomic request to start the machine.  The
 * config must be complete and the loader path executable; otherwise the start
 * fails and the machine stays stopped.  Only "stopped" is removable.
 */
static int
vmmnode_nremove(struct vmmfs_node *dnode, struct vop_nremove_args *ap)
{
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	if (dnode->vn_type == VMMFS_NDEVICES)
		return vmmfs_nremove_device(ap, dnode);
	if (dnode->vn_type != VMMFS_NMACHINE)
		return EPERM;
	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;

	m = dnode->vn_machine;
	if (!vmm_machine_is_stopped(&m->state))
		return ENOENT;

	if (!vmm_machine_config_complete(&m->state))
		return EINVAL;
	error = vmmfs_validate_loader(m, ap->a_cred);
	if (error)
		return error;

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	vmm_machine_start(&m->state);

	cache_unlink(ap->a_nch);
	vrele(vp);
	return 0;
}

/*
 * `mv <devices>/<dev> <devices>/` rebinds a device: it changes which machine
 * owns it.  Both sides must be devices/ directories; the BDF name is unchanged.
 * Desired-state semantics.  (cp is impossible: devices/ rejects file creation.)
 */
static int
vmmnode_nrename(struct vmmfs_node *fdnode, struct vop_nrename_args *ap)
{
	struct namecache *fncp = ap->a_fnch->ncp;
	struct namecache *tncp = ap->a_tnch->ncp;
	struct vmmfs_node *tdnode = VP_TO_VMMFS(ap->a_tdvp);
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_fdvp->v_mount);
	struct vmmfs_device *d;

	if (fdnode->vn_type != VMMFS_NDEVICES ||
	    tdnode->vn_type != VMMFS_NDEVICES)
		return EXDEV;
	if (fncp->nc_nlen != tncp->nc_nlen ||
	    bcmp(fncp->nc_name, tncp->nc_name, fncp->nc_nlen) != 0)
		return EINVAL;	/* a device keeps its BDF name */

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	d = vmmfs_find_device(vmp, fdnode->vn_owner, fncp->nc_name,
	    fncp->nc_nlen);
	if (d == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return ENOENT;
	}
	if (fdnode->vn_owner == tdnode->vn_owner) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return 0;	/* no-op rebind */
	}
	if (vmmfs_find_device(vmp, tdnode->vn_owner, tncp->nc_name,
	    tncp->nc_nlen) != NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EEXIST;
	}
	d->owner = tdnode->vn_owner;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	cache_rename(ap->a_fnch, ap->a_tnch);
	return 0;
}

static int
vmmnode_open(struct vmmfs_node *node, struct vop_open_args *ap)
{

	/*
	 * Opening the lease takes a reference; refuse once deletion has begun.
	 * Register files need no per-open work here: the scratch buffer is
	 * created lazily on first write, and reads fall back to the current
	 * value, so a read-only open allocates nothing.
	 */
	if (node->vn_type == VMMFS_NCONFIG && node->vn_cfg == VMMFS_CFG_LEASE) {
		if (vmm_machine_lease_open(&node->vn_machine->state) == 0)
			return ENXIO;
	}
	return vop_stdopen(ap);
}

static int
vmmnode_close(struct vmmfs_node *node, struct vop_close_args *ap)
{
	int error;

	/* Commit a register's open buffer before the fd goes away. */
	if (node->vn_type == VMMFS_NCONFIG &&
	    vmmfs_cfg_is_register(node->vn_cfg))
		vmmfs_obuf_commit_close(node, ap->a_fp);

	error = vop_stdclose(ap);

	/* Releasing the last lease of an armed machine destroys it (source 3). */
	if (node->vn_type == VMMFS_NCONFIG && node->vn_cfg == VMMFS_CFG_LEASE) {
		if (vmm_machine_lease_close(&node->vn_machine->state)) {
			struct vmmfs_mount *vmp =
			    VFS_TO_VMMFS(ap->a_vp->v_mount);

			vmmfs_machine_mark_deleted(vmp, node->vn_machine);
		}
	}
	return error;
}

static int
vmmnode_access(struct vmmfs_node *node, struct vop_access_args *ap)
{

	return vop_helper_access(ap, 0, 0, node->vn_mode, 0);
}

static int
vmmnode_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	int is_file = (node->vn_type == VMMFS_NCONFIG ||
	    node->vn_type == VMMFS_NDEVICE);
	int is_link = (node->vn_type == VMMFS_NDEVLINK);
	uint8_t tmp[300];

	vap->va_type = is_link ? VLNK : (is_file ? VREG : VDIR);
	vap->va_mode = node->vn_mode;
	vap->va_nlink = (is_file || is_link) ? 1 :
	    ((node->vn_type == VMMFS_NROOT) ? 3 : 2);
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = node->vn_ino;
	if (node->vn_type == VMMFS_NCONFIG &&
	    vmmfs_cfg_is_register(node->vn_cfg)) {
		vap->va_size = vmmfs_cfg_text(node, tmp, sizeof(tmp));
	} else if (node->vn_type == VMMFS_NDEVICE) {
		vap->va_size = vmmfs_device_format(VMMFS_DEV_OF_NODE(node),
		    (char *)tmp, sizeof(tmp));
	} else if (is_link) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
		int len;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		len = vmmfs_devlink_target(vmp, VMMFS_DEV_OF_LINK(node),
		    (char *)tmp, sizeof(tmp));
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vap->va_size = (len < 0) ? 0 : len;
	} else {
		vap->va_size = 0;
	}
	vap->va_blocksize = PAGE_SIZE;
	vap->va_atime.tv_sec = 0;
	vap->va_atime.tv_nsec = 0;
	vap->va_mtime = vap->va_atime;
	vap->va_ctime = vap->va_atime;
	vap->va_gen = 1;
	vap->va_flags = 0;
	vap->va_bytes = 0;
	vap->va_filerev = 0;

	return 0;
}

/*
 * Accept no-op size changes (the O_TRUNC from `echo ... > file`) on writable
 * config files; everything else is read-only.
 */
static int
vmmnode_setattr(struct vmmfs_node *node, struct vop_setattr_args *ap)
{

	if (node->vn_type == VMMFS_NCONFIG && (node->vn_mode & 0200))
		return 0;
	return EPERM;
}

static int
vmmnode_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;

	if (vp->v_type != VREG)
		return EINVAL;

	if (node->vn_type == VMMFS_NDEVICE) {
		struct vmmfs_device *d = VMMFS_DEV_OF_NODE(node);
		char dbuf[64];
		int len;
		off_t off;

		if (uio->uio_offset < 0)
			return EINVAL;
		len = vmmfs_device_format(d, dbuf, sizeof(dbuf));
		off = uio->uio_offset;
		if (off >= len)
			return 0;
		return uiomove(dbuf + off, (size_t)(len - off), uio);
	}

	if (node->vn_type != VMMFS_NCONFIG)
		return EINVAL;

	/*
	 * events is a stream: each read drains and consumes the queued event
	 * lines (shared one-shot cursor).  Non-blocking; blocking read and
	 * kqueue are a later step.
	 */
	if (node->vn_cfg == VMMFS_CFG_EVENTS) {
		uint8_t ebuf[256];
		size_t n;

		n = vmm_machine_read_events(&node->vn_machine->state, ebuf,
		    sizeof(ebuf));
		if (n == 0)
			return 0;
		return uiomove(ebuf, n, uio);
	}

	if (vmmfs_cfg_is_register(node->vn_cfg))
		return vmmfs_obuf_read(node, ap->a_fp, uio);

	/* console / status.tar.gz / lease / stopped: empty for now. */
	return 0;
}

static int
vmmnode_write(struct vmmfs_node *node, struct vop_write_args *ap)
{
	struct uio *uio = ap->a_uio;
	char buf[16];
	size_t take;
	int error, force;

	if (node->vn_type != VMMFS_NCONFIG)
		return EPERM;

	if (vmmfs_cfg_is_register(node->vn_cfg))
		return vmmfs_obuf_write(node, ap->a_fp, uio);

	if (node->vn_cfg == VMMFS_CFG_CONSOLE) {
		/* Console input is a stub: accept and discard. */
		while (uio->uio_resid > 0) {
			char dump[64];
			size_t d = (uio->uio_resid < (int)sizeof(dump)) ?
			    (size_t)uio->uio_resid : sizeof(dump);

			error = uiomove(dump, d, uio);
			if (error)
				return error;
		}
		return 0;
	}

	if (node->vn_cfg != VMMFS_CFG_STOPPED)
		return EPERM;

	/*
	 * Writing the stopped control file selects the stop method (apic|force)
	 * and (re)applies the stop.  Idempotent.
	 */
	take = (uio->uio_resid < (int)(sizeof(buf) - 1)) ?
	    (size_t)uio->uio_resid : sizeof(buf) - 1;
	error = uiomove(buf, take, uio);
	if (error)
		return error;
	buf[take] = '\0';
	force = (take >= 5 && strncmp(buf, "force", 5) == 0);

	while (uio->uio_resid > 0) {
		char dump[32];
		size_t d = (uio->uio_resid < (int)sizeof(dump)) ?
		    (size_t)uio->uio_resid : sizeof(dump);

		error = uiomove(dump, d, uio);
		if (error)
			return error;
	}

	vmm_machine_stop(&node->vn_machine->state, force);
	return 0;
}

/* A device-index symlink resolves to its owner's devices/ entry. */
static int
vmmnode_readlink(struct vmmfs_node *node, struct vop_readlink_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
	char buf[128];
	int len;

	if (node->vn_type != VMMFS_NDEVLINK)
		return EINVAL;
	lockmgr(&vmp->vm_lock, LK_SHARED);
	len = vmmfs_devlink_target(vmp, VMMFS_DEV_OF_LINK(node), buf,
	    sizeof(buf));
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (len < 0)
		return ENOENT;
	return uiomove(buf, (size_t)len, ap->a_uio);
}

static int
vmmnode_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	int error = 0;
	int r;
	int full = 0;
	off_t off;

	if (vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0)
		return EINVAL;

	off = uio->uio_offset;

	if (off == 0) {
		r = vop_write_dirent(&error, uio, node->vn_ino, DT_DIR, 1, ".");
		if (r) {
			full = 1;
			goto done;
		}
		off = 1;
	}
	if (off == 1) {
		r = vop_write_dirent(&error, uio, vmmfs_parent_ino(node),
		    DT_DIR, 2, "..");
		if (r) {
			full = 1;
			goto done;
		}
		off = 2;
	}

	if (node->vn_type == VMMFS_NROOT) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);

		if (off == 2) {
			r = vop_write_dirent(&error, uio,
			    vmp->vm_machines.vn_ino, DT_DIR, 8, "machines");
			if (r) {
				full = 1;
				goto done;
			}
			off = 3;
		}
		if (off == 3) {
			r = vop_write_dirent(&error, uio, vmp->vm_devroot.vn_ino,
			    DT_DIR, 7, "devices");
			if (r) {
				full = 1;
				goto done;
			}
			off = 4;
		}
	} else if (node->vn_type == VMMFS_NMACHINES) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
		int i;

		/* host is always the first entry. */
		if (off == 2) {
			r = vop_write_dirent(&error, uio, vmp->vm_host.vn_ino,
			    DT_DIR, 4, "host");
			if (r) {
				full = 1;
				goto done;
			}
			off = 3;
		}
		lockmgr(&vmp->vm_lock, LK_SHARED);
		for (i = (int)off - 3; i < VMMFS_MAX_MACHINES; i++) {
			struct vmmfs_machine *m = &vmp->vm_mach[i];

			if (!m->in_use)
				continue;
			r = vop_write_dirent(&error, uio, m->node.vn_ino,
			    DT_DIR, (uint16_t)strlen(m->name), m->name);
			if (r) {
				off = 3 + i;
				full = 1;
				break;
			}
			off = 3 + i + 1;
		}
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		if (!full && off < 3 + VMMFS_MAX_MACHINES)
			off = 3 + VMMFS_MAX_MACHINES;
	} else if (node->vn_type == VMMFS_NHOST) {
		if (off == 2) {
			struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);

			r = vop_write_dirent(&error, uio,
			    vmp->vm_host_devices.vn_ino, DT_DIR, 7, "devices");
			if (r) {
				full = 1;
				goto done;
			}
			off = 3;
		}
	} else if (node->vn_type == VMMFS_NDEVICES) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
		int i;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		for (i = (int)off - 2; i < VMMFS_MAX_DEVICES; i++) {
			struct vmmfs_device *d = &vmp->vm_dev[i];

			if (!d->in_use || d->owner != node->vn_owner)
				continue;
			r = vop_write_dirent(&error, uio, d->node.vn_ino,
			    DT_REG, (uint16_t)strlen(d->bdf), d->bdf);
			if (r) {
				off = 2 + i;
				full = 1;
				break;
			}
			off = 2 + i + 1;
		}
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		if (!full && off < 2 + VMMFS_MAX_DEVICES)
			off = 2 + VMMFS_MAX_DEVICES;
	} else if (node->vn_type == VMMFS_NDEVROOT) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
		int i;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		for (i = (int)off - 2; i < VMMFS_MAX_DEVICES; i++) {
			struct vmmfs_device *d = &vmp->vm_dev[i];

			if (!d->in_use)
				continue;
			r = vop_write_dirent(&error, uio, d->link.vn_ino,
			    DT_LNK, (uint16_t)strlen(d->bdf), d->bdf);
			if (r) {
				off = 2 + i;
				full = 1;
				break;
			}
			off = 2 + i + 1;
		}
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		if (!full && off < 2 + VMMFS_MAX_DEVICES)
			off = 2 + VMMFS_MAX_DEVICES;
	} else if (node->vn_type == VMMFS_NMACHINE) {
		struct vmmfs_machine *m = node->vn_machine;
		int i;

		for (i = (int)off - 2; i < VMMFS_NCFG; i++) {
			if (!vmmfs_cfg_present(m, i))
				continue;
			r = vop_write_dirent(&error, uio, m->cfg[i].vn_ino,
			    DT_REG, (uint16_t)strlen(vmmfs_cfg_name[i]),
			    vmmfs_cfg_name[i]);
			if (r) {
				off = 2 + i;
				full = 1;
				break;
			}
			off = 2 + i + 1;
		}
		if (!full && off < 2 + VMMFS_NCFG)
			off = 2 + VMMFS_NCFG;
		/* devices/ follows the config files. */
		if (!full && off == 2 + VMMFS_NCFG) {
			r = vop_write_dirent(&error, uio, m->vn_devices.vn_ino,
			    DT_DIR, 7, "devices");
			if (r)
				full = 1;
			else
				off = 2 + VMMFS_NCFG + 1;
		}
	}

done:
	uio->uio_offset = off;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !full;
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	return error;
}

static int
vmmnode_inactive(struct vmmfs_node *node, struct vop_inactive_args *ap)
{
	return 0;
}

static int
vmmnode_reclaim(struct vmmfs_node *node, struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;

	/* No open fd can remain here, but free any stray buffers defensively. */
	vmmfs_obuf_drain(node);

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	KKASSERT(node->vn_vnode == vp);
	node->vn_vnode = NULL;
	vp->v_data = NULL;
	lockmgr(&node->vn_interlock, LK_RELEASE);

	return 0;
}

static int
vmmnode_print(struct vmmfs_node *node, struct vop_print_args *ap)
{

	kprintf("\tvmmfs_node %p ino %ju type %d\n", node,
	    (uintmax_t)(node != NULL ? node->vn_ino : 0),
	    node != NULL ? (int)node->vn_type : -1);
	return 0;
}

/*
 * KOBJ dispatch.  Each vop_ops entry is a thin shim that resolves the node
 * and forwards to its class.  For now every node uses one catch-all class
 * (vmm_legacy) whose methods are the handlers above; step 2 splits these
 * into per-object classes.
 */
static int
vmmfs_nresolve(struct vop_nresolve_args *ap)
{
	return VMM_NODE_NRESOLVE(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	return VMM_NODE_NLOOKUPDOTDOT(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nmkdir(struct vop_nmkdir_args *ap)
{
	return VMM_NODE_NMKDIR(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_ncreate(struct vop_ncreate_args *ap)
{
	return VMM_NODE_NCREATE(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nremove(struct vop_nremove_args *ap)
{
	return VMM_NODE_NREMOVE(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nrmdir(struct vop_nrmdir_args *ap)
{
	return VMM_NODE_NRMDIR(VP_TO_VMMFS(ap->a_dvp), ap);
}

static int
vmmfs_nrename(struct vop_nrename_args *ap)
{
	return VMM_NODE_NRENAME(VP_TO_VMMFS(ap->a_fdvp), ap);
}

static int
vmmfs_readlink(struct vop_readlink_args *ap)
{
	return VMM_NODE_READLINK(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_open(struct vop_open_args *ap)
{
	return VMM_NODE_OPEN(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_close(struct vop_close_args *ap)
{
	return VMM_NODE_CLOSE(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_access(struct vop_access_args *ap)
{
	return VMM_NODE_ACCESS(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_getattr(struct vop_getattr_args *ap)
{
	return VMM_NODE_GETATTR(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_setattr(struct vop_setattr_args *ap)
{
	return VMM_NODE_SETATTR(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_read(struct vop_read_args *ap)
{
	return VMM_NODE_READ(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_write(struct vop_write_args *ap)
{
	return VMM_NODE_WRITE(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_readdir(struct vop_readdir_args *ap)
{
	return VMM_NODE_READDIR(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_inactive(struct vop_inactive_args *ap)
{
	return VMM_NODE_INACTIVE(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_reclaim(struct vop_reclaim_args *ap)
{
	return VMM_NODE_RECLAIM(VP_TO_VMMFS(ap->a_vp), ap);
}

static int
vmmfs_print(struct vop_print_args *ap)
{
	return VMM_NODE_PRINT(VP_TO_VMMFS(ap->a_vp), ap);
}

static kobj_method_t vmm_legacy_methods[] = {
	KOBJMETHOD(vmm_node_nresolve, vmmnode_nresolve),
	KOBJMETHOD(vmm_node_nlookupdotdot, vmmnode_nlookupdotdot),
	KOBJMETHOD(vmm_node_nmkdir, vmmnode_nmkdir),
	KOBJMETHOD(vmm_node_ncreate, vmmnode_ncreate),
	KOBJMETHOD(vmm_node_nremove, vmmnode_nremove),
	KOBJMETHOD(vmm_node_nrmdir, vmmnode_nrmdir),
	KOBJMETHOD(vmm_node_nrename, vmmnode_nrename),
	KOBJMETHOD(vmm_node_readlink, vmmnode_readlink),
	KOBJMETHOD(vmm_node_open, vmmnode_open),
	KOBJMETHOD(vmm_node_close, vmmnode_close),
	KOBJMETHOD(vmm_node_access, vmmnode_access),
	KOBJMETHOD(vmm_node_getattr, vmmnode_getattr),
	KOBJMETHOD(vmm_node_setattr, vmmnode_setattr),
	KOBJMETHOD(vmm_node_read, vmmnode_read),
	KOBJMETHOD(vmm_node_write, vmmnode_write),
	KOBJMETHOD(vmm_node_readdir, vmmnode_readdir),
	KOBJMETHOD(vmm_node_inactive, vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim, vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print, vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_legacy, vmm_legacy_methods, 0);

struct vop_ops vmmfs_vnode_vops = {
	.vop_default =		vop_defaultop,
	.vop_nresolve =		vmmfs_nresolve,
	.vop_nlookupdotdot =	vmmfs_nlookupdotdot,
	.vop_nmkdir =		vmmfs_nmkdir,
	.vop_ncreate =		vmmfs_ncreate,
	.vop_nremove =		vmmfs_nremove,
	.vop_nrmdir =		vmmfs_nrmdir,
	.vop_nrename =		vmmfs_nrename,
	.vop_readlink =		vmmfs_readlink,
	.vop_open =		vmmfs_open,
	.vop_close =		vmmfs_close,
	.vop_access =		vmmfs_access,
	.vop_getattr =		vmmfs_getattr,
	.vop_setattr =		vmmfs_setattr,
	.vop_read =		vmmfs_read,
	.vop_write =		vmmfs_write,
	.vop_readdir =		vmmfs_readdir,
	.vop_inactive =		vmmfs_inactive,
	.vop_reclaim =		vmmfs_reclaim,
	.vop_print =		vmmfs_print,
};
