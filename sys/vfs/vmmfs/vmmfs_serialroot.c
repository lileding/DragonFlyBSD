/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs serial-port directory object.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include "vmmfs.h"
#include "vmmfs_serialport.h"
#include "vmmfs_serialroot.h"

#define VMMFS_SERIALROOT_MODE 0555

struct vmmfs_serialroot_item {
	ino_t inode;
	char name[sizeof("com4")];
};

static int vmmfs_serialroot_access(struct vop_access_args *);
static int vmmfs_serialroot_getattr(struct vop_getattr_args *);
static int vmmfs_serialroot_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_serialroot_ncreate(struct vop_ncreate_args *);
static int vmmfs_serialroot_nlookupdotdot(struct vop_nlookupdotdot_args *);
static int vmmfs_serialroot_nremove(struct vop_nremove_args *);
static int vmmfs_serialroot_nresolve(struct vop_nresolve_args *);
static int vmmfs_serialroot_readdir(struct vop_readdir_args *);
static int vmmfs_serialroot_inactive(struct vop_inactive_args *);
static int vmmfs_serialroot_read_item(struct vmmfs_serialroot *, uint64_t,
	struct vmmfs_serialroot_item *);
static int vmmfs_serialroot_port_compare(struct vmmfs_serialroot_port *,
	struct vmmfs_serialroot_port *);
static struct vmmfs_serialroot_port *vmmfs_serialroot_find_locked(
	struct vmmfs_serialroot *, const char *);
static void vmmfs_serialroot_drop(struct vmmfs_node *);
static void vmmfs_serialroot_deactivate_port(struct vmmfs_serialport *,
	struct vnode *);

struct vop_ops vmmfs_serialroot_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_serialroot_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_serialroot_getattr,
	.vop_getattr_lite = vmmfs_serialroot_getattr_lite,
	.vop_ncreate = vmmfs_serialroot_ncreate,
	.vop_nlookupdotdot = vmmfs_serialroot_nlookupdotdot,
	.vop_nremove = vmmfs_serialroot_nremove,
	.vop_nresolve = vmmfs_serialroot_nresolve,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_serialroot_readdir,
	.vop_inactive = vmmfs_serialroot_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
};

static int
vmmfs_serialroot_port_compare(struct vmmfs_serialroot_port *left,
	struct vmmfs_serialroot_port *right)
{
	return (strcmp(left->port->name, right->port->name));
}

RB_GENERATE(vmmfs_serialport_tree, vmmfs_serialroot_port, entry,
	vmmfs_serialroot_port_compare);

int
vmmfs_serialroot_init(struct vmmfs_machine *machine,
	struct vmmfs_serialroot *serialroot, struct vnode **vnodep)
{
	struct vmmfs_mount *state;
	int error;

	if (machine == NULL || serialroot == NULL || vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	state = (struct vmmfs_mount *)vmmfs_machine_root(machine)->mount->mnt_data;
	if (state->serialroot_vops == NULL)
		return (ENXIO);
	bzero(serialroot, sizeof(*serialroot));
	serialroot->inode = atomic_fetchadd_int(&state->next_inode, 1);
	RB_INIT(&serialroot->ports);
	vmmfs_branch_init(&serialroot->branch, &machine->branch,
	    vmmfs_serialroot_drop);
	error = vmmfs_vnode_create_regular(vmmfs_machine_root(machine)->mount,
	    &state->serialroot_vops, VDIR, &serialroot->branch.node, vnodep);
	if (error != 0)
		vmmfs_node_drop(&serialroot->branch.node);
	return (error);
}

static void
vmmfs_serialroot_drop(struct vmmfs_node *node)
{
	struct vmmfs_serialroot *serialroot;

	serialroot = (struct vmmfs_serialroot *)node;
	KKASSERT(serialroot != NULL);
	KKASSERT(serialroot->branch.references == 0);
	KKASSERT(RB_EMPTY(&serialroot->ports));
}

void
vmmfs_serialroot_deactivate_begin(struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_serialroot_port *entry;
	struct vmmfs_serialport *port;

	if (serialroot == NULL)
		return;
	lwkt_gettoken(&serialroot->branch.token);
	vmmfs_node_default_deactivate(&serialroot->branch.node);
	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->ports) {
		port = entry->port;
		lwkt_gettoken(&port->token);
		port->destroying = true;
		vmmfs_node_default_deactivate(&port->node);
		lwkt_reltoken(&port->token);
	}
	lwkt_reltoken(&serialroot->branch.token);
}

static void
vmmfs_serialroot_deactivate_port(struct vmmfs_serialport *port,
	struct vnode *vnode)
{
	KKASSERT(port != NULL);
	lwkt_gettoken(&port->token);
	port->destroying = true;
	lwkt_reltoken(&port->token);
	vmmfs_serialport_revoke(port);
	vmmfs_node_deactivate(&port->node);
	vmmfs_vnode_deactivate(vnode);
}

void
vmmfs_serialroot_deactivate_ports(struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_serialroot_port *entry;

	if (serialroot == NULL || vmmfs_serialroot_machine(serialroot) == NULL)
		return;
	for (;;) {
		lwkt_gettoken(&serialroot->branch.token);
		entry = RB_ROOT(&serialroot->ports);
		if (entry != NULL)
			RB_REMOVE(vmmfs_serialport_tree, &serialroot->ports, entry);
		lwkt_reltoken(&serialroot->branch.token);
		if (entry == NULL)
			break;
		vmmfs_serialroot_deactivate_port(entry->port, entry->vnode);
		kfree(entry, M_VMMFS);
	}
}

int
vmmfs_serialroot_start(struct vmmfs_serialroot *serialroot,
	vmm_machine_t machine)
{
	struct vmmfs_serialroot_port *entry;
	struct vmmfs_serialport *port;
	int error;

	if (serialroot == NULL || vmmfs_serialroot_machine(serialroot) == NULL || machine == NULL)
		return (EINVAL);
	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->ports) {
		port = entry->port;
		error = vmmfs_serialport_start(port, machine);
		if (error != 0) {
			(void)vmmfs_serialroot_stop(serialroot);
			return (error);
		}
	}
	return (0);
}

int
vmmfs_serialroot_stop(struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_serialroot_port *entry;
	struct vmmfs_serialport *port;
	int error;
	int result;

	if (serialroot == NULL || vmmfs_serialroot_machine(serialroot) == NULL)
		return (EINVAL);
	result = 0;
	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->ports) {
		port = entry->port;
		error = vmmfs_serialport_stop(port);
		if (result == 0 && error != 0)
			result = error;
	}
	return (result);
}

static int
vmmfs_serialroot_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_SERIALROOT_MODE, 0));
}

static int
vmmfs_serialroot_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_serialroot *serialroot;
	struct vattr *vattr;

	serialroot = ap->a_vp->v_data;
	if (serialroot == NULL || serialroot->branch.node.dead)
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VDIR;
	vattr->va_mode = VMMFS_SERIALROOT_MODE;
	vattr->va_nlink = 2;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = serialroot->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	vattr->va_flags = 0;
	vattr->va_filerev = 0;
	return (0);
}

static int
vmmfs_serialroot_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VDIR;
	ap->a_lvap->va_mode = VMMFS_SERIALROOT_MODE;
	ap->a_lvap->va_nlink = 2;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_serialroot_ncreate(struct vop_ncreate_args *ap)
{
	struct vmmfs_machine *machine;
	struct vmmfs_serialroot *serialroot;
	struct vmmfs_serialroot_port *entry;
	struct vmmfs_serialport *port;
	struct namecache *ncp;
	struct vnode *vnode;
	char name[sizeof(((struct vmmfs_serialport *)0)->name)];
	int error;

	serialroot = ap->a_dvp->v_data;
	machine = serialroot == NULL ? NULL : vmmfs_serialroot_machine(serialroot);
	if (machine == NULL)
		return (ENOENT);
	if (ap->a_vap->va_type != VREG)
		return (EINVAL);
	ncp = ap->a_nch->ncp;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen >= sizeof(name))
		return (EINVAL);
	bcopy(ncp->nc_name, name, ncp->nc_nlen);
	name[ncp->nc_nlen] = 0;
	entry = kmalloc(sizeof(*entry), M_VMMFS, M_WAITOK | M_ZERO);

	lwkt_gettoken(&machine->branch.token);
	lwkt_gettoken(&serialroot->branch.token);
	if (machine->branch.node.dead || machine->machine != NULL ||
	    serialroot->branch.node.dead) {
		error = machine->branch.node.dead || serialroot->branch.node.dead ?
		    ENOENT : EBUSY;
		goto failed_locked;
	}
	if (vmmfs_serialroot_find_locked(serialroot, name) != NULL) {
		error = EEXIST;
		goto failed_locked;
	}
	error = vmmfs_serialport_create(serialroot, name, ncp->nc_nlen,
	    &port, &vnode);
	if (error != 0)
		goto failed_locked;
	entry->port = port;
	entry->vnode = vnode;
	KKASSERT(RB_INSERT(vmmfs_serialport_tree, &serialroot->ports, entry) == NULL);
	lwkt_reltoken(&serialroot->branch.token);
	lwkt_reltoken(&machine->branch.token);

	error = vget(vnode, LK_EXCLUSIVE);
	if (error != 0) {
		lwkt_gettoken(&machine->branch.token);
		lwkt_gettoken(&serialroot->branch.token);
		RB_REMOVE(vmmfs_serialport_tree, &serialroot->ports, entry);
		lwkt_reltoken(&serialroot->branch.token);
		lwkt_reltoken(&machine->branch.token);
		vmmfs_serialroot_deactivate_port(port, vnode);
		goto failed;
	}
	*ap->a_vpp = vnode;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
	return (0);

failed:
	kfree(entry, M_VMMFS);
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED,
	    "name=%.*s error=%d", (int)ncp->nc_nlen, ncp->nc_name, error);
	return (error);

failed_locked:
	lwkt_reltoken(&serialroot->branch.token);
	lwkt_reltoken(&machine->branch.token);
	goto failed;
}

static int
vmmfs_serialroot_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_serialroot *serialroot;
	struct vnode *vnode;
	int error;

	serialroot = ap->a_dvp->v_data;
	if (serialroot == NULL || serialroot->branch.node.dead ||
	    vmmfs_serialroot_machine(serialroot) == NULL)
		return (ENOENT);
	vnode = vmmfs_root_machine_vnode(
	    vmmfs_machine_root(vmmfs_serialroot_machine(serialroot)),
	    vmmfs_serialroot_machine(serialroot));
	if (vnode == NULL)
		return (ENOENT);
	error = vget(vnode, LK_EXCLUSIVE | LK_RETRY);
	vdrop(vnode);
	if (error != 0)
		return (error);
	*ap->a_vpp = vnode;
	vn_unlock(vnode);
	return (0);
}

static int
vmmfs_serialroot_nremove(struct vop_nremove_args *ap)
{
	struct vmmfs_machine *machine;
	struct vmmfs_serialroot *serialroot;
	struct vmmfs_serialroot_port *entry;
	struct vmmfs_serialport *port;
	struct vnode *vnode;
	int error;

	serialroot = ap->a_dvp->v_data;
	machine = serialroot == NULL ? NULL : vmmfs_serialroot_machine(serialroot);
	if (machine == NULL)
		return (ENOENT);
	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vnode);
	if (error != 0)
		return (error);
	vn_unlock(vnode);
	if (vnode->v_type != VCHR) {
		vrele(vnode);
		return (EISDIR);
	}
	port = vnode->v_data;
	if (port == NULL) {
		vrele(vnode);
		return (ENOENT);
	}
	lwkt_gettoken(&machine->branch.token);
	lwkt_gettoken(&serialroot->branch.token);
	entry = vmmfs_serialroot_find_locked(serialroot, port->name);
	if (entry == NULL || entry->port != port ||
	    machine->machine != NULL || serialroot->branch.node.dead) {
		lwkt_reltoken(&serialroot->branch.token);
		lwkt_reltoken(&machine->branch.token);
		vrele(vnode);
		return (entry == NULL || entry->port != port ? ENOENT : EBUSY);
	}
	RB_REMOVE(vmmfs_serialport_tree, &serialroot->ports, entry);
	lwkt_reltoken(&serialroot->branch.token);
	lwkt_reltoken(&machine->branch.token);

	cache_unlink(ap->a_nch);
	vmmfs_serialroot_deactivate_port(port, entry->vnode);
	kfree(entry, M_VMMFS);
	vrele(vnode);
	return (0);
}

static int
vmmfs_serialroot_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_serialroot *serialroot;
	struct vmmfs_serialroot_port *entry;
	struct namecache *ncp;
	struct vnode *vnode;
	char name[sizeof(((struct vmmfs_serialport *)0)->name)];
	int error;

	serialroot = ap->a_dvp->v_data;
	if (serialroot == NULL || serialroot->branch.node.dead ||
	    vmmfs_serialroot_machine(serialroot) == NULL)
		return (ENOENT);
	ncp = ap->a_nch->ncp;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen >= sizeof(name)) {
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	bcopy(ncp->nc_name, name, ncp->nc_nlen);
	name[ncp->nc_nlen] = '\0';
	lwkt_gettoken(&serialroot->branch.token);
	entry = vmmfs_serialroot_find_locked(serialroot, name);
	vnode = entry == NULL ? NULL : entry->vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&serialroot->branch.token);
	if (vnode == NULL) {
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	error = vget(vnode, LK_EXCLUSIVE);
	vdrop(vnode);
	if (error != 0)
		return (error);
	vn_unlock(vnode);
	cache_setvp(ap->a_nch, vnode);
	vrele(vnode);
	return (0);
}

static int
vmmfs_serialroot_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_serialroot *serialroot;
	struct vmmfs_serialroot_item item;
	struct uio *uio;
	off_t offset;
	uint64_t index;
	int error;
	int stop;

	serialroot = ap->a_vp->v_data;
	if (serialroot == NULL || serialroot->branch.node.dead ||
	    vmmfs_serialroot_machine(serialroot) == NULL)
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
		stop = vop_write_dirent(&error, uio, serialroot->inode, DT_DIR, 1,
		    ".");
		if (!stop)
			offset = 1;
	}
	if (!stop && offset == 1) {
		stop = vop_write_dirent(&error, uio, vmmfs_serialroot_machine(serialroot)->inode,
		    DT_DIR, 2, "..");
		if (!stop)
			offset = 2;
	}
	index = offset - 2;
	while (!stop) {
		error = vmmfs_serialroot_read_item(serialroot, index, &item);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0)
			break;
		stop = vop_write_dirent(&error, uio, item.inode, DT_CHR,
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
vmmfs_serialroot_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_serialroot *serialroot;

	serialroot = ap->a_vp->v_data;
	if (serialroot == NULL || !serialroot->branch.node.dead)
		return (0);
	vmmfs_node_inactive(&serialroot->branch.node, ap->a_vp);
	return (0);
}


static struct vmmfs_serialroot_port *
vmmfs_serialroot_find_locked(struct vmmfs_serialroot *serialroot,
	const char *name)
{
	struct vmmfs_serialroot_port *entry;

	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->ports) {
		if (strcmp(entry->port->name, name) == 0)
			return (entry);
	}
	return (NULL);
}

static int
vmmfs_serialroot_read_item(struct vmmfs_serialroot *serialroot,
	uint64_t index, struct vmmfs_serialroot_item *item)
{
	struct vmmfs_serialroot_port *entry;
	uint64_t current;

	lwkt_gettoken(&serialroot->branch.token);
	current = 0;
	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->ports) {
		if (current++ != index)
			continue;
		item->inode = entry->port->inode;
		bcopy(entry->port->name, item->name, sizeof(item->name));
		lwkt_reltoken(&serialroot->branch.token);
		return (0);
	}
	lwkt_reltoken(&serialroot->branch.token);
	return (ENOENT);
}
