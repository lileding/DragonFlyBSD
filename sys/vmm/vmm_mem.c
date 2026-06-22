/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The mem config object: the machines/<name>/mem register file.  It wires the
 * shared register vops to the mem core (parse/serialize in vmm_machine.c).
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
vmm_mem_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	return vmmfs_register_getattr(node, ap, vmm_machine_mem_text);
}

static int
vmm_mem_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	return vmmfs_register_read(node, ap, vmm_machine_mem_text);
}

static int
vmm_mem_close(struct vmmfs_node *node, struct vop_close_args *ap)
{
	return vmmfs_register_close(node, ap, vmm_machine_commit_mem);
}

static kobj_method_t vmm_mem_methods[] = {
	KOBJMETHOD(vmm_node_getattr,	vmm_mem_getattr),
	KOBJMETHOD(vmm_node_read,	vmm_mem_read),
	KOBJMETHOD(vmm_node_write,	vmmfs_register_write),
	KOBJMETHOD(vmm_node_open,	vmmfs_register_open),
	KOBJMETHOD(vmm_node_close,	vmm_mem_close),
	KOBJMETHOD(vmm_node_access,	vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmm_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_mem, vmm_mem_methods, 0);
