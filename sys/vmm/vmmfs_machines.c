/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The machines/ collection: the filesystem registry of user VM names.  It owns
 * lookup, listing, mkdir, and rmdir for the machines directory.  The single
 * machine filesystem object and its lifecycle files live in vmmfs_machine.c.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namecache.h>
#include <sys/thread2.h>
#include <sys/dirent.h>
#include <sys/uio.h>
#include <sys/tree.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmmfs.h"
#include "vmmfs_device.h"
#include "vmmfs_machine.h"
#include "vmmfs_node_if.h"

/* ---- registry: an RB tree keyed by name (guarded by vm_lock) ---- */

static void	vmmfs_machine_reaper(void *arg);

int
vmmfs_machine_cmp(struct vmmfs_machine *a, struct vmmfs_machine *b)
{
	return strcmp(a->name, b->name);
}
RB_GENERATE(vmmfs_machtree, vmmfs_machine, vm_link, vmmfs_machine_cmp);

/* Caller holds vm_lock.  name need not be NUL-terminated. */
static struct vmmfs_machine *
vmmfs_machines_find(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	struct vmmfs_machine *m;

	if (nlen < 0 || nlen > VMMFS_NAME_MAX)
		return NULL;
	m = RB_ROOT(&vmp->vm_machtree);
	while (m != NULL) {
		const char *mname = m->name;
		int mlen = strlen(mname);
		int cmp;

		cmp = strncmp(name, mname, (nlen < mlen) ? nlen : mlen);
		if (cmp == 0) {
			if (nlen < mlen)
				cmp = -1;
			else if (nlen > mlen)
				cmp = 1;
		}
		if (cmp < 0)
			m = RB_LEFT(m, vm_link);
		else if (cmp > 0)
			m = RB_RIGHT(m, vm_link);
		else
			return m;
	}
	return NULL;
}

/* ---- machines/ directory vops ---- */

static int
vmmfs_machines_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child = NULL;

	(void)dnode;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0) {
		child = &vmp->vm_host;
	} else {
		struct vmmfs_machine *m;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		m = vmmfs_machines_find(vmp, ncp->nc_name, ncp->nc_nlen);
		if (m != NULL)
			child = &m->node;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
	}
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmmfs_machines_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct vmmfs_mount *vmp;
	off_t off;
	int full, error, i;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error || full)
		goto out;
	vmp = VFS_TO_VMMFS(ap->a_vp->v_mount);

	/* host is always the first entry. */
	if (off == 2) {
		if (vop_write_dirent(&error, uio, vmp->vm_host.vn_ino, DT_DIR, 4,
		    "host")) {
			full = 1;
			goto out;
		}
		off = 3;
	}
	lockmgr(&vmp->vm_lock, LK_SHARED);
	{
		struct vmmfs_machine *m;
		int skip = (int)off - 3;

		i = 0;
		RB_FOREACH(m, vmmfs_machtree, &vmp->vm_machtree) {
			if (i++ < skip)
				continue;
			if (vop_write_dirent(&error, uio, m->node.vn_ino, DT_DIR,
			    (uint16_t)strlen(m->name), m->name)) {
				full = 1;
				break;
			}
			off++;
		}
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);
out:
	return vmmfs_readdir_end(ap, off, full, error);
}

/*
 * `mkdir machines/<name>` creates a machine: always stopped, empty config.
 * The user then writes vcpu/mem/loader and `rm stopped` to start.
 */
static int
vmmfs_machines_nmkdir(struct vmmfs_node *dnode, struct vop_nmkdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	(void)dnode;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen > VMMFS_NAME_MAX)
		return ENAMETOOLONG;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0)
		return EEXIST;	/* host is reserved */

	kprintf("vmm klog: nmkdir begin vmp=%p name=%.*s\n", vmp,
	    ncp->nc_nlen, ncp->nc_name);
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmp->vm_closing) {
		kprintf("vmm klog: nmkdir closing vmp=%p\n", vmp);
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EBUSY;
	}
	vmp->vm_machine_count++;
	kprintf("vmm klog: nmkdir count hold vmp=%p count=%d\n", vmp,
	    vmp->vm_machine_count);
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	m = vmmfs_machine_create(vmp, ncp->nc_name, ncp->nc_nlen);
	kprintf("vmm klog: nmkdir created m=%p machine=%p\n", m,
	    &m->machine);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	kprintf("vmm klog: nmkdir locked vmp=%p m=%p\n", vmp, m);
	if (vmmfs_machines_find(vmp, ncp->nc_name, ncp->nc_nlen) != NULL) {
		kprintf("vmm klog: nmkdir duplicate m=%p\n", m);
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vmmfs_machine_free(m);
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		KKASSERT(vmp->vm_machine_count > 0);
		vmp->vm_machine_count--;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EEXIST;
	}
	kprintf("vmm klog: nmkdir rb_insert begin m=%p\n", m);
	RB_INSERT(vmmfs_machtree, &vmp->vm_machtree, m);
	m->vm_in_tree = 1;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	kprintf("vmm klog: nmkdir vnode gate m=%p allow=%d\n", m,
	    vmm_debug_allow_nmkdir_vnode);
	vmm_debug_trace("nmkdir inserted name=%s m=%p allow_vnode=%d", m->name,
	    &m->machine, vmm_debug_allow_nmkdir_vnode);
	if (!vmm_debug_allow_nmkdir_vnode) {
		kprintf("vmm klog: nmkdir gated cleanup begin m=%p\n", m);
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		m->vm_in_tree = 0;
		RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vmmfs_machine_free(m);
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		KKASSERT(vmp->vm_machine_count > 0);
		vmp->vm_machine_count--;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		kprintf("vmm klog: nmkdir gated cleanup done m=%p\n", m);
		return EBUSY;
	}

	kprintf("vmm klog: nmkdir alloc_vp begin m=%p node=%p\n", m,
	    &m->node);
	error = vmmfs_alloc_vp(dvp->v_mount, &m->node, LK_EXCLUSIVE | LK_RETRY,
	    &vp);
	if (error) {
		kprintf("vmm klog: nmkdir alloc_vp error=%d m=%p\n", error, m);
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		m->vm_in_tree = 0;
		RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vmmfs_machine_free(m);
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		KKASSERT(vmp->vm_machine_count > 0);
		vmp->vm_machine_count--;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return error;
	}
	kprintf("vmm klog: nmkdir alloc_vp done m=%p vp=%p\n", m, vp);

	*ap->a_vpp = vp;
	kprintf("vmm klog: nmkdir cache begin m=%p vp=%p\n", m, vp);
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	kprintf("vmm klog: nmkdir done m=%p vp=%p\n", m, vp);
	return 0;
}

/*
 * `rmdir machines/<name>` is allowed only after the control plane already says
 * desired stopped.  Deletion first appends a force-stop command.  Only after
 * that succeeds may it remove the name and let the reaper free the machine
 * after the command queue drains.
 */
static int
vmmfs_machines_nrmdir(struct vmmfs_node *dnode, struct vop_nrmdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	(void)dnode;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0)
		return EPERM;	/* host is not removable */

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m = vmmfs_machines_find(vmp, ncp->nc_name, ncp->nc_nlen);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	lwkt_gettoken(&m->machine.token_config);
	if (!m->machine.mut_desired_stopped) {
		lwkt_reltoken(&m->machine.token_config);
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return EBUSY;
	}
	lwkt_reltoken(&m->machine.token_config);
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	error = vmm_machine_execute(&m->machine, vmm_machine_stop_force, NULL);
	if (error != 0) {
		vrele(vp);
		return error;
	}

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	KKASSERT(m->vm_in_tree != 0);
	m->vm_in_tree = 0;
	RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
	vmmfs_device_return_owner_locked(vmp, &m->machine);
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	cache_inval_vp(vp, CINV_DESTROY | CINV_CHILDREN);
	vrele(vp);
	error = lwkt_create(vmmfs_machine_reaper, m, NULL, NULL, 0, -1,
	    "vmmfsreap");
	if (error)
		vmmfs_machine_reaper(m);
	return 0;
}

static kobj_method_t vmmfs_machines_methods[] = {
	KOBJMETHOD(vmmfs_node_nresolve,		vmmfs_machines_nresolve),
	KOBJMETHOD(vmmfs_node_readdir,		vmmfs_machines_readdir),
	KOBJMETHOD(vmmfs_node_nmkdir,		vmmfs_machines_nmkdir),
	KOBJMETHOD(vmmfs_node_nrmdir,		vmmfs_machines_nrmdir),
	KOBJMETHOD(vmmfs_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmmfs_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmmfs_node_access,		vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_open,		vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,		vmmnode_close),
	KOBJMETHOD(vmmfs_node_inactive,		vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,		vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_machines, vmmfs_machines_methods, 0);

static void
vmmfs_machine_reaper(void *arg)
{
	struct vmmfs_machine *m = arg;
	struct vmmfs_mount *vmp = m->vm_mount;

	vmm_debug_trace("machine_reaper begin m=%p vmp=%p", m, vmp);
	vmm_machine_drain(&m->machine);
	vmm_debug_trace("machine_reaper drained m=%p", m);
	vmmfs_machine_free(m);
	vmm_debug_trace("machine_reaper freed vmp=%p", vmp);
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	KKASSERT(vmp->vm_machine_count > 0);
	vmp->vm_machine_count--;
	vmm_debug_trace("machine_reaper decremented vmp=%p count=%d", vmp,
	    vmp->vm_machine_count);
	lockmgr(&vmp->vm_lock, LK_RELEASE);
}
