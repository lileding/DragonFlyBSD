/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs mount root.
 */
#include <sys/errno.h>
#include <sys/dirent.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/tree.h>
#include <sys/vnode.h>

#include "vmmfs.h"
#include "vmmfs_branch.h"
#include "vmmfs_events.h"
#include "vmmfs_machine.h"
#include "vmmfs_node.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs objects");

#define VMMFS_ROOT_MODE	0555

struct vmmfs_root_machine {
	RB_ENTRY(vmmfs_root_machine) entry;
	struct vnode *vnode;
};

RB_HEAD(vmmfs_machine_tree, vmmfs_root_machine);
RB_PROTOTYPE(vmmfs_machine_tree, vmmfs_root_machine, entry,
	vmmfs_root_machine_compare);

struct vmmfs_root {
	struct vmmfs_branch branch;
	ino_t inode;
	volatile u_int next_inode;
	struct vmmfs_machine_tree machines;
};

static int vmmfs_root_nmkdir(struct vop_nmkdir_args *);
static int vmmfs_root_nresolve(struct vop_nresolve_args *);
static int vmmfs_root_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_root_readdir(struct vop_readdir_args *);
static int vmmfs_root_read_item(struct vmmfs_root *, uint64_t,
	struct vnode **);
static struct vmmfs_root_machine *vmmfs_root_find_locked(
	struct vmmfs_root *, const char *, size_t);
static int vmmfs_root_create_item(struct vmmfs_root *,
	struct vmmfs_mount *, const char *, size_t, struct vmmfs_machine **,
	struct vnode **);
static int vmmfs_root_remove_item(struct vmmfs_root *, const char *, size_t,
	struct vmmfs_machine **, struct vnode **);
static void vmmfs_root_drop(struct vmmfs_node *);


struct vop_ops vmmfs_root_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_node_open,
	.vop_nmkdir = vmmfs_root_nmkdir,
	.vop_nresolve = vmmfs_root_nresolve,
	.vop_nrmdir = vmmfs_root_nrmdir,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_root_readdir,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
};

static int
vmmfs_root_machine_compare(struct vmmfs_root_machine *left,
	struct vmmfs_root_machine *right)
{
	struct vmmfs_machine *left_machine;
	struct vmmfs_machine *right_machine;

	left_machine = left->vnode->v_data;
	right_machine = right->vnode->v_data;
	KKASSERT(left_machine != NULL);
	KKASSERT(right_machine != NULL);
	return (strcmp(left_machine->name, right_machine->name));
}

RB_GENERATE(vmmfs_machine_tree, vmmfs_root_machine, entry,
	vmmfs_root_machine_compare);

int
vmmfs_root_create(struct mount *mount, struct vnode **vnodep)
{
	struct vmmfs_mount *state;
	struct vmmfs_root *root;
	struct vnode *vnode;
	int error;

	if (mount == NULL || vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	state = (struct vmmfs_mount *)mount->mnt_data;
	if (state == NULL || state->root_vops == NULL)
		return (ENXIO);
	root = kmalloc(sizeof(*root), M_VMMFS, M_WAITOK | M_ZERO);
	vmmfs_branch_init(&root->branch, NULL, vmmfs_root_drop);
	root->next_inode = 1;
	root->inode = vmmfs_root_allocate_inode(root);
	state->root_inode = root->inode;
	vmmfs_node_set_metadata(&root->branch.node, root->inode,
	    VMMFS_ROOT_MODE, 0);
	RB_INIT(&root->machines);
	error = vmmfs_vnode_create_regular(mount, &state->root_vops, VDIR,
		&root->branch.node, &vnode);
	if (error != 0) {
		vmmfs_node_drop(&root->branch.node);
		return (error);
	}
	error = vget(vnode, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0) {
		vmmfs_vnode_discard(vnode);
		vmmfs_node_drop(&root->branch.node);
		return (error);
	}
	vsetflags(vnode, VROOT);
	vn_unlock(vnode);
	vrele(vnode);
	*vnodep = vnode;
	return (0);
}

ino_t
vmmfs_root_allocate_inode(struct vmmfs_root *root)
{
	if (root == NULL)
		return (0);
	return atomic_fetchadd_int(&root->next_inode, 1);
}

bool
vmmfs_root_empty(struct vmmfs_root *root)
{
	bool empty;

	if (root == NULL)
		return (true);
	lwkt_gettoken(&root->branch.token);
	empty = RB_EMPTY(&root->machines);
	lwkt_reltoken(&root->branch.token);
	return (empty);
}


static void
vmmfs_root_drop(struct vmmfs_node *node)
{
	struct vmmfs_root *root;

	root = (struct vmmfs_root *)node;
	KKASSERT(root->branch.references == 0);
	KKASSERT(RB_EMPTY(&root->machines));
	kfree(root, M_VMMFS);
}


static struct vmmfs_root_machine *
vmmfs_root_find_locked(struct vmmfs_root *root, const char *name,
	size_t namelen)
{
	struct vmmfs_root_machine *entry;
	struct vmmfs_machine *machine;

	RB_FOREACH(entry, vmmfs_machine_tree, &root->machines) {
		machine = entry->vnode->v_data;
		KKASSERT(machine != NULL);
		if (strlen(machine->name) == namelen &&
		    bcmp(machine->name, name, namelen) == 0)
			return (entry);
	}
	return (NULL);
}

static int
vmmfs_root_read_item(struct vmmfs_root *root, uint64_t index,
	struct vnode **vnodep)
{
	struct vmmfs_root_machine *entry;
	uint64_t current;

	if (vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	lwkt_gettoken(&root->branch.token);
	current = 0;
	RB_FOREACH(entry, vmmfs_machine_tree, &root->machines) {
		if (current++ != index)
			continue;
		*vnodep = entry->vnode;
		vhold(*vnodep);
		lwkt_reltoken(&root->branch.token);
		return (0);
	}
	lwkt_reltoken(&root->branch.token);
	return (ENOENT);
}

static int
vmmfs_root_create_item(struct vmmfs_root *root, struct vmmfs_mount *mount,
	const char *name, size_t namelen, struct vmmfs_machine **machinep,
	struct vnode **vnodep)
{
	struct vmmfs_root_machine *entry;
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	int error;

	if (mount == NULL || machinep == NULL || vnodep == NULL || namelen == 0 ||
	    namelen > NAME_MAX)
		return (ENAMETOOLONG);
	*machinep = NULL;
	*vnodep = NULL;
	entry = kmalloc(sizeof(*entry), M_VMMFS, M_WAITOK | M_ZERO);
	lwkt_gettoken(&root->branch.token);
	if (vmmfs_root_find_locked(root, name, namelen) != NULL) {
		lwkt_reltoken(&root->branch.token);
		kfree(entry, M_VMMFS);
		return (EEXIST);
	}
	/* The root token covers construction through the registry commit point. */
	error = vmmfs_machine_create(&root->branch, mount,
	    vmmfs_root_allocate_inode(root), name, namelen, &machine, &vnode);
	if (error != 0) {
		lwkt_reltoken(&root->branch.token);
		kfree(entry, M_VMMFS);
		return (error);
	}
	entry->vnode = vnode;
	RB_INSERT(vmmfs_machine_tree, &root->machines, entry);
	lwkt_reltoken(&root->branch.token);
	*machinep = machine;
	*vnodep = vnode;
	return (0);
}

static int
vmmfs_root_remove_item(struct vmmfs_root *root, const char *name,
	size_t namelen, struct vmmfs_machine **machinep, struct vnode **vnodep)
{
	struct vmmfs_root_machine *entry;
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	int runtime_active;

	if (machinep == NULL || vnodep == NULL)
		return (EINVAL);
	if (namelen == 0 || namelen > NAME_MAX)
		return (ENAMETOOLONG);
	*machinep = NULL;
	*vnodep = NULL;
	lwkt_gettoken(&root->branch.token);
	entry = vmmfs_root_find_locked(root, name, namelen);
	if (entry == NULL) {
		lwkt_reltoken(&root->branch.token);
		return (ENOENT);
	}
	machine = entry->vnode->v_data;
	if (machine == NULL) {
		lwkt_reltoken(&root->branch.token);
		return (ENOENT);
	}
	lwkt_gettoken(&machine->branch.token);
	if (vmmfs_machine_root(machine) != root || machine->branch.node.dead) {
		lwkt_reltoken(&machine->branch.token);
		lwkt_reltoken(&root->branch.token);
		return (ENOENT);
	}
	runtime_active = machine->machine != NULL;
	if (runtime_active) {
		lwkt_reltoken(&machine->branch.token);
		lwkt_reltoken(&root->branch.token);
		vmmfs_events_log(&machine->events,
		    VMMFS_MACHINE_EVENT_DESTROY_REFUSED, "runtime=%d",
		    runtime_active);
		return (EBUSY);
	}
	vmmfs_branch_hold(&machine->branch);
	RB_REMOVE(vmmfs_machine_tree, &root->machines, entry);
	vnode = entry->vnode;
	machine->vnode = NULL;
	kfree(entry, M_VMMFS);
	lwkt_reltoken(&machine->branch.token);
	lwkt_reltoken(&root->branch.token);
	*machinep = machine;
	*vnodep = vnode;
	return (0);
}

static int
vmmfs_root_readdir(struct vop_readdir_args *ap)
{
	struct vmmfs_root *root;
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	struct uio *uio;
	char name[NAME_MAX + 1];
	off_t offset;
	ino_t inode;
	uint64_t index;
	int error;
	int stop;
	bool visible;

	if (ap->a_vp->v_type != VDIR)
		return (ENOTDIR);
	root = ap->a_vp->v_data;
	if (root == NULL)
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
		stop = vop_write_dirent(&error, uio, root->inode, DT_DIR, 1,
		    ".");
		if (!stop)
			offset = 1;
	}
	if (!stop && offset == 1) {
		stop = vop_write_dirent(&error, uio, root->inode, DT_DIR, 2,
		    "..");
		if (!stop)
			offset = 2;
	}
	index = offset - 2;
	while (!stop) {
		vnode = NULL;
		error = vmmfs_root_read_item(root, index, &vnode);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0)
			break;
		machine = vnode->v_data;
		visible = false;
		if (machine != NULL) {
			lwkt_gettoken(&machine->branch.token);
			if (!machine->branch.node.dead) {
				inode = machine->inode;
				bcopy(machine->name, name, sizeof(name));
				visible = true;
			}
			lwkt_reltoken(&machine->branch.token);
		}
		vdrop(vnode);
		if (!visible) {
			offset++;
			index++;
			continue;
		}
		stop = vop_write_dirent(&error, uio, inode, DT_DIR,
		    (uint16_t)strlen(name), name);
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
	struct vmmfs_root *root;
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	struct namecache *ncp;
	uint64_t index;
	int error;
	bool match;

	root = ap->a_dvp->v_data;
	if (root == NULL)
		return (ENXIO);
	ncp = ap->a_nch->ncp;
	machine = NULL;
	vnode = NULL;
	for (index = 0;; index++) {
		vnode = NULL;
		error = vmmfs_root_read_item(root, index, &vnode);
		if (error != 0)
			break;
		machine = vnode->v_data;
		match = false;
		if (machine != NULL) {
			lwkt_gettoken(&machine->branch.token);
			match = !machine->branch.node.dead &&
			    ncp->nc_nlen == strlen(machine->name) &&
			    strncmp(ncp->nc_name, machine->name, ncp->nc_nlen) == 0;
			lwkt_reltoken(&machine->branch.token);
		}
		if (match)
			break;
		vdrop(vnode);
		vnode = NULL;
		machine = NULL;
	}
	if (machine == NULL || vnode == NULL) {
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
vmmfs_root_nmkdir(struct vop_nmkdir_args *ap)
{
	struct vmmfs_root *root;
	struct vmmfs_machine *machine;
	struct vmmfs_mount *mount;
	struct namecache *ncp;
	struct vnode *vnode;
	struct vnode *owner_vnode;
	int error;
	int cleanup_error;

	root = ap->a_dvp->v_data;
	if (root == NULL)
		return (ENXIO);
	if (ap->a_vap->va_type != VDIR)
		return (EINVAL);
	mount = (struct vmmfs_mount *)ap->a_dvp->v_mount->mnt_data;
	if (mount == NULL)
		return (ENXIO);
	ncp = ap->a_nch->ncp;
	error = vmmfs_root_create_item(root, mount, ncp->nc_name, ncp->nc_nlen,
	    &machine, &vnode);
	if (error != 0)
		return (error);
	if (vnode == NULL)
		return (ENOMEM);
	*ap->a_vpp = vnode;
	error = vget(vnode, LK_EXCLUSIVE);
	if (error != 0) {
		cleanup_error = vmmfs_root_remove_item(root, ncp->nc_name,
		    ncp->nc_nlen, &machine, &owner_vnode);
		if (cleanup_error == 0) {
			vmmfs_vnode_deactivate(owner_vnode);
			vmmfs_branch_put(&machine->branch);
		}
		return (cleanup_error != 0 ? cleanup_error : error);
	}
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vnode);
	return (0);
}

static int
vmmfs_root_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vmmfs_root *root;
	struct vnode *vnode;
	struct vnode *owner_vnode;
	struct vmmfs_machine *machine;
	struct namecache *ncp;
	int error;

	root = ap->a_dvp->v_data;
	if (root == NULL)
		return (ENXIO);
	ncp = ap->a_nch->ncp;
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
	error = vmmfs_root_remove_item(root, ncp->nc_name, ncp->nc_nlen,
	    &machine, &owner_vnode);
	if (error == 0) {
		cache_unlink(ap->a_nch);
		vmmfs_vnode_deactivate(owner_vnode);
		vmmfs_branch_put(&machine->branch);
	}
	vrele(vnode);
	return (error);
}
