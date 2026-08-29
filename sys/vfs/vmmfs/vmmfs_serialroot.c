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
static int vmmfs_serialroot_open(struct vop_open_args *);
static int vmmfs_serialroot_readdir(struct vop_readdir_args *);
static int vmmfs_serialroot_inactive(struct vop_inactive_args *);
static int vmmfs_serialroot_reclaim(struct vop_reclaim_args *);
static int vmmfs_serialroot_read_item(struct vmmfs_serialroot *, uint64_t,
	struct vmmfs_serialroot_item *);
static void vmmfs_serialroot_drop(struct vmmfs_node *);
static bool vmmfs_serialroot_is_dead(struct vmmfs_node *);

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
	.vop_open = vmmfs_serialroot_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_serialroot_readdir,
	.vop_inactive = vmmfs_serialroot_inactive,
	.vop_reclaim = vmmfs_serialroot_reclaim,
};

int
vmmfs_serialroot_init(struct vmmfs_machine *machine,
	struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_mount *state;

	if (machine == NULL || serialroot == NULL)
		return (EINVAL);
	state = (struct vmmfs_mount *)machine->root->mount->mnt_data;
	if (state->serialroot_vops == NULL)
		return (ENXIO);
	bzero(serialroot, sizeof(*serialroot));
	serialroot->machine = machine;
	serialroot->inode = atomic_fetchadd_int(&state->next_inode, 1);
	RB_INIT(&serialroot->ports);
	vmmfs_branch_init(&serialroot->branch, &machine->branch.node,
	    vmmfs_serialroot_drop, vmmfs_serialroot_is_dead);
	vmmfs_machine_hold(machine);
	return (0);

}

int
vmmfs_serialroot_publish(struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_mount *state;
	int error;

	if (serialroot == NULL || serialroot->machine == NULL)
		return (EINVAL);
	state = (struct vmmfs_mount *)serialroot->machine->root->mount->mnt_data;
	if (state == NULL || state->serialroot_vops == NULL)
		return (ENXIO);
	error = vmmfs_node_publish_regular(&serialroot->branch.node,
	    serialroot->machine->root->mount, &state->serialroot_vops, VDIR,
	    serialroot);
	if (error == 0)
		vmmfs_branch_hold(&serialroot->branch);
	return (error);
}

void
vmmfs_serialroot_fini(struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_machine *machine;

	if (serialroot == NULL)
		return;
	machine = serialroot->machine;
	if (machine == NULL)
		return;
	if (serialroot->branch.node.vnode != NULL)
		panic("vmmfs_serialroot_fini: vnode is still published");
	KKASSERT(serialroot->branch.references == 0);
	KKASSERT(RB_EMPTY(&serialroot->ports));
	serialroot->machine = NULL;
	return;
}

void
vmmfs_serialroot_release_vnodes(struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_serialport *port;

	if (serialroot == NULL || serialroot->machine == NULL)
		return;
	for (;;) {
		lwkt_gettoken(&serialroot->machine->token);
		port = RB_ROOT(&serialroot->ports);
		if (port != NULL) {
			lwkt_gettoken(&port->token);
			port->destroying = true;
			lwkt_reltoken(&port->token);
			RB_REMOVE(vmmfs_serialport_tree, &serialroot->ports, port);
		}
		lwkt_reltoken(&serialroot->machine->token);
		if (port == NULL)
			break;
		vmmfs_serialport_revoke(port);
		vmmfs_node_unpublish(&port->node);
	}
	vmmfs_node_unpublish(&serialroot->branch.node);
}

int
vmmfs_serialroot_start(struct vmmfs_serialroot *serialroot,
	vmm_machine_t machine)
{
	struct vmmfs_serialport *port;
	int error;

	if (serialroot == NULL || serialroot->machine == NULL || machine == NULL)
		return (EINVAL);
	RB_FOREACH(port, vmmfs_serialport_tree, &serialroot->ports) {
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
	struct vmmfs_serialport *port;
	int error;
	int result;

	if (serialroot == NULL || serialroot->machine == NULL)
		return (EINVAL);
	result = 0;
	RB_FOREACH(port, vmmfs_serialport_tree, &serialroot->ports) {
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
	if (serialroot == NULL)
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

static void
vmmfs_serialroot_abort_port(struct vmmfs_serialport *port)
{
	KKASSERT(port != NULL && port->serialroot != NULL);
	lwkt_gettoken(&port->token);
	port->destroying = true;
	lwkt_reltoken(&port->token);
	vmmfs_node_abort(&port->node);
	vmmfs_serialport_destroy(port);
}

static int
vmmfs_serialroot_ncreate(struct vop_ncreate_args *ap)
{
	struct vmmfs_serialroot *serialroot;
	struct vmmfs_serialport *port;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	serialroot = ap->a_dvp->v_data;
	if (serialroot == NULL || serialroot->machine == NULL)
		return (ENOENT);
	if (ap->a_vap->va_type != VREG)
		return (EINVAL);
	ncp = ap->a_nch->ncp;
	error = vmmfs_serialport_create(serialroot, ncp->nc_name, ncp->nc_nlen,
	    &port);
	if (error != 0) {
		vmmfs_events_log(&serialroot->machine->events,
		    VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED,
		    "name=%.*s error=%d", (int)ncp->nc_nlen, ncp->nc_name, error);
		return (error);
	}
	error = vmmfs_serialport_publish(port);
	if (error != 0) {
		vmmfs_serialroot_abort_port(port);
		vmmfs_events_log(&serialroot->machine->events,
		    VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED,
		    "name=%.*s error=%d", (int)ncp->nc_nlen, ncp->nc_name, error);
		return (error);
	}
	lwkt_gettoken(&serialroot->machine->token);
	if (serialroot->machine->root == NULL || serialroot->machine->dead) {
		lwkt_reltoken(&serialroot->machine->token);
		vmmfs_serialroot_abort_port(port);
		vmmfs_events_log(&serialroot->machine->events,
		    VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED,
		    "name=%.*s error=%d", (int)ncp->nc_nlen, ncp->nc_name, ENOENT);
		return (ENOENT);
	}
	if (serialroot->machine->machine != NULL) {
		lwkt_reltoken(&serialroot->machine->token);
		vmmfs_serialroot_abort_port(port);
		vmmfs_events_log(&serialroot->machine->events,
		    VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED,
		    "name=%.*s error=%d", (int)ncp->nc_nlen, ncp->nc_name, EBUSY);
		return (EBUSY);
	}
	if (RB_INSERT(vmmfs_serialport_tree, &serialroot->ports, port) != NULL) {
		lwkt_reltoken(&serialroot->machine->token);
		vmmfs_serialroot_abort_port(port);
		vmmfs_events_log(&serialroot->machine->events,
		    VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED,
		    "name=%.*s error=%d", (int)ncp->nc_nlen, ncp->nc_name, EEXIST);
		return (EEXIST);
	}
	lwkt_reltoken(&serialroot->machine->token);
	vnode = port->node.vnode;
	error = vget(vnode, LK_EXCLUSIVE);
	if (error != 0) {
		lwkt_gettoken(&serialroot->machine->token);
		RB_REMOVE(vmmfs_serialport_tree, &serialroot->ports, port);
		lwkt_reltoken(&serialroot->machine->token);
		lwkt_gettoken(&port->token);
		port->destroying = true;
		lwkt_reltoken(&port->token);
		vmmfs_serialport_revoke(port);
		vmmfs_node_unpublish(&port->node);
		vmmfs_events_log(&serialroot->machine->events,
		    VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED,
		    "name=%.*s error=%d", (int)ncp->nc_nlen, ncp->nc_name, error);
		return (error);
	}
	*ap->a_vpp = vnode;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
	return (0);
}

static int
vmmfs_serialroot_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vmmfs_serialroot *serialroot;
	struct vnode *vnode;
	int error;

	serialroot = ap->a_dvp->v_data;
	if (serialroot == NULL || serialroot->machine == NULL)
		return (ENOENT);
	lwkt_gettoken(&serialroot->machine->token);
	vnode = serialroot->machine->branch.node.vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&serialroot->machine->token);
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
	struct vmmfs_serialroot *serialroot;
	struct vmmfs_serialport *port;
	struct vnode *vnode;
	int error;

	serialroot = ap->a_dvp->v_data;
	if (serialroot == NULL || serialroot->machine == NULL)
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
	lwkt_gettoken(&serialroot->machine->token);
	if (port->serialroot != serialroot) {
		lwkt_reltoken(&serialroot->machine->token);
		vrele(vnode);
		return (ENOENT);
	}
	if (serialroot->machine->machine != NULL) {
		lwkt_reltoken(&serialroot->machine->token);
		vrele(vnode);
		return (EBUSY);
	}
	lwkt_gettoken(&port->token);
	port->destroying = true;
	lwkt_reltoken(&port->token);
	RB_REMOVE(vmmfs_serialport_tree, &serialroot->ports, port);
	lwkt_reltoken(&serialroot->machine->token);
	vmmfs_serialport_revoke(port);
	cache_unlink(ap->a_nch);
	vmmfs_node_unpublish(&port->node);
	vrele(vnode);
	return (0);
}

static int
vmmfs_serialroot_nresolve(struct vop_nresolve_args *ap)
{
	struct vmmfs_serialroot *serialroot;
	struct vmmfs_serialport key;
	struct vmmfs_serialport *port;
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

	serialroot = ap->a_dvp->v_data;
	if (serialroot == NULL || serialroot->machine == NULL)
		return (ENOENT);
	ncp = ap->a_nch->ncp;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen >= sizeof(key.name)) {
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	bzero(&key, sizeof(key));
	bcopy(ncp->nc_name, key.name, ncp->nc_nlen);
	key.name[ncp->nc_nlen] = '\0';
	lwkt_gettoken(&serialroot->machine->token);
	port = RB_FIND(vmmfs_serialport_tree, &serialroot->ports, &key);
	vnode = port == NULL ? NULL : port->node.vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&serialroot->machine->token);
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
vmmfs_serialroot_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
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
	if (serialroot == NULL || serialroot->machine == NULL)
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
		stop = vop_write_dirent(&error, uio, serialroot->machine->inode,
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
	struct vmmfs_machine *machine;

	serialroot = ap->a_vp->v_data;
	if (serialroot == NULL || serialroot->machine == NULL)
		return (0);
	machine = serialroot->machine;
	if (!vmmfs_machine_is_dead(machine))
		return (0);
	vmmfs_node_inactive(&serialroot->branch.node, ap->a_vp);
	return (0);
}

static int
vmmfs_serialroot_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_serialroot *serialroot;
	struct vmmfs_machine *machine;
	bool reclaim;

	serialroot = ap->a_vp->v_data;
	if (serialroot == NULL || serialroot->machine == NULL)
		return (0);
	machine = serialroot->machine;
	lwkt_gettoken(&machine->token);
	reclaim = vmmfs_node_reclaim(&serialroot->branch.node, ap->a_vp);
	lwkt_reltoken(&machine->token);
	if (reclaim)
		vmmfs_branch_put(&serialroot->branch);
	return (0);
}

static void
vmmfs_serialroot_drop(struct vmmfs_node *node)
{
	struct vmmfs_serialroot *serialroot;
	struct vmmfs_machine *machine;

	serialroot = (struct vmmfs_serialroot *)node;
	machine = serialroot->machine;
	KKASSERT(machine != NULL);
	KKASSERT(serialroot->branch.references == 0);
	KKASSERT(vmmfs_node_detach_parent(&serialroot->branch.node) ==
	    &machine->branch.node);
	vmmfs_serialroot_fini(serialroot);
	vmmfs_machine_put(machine);
}

static bool
vmmfs_serialroot_is_dead(struct vmmfs_node *node)
{
	struct vmmfs_serialroot *serialroot;

	serialroot = (struct vmmfs_serialroot *)node;
	return (serialroot->machine == NULL ||
	    vmmfs_machine_is_dead(serialroot->machine));
}

static int
vmmfs_serialroot_read_item(struct vmmfs_serialroot *serialroot,
	uint64_t index, struct vmmfs_serialroot_item *item)
{
	struct vmmfs_serialport *port;
	uint64_t current;

	lwkt_gettoken(&serialroot->machine->token);
	current = 0;
	RB_FOREACH(port, vmmfs_serialport_tree, &serialroot->ports) {
		if (current++ != index)
			continue;
		item->inode = port->inode;
		bcopy(port->name, item->name, sizeof(item->name));
		lwkt_reltoken(&serialroot->machine->token);
		return (0);
	}
	lwkt_reltoken(&serialroot->machine->token);
	return (ENOENT);
}
