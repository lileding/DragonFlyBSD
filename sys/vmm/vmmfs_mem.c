/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem presentation of the mem register file: a KOBJ class wiring the
 * shared register vops to the vmm_mem core object.  vmm.ko only.
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
#include "vmm_mem.h"
#include "vmmfs.h"
#include "vmm_node_if.h"

static size_t
mem_text(const struct vmm_machine *m, char *out, size_t cap)
{
	return vmm_mem_format(&m->mem, out, cap);
}

static int
mem_commit(struct vmm_machine *m, const char *buf, size_t len)
{
	return vmm_mem_parse(&m->mem, buf, len);
}

static int
vmmfs_mem_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	return vmmfs_register_getattr(node, ap, mem_text);
}

static int
vmmfs_mem_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	return vmmfs_register_read(node, ap, mem_text);
}

static int
vmmfs_mem_close(struct vmmfs_node *node, struct vop_close_args *ap)
{
	return vmmfs_register_close(node, ap, mem_commit);
}

static kobj_method_t vmm_mem_methods[] = {
	KOBJMETHOD(vmm_node_getattr,	vmmfs_mem_getattr),
	KOBJMETHOD(vmm_node_read,	vmmfs_mem_read),
	KOBJMETHOD(vmm_node_write,	vmmfs_register_write),
	KOBJMETHOD(vmm_node_open,	vmmfs_register_open),
	KOBJMETHOD(vmm_node_close,	vmmfs_mem_close),
	KOBJMETHOD(vmm_node_access,	vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmm_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_mem, vmm_mem_methods, 0);
