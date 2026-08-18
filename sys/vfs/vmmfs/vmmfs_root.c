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
static int vmmfs_root_reclaim(struct vop_reclaim_args *);
static int vmmfs_root_read_item(struct vmmfs_root *, uint64_t,
	struct vmmfs_item *);
static int vmmfs_root_create_item(struct vmmfs_root *, const char *, size_t,
	struct vmmfs_machine **);
static int vmmfs_root_remove_item(struct vmmfs_root *, const char *, size_t);

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
	.vop_reclaim = vmmfs_root_reclaim,
};

int
vmmfs_root_create(struct mount *mount, struct vmmfs_root **rootp)
{
	struct vmmfs_mount *state;
	struct vmmfs_root *root;
	struct vnode *vnode;
	int error;

	root = kmalloc(sizeof(*root), M_VMMFS, M_WAITOK | M_ZERO);
	root->mount = mount;
	state = (struct vmmfs_mount *)mount->mnt_data;
	lwkt_token_init(&root->token, "vmmfsroot");
	RB_INIT(&root->machines);
	error = getnewvnode(VT_SYNTH, mount, &vnode, 0, 0);
	if (error != 0) {
		lwkt_token_uninit(&root->token);
		kfree(root, M_VMMFS);
		return (error);
	}
	vnode->v_data = root;
	vnode->v_ops = &state->root_vops;
	vnode->v_type = VDIR;
	vsetflags(vnode, VROOT);
	root->vnode = vnode;
	vx_downgrade(vnode);
	vn_unlock(vnode);
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
	busy = root->vnode != NULL || !RB_EMPTY(&root->machines);
	lwkt_reltoken(&root->token);
	if (busy)
		return (EBUSY);
	lwkt_token_uninit(&root->token);
	kfree(root, M_VMMFS);
	return (0);
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
	machine = vmmfs_machine_create(root, name, namelen);
	if (machine == NULL)
		return (ENOMEM);
	lwkt_gettoken(&root->token);
	RB_FOREACH(cursor, vmmfs_machine_tree, &root->machines) {
		if (strcmp(cursor->name, machine->name) == 0)
			break;
	}
	if (cursor != NULL) {
		lwkt_reltoken(&root->token);
		machine->root = NULL;
		machine->vnode->v_type = VBAD;
		vx_put(machine->vnode);
		machine->vnode = NULL;
		vmmfs_machine_free(machine);
		return (EEXIST);
	}
	RB_INSERT(vmmfs_machine_tree, &root->machines, machine);
	lwkt_reltoken(&root->token);
	*machinep = machine;
	return (0);
}

static int
vmmfs_root_remove_item(struct vmmfs_root *root, const char *name,
	size_t namelen)
{
	struct vmmfs_machine key;
	struct vmmfs_machine *machine;
	int expected_stopped;
	int error;
	int runtime_active;

	if (namelen == 0 || namelen > NAME_MAX)
		return (ENAMETOOLONG);
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
	if (machine->root != root) {
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&root->token);
		return (ENOENT);
	}
	expected_stopped = machine->stopped.expect_stopped;
	runtime_active = machine->machine != NULL;
	if (!expected_stopped || runtime_active) {
		lwkt_reltoken(&machine->token);
		lwkt_reltoken(&root->token);
		vmmfs_events_log(&machine->events,
		    "destroy refused stopped=%d runtime=%d", expected_stopped,
		    runtime_active);
		return (EBUSY);
	}
	RB_REMOVE(vmmfs_machine_tree, &root->machines, machine);
	machine->root = NULL;
	lwkt_reltoken(&machine->token);
	error = vmmfs_machine_destroy(machine);
	if (error != 0) {
		lwkt_gettoken(&machine->token);
		machine->root = root;
		(void)RB_INSERT(vmmfs_machine_tree, &root->machines, machine);
		lwkt_reltoken(&machine->token);
		vmmfs_events_log(&machine->events, "destroy rejected error=%d",
		    error);
	}
	lwkt_reltoken(&root->token);
	return (error);
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
	vnode = machine->vnode;
	if (vnode == NULL)
		return (ENOENT);
	vhold(vnode);
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
	struct namecache *ncp;
	struct vnode *vnode;
	int error;

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
	vnode = machine->vnode;
	if (vnode == NULL) {
		error = vmmfs_root_remove_item(root, ncp->nc_name, ncp->nc_nlen);
		if (error != 0)
			return (error);
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
	error = vmmfs_root_remove_item(root, ncp->nc_name, ncp->nc_nlen);
	if (error == 0)
		cache_inval_vp(vnode, CINV_DESTROY | CINV_CHILDREN);
	vrele(vnode);
	return (error);
}

static int
vmmfs_root_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_root *root;

	root = ap->a_vp->v_data;
	if (root != NULL) {
		lwkt_gettoken(&root->token);
		if (root->vnode == ap->a_vp)
			root->vnode = NULL;
		lwkt_reltoken(&root->token);
	}
	ap->a_vp->v_data = NULL;
	return (0);
}
