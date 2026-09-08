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
#include <sys/tree.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include "vmmfs.h"
#include "vmmfs_root.h"
#include "vmmfs_parent.h"
#include "vmmfs_node.h"
#include "vmmfs_machine.h"
#include "vmmfs_serialport.h"
#include "vmmfs_serialroot.h"

#define VMMFS_SERIALROOT_MODE 0555

struct vmmfs_serialroot_port {
	RB_ENTRY(vmmfs_serialroot_port) entry;
	struct vmmfs_serialport *port;
};

RB_HEAD(vmmfs_serialport_tree, vmmfs_serialroot_port);

struct vmmfs_serialroot_registry {
	struct vmmfs_serialport_tree ports;
};

static int vmmfs_serialroot_get_item(struct vmmfs_node *, const char *,
	size_t, struct vnode **);
static int vmmfs_serialroot_remove_item(struct vmmfs_node *, const char *,
	size_t, struct ucred *);
static int vmmfs_serialroot_create_item(struct vmmfs_node *, const char *,
	size_t, struct vnode **);
static int vmmfs_serialroot_read_item(struct vmmfs_node *, uint64_t,
	struct vmmfs_node_item *);
static int vmmfs_serialroot_port_compare(struct vmmfs_serialroot_port *,
	struct vmmfs_serialroot_port *);
RB_PROTOTYPE(vmmfs_serialport_tree, vmmfs_serialroot_port, entry,
	vmmfs_serialroot_port_compare);
static struct vmmfs_serialroot_port *vmmfs_serialroot_find_locked(
	struct vmmfs_serialroot *, const char *);
static void vmmfs_serialroot_drop(struct vmmfs_node *);
static bool vmmfs_serialroot_deactivate(struct vmmfs_node *);

static int
vmmfs_serialroot_port_compare(struct vmmfs_serialroot_port *left,
	struct vmmfs_serialroot_port *right)
{
	return (strcmp(left->port->name, right->port->name));
}

RB_GENERATE(vmmfs_serialport_tree, vmmfs_serialroot_port, entry,
	vmmfs_serialroot_port_compare);

int
vmmfs_serialroot_init(struct vmmfs_node *parent,
	struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_root *root;
	struct vmmfs_serialroot_registry *registry;
	int error;

	if (parent == NULL || serialroot == NULL)
		return (EINVAL);
	root = (struct vmmfs_root *)parent->mount->root;
	bzero(serialroot, sizeof(*serialroot));
	registry = kmalloc(sizeof(*registry), M_VMMFS, M_WAITOK | M_ZERO);
	if (registry == NULL)
		return (ENOMEM);
	serialroot->registry = registry;
	serialroot->node.inode = vmmfs_root_allocate_inode(root);
	RB_INIT(&serialroot->registry->ports);
	serialroot->node.parent = parent;
	serialroot->node.mount = parent->mount;
	serialroot->node.references = 1;
	lwkt_token_init(&serialroot->token, "vmmfsnode");
	lockinit(&serialroot->node.lock, "vmmfsnode", 0, 0);
	serialroot->node.drop = vmmfs_serialroot_drop;
	serialroot->node.read_item = vmmfs_serialroot_read_item;
	serialroot->node.remove_item = vmmfs_serialroot_remove_item;
	serialroot->node.create_item = vmmfs_serialroot_create_item;
	vmmfs_node_hold(parent);
	serialroot->node.get_item = vmmfs_serialroot_get_item;
	serialroot->node.mode = VMMFS_SERIALROOT_MODE;
	serialroot->node.size = 0;
	error = vmmfs_vnode_create_regular(parent->mount->mount,
	    &parent->mount->node_vops, VDIR, &serialroot->node);
	if (error != 0)
		vmmfs_node_put(&serialroot->node);
	else
		serialroot->node.deactivate = vmmfs_serialroot_deactivate;
	return (error);
}

static void
vmmfs_serialroot_drop(struct vmmfs_node *node)
{
	struct vmmfs_serialroot *serialroot;

	serialroot = (struct vmmfs_serialroot *)node;
	KKASSERT(serialroot != NULL);
	KKASSERT(serialroot->node.references == 0);
	KKASSERT(serialroot->registry != NULL);
	KKASSERT(RB_EMPTY(&serialroot->registry->ports));
	kfree(serialroot->registry, M_VMMFS);
	serialroot->registry = NULL;
	lwkt_token_uninit(&serialroot->token);
}

static void
vmmfs_serialroot_release_entry(struct vmmfs_serialroot *root,
	struct vmmfs_serialroot_port *entry)
{
	struct vmmfs_machine *machine = vmmfs_serialroot_machine(root);
	struct vmmfs_serialport *port = entry->port;

	lwkt_gettoken(&machine->token);
	port->entry = NULL;
	lwkt_reltoken(&machine->token);
}

static bool
vmmfs_serialroot_deactivate(struct vmmfs_node *node)
{
	struct vmmfs_serialroot *root = (struct vmmfs_serialroot *)node;
	struct vmmfs_serialroot_port *entry;
	struct vnode *vnode;

	for (;;) {
		entry = RB_ROOT(&root->registry->ports);
		if (entry == NULL)
			return (true);
		vnode = entry->port->node.vnode;
		/* Detach before cleanup can sleep; retain the registry vnode ref. */
		RB_REMOVE(vmmfs_serialport_tree, &root->registry->ports, entry);
		vmmfs_serialroot_release_entry(root, entry);
		kfree(entry, M_VMMFS);
		/* An existing closer retains its own reference. */
		if (!vmmfs_node_deactivate(vnode->v_data))
			vrele(vnode);
	}
}

int
vmmfs_serialroot_start(struct vmmfs_serialroot *serialroot,
	vmm_machine_t machine)
{
	struct vmmfs_serialroot_port *entry;
	struct vmmfs_serialport *port;
	int error;

	if (serialroot == NULL || machine == NULL)
		return (EINVAL);
	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->registry->ports) {
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

	if (serialroot == NULL)
		return (EINVAL);
	result = 0;
	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->registry->ports) {
		port = entry->port;
		error = vmmfs_serialport_stop(port);
		if (result == 0 && error != 0)
			result = error;
	}
	return (result);
}

size_t
vmmfs_serialroot_port_count(struct vmmfs_serialroot *serialroot)
{
	struct vmmfs_serialroot_port *entry;
	size_t count;

	if (serialroot == NULL || serialroot->registry == NULL)
		return (0);
	lwkt_gettoken(&serialroot->token);
	count = 0;
	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->registry->ports)
		++count;
	lwkt_reltoken(&serialroot->token);
	return (count);
}

int
vmmfs_serialroot_port_info(struct vmmfs_serialroot *serialroot,
	size_t index, struct vmmfs_serialport_info *info)
{
	struct vmmfs_serialroot_port *entry;
	size_t current;

	if (serialroot == NULL || serialroot->registry == NULL || info == NULL)
		return (EINVAL);
	lwkt_gettoken(&serialroot->token);
	current = 0;
	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->registry->ports) {
		if (current++ != index)
			continue;
		info->number = entry->port->number;
		info->base = entry->port->base;
		info->gsi = entry->port->gsi;
		lwkt_reltoken(&serialroot->token);
		return (0);
	}
	lwkt_reltoken(&serialroot->token);
	return (ENOENT);
}

static int vmmfs_serialroot_remove_port(struct vmmfs_serialroot *,
	struct vnode *);

static int
vmmfs_serialroot_create_item(struct vmmfs_node *node,
	const char *input, size_t length, struct vnode **vnodep)
{
	struct vmmfs_serialroot *serialroot = (struct vmmfs_serialroot *)node;
	struct vmmfs_machine *machine = vmmfs_serialroot_machine(serialroot);
	struct vmmfs_serialroot_port *entry;
	struct vmmfs_serialport *port;
	struct vnode *vnode;
	char name[sizeof(((struct vmmfs_serialport *)0)->name)];
	int error;

	if (length == 0 || length >= sizeof(name))
		return (EINVAL);
	bcopy(input, name, length);
	name[length] = 0;
	lwkt_gettoken(&serialroot->token);
	lwkt_gettoken(&machine->token);
	if (machine->machine != NULL) {
		error = EBUSY;
	} else if (vmmfs_serialroot_find_locked(serialroot, name) != NULL) {
		error = EEXIST;
	} else {
		error = 0;
	}
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&serialroot->token);
	if (error != 0)
		goto failed;

	entry = kmalloc(sizeof(*entry), M_VMMFS, M_WAITOK | M_ZERO);
	error = vmmfs_serialport_create(&serialroot->node, name,
	    length, &port);
	if (error != 0)
		goto failed_entry;
	vnode = port->node.vnode;

	lwkt_gettoken(&serialroot->token);
	lwkt_gettoken(&machine->token);
	if (machine->machine != NULL) {
		error = EBUSY;
	} else if (vmmfs_serialroot_find_locked(serialroot, name) != NULL) {
		error = EEXIST;
	} else {
		entry->port = port;
		if (RB_INSERT(vmmfs_serialport_tree, &serialroot->registry->ports,
		    entry) != NULL)
			error = EEXIST;
		else {
			port->entry = entry;
			error = 0;
		}
	}
	lwkt_reltoken(&machine->token);
	lwkt_reltoken(&serialroot->token);
	if (error != 0)
		goto failed_vnode;

	vref(vnode); /* Caller reference, separate from the registry. */
	*vnodep = vnode;
	return (0);

failed_vnode:
	(void)vmmfs_node_deactivate(&port->node);
failed_entry:
	kfree(entry, M_VMMFS);
failed:
	vmmfs_events_log(&machine->events, VMMFS_MACHINE_EVENT_SERIAL_CREATE_FAILED,
	    "name=%.*s error=%d", (int)length, name, error);
	return (error);
}

static int
vmmfs_serialroot_remove_port(struct vmmfs_serialroot *serialroot,
	struct vnode *vnode)
{
	struct vmmfs_serialroot_port *entry;
	struct vmmfs_serialport *port;
	struct vnode *entry_vnode;

	port = vnode->v_data;
	if (port == NULL) {
		return (ENOENT);
	}
	/* The caller retains its reference until remove_port returns. */
	vref(vnode);
	if (!vmmfs_node_deactivate(&port->node)) {
		vrele(vnode);
		return (EBUSY);
	}
	lwkt_gettoken(&serialroot->token);
	entry = vmmfs_serialroot_find_locked(serialroot, port->name);
	if (entry == NULL || entry->port != port) {
		lwkt_reltoken(&serialroot->token);
		return (ENOENT);
	}
	RB_REMOVE(vmmfs_serialport_tree, &serialroot->registry->ports, entry);
	entry_vnode = entry->port->node.vnode;
	vmmfs_serialroot_release_entry(serialroot, entry);
	lwkt_reltoken(&serialroot->token);
	vrele(entry_vnode);
	kfree(entry, M_VMMFS);
	return (0);
}

static int
vmmfs_serialroot_get_item(struct vmmfs_node *node, const char *name,
	size_t length, struct vnode **vnodep)
{
	struct vmmfs_serialroot *root = (struct vmmfs_serialroot *)node;
	struct vmmfs_serialroot_port *entry;
	char buffer[sizeof(((struct vmmfs_serialport *)0)->name)];

	*vnodep = NULL;
	if (length == 0 || length >= sizeof(buffer))
		return (ENOENT);
	bcopy(name, buffer, length);
	buffer[length] = '\0';
	lwkt_gettoken(&root->token);
	entry = vmmfs_serialroot_find_locked(root, buffer);
	if (entry != NULL) {
		*vnodep = entry->port->node.vnode;
		vref(*vnodep);
	}
	lwkt_reltoken(&root->token);
	return (*vnodep == NULL ? ENOENT : 0);
}

static struct vmmfs_serialroot_port *
vmmfs_serialroot_find_locked(struct vmmfs_serialroot *serialroot,
	const char *name)
{
	struct vmmfs_serialroot_port *entry;

	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->registry->ports) {
		if (strcmp(entry->port->name, name) == 0)
			return (entry);
	}
	return (NULL);
}

static int
vmmfs_serialroot_read_item(struct vmmfs_node *node,
	uint64_t index, struct vmmfs_node_item *item)
{
	struct vmmfs_serialroot *serialroot = (struct vmmfs_serialroot *)node;
	struct vmmfs_serialroot_port *entry;
	uint64_t current;

	lwkt_gettoken(&serialroot->token);
	current = 0;
	RB_FOREACH(entry, vmmfs_serialport_tree, &serialroot->registry->ports) {
		if (current++ != index)
			continue;
		item->inode = entry->port->node.inode;
		item->type = DT_CHR;
		bcopy(entry->port->name, item->name, strlen(entry->port->name) + 1);
		lwkt_reltoken(&serialroot->token);
		return (0);
	}
	lwkt_reltoken(&serialroot->token);
	return (ENOENT);
}

static int
vmmfs_serialroot_remove_item(struct vmmfs_node *node, const char *name,
    size_t length, struct ucred *cred)
{
	struct vmmfs_serialroot *serialroot = (struct vmmfs_serialroot *)node;
	struct vnode *vnode;
	int error;

	(void)cred;
	error = vmmfs_serialroot_get_item(node, name, length, &vnode);
	if (error != 0)
		return (error);
	error = vmmfs_serialroot_remove_port(serialroot, vnode);
	if (error == 0)
		cache_inval_vp(node->vnode, CINV_CHILDREN);
	vrele(vnode);
	return (error);
}
