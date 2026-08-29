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
#include <sys/vnode.h>

#include "vmmfs.h"

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs objects");

#define VMMFS_ROOT_MODE	0555

static int vmmfs_root_access(struct vop_access_args *);
static int vmmfs_root_getattr(struct vop_getattr_args *);
static int vmmfs_root_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_root_nmkdir(struct vop_nmkdir_args *);
static int vmmfs_root_nresolve(struct vop_nresolve_args *);
static int vmmfs_root_nrmdir(struct vop_nrmdir_args *);
static int vmmfs_root_readdir(struct vop_readdir_args *);
static int vmmfs_root_inactive(struct vop_inactive_args *);
static int vmmfs_root_reclaim(struct vop_reclaim_args *);
static int vmmfs_root_read_item(struct vmmfs_root *, uint64_t,
	struct vmmfs_item *);
static int vmmfs_root_create_item(struct vmmfs_root *, const char *, size_t,
	struct vmmfs_machine **);
static int vmmfs_root_remove_item(struct vmmfs_root *, const char *, size_t,
	struct vmmfs_machine **);
static void vmmfs_root_drop(struct vmmfs_node *);

struct vop_ops vmmfs_root_vops = {
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
	.vop_inactive = vmmfs_root_inactive,
	.vop_reclaim = vmmfs_root_reclaim,
};

int
vmmfs_root_create(struct mount *mount, struct vmmfs_root **rootp)
{
	struct vmmfs_mount *state;
	struct vmmfs_root *root;
	int error;

	root = kmalloc(sizeof(*root), M_VMMFS, M_WAITOK | M_ZERO);
	vmmfs_branch_init(&root->branch, NULL, vmmfs_root_drop, NULL);
	root->mount = mount;
	state = (struct vmmfs_mount *)mount->mnt_data;
	lwkt_token_init(&root->token, "vmmfsroot");
	RB_INIT(&root->machines);
	error = vmmfs_node_publish_regular(&root->branch.node, mount,
		&state->root_vops, VDIR, root);
	if (error != 0) {
		vmmfs_branch_abort(&root->branch);
		return (error);
	}
	vmmfs_branch_hold(&root->branch);
	error = vget(root->branch.node.vnode, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0) {
		vmmfs_branch_abort(&root->branch);
		return (error);
	}
	vsetflags(root->branch.node.vnode, VROOT);
	vn_unlock(root->branch.node.vnode);
	vrele(root->branch.node.vnode);
	*rootp = root;
	return (0);
}
int
vmmfs_root_destroy(struct vmmfs_root *root)
{
	int busy;

	if (root == NULL)
		return (EINVAL);
	lwkt_gettoken(&root->token);
	busy = root->branch.node.vnode != NULL || root->machine_count != 0 ||
	    !RB_EMPTY(&root->machines);
	lwkt_reltoken(&root->token);
	if (busy)
		return (EBUSY);
	vmmfs_branch_put(&root->branch);
	return (0);
}

static void
vmmfs_root_drop(struct vmmfs_node *node)
{
	struct vmmfs_root *root;

	root = (struct vmmfs_root *)node;
	KKASSERT(root->branch.references == 0);
	KKASSERT(RB_EMPTY(&root->machines));
	lwkt_token_uninit(&root->token);
	kfree(root, M_VMMFS);
}


static int
vmmfs_root_read_item(struct vmmfs_root *root, uint64_t index,
	struct vmmfs_item *item)
{
	struct vmmfs_machine *machine;
	uint64_t current;

	lwkt_gettoken(&root->token);
	current = 0;
	RB_FOREACH(machine, vmmfs_machine_tree, &root->machines) {
		if (current++ != index)
			continue;
		item->id = machine->inode;
		bcopy(machine->name, item->name, sizeof(item->name));
		item->machine = machine;
		vmmfs_machine_hold(machine);
		lwkt_reltoken(&root->token);
		return (0);
	}
	lwkt_reltoken(&root->token);
	return (ENOENT);
}

static int
vmmfs_root_create_item(struct vmmfs_root *root, const char *name,
	size_t namelen, struct vmmfs_machine **machinep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_machine *cursor;
	int error;

	if (namelen == 0 || namelen > NAME_MAX)
		return (ENAMETOOLONG);
	lwkt_gettoken(&root->token);
	RB_FOREACH(cursor, vmmfs_machine_tree, &root->machines) {
		if (strcmp(cursor->name, name) == 0)
			break;
	}
	lwkt_reltoken(&root->token);
	if (cursor != NULL)
		return (EEXIST);
	error = vmmfs_machine_create(root, name, namelen, &machine);
	if (error != 0)
		return (error);
	error = vmmfs_machine_publish(machine);
	if (error != 0)
		return (error);
	lwkt_gettoken(&root->token);
	RB_FOREACH(cursor, vmmfs_machine_tree, &root->machines) {
		if (strcmp(cursor->name, machine->name) == 0)
			break;
	}
	if (cursor != NULL) {
		lwkt_reltoken(&root->token);
		vmmfs_machine_abort_create(machine);
		return (EEXIST);
	}
	RB_INSERT(vmmfs_machine_tree, &root->machines, machine);
	machine->root_counted = true;
	++root->machine_count;
	lwkt_reltoken(&root->token);
	*machinep = machine;
	return (0);
}

static int
vmmfs_root_remove_item(struct vmmfs_root *root, const char *name,
	size_t namelen, struct vmmfs_machine **machinep)
{
	struct vmmfs_machine key;
	struct vmmfs_machine *machine;
	int runtime_active;

	if (machinep == NULL)
		return (EINVAL);
	if (namelen == 0 || namelen > NAME_MAX)
		return (ENAMETOOLONG);
	*machinep = NULL;
	bzero(&key, sizeof(key));
	bcopy(name, key.name, namelen);
	key.name[namelen] = '\0';
	lwkt_gettoken(&root->token);
	machine = RB_FIND(vmmfs_machine_tree, &root->machines, &key);
	if (machine == NULL) {
		lwkt_reltoken(&root->token);
		return (ENOENT);
	}
	lwkt_gettoken(&machine->token);
	if (vmmfs_machine_root(machine) != root || machine->dead) {
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&root->token);
		return (ENOENT);
	}
	runtime_active = machine->machine != NULL;
	if (runtime_active) {
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&root->token);
		vmmfs_events_log(&machine->events,
		    VMMFS_MACHINE_EVENT_DESTROY_REFUSED, "runtime=%d",
		    runtime_active);
		return (EBUSY);
	}
	RB_REMOVE(vmmfs_machine_tree, &root->machines, machine);
	machine->dead = true;
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&root->token);
	*machinep = machine;
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
	struct vmmfs_root *root;
	struct vmmfs_item item;
	struct uio *uio;
	off_t offset;
	uint64_t index;
	int error;
	int stop;

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
		error = vmmfs_root_read_item(root, index, &item);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0)
			break;
		stop = vop_write_dirent(&error, uio, item.id, DT_DIR,
		    (uint16_t)strlen(item.name), item.name);
		vmmfs_machine_put(item.machine);
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
	struct vmmfs_item item;
	struct vmmfs_machine *machine;
	struct vnode *vnode;
	struct namecache *ncp;
	uint64_t index;
	int error;

	root = ap->a_dvp->v_data;
	if (root == NULL)
		return (ENXIO);
	ncp = ap->a_nch->ncp;
	machine = NULL;
	for (index = 0;; index++) {
		error = vmmfs_root_read_item(root, index, &item);
		if (error != 0)
			break;
		if (ncp->nc_nlen != strlen(item.name)) {
			vmmfs_machine_put(item.machine);
			continue;
		}
		if (strncmp(ncp->nc_name, item.name, ncp->nc_nlen) == 0) {
			machine = item.machine;
			break;
		}
		vmmfs_machine_put(item.machine);
	}
	if (machine == NULL) {
		cache_setvp(ap->a_nch, NULL);
		return (ENOENT);
	}
	lwkt_gettoken(&machine->token);
	vnode = machine->dead ? NULL : machine->branch.node.vnode;
	if (vnode != NULL)
		vhold(vnode);
	lwkt_reltoken(&machine->token);
	if (vnode == NULL) {
		error = ENOENT;
	} else {
		error = vget(vnode, LK_EXCLUSIVE);
		vdrop(vnode);
	}
	vmmfs_machine_put(machine);
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
	struct namecache *ncp;
	struct vnode *vnode;
	int error;
	int cleanup_error;

	root = ap->a_dvp->v_data;
	if (root == NULL)
		return (ENXIO);
	if (ap->a_vap->va_type != VDIR)
		return (EINVAL);
	ncp = ap->a_nch->ncp;
	error = vmmfs_root_create_item(root, ncp->nc_name, ncp->nc_nlen,
	    &machine);
	if (error != 0)
		return (error);
	vnode = machine->branch.node.vnode;
	if (vnode == NULL) {
		error = vmmfs_root_remove_item(root, ncp->nc_name, ncp->nc_nlen,
			&machine);
		if (error != 0)
			return (error);
		error = vmmfs_machine_unpublish(machine);
		vmmfs_machine_put(machine);
		if (error != 0)
			return (error);
		return (ENOMEM);
	}
	*ap->a_vpp = vnode;
	error = vget(vnode, LK_EXCLUSIVE);
	if (error != 0) {
		cleanup_error = vmmfs_root_remove_item(root, ncp->nc_name,
			ncp->nc_nlen, &machine);
		if (cleanup_error == 0) {
			cleanup_error = vmmfs_machine_unpublish(machine);
			vmmfs_machine_put(machine);
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
		&machine);
	if (error == 0) {
		cache_unlink(ap->a_nch);
		error = vmmfs_machine_unpublish(machine);
		vmmfs_machine_put(machine);
	}
	vrele(vnode);
	return (error);
}

static int
vmmfs_root_inactive(struct vop_inactive_args *ap)
{
	/* The root remains alive until the mount teardown releases its base ref. */
	(void)ap;
	return (0);
}

static int
vmmfs_root_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_root *root;
	bool reclaim;

	root = ap->a_vp->v_data;
	if (root == NULL)
		return (0);
	lwkt_gettoken(&root->token);
	reclaim = vmmfs_node_reclaim(&root->branch.node, ap->a_vp);
	lwkt_reltoken(&root->token);
	if (reclaim)
		vmmfs_branch_put(&root->branch);
	return (0);
}
