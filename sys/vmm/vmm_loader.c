/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The loader config object: the machines/<name>/loader register file.  Its
 * value is the path to an executable (often a sh script) run at start.  It
 * wires the shared register vops to the loader core (vmm_machine.c).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmmfs.h"
#include "vmm_node_if.h"

static int
vmm_loader_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	return vmmfs_register_getattr(node, ap, vmm_machine_loader_text);
}

static int
vmm_loader_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	return vmmfs_register_read(node, ap, vmm_machine_loader_text);
}

static int
vmm_loader_close(struct vmmfs_node *node, struct vop_close_args *ap)
{
	return vmmfs_register_close(node, ap, vmm_machine_commit_loader);
}

static kobj_method_t vmm_loader_methods[] = {
	KOBJMETHOD(vmm_node_getattr,	vmm_loader_getattr),
	KOBJMETHOD(vmm_node_read,	vmm_loader_read),
	KOBJMETHOD(vmm_node_write,	vmmfs_register_write),
	KOBJMETHOD(vmm_node_open,	vmmfs_register_open),
	KOBJMETHOD(vmm_node_close,	vmm_loader_close),
	KOBJMETHOD(vmm_node_access,	vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmm_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_loader, vmm_loader_methods, 0);
