/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs mount domain.
 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include "vmmfs.h"

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs objects");

static struct vnode *vmmfs_domain_vnode(struct vmmfs_domain *);
static int vmmfs_domain_read_item(struct vmmfs_domain *, uint64_t,
	struct vmmfs_item *);
static int vmmfs_domain_create_item(struct vmmfs_domain *, const char *,
	size_t, struct vmmfs_machine **);
static int vmmfs_domain_remove_item(struct vmmfs_domain *,
	struct vmmfs_machine *);

int
vmmfs_domain_create(struct mount *mount, struct vmmfs_domain **domainp)
{
	struct vmmfs_domain *domain;

	domain = kmalloc(sizeof(*domain), M_VMMFS, M_WAITOK | M_ZERO);
	domain->mount = mount;
	domain->as_vnode = vmmfs_domain_vnode;
	domain->read_item = vmmfs_domain_read_item;
	domain->create_item = vmmfs_domain_create_item;
	domain->remove_item = vmmfs_domain_remove_item;
	lwkt_token_init(&domain->token, "vmmfsdomain");
	RB_INIT(&domain->machines);
	domain->next_ino = 2;
	*domainp = domain;
	return (0);
}
void
vmmfs_domain_destroy(struct vmmfs_domain *domain)
{
	KKASSERT(domain->vnode == NULL);
	KKASSERT(RB_EMPTY(&domain->machines));
	KKASSERT(domain->machine_vops == NULL);
	lwkt_token_uninit(&domain->token);
	kfree(domain, M_VMMFS);
}

/*
 * Materialize the domain as its canonical root vnode.  The vnode is reused
 * while live and recreated after reclaim.  A NULL result means that VFS
 * could not obtain a vnode.
 */
static struct vnode *
vmmfs_domain_vnode(struct vmmfs_domain *domain)
{
	struct vnode *vnode;
	int error;

retry:
	lwkt_gettoken(&domain->token);
	vnode = domain->vnode;
	if (vnode != NULL) {
		vhold(vnode);
		lwkt_reltoken(&domain->token);
		error = vget(vnode, LK_EXCLUSIVE | LK_RETRY);
		vdrop(vnode);
		if (error == 0)
			return (vnode);
		if (error != ENOENT)
			return (NULL);
		goto retry;
	}
	lwkt_reltoken(&domain->token);

	error = getnewvnode(VT_SYNTH, domain->mount, &vnode, 0, 0);
	if (error != 0)
		return (NULL);

	lwkt_gettoken(&domain->token);
	if (domain->vnode != NULL) {
		vnode->v_type = VBAD;
		vx_put(vnode);
		lwkt_reltoken(&domain->token);
		goto retry;
	}
	vnode->v_data = domain;
	vnode->v_type = VDIR;
	vsetflags(vnode, VROOT);
	domain->vnode = vnode;
	lwkt_reltoken(&domain->token);
	vx_downgrade(vnode);
	return (vnode);
}

static int
vmmfs_domain_read_item(struct vmmfs_domain *domain, uint64_t index,
	struct vmmfs_item *item)
{
	struct vmmfs_machine *machine;
	uint64_t current;

	lwkt_gettoken(&domain->token);
	current = 0;
	RB_FOREACH(machine, vmmfs_machine_tree, &domain->machines) {
		if (current++ != index)
			continue;
		item->id = machine->inode;
		bcopy(machine->name, item->name, sizeof(item->name));
		item->machine = machine;
		lwkt_reltoken(&domain->token);
		return (0);
	}
	lwkt_reltoken(&domain->token);
	return (ENOENT);
}

static int
vmmfs_domain_create_item(struct vmmfs_domain *domain, const char *name,
	size_t namelen, struct vmmfs_machine **machinep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_machine *cursor;

	if (namelen == 0 || namelen > NAME_MAX)
		return (ENAMETOOLONG);
	machine = vmmfs_machine_create(domain, name, namelen);
	if (machine == NULL)
		return (ENOMEM);
	lwkt_gettoken(&domain->token);
	RB_FOREACH(cursor, vmmfs_machine_tree, &domain->machines) {
		if (strcmp(cursor->name, machine->name) == 0)
			break;
	}
	if (cursor != NULL) {
		lwkt_reltoken(&domain->token);
		vmmfs_machine_destroy(machine);
		return (EEXIST);
	}
	machine->inode = domain->next_ino++;
	RB_INSERT(vmmfs_machine_tree, &domain->machines, machine);
	lwkt_reltoken(&domain->token);
	*machinep = machine;
	return (0);
}

static int
vmmfs_domain_remove_item(struct vmmfs_domain *domain,
	struct vmmfs_machine *machine)
{
	lwkt_gettoken(&domain->token);
	lwkt_gettoken(&machine->spec_token);
	if (machine->domain != domain) {
		lwkt_reltoken(&machine->spec_token);
		lwkt_reltoken(&domain->token);
		return (ENOENT);
	}
	RB_REMOVE(vmmfs_machine_tree, &domain->machines, machine);
	machine->domain = NULL;
	lwkt_reltoken(&machine->spec_token);
	lwkt_reltoken(&domain->token);
	return (0);
}
