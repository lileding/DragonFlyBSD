/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem presentation of one machine: its directory, config-file table,
 * lifetime references, and lifecycle files (lease, events, status.tar.gz,
 * stopped).  The executable VM state is the embedded struct vmm_machine.
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
#include "vmmfs_device.h"
#include "vmmfs_loader.h"
#include "vmmfs_machine.h"
#include "vmmfs_node_if.h"

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
	{ "vcpu",	  VREG, 0644, &vmmfs_vcpu_class,
	    __offsetof(struct vmmfs_machine, n_vcpu),	  NULL },
	{ "mem",	  VREG, 0644, &vmmfs_mem_class,
	    __offsetof(struct vmmfs_machine, n_mem),	  NULL },
	{ "loader",	  VREG, 0644, &vmmfs_loader_class,
	    __offsetof(struct vmmfs_machine, n_loader),  NULL },
	{ "lease",	  VREG, 0444, &vmmfs_lease_class,
	    __offsetof(struct vmmfs_machine, n_lease),	  NULL },
	{ "events",	  VREG, 0444, &vmmfs_events_class,
	    __offsetof(struct vmmfs_machine, n_events),  NULL },
	{ "console",	  VREG, 0644, &vmmfs_console_class,
	    __offsetof(struct vmmfs_machine, n_console), NULL },
	{ "status.tar.gz", VREG, 0444, &vmmfs_status_class,
	    __offsetof(struct vmmfs_machine, n_status),  NULL },
	{ "stopped",	  VREG, 0644, &vmmfs_stopped_class,
	    __offsetof(struct vmmfs_machine, n_stopped), cfg_present_stopped },
};
#define VMMFS_NCFG_FILES \
	((int)(sizeof(vmmfs_cfg_table) / sizeof(vmmfs_cfg_table[0])))

/* The node backing a config descriptor within a machine slot. */
static struct vmmfs_node *
cfg_node(struct vmmfs_machine *m, const struct vmmfs_cfg_desc *d)
{
	return (struct vmmfs_node *)((char *)m + d->node_off);
}

/* A config file is listed unless its predicate hides it (e.g. stopped). */
static int
cfg_present(struct vmmfs_machine *m, const struct vmmfs_cfg_desc *d)
{
	return d->present == NULL || d->present(&m->machine);
}

static void
vmmfs_machine_owner_hold(void *arg)
{
	struct vmmfs_machine *m = arg;

	vmmfs_machine_ref(m->vm_mount, m);
}

static void
vmmfs_machine_owner_release(void *arg)
{
	struct vmmfs_machine *m = arg;

	vmmfs_machine_unref(m->vm_mount, m);
}

static const struct vmm_machine_owner_ops vmmfs_machine_owner_ops = {
	.hold = vmmfs_machine_owner_hold,
	.release = vmmfs_machine_owner_release,
};


/*
 * Allocate a machine, wire up its nodes with fresh inos, and insert it.  Caller
 * holds vm_lock and has checked the name is free.  vm_refs starts at 1 for the
 * tree reference.
 */
struct vmmfs_machine *
vmmfs_machine_create(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	struct vmmfs_machine *m;
	ino_t base;
	int j;

	m = kmalloc(sizeof(*m), M_VMMFS, M_WAITOK | M_ZERO);
	m->vm_mount = vmp;
	bcopy(name, m->name, nlen);
	m->name[nlen] = '\0';
	m->vm_refs = 1;
	vmm_machine_init(&m->machine);
	vmm_machine_set_owner(&m->machine, &vmmfs_machine_owner_ops, m);

	base = vmp->vm_next_ino;
	vmp->vm_next_ino += VMMFS_MACHINE_INO_STRIDE;
	vmmfs_node_init(&m->node, &vmmfs_machine_class, VDIR, VMMFS_DIR_MODE,
	    base, &vmp->vm_machines, m);
	for (j = 0; j < VMMFS_NCFG_FILES; j++) {
		const struct vmmfs_cfg_desc *d = &vmmfs_cfg_table[j];

		vmmfs_node_init(cfg_node(m, d), d->class, d->vtype, d->mode,
		    base + 1 + j, &m->node, m);
	}
	vmmfs_node_init(&m->vn_devices, &vmmfs_devices_class, VDIR, VMMFS_DIR_MODE,
	    base + VMMFS_MACHINE_DEV_OFF, &m->node, m);

	RB_INSERT(vmmfs_machtree, &vmp->vm_machtree, m);
	m->vm_in_tree = 1;
	return m;
}

void
vmmfs_machine_ref(struct vmmfs_mount *vmp, struct vmmfs_machine *m)
{
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m->vm_refs++;
	lockmgr(&vmp->vm_lock, LK_RELEASE);
}

static void
vmmfs_machine_free(struct vmmfs_machine *m)
{
	int j;

	vmm_machine_uninit(&m->machine);
	vmmfs_node_uninit(&m->node);
	for (j = 0; j < VMMFS_NCFG_FILES; j++)
		vmmfs_node_uninit(cfg_node(m, &vmmfs_cfg_table[j]));
	vmmfs_node_uninit(&m->vn_devices);
	kfree(m, M_VMMFS);
}

/* Drop one reference; free once it reaches 0 (no vnode can reference it then). */
void
vmmfs_machine_unref(struct vmmfs_mount *vmp, struct vmmfs_machine *m)
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
vmmfs_machine_mark_deleted(struct vmmfs_mount *vmp, struct vmmfs_machine *m)
{
	struct vmmfs_devlist tofree = SLIST_HEAD_INITIALIZER(tofree);
	int first;

	vmm_machine_request_stopped(&m->machine, 1);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	first = m->vm_in_tree;	/* the lease path already set "deleting" */
	if (first) {
		m->vm_in_tree = 0;
		(void)vmm_machine_begin_delete(&m->machine);
		RB_REMOVE(vmmfs_machtree, &vmp->vm_machtree, m);
		vmmfs_device_unbind_owner_locked(vmp, &m->machine, &tofree);
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	vmmfs_device_free_list(&tofree);
	if (first)
		vmmfs_machine_unref(vmp, m);	/* the tree reference */
}


/* --------------------------------------------------------------------- */
/*
 * The per-machine directory and its lifecycle files (lease/events/status/
 * stopped).  These present the embedded struct vmm_machine through vmmfs.
 */
static int
vmmfs_machine_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machine *m = dnode->vn_machine;
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
vmmfs_machine_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct vmmfs_machine *m = node->vn_machine;
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
vmmfs_machine_ncreate(struct vmmfs_node *dnode, struct vop_ncreate_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machine *m = dnode->vn_machine;
	struct vnode *vp;
	int error;

	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;

	vmm_machine_request_stopped(&m->machine, 0);
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
 * `rm machines/<name>/stopped` declares that the machine should run.  The
 * operation only updates desired state and queues the vmm worker; it never
 * waits for loader execution.
 */
static int
vmmfs_machine_nremove(struct vmmfs_node *dnode, struct vop_nremove_args *ap)
{
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machine *m = dnode->vn_machine;
	struct vnode *vp;
	int error;

	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;
	error = vmm_machine_request_running(&m->machine, ap->a_cred);
	if (error)
		return error;

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	cache_unlink(ap->a_nch);
	vrele(vp);
	return 0;
}

static kobj_method_t vmmfs_machine_methods[] = {
	KOBJMETHOD(vmmfs_node_nresolve,		vmmfs_machine_nresolve),
	KOBJMETHOD(vmmfs_node_readdir,		vmmfs_machine_readdir),
	KOBJMETHOD(vmmfs_node_ncreate,		vmmfs_machine_ncreate),
	KOBJMETHOD(vmmfs_node_nremove,		vmmfs_machine_nremove),
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
DEFINE_CLASS(vmmfs_machine, vmmfs_machine_methods, 0);

/*
 * The machine's lifecycle files: lease (a reference handle whose last close
 * destroys an armed machine), events (a drained stream), status (a stub), and
 * stopped (the lifecycle control written to stop the machine).
 */
static int
vmmfs_lease_open(struct vmmfs_node *node, struct vop_open_args *ap)
{
	if (vmm_machine_lease_open(&node->vn_machine->machine) == 0)
		return ENXIO;
	return vop_stdopen(ap);
}

static int
vmmfs_lease_close(struct vmmfs_node *node, struct vop_close_args *ap)
{
	int error = vop_stdclose(ap);

	if (vmm_machine_lease_close(&node->vn_machine->machine)) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_vp->v_mount);

		vmmfs_machine_mark_deleted(vmp, node->vn_machine);
	}
	return error;
}

static kobj_method_t vmmfs_lease_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmmfs_node_read,	vmmfs_zero_read),
	KOBJMETHOD(vmmfs_node_open,	vmmfs_lease_open),
	KOBJMETHOD(vmmfs_node_close,	vmmfs_lease_close),
	KOBJMETHOD(vmmfs_node_access,	vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_lease, vmmfs_lease_methods, 0);

static int
vmmfs_events_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	char ebuf[256];
	size_t n;

	n = vmm_machine_read_events(&node->vn_machine->machine, ebuf, sizeof(ebuf));
	if (n == 0)
		return 0;
	return uiomove(ebuf, n, ap->a_uio);
}

static kobj_method_t vmmfs_events_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmmfs_node_read,	vmmfs_events_read),
	KOBJMETHOD(vmmfs_node_open,	vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,	vmmnode_close),
	KOBJMETHOD(vmmfs_node_access,	vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_events, vmmfs_events_methods, 0);

static kobj_method_t vmmfs_status_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmmfs_node_read,	vmmfs_zero_read),
	KOBJMETHOD(vmmfs_node_open,	vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,	vmmnode_close),
	KOBJMETHOD(vmmfs_node_access,	vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_status, vmmfs_status_methods, 0);

/*
 * Writing the stopped control file selects the stop method (apic|force) and
 * (re)applies the stop.  Idempotent.
 */
static int
vmmfs_stopped_write(struct vmmfs_node *node, struct vop_write_args *ap)
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

	vmm_machine_request_stopped(&node->vn_machine->machine, force);
	return 0;
}

static kobj_method_t vmmfs_stopped_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmmfs_node_read,	vmmfs_zero_read),
	KOBJMETHOD(vmmfs_node_write,	vmmfs_stopped_write),
	KOBJMETHOD(vmmfs_node_open,	vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,	vmmnode_close),
	KOBJMETHOD(vmmfs_node_access,	vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_stopped, vmmfs_stopped_methods, 0);
