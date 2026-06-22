/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The console config object: machines/<name>/console.  A stub for now -- reads
 * return EOF and writes are accepted and discarded.  It will become the guest
 * serial console.
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
#include "vmmfs.h"
#include "vmm_node_if.h"

static int
vmm_console_write(struct vmmfs_node *node, struct vop_write_args *ap)
{
	struct uio *uio = ap->a_uio;
	int error;

	(void)node;
	while (uio->uio_resid > 0) {
		char dump[64];
		size_t d = (uio->uio_resid < (int)sizeof(dump)) ?
		    (size_t)uio->uio_resid : sizeof(dump);

		error = uiomove(dump, d, uio);
		if (error)
			return error;
	}
	return 0;
}

static kobj_method_t vmm_console_methods[] = {
	KOBJMETHOD(vmm_node_getattr,	vmmfs_zero_getattr),
	KOBJMETHOD(vmm_node_read,	vmmfs_zero_read),
	KOBJMETHOD(vmm_node_write,	vmm_console_write),
	KOBJMETHOD(vmm_node_open,	vmmnode_open),
	KOBJMETHOD(vmm_node_close,	vmmnode_close),
	KOBJMETHOD(vmm_node_access,	vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmm_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_console, vmm_console_methods, 0);
