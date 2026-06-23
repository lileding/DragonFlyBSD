/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The machines/ collection: the registry of user VMs and the directory vops
 * that create (mkdir), remove (rmdir), resolve, and list them.  host is a
 * permanent reserved entry handled by vmm_host.c.  The registry is still a
 * fixed array; a later step replaces it with a dynamic RB tree.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namecache.h>
#include <sys/dirent.h>
#include <sys/uio.h>
#include <sys/tree.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmmfs.h"
#include "vmmfs_machines.h"
#include "vmm_node_if.h"

/*
 * The config files presented under a machine directory.  This table is the
 * single source of truth -- it drives node init (create), name lookup
 * (nresolve), and listing (readdir), so there is no per-file switch.  The row
 * order fixes both the ino offset and the readdir order; node_off locates each
 * node within the machine slot; present() hides a file when it returns 0.
 */
struct vmmfs_cfg_desc {
	const char	*name;
	enum vtype	 vtype;
	mode_t		 mode;
	kobj_class_t	 class;		/* kobj_class_t is already a pointer */
	size_t		 node_off;
	int		(*present)(const struct vmm_machine *m);
};

static int
cfg_present_stopped(const struct vmm_machine *m)
{
	return vmm_machine_is_stopped(m);
}

static const struct vmmfs_cfg_desc vmmfs_cfg_table[] = {
	{ "vcpu",	  VREG, 0644, &vmm_vcpu_class,
	    __offsetof(struct vmmfs_machines, n_vcpu),	  NULL },
	{ "mem",	  VREG, 0644, &vmm_mem_class,
	    __offsetof(struct vmmfs_machines, n_mem),	  NULL },
	{ "loader",	  VREG, 0644, &vmm_loader_class,
	    __offsetof(struct vmmfs_machines, n_loader),  NULL },
	{ "lease",	  VREG, 0444, &vmm_lease_class,
	    __offsetof(struct vmmfs_machines, n_lease),	  NULL },
	{ "events",	  VREG, 0444, &vmm_events_class,
	    __offsetof(struct vmmfs_machines, n_events),  NULL },
	{ "console",	  VREG, 0644, &vmm_console_class,
	    __offsetof(struct vmmfs_machines, n_console), NULL },
	{ "status.tar.gz", VREG, 0444, &vmm_status_class,
	    __offsetof(struct vmmfs_machines, n_status),  NULL },
	{ "stopped",	  VREG, 0644, &vmm_stopped_class,
	    __offsetof(struct vmmfs_machines, n_stopped), cfg_present_stopped },
};
#define VMMFS_NCFG_FILES \
	((int)(sizeof(vmmfs_cfg_table) / sizeof(vmmfs_cfg_table[0])))

/* The node backing a config descriptor within a machine slot. */
static struct vmmfs_node *
cfg_node(struct vmmfs_machines *m, const struct vmmfs_cfg_desc *d)
{
	return (struct vmmfs_node *)((char *)m + d->node_off);
}

/* A config file is listed unless its predicate hides it (e.g. stopped). */
static int
cfg_present(struct vmmfs_machines *m, const struct vmmfs_cfg_desc *d)
{
	return d->present == NULL || d->present(&m->state);
}

/* ---- registry: an RB tree keyed by name (guarded by vm_lock) ---- */

int
vmmfs_machine_cmp(struct vmmfs_machines *a, struct vmmfs_machines *b)
{
	return strcmp(a->name, b->name);
}
RB_GENERATE(vmmfs_machtree, vmmfs_machines, vm_link, vmmfs_machine_cmp);

/* Caller holds vm_lock.  name need not be NUL-terminated. */
static struct vmmfs_machines *
vmmfs_find_machine(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	struct vmmfs_machines key;

	if (nlen > VMMFS_NAME_MAX)
		return NULL;
	bcopy(name, key.name, nlen);
	key.name[nlen] = '\0';
	return RB_FIND(vmmfs_machtree, &vmp->vm_machtree, &key);
}

/*
 * Allocate a machine, wire up its nodes with fresh inos, and insert it.  Caller
 * holds vm_lock and has checked the name is free.  vm_refs starts at 1 for the
 * tree reference.
 */
static struct vmmfs_machines *
vmmfs_machine_create(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	struct vmmfs_machines *m;
	ino_t base;
	int j;

	m = kmalloc(sizeof(*m), M_VMMFS, M_WAITOK | M_ZERO);
	bcopy(name, m->name, nlen);
	m->name[nlen] = '\0';
	m->vm_refs = 1;
	vmm_machine_init(&m->state);

	base = vmp->vm_next_ino;
	vmp->vm_next_ino += VMMFS_MACHINE_INO_STRIDE;
	vmmfs_node_init(&m->node, &vmm_machine_class, VDIR, VMMFS_DIR_MODE,
	    base, &vmp->vm_machines, m);
	for (j = 0; j < VMMFS_NCFG_FILES; j++) {
		const struct vmmfs_cfg_desc *d = &vmmfs_cfg_table[j];

		vmmfs_node_init(cfg_node(m, d), d->class, d->vtype, d->mode,
		    base + 1 + j, &m->node, m);
	}
	vmmfs_node_init(&m->vn_devices, &vmm_devices_class, VDIR, VMMFS_DIR_MODE,
	    base + VMMFS_MACHINE_DEV_OFF, &m->node, m);

	RB_INSERT(vmmfs_machtree, &vmp->vm_machtree, m);
	m->vm_in_tree = 1;
	return m;
}

void
vmmfs_machine_ref(struct vmmfs_mount *vmp, struct vmmfs_machines *m)
{
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m->vm_refs++;
	lockmgr(&vmp->vm_lock, LK_RELEASE);
}

static void
vmmfs_machine_free(struct vmmfs_machines *m)
{
	int j;

	vmmfs_node_uninit(&m->node);
	for (j = 0; j < VMMFS_NCFG_FILES; j++)
		vmmfs_node_uninit(cfg_node(m, &vmmfs_cfg_table[j]));
	vmmfs_node_uninit(&m->vn_devices);
	kfree(m, M_VMMFS);
}

/* Drop one reference; free once it reaches 0 (no vnode can reference it then). */
void
vmmfs_machine_unref(struct vmmfs_mount *vmp, struct vmmfs_machines *m)
{
	int dofree;

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	dofree = (--m->vm_refs == 0);
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (dofree)
		vmmfs_machine_free(m);
}

/*
 * Mark a machine deleted (rmdir source 1, or the last lease close) and drop it
 * from the tree.  Idempotent via vmm_machine_begin_delete, so the two sources
 * can both fire.  The struct lives on (out of the tree) until its last vnode
 * is reclaimed; dropping the tree reference here may free it immediately.
 */
void
vmmfs_machine_mark_deleted(struct vmmfs_mount *vmp, struct vmmfs_machines *m)
{
	struct vmmfs_devlist tofree = SLIST_HEAD_INITIALIZER(tofree);
	struct vmmfs_device *d, *nd;
	int first;

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	first = m->vm_in_tree;	/* the lease path already set "deleting" */
	if (first) {
		m->vm_in_tree = 0;
		(void)vmm_machine_begin_delete(&m->state);
		RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
		/* This machine's devices: host devices return to the host pool,
		 * user backends are unloaded (freed outside the lock below). */
		SLIST_FOREACH_MUTABLE(d, &vmp->vm_devs, dv_link, nd) {
			if (!vmm_device_owned_by(&d->dev, &m->state))
				continue;
			if (d->dev.is_host) {
				vmm_device_unbind(&d->dev);
			} else {
				SLIST_REMOVE(&vmp->vm_devs, d, vmmfs_device,
				    dv_link);
				SLIST_INSERT_HEAD(&tofree, d, dv_link);
			}
		}
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	while (!SLIST_EMPTY(&tofree)) {
		d = SLIST_FIRST(&tofree);
		SLIST_REMOVE_HEAD(&tofree, dv_link);
		vmmfs_node_uninit(&d->node);
		vmmfs_node_uninit(&d->link);
		kfree(d, M_VMMFS);
	}
	if (first)
		vmmfs_machine_unref(vmp, m);	/* the tree reference */
}

/* ---- machines/ directory vops ---- */

static int
vmm_machines_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *child = NULL;

	(void)dnode;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0) {
		child = &vmp->vm_host;
	} else {
		struct vmmfs_machines *m;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		m = vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen);
		if (m != NULL)
			child = &m->node;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
	}
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmm_machines_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
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
		struct vmmfs_machines *m;
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
vmm_machines_nmkdir(struct vmmfs_node *dnode, struct vop_nmkdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machines *m;
	struct vnode *vp;
	int error;

	(void)dnode;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen > VMMFS_NAME_MAX)
		return ENAMETOOLONG;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0)
		return EEXIST;	/* host is reserved */

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen) != NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EEXIST;
	}
	m = vmmfs_machine_create(vmp, ncp->nc_name, ncp->nc_nlen);
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	error = vmmfs_alloc_vp(dvp->v_mount, &m->node, LK_EXCLUSIVE | LK_RETRY,
	    &vp);
	if (error) {
		vmmfs_machine_mark_deleted(vmp, m);
		return error;
	}

	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	return 0;
}

/*
 * `rmdir machines/<name>` removes a stopped machine (source 1): it deletes
 * regardless of leases.  The slot is freed lazily so still-open fds keep
 * working until reclaimed.
 */
static int
vmm_machines_nrmdir(struct vmmfs_node *dnode, struct vop_nrmdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_machines *m;
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

static kobj_method_t vmm_machines_methods[] = {
	KOBJMETHOD(vmm_node_nresolve,		vmm_machines_nresolve),
	KOBJMETHOD(vmm_node_readdir,		vmm_machines_readdir),
	KOBJMETHOD(vmm_node_nmkdir,		vmm_machines_nmkdir),
	KOBJMETHOD(vmm_node_nrmdir,		vmm_machines_nrmdir),
	KOBJMETHOD(vmm_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmm_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmm_node_access,		vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmm_node_open,		vmmnode_open),
	KOBJMETHOD(vmm_node_close,		vmmnode_close),
	KOBJMETHOD(vmm_node_inactive,		vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,		vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_machines, vmm_machines_methods, 0);

/* --------------------------------------------------------------------- */
/*
 * The per-machine directory and its lifecycle files (lease/events/status/
 * stopped).  These present the vmm_machine core through the VFS; they live in
 * the fs layer with the machines/ registry, not in the pure core.
 */
static int
vmm_machine_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machines *m = dnode->vn_machine;
	struct vmmfs_node *child = NULL;
	int i;

	if (ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "devices", 7) == 0) {
		child = &m->vn_devices;
	} else {
		for (i = 0; i < VMMFS_NCFG_FILES; i++) {
			const struct vmmfs_cfg_desc *d = &vmmfs_cfg_table[i];

			if (!cfg_present(m, d))
				continue;
			if ((int)strlen(d->name) == ncp->nc_nlen &&
			    bcmp(d->name, ncp->nc_name, ncp->nc_nlen) == 0) {
				child = cfg_node(m, d);
				break;
			}
		}
	}
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmm_machine_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct vmmfs_machines *m = node->vn_machine;
	off_t off;
	int full, error, i;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error || full)
		goto out;
	for (i = (int)off - 2; i < VMMFS_NCFG_FILES; i++) {
		const struct vmmfs_cfg_desc *d = &vmmfs_cfg_table[i];

		if (!cfg_present(m, d))
			continue;
		if (vop_write_dirent(&error, uio, cfg_node(m, d)->vn_ino, DT_REG,
		    (uint16_t)strlen(d->name), d->name)) {
			off = 2 + i;
			full = 1;
			break;
		}
		off = 2 + i + 1;
	}
	if (!full && off < 2 + VMMFS_NCFG_FILES)
		off = 2 + VMMFS_NCFG_FILES;
	/* devices/ follows the config files. */
	if (!full && off == 2 + VMMFS_NCFG_FILES) {
		if (vop_write_dirent(&error, uio, m->vn_devices.vn_ino, DT_DIR,
		    7, "devices"))
			full = 1;
		else
			off = 2 + VMMFS_NCFG_FILES + 1;
	}
out:
	return vmmfs_readdir_end(ap, off, full, error);
}

/*
 * Create "stopped" under a machine: an atomic, idempotent request to stop it.
 * `echo apic > stopped` opens with O_CREAT.
 */
static int
vmm_machine_ncreate(struct vmmfs_node *dnode, struct vop_ncreate_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machines *m = dnode->vn_machine;
	struct vnode *vp;
	int error;

	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;

	vmm_machine_stop(&m->state, 0);
	error = vmmfs_alloc_vp(dvp->v_mount, &m->n_stopped,
	    LK_EXCLUSIVE | LK_RETRY, &vp);
	if (error)
		return error;
	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	return 0;
}

/*
 * `rm machines/<name>/stopped` requests start: config must be complete and the
 * loader executable, else the start fails and the machine stays stopped.
 */
static int
vmm_machine_nremove(struct vmmfs_node *dnode, struct vop_nremove_args *ap)
{
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machines *m = dnode->vn_machine;
	struct vnode *vp;
	int error;

	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;
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

static kobj_method_t vmm_machine_methods[] = {
	KOBJMETHOD(vmm_node_nresolve,		vmm_machine_nresolve),
	KOBJMETHOD(vmm_node_readdir,		vmm_machine_readdir),
	KOBJMETHOD(vmm_node_ncreate,		vmm_machine_ncreate),
	KOBJMETHOD(vmm_node_nremove,		vmm_machine_nremove),
	KOBJMETHOD(vmm_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmm_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmm_node_access,		vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmm_node_open,		vmmnode_open),
	KOBJMETHOD(vmm_node_close,		vmmnode_close),
	KOBJMETHOD(vmm_node_inactive,		vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,		vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_machine, vmm_machine_methods, 0);

/*
 * The machine's own config files: lease (a reference handle whose last close
 * destroys an armed machine), events (a drained stream), status (a stub), and
 * stopped (the lifecycle control written to stop the machine).
 */
static int
vmm_lease_open(struct vmmfs_node *node, struct vop_open_args *ap)
{
	if (vmm_machine_lease_open(&node->vn_machine->state) == 0)
		return ENXIO;
	return vop_stdopen(ap);
}

static int
vmm_lease_close(struct vmmfs_node *node, struct vop_close_args *ap)
{
	int error = vop_stdclose(ap);

	if (vmm_machine_lease_close(&node->vn_machine->state)) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_vp->v_mount);

		vmmfs_machine_mark_deleted(vmp, node->vn_machine);
	}
	return error;
}

static kobj_method_t vmm_lease_methods[] = {
	KOBJMETHOD(vmm_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmm_node_read,	vmmfs_zero_read),
	KOBJMETHOD(vmm_node_open,	vmm_lease_open),
	KOBJMETHOD(vmm_node_close,	vmm_lease_close),
	KOBJMETHOD(vmm_node_access,	vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmm_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_lease, vmm_lease_methods, 0);

static int
vmm_events_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	char ebuf[256];
	size_t n;

	n = vmm_machine_read_events(&node->vn_machine->state, ebuf, sizeof(ebuf));
	if (n == 0)
		return 0;
	return uiomove(ebuf, n, ap->a_uio);
}

static kobj_method_t vmm_events_methods[] = {
	KOBJMETHOD(vmm_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmm_node_read,	vmm_events_read),
	KOBJMETHOD(vmm_node_open,	vmmnode_open),
	KOBJMETHOD(vmm_node_close,	vmmnode_close),
	KOBJMETHOD(vmm_node_access,	vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmm_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_events, vmm_events_methods, 0);

static kobj_method_t vmm_status_methods[] = {
	KOBJMETHOD(vmm_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmm_node_read,	vmmfs_zero_read),
	KOBJMETHOD(vmm_node_open,	vmmnode_open),
	KOBJMETHOD(vmm_node_close,	vmmnode_close),
	KOBJMETHOD(vmm_node_access,	vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmm_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_status, vmm_status_methods, 0);

/*
 * Writing the stopped control file selects the stop method (apic|force) and
 * (re)applies the stop.  Idempotent.
 */
static int
vmm_stopped_write(struct vmmfs_node *node, struct vop_write_args *ap)
{
	struct uio *uio = ap->a_uio;
	char buf[16];
	size_t take;
	int error, force;

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

static kobj_method_t vmm_stopped_methods[] = {
	KOBJMETHOD(vmm_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmm_node_read,	vmmfs_zero_read),
	KOBJMETHOD(vmm_node_write,	vmm_stopped_write),
	KOBJMETHOD(vmm_node_open,	vmmnode_open),
	KOBJMETHOD(vmm_node_close,	vmmnode_close),
	KOBJMETHOD(vmm_node_access,	vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmm_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_stopped, vmm_stopped_methods, 0);
