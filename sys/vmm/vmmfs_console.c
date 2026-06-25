/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem presentation of the console file: machines/<name>/console, wired
 * to the vmm_console core.  Reads expose retained guest output; writes feed
 * host input to the guest (currently counted only).  vmm.ko only.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmm_console.h"
#include "vmmfs.h"
#include "vmmfs_machine.h"
#include "vmmfs_node_if.h"

static int
vmmfs_console_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	char cbuf[256];
	size_t n;

	if (ap->a_uio->uio_resid <= 0)
		return 0;
	n = vmm_console_read(&node->vn_machine->machine.own_mut_console,
	    ap->a_uio->uio_offset, cbuf,
	    (size_t)ap->a_uio->uio_resid < sizeof(cbuf) ?
	    (size_t)ap->a_uio->uio_resid : sizeof(cbuf));
	if (n == 0)
		return 0;		/* EOF */
	return uiomove(cbuf, n, ap->a_uio);
}

static int
vmmfs_console_write(struct vmmfs_node *node, struct vop_write_args *ap)
{
	struct uio *uio = ap->a_uio;
	int error;

	while (uio->uio_resid > 0) {
		char dump[64];
		size_t d = (uio->uio_resid < (int)sizeof(dump)) ?
		    (size_t)uio->uio_resid : sizeof(dump);

		error = uiomove(dump, d, uio);
		if (error)
			return error;
		vmm_console_write(&node->vn_machine->machine.own_mut_console,
		    dump, d);
	}
	return 0;
}

static kobj_method_t vmmfs_console_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmmfs_node_read,	vmmfs_console_read),
	KOBJMETHOD(vmmfs_node_write,	vmmfs_console_write),
	KOBJMETHOD(vmmfs_node_open,	vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,	vmmnode_close),
	KOBJMETHOD(vmmfs_node_access,	vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_console, vmmfs_console_methods, 0);
