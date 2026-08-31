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
	volatile u_int next_inode;
	struct vmmfs_machine_tree machines;
};

static int vmmfs_root_read_item(struct vmmfs_branch *, uint64_t,
	struct vmmfs_branch_item *);
static struct vmmfs_root_machine *vmmfs_root_find_locked(
	struct vmmfs_root *, const char *, size_t);
static int vmmfs_root_get_item(struct vmmfs_branch *, const char *, size_t,
	struct vnode **);
static int vmmfs_root_create_item(struct vmmfs_branch *, struct mount *,
	const char *, size_t, struct vnode **);
static void vmmfs_root_remove_item(struct vmmfs_branch *, const char *,
	size_t);
static int vmmfs_root_deactivate(struct vmmfs_node *);
static void vmmfs_root_drop(struct vmmfs_node *);

static const struct vmmfs_branch_ops vmmfs_root_branch_ops = {
	.get_item = vmmfs_root_get_item,
	.read_item = vmmfs_root_read_item,
	.create_item = vmmfs_root_create_item,
	.remove_item = vmmfs_root_remove_item,
};

struct vop_ops vmmfs_root_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_node_open,
	.vop_nmkdir = vmmfs_branch_nmkdir,
	.vop_nresolve = vmmfs_branch_nresolve,
	.vop_nrmdir = vmmfs_branch_nrmdir,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_branch_readdir,
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
	root->branch.node.deactivate = vmmfs_root_deactivate;
	root->branch.ops = &vmmfs_root_branch_ops;
	root->next_inode = 1;
	root->branch.node.inode = vmmfs_root_allocate_inode(root);
	state->root_inode = root->branch.node.inode;
	root->branch.node.mode = VMMFS_ROOT_MODE;
	root->branch.node.size = 0;
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

static int
vmmfs_root_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_root *root;

	root = (struct vmmfs_root *)node;
	if (root == NULL)
		return (EINVAL);
	lwkt_gettoken(&root->branch.token);
	if (root->branch.node.dead) {
		lwkt_reltoken(&root->branch.token);
		return (0);
	}
	if (!RB_EMPTY(&root->machines)) {
		lwkt_reltoken(&root->branch.token);
		return (EBUSY);
	}
	(void)vmmfs_node_default_deactivate(&root->branch.node);
	lwkt_reltoken(&root->branch.token);
	return (0);
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
vmmfs_root_get_item(struct vmmfs_branch *branch, const char *name,
	size_t namelen, struct vnode **vnodep)
{
	struct vmmfs_root *root;
	struct vmmfs_root_machine *entry;

	if (branch == NULL || vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	root = (struct vmmfs_root *)branch;
	lwkt_gettoken(&root->branch.token);
	entry = vmmfs_root_find_locked(root, name, namelen);
	if (entry != NULL) {
		*vnodep = entry->vnode;
		vhold(*vnodep);
	}
	lwkt_reltoken(&root->branch.token);
	return (*vnodep != NULL ? 0 : ENOENT);
}

static int
vmmfs_root_read_item(struct vmmfs_branch *branch, uint64_t index,
	struct vmmfs_branch_item *item)
{
	struct vmmfs_root *root;
	struct vmmfs_root_machine *entry;
	struct vmmfs_machine *machine;
	uint64_t current;

	if (branch == NULL || item == NULL)
		return (EINVAL);
	root = (struct vmmfs_root *)branch;
	bzero(item, sizeof(*item));
	lwkt_gettoken(&root->branch.token);
	current = 0;
	RB_FOREACH(entry, vmmfs_machine_tree, &root->machines) {
		if (current++ != index)
			continue;
		item->vnode = entry->vnode;
		machine = item->vnode->v_data;
		KKASSERT(machine != NULL);
		item->inode = machine->branch.node.inode;
		bcopy(machine->name, item->name, sizeof(item->name));
		vhold(item->vnode);
		lwkt_reltoken(&root->branch.token);
		return (0);
	}
	lwkt_reltoken(&root->branch.token);
	return (ENOENT);
}

static int
vmmfs_root_create_item(struct vmmfs_branch *branch, struct mount *mount,
	const char *name, size_t namelen, struct vnode **vnodep)
{
	struct vmmfs_root *root;
	struct vmmfs_mount *state;
	struct vmmfs_root_machine *entry;
	struct vnode *vnode;
	int error;

	if (branch == NULL || mount == NULL || vnodep == NULL)
		return (EINVAL);
	if (namelen == 0 || namelen > NAME_MAX)
		return (ENAMETOOLONG);
	root = (struct vmmfs_root *)branch;
	state = (struct vmmfs_mount *)mount->mnt_data;
	if (state == NULL)
		return (ENXIO);
	*vnodep = NULL;
	entry = kmalloc(sizeof(*entry), M_VMMFS, M_WAITOK | M_ZERO);
	lwkt_gettoken(&root->branch.token);
	if (vmmfs_root_find_locked(root, name, namelen) != NULL) {
		lwkt_reltoken(&root->branch.token);
		kfree(entry, M_VMMFS);
		return (EEXIST);
	}
	/* The root token covers construction through the registry commit point. */
	error = vmmfs_machine_create(state, &root->branch, name, namelen, &vnode);
	if (error != 0) {
		lwkt_reltoken(&root->branch.token);
		kfree(entry, M_VMMFS);
		return (error);
	}
	entry->vnode = vnode;
	RB_INSERT(vmmfs_machine_tree, &root->machines, entry);
	lwkt_reltoken(&root->branch.token);
	*vnodep = vnode;
	return (0);
}

static void
vmmfs_root_remove_item(struct vmmfs_branch *branch, const char *name,
	size_t namelen)
{
	struct vmmfs_root *root;
	struct vmmfs_root_machine *entry;
	struct vnode *vnode;

	KKASSERT(branch != NULL);
	root = (struct vmmfs_root *)branch;
	lwkt_gettoken(&root->branch.token);
	entry = vmmfs_root_find_locked(root, name, namelen);
	KKASSERT(entry != NULL);
	RB_REMOVE(vmmfs_machine_tree, &root->machines, entry);
	vnode = entry->vnode;
	kfree(entry, M_VMMFS);
	lwkt_reltoken(&root->branch.token);
	vrele(vnode);
}
