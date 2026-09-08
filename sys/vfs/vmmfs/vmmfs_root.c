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
	struct vmmfs_machine *machine;
};

RB_HEAD(vmmfs_machine_tree, vmmfs_root_machine);
RB_PROTOTYPE(vmmfs_machine_tree, vmmfs_root_machine, entry,
	vmmfs_root_machine_compare);

struct vmmfs_root {
	struct vmmfs_node node;
	struct lwkt_token token;
	u_int next_inode;
	struct vmmfs_machine_tree machines;
};

static int vmmfs_root_read_item(struct vmmfs_node *, uint64_t,
	struct vmmfs_node_item *);
static struct vmmfs_root_machine *vmmfs_root_find_locked(
	struct vmmfs_root *, const char *, size_t);
static int vmmfs_root_get_item(struct vmmfs_node *, const char *, size_t,
	struct vnode **);
static int vmmfs_root_create_object(struct vmmfs_node *,
	const char *, size_t, struct vnode **);
static int vmmfs_root_remove_object(struct vmmfs_node *, const char *,
	size_t);
static bool vmmfs_root_deactivate(struct vmmfs_node *);
static void vmmfs_root_drop(struct vmmfs_node *);

static int
vmmfs_root_machine_compare(struct vmmfs_root_machine *left,
	struct vmmfs_root_machine *right)
{
	struct vmmfs_machine *left_machine;
	struct vmmfs_machine *right_machine;

	left_machine = left->machine;
	right_machine = right->machine;
	return (strcmp(left_machine->name, right_machine->name));
}

RB_GENERATE(vmmfs_machine_tree, vmmfs_root_machine, entry,
	vmmfs_root_machine_compare);

int
vmmfs_root_create(struct mount *mount, struct vmmfs_node **objectp)
{
	struct vmmfs_mount *state;
	struct vmmfs_root *root;
	struct vnode *vnode;
	int error;

	if (mount == NULL || objectp == NULL)
		return (EINVAL);
	*objectp = NULL;
	state = (struct vmmfs_mount *)mount->mnt_data;
	root = kmalloc(sizeof(*root), M_VMMFS, M_WAITOK | M_ZERO);
	atomic_add_int(&vmmfs_root_count, 1);
	root->node.parent = NULL;
	root->node.mount = state;
	root->node.references = 1;
	lwkt_token_init(&root->token, "vmmfsnode");
	lockinit(&root->node.lock, "vmmfsnode", 0, 0);
	root->node.drop = vmmfs_root_drop;
	root->node.get_item = vmmfs_root_get_item;
	root->node.read_item = vmmfs_root_read_item;
	root->node.create_object = vmmfs_root_create_object;
	root->node.remove_object = vmmfs_root_remove_object;
	root->next_inode = 1;
	root->node.inode = vmmfs_root_allocate_inode(root);
	state->root_inode = root->node.inode;
	root->node.mode = VMMFS_ROOT_MODE;
	root->node.size = 0;
	RB_INIT(&root->machines);
	error = vmmfs_vnode_create_regular(mount, &state->node_vops, VDIR,
		&root->node);
	vnode = root->node.vnode;
	if (error != 0) {
		vmmfs_node_put(&root->node);
		return (error);
	}
	root->node.deactivate = vmmfs_root_deactivate;
	error = vget(vnode, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0) {
		(void)vmmfs_node_deactivate(&root->node);
		return (error);
	}
	vsetflags(vnode, VROOT);
	vn_unlock(vnode);
	vrele(vnode);
	*objectp = &root->node;
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

static bool
vmmfs_root_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_root *root = (struct vmmfs_root *)node;

	return (RB_EMPTY(&root->machines));
}

static void
vmmfs_root_drop(struct vmmfs_node *node)
{
	struct vmmfs_root *root;

	root = (struct vmmfs_root *)node;
	KKASSERT(root->node.references == 0);
	KKASSERT(RB_EMPTY(&root->machines));
	lwkt_token_uninit(&root->token);
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
		machine = entry->machine;
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
	lwkt_gettoken(&root->token);
	entry = vmmfs_root_find_locked(root, name, namelen);
	if (entry != NULL) {
		*vnodep = entry->machine->node.vnode;
		vref(*vnodep);
	}
	lwkt_reltoken(&root->token);
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
	lwkt_gettoken(&root->token);
	current = 0;
	RB_FOREACH(entry, vmmfs_machine_tree, &root->machines) {
		if (current++ != index)
			continue;
		machine = entry->machine;
		item->inode = machine->node.inode;
		bcopy(machine->name, item->name, sizeof(item->name));
		item->type = DT_DIR;
		lwkt_reltoken(&root->token);
		return (0);
	}
	lwkt_reltoken(&root->token);
	return (ENOENT);
}

static int
vmmfs_root_create_object(struct vmmfs_node *node,
	const char *name, size_t namelen, struct vnode **vnodep)
{
	struct vmmfs_root *root = (struct vmmfs_root *)node;
	struct vmmfs_root_machine *entry;
	struct vnode *vnode;
	struct vmmfs_machine *machine;
	int error;

	if (namelen == 0 || namelen > NAME_MAX)
		return (ENAMETOOLONG);
	*vnodep = NULL;
	entry = kmalloc(sizeof(*entry), M_VMMFS, M_WAITOK | M_ZERO);
	error = vmmfs_machine_create(node, name, namelen, &machine);
	if (error != 0) {
		kfree(entry, M_VMMFS);
		return (error);
	}
	vnode = machine->node.vnode;
	lwkt_gettoken(&root->token);
	if (vmmfs_root_find_locked(root, name, namelen) != NULL)
		error = EEXIST;
	else {
		entry->machine = machine;
		RB_INSERT(vmmfs_machine_tree, &root->machines, entry);
		vref(vnode); /* create_object caller, independent of registry. */
	}
	lwkt_reltoken(&root->token);
	if (error != 0) {
		(void)vmmfs_node_deactivate(&machine->node);
		kfree(entry, M_VMMFS);
		return (error);
	}
	*vnodep = vnode;
	return (0);
}

static int
vmmfs_root_remove_object(struct vmmfs_node *node, const char *name,
	size_t namelen)
{
	struct vmmfs_root *root;
	struct vmmfs_root_machine *entry;
	struct vnode *vnode;

	KKASSERT(node != NULL);
	root = (struct vmmfs_root *)node;
	lwkt_gettoken(&root->token);
	entry = vmmfs_root_find_locked(root, name, namelen);
	KKASSERT(entry != NULL);
	RB_REMOVE(vmmfs_machine_tree, &root->machines, entry);
	vnode = entry->machine->node.vnode;
	kfree(entry, M_VMMFS);
	lwkt_reltoken(&root->token);
	vrele(vnode);
	return (0);
}
