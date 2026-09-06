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
#include <machine/atomic.h>

#include "vmmfs.h"
#include "vmmfs_node.h"
#include "vmmfs_events.h"
#include "vmmfs_machine.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs objects");

#define VMMFS_ROOT_MODE	0555

static u_int vmmfs_root_count;

struct vmmfs_root_machine {
	RB_ENTRY(vmmfs_root_machine) entry;
	struct vnode *vnode;
};

RB_HEAD(vmmfs_machine_tree, vmmfs_root_machine);
RB_PROTOTYPE(vmmfs_machine_tree, vmmfs_root_machine, entry,
	vmmfs_root_machine_compare);

struct vmmfs_root {
	struct vmmfs_node node;
	u_int next_inode;
	struct vmmfs_machine_tree machines;
};

static int vmmfs_root_read_item(struct vmmfs_node *, uint64_t,
	struct vmmfs_node_item *);
static struct vmmfs_root_machine *vmmfs_root_find_locked(
	struct vmmfs_root *, const char *, size_t);
static int vmmfs_root_get_item(struct vmmfs_node *, const char *, size_t,
	struct vnode **);
static int vmmfs_root_create_item(struct vmmfs_node *, struct mount *,
	const char *, size_t, struct vnode **);
static void vmmfs_root_remove_item(struct vmmfs_node *, const char *,
	size_t);
static int vmmfs_root_deactivate(struct vmmfs_node *);
static void vmmfs_root_drop(struct vmmfs_node *);


struct vop_ops vmmfs_root_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_node_open,
	.vop_nmkdir = vmmfs_node_nmkdir,
	.vop_nresolve = vmmfs_node_nresolve,
	.vop_nrmdir = vmmfs_node_nrmdir,
	.vop_pathconf = vop_stdpathconf,
	.vop_readdir = vmmfs_node_readdir,
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
	root = kmalloc(sizeof(*root), M_VMMFS, M_WAITOK | M_ZERO);
	atomic_add_int(&vmmfs_root_count, 1);
	root->node.parent = NULL;
	root->node.mount = state;
	root->node.references = 1;
	lwkt_token_init(&root->node.token, "vmmfsnode");
	root->node.drop = vmmfs_root_drop;
	root->node.deactivate = vmmfs_root_deactivate;
	root->node.get_item = vmmfs_root_get_item;
	root->node.read_item = vmmfs_root_read_item;
	root->node.create_item = vmmfs_root_create_item;
	root->node.remove_item = vmmfs_root_remove_item;
	root->next_inode = 1;
	root->node.inode = vmmfs_root_allocate_inode(root);
	state->root_inode = root->node.inode;
	root->node.mode = VMMFS_ROOT_MODE;
	root->node.size = 0;
	RB_INIT(&root->machines);
	error = vmmfs_vnode_create_regular(mount, &state->root_vops, VDIR,
		&root->node, &vnode);
	if (error != 0) {
		vmmfs_node_put(&root->node);
		return (error);
	}
	error = vget(vnode, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0) {
		vmmfs_vnode_discard(vnode);
		vmmfs_node_put(&root->node);
		return (error);
	}
	vsetflags(vnode, VROOT);
	vn_unlock(vnode);
	vrele(vnode);
	*vnodep = vnode;
	return (0);
}

int
vmmfs_root_module_fini(void)
{
	/* Detached descendants retain their root after the mount is gone. */
	return (atomic_load_acq_int(&vmmfs_root_count) == 0 ? 0 : EBUSY);
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
	struct vmmfs_root *root = (struct vmmfs_root *)node;
	int error;

	error = RB_EMPTY(&root->machines) ? 0 : EBUSY;
	return (error);
}


static void
vmmfs_root_drop(struct vmmfs_node *node)
{
	struct vmmfs_root *root;

	root = (struct vmmfs_root *)node;
	KKASSERT(root->node.references == 0);
	KKASSERT(RB_EMPTY(&root->machines));
	kfree(root, M_VMMFS);
	atomic_add_int(&vmmfs_root_count, -1);
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
vmmfs_root_get_item(struct vmmfs_node *node, const char *name,
	size_t namelen, struct vnode **vnodep)
{
	struct vmmfs_root *root;
	struct vmmfs_root_machine *entry;

	if (node == NULL || vnodep == NULL)
		return (EINVAL);
	*vnodep = NULL;
	root = (struct vmmfs_root *)node;
	lwkt_gettoken(&root->node.token);
	if (node->dead) {
		lwkt_reltoken(&root->node.token);
		return (ENOENT);
	}
	entry = vmmfs_root_find_locked(root, name, namelen);
	if (entry != NULL) {
		*vnodep = entry->vnode;
		vhold(*vnodep);
	}
	lwkt_reltoken(&root->node.token);
	return (*vnodep != NULL ? 0 : ENOENT);
}

static int
vmmfs_root_read_item(struct vmmfs_node *node, uint64_t index,
	struct vmmfs_node_item *item)
{
	struct vmmfs_root *root;
	struct vmmfs_root_machine *entry;
	struct vmmfs_machine *machine;
	uint64_t current;

	if (node == NULL || item == NULL)
		return (EINVAL);
	root = (struct vmmfs_root *)node;
	bzero(item, sizeof(*item));
	lwkt_gettoken(&root->node.token);
	if (node->dead) {
		lwkt_reltoken(&root->node.token);
		return (ENOENT);
	}
	current = 0;
	RB_FOREACH(entry, vmmfs_machine_tree, &root->machines) {
		if (current++ != index)
			continue;
		item->vnode = entry->vnode;
		machine = item->vnode->v_data;
		KKASSERT(machine != NULL);
		item->inode = machine->node.inode;
		bcopy(machine->name, item->name, sizeof(item->name));
		vhold(item->vnode);
		lwkt_reltoken(&root->node.token);
		return (0);
	}
	lwkt_reltoken(&root->node.token);
	return (ENOENT);
}

static int
vmmfs_root_create_item(struct vmmfs_node *node, struct mount *mount,
	const char *name, size_t namelen, struct vnode **vnodep)
{
	struct vmmfs_root *root = (struct vmmfs_root *)node;
	struct vmmfs_root_machine *entry;
	struct vnode *vnode;
	int error, cleanup_error;

	if (namelen == 0 || namelen > NAME_MAX)
		return (ENAMETOOLONG);
	*vnodep = NULL;
	entry = kmalloc(sizeof(*entry), M_VMMFS, M_WAITOK | M_ZERO);
	error = vmmfs_machine_create(node, name, namelen, &vnode);
	if (error != 0) {
		kfree(entry, M_VMMFS);
		return (error);
	}
	lwkt_gettoken(&node->token);
	if (node->dead)
		error = ENOENT;
	else if (vmmfs_root_find_locked(root, name, namelen) != NULL)
		error = EEXIST;
	else {
		entry->vnode = vnode;
		RB_INSERT(vmmfs_machine_tree, &root->machines, entry);
		vref(vnode); /* create_item caller, independent of registry. */
	}
	lwkt_reltoken(&node->token);
	if (error != 0) {
		cleanup_error = vmmfs_vnode_deactivate(vnode);
		if (cleanup_error != 0)
			kprintf("vmmfs: rejected machine cleanup: %d\n", cleanup_error);
		vrele(vnode);
		kfree(entry, M_VMMFS);
		return (error);
	}
	*vnodep = vnode;
	return (0);
}

static void
vmmfs_root_remove_item(struct vmmfs_node *node, const char *name,
	size_t namelen)
{
	struct vmmfs_root *root;
	struct vmmfs_root_machine *entry;
	struct vnode *vnode;

	KKASSERT(node != NULL);
	root = (struct vmmfs_root *)node;
	lwkt_gettoken(&root->node.token);
	entry = vmmfs_root_find_locked(root, name, namelen);
	KKASSERT(entry != NULL);
	RB_REMOVE(vmmfs_machine_tree, &root->machines, entry);
	vnode = entry->vnode;
	kfree(entry, M_VMMFS);
	lwkt_reltoken(&root->node.token);
	vrele(vnode);
}
