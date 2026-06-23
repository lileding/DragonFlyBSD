/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem presentation of the loader register file: a KOBJ class wiring the
 * shared register vops to the vmm_loader core object.  vmm.ko only.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/nlookup.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmm_loader.h"
#include "vmmfs.h"
#include "vmmfs_loader.h"
#include "vmmfs_machine.h"
#include "vmmfs_node_if.h"

static size_t
loader_text(const struct vmm_machine *m, char *out, size_t cap)
{
	return vmm_loader_format(&m->loader, out, cap);
}

static int
loader_commit(struct vmm_machine *m, const char *buf, size_t len)
{
	return vmm_loader_parse(&m->loader, buf, len);
}

static int
vmmfs_loader_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	return vmmfs_register_getattr(node, ap, loader_text);
}

static int
vmmfs_loader_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	return vmmfs_register_read(node, ap, loader_text);
}

static int
vmmfs_loader_close(struct vmmfs_node *node, struct vop_close_args *ap)
{
	return vmmfs_register_close(node, ap, loader_commit);
}

static kobj_method_t vmmfs_loader_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,	vmmfs_loader_getattr),
	KOBJMETHOD(vmmfs_node_read,	vmmfs_loader_read),
	KOBJMETHOD(vmmfs_node_write,	vmmfs_register_write),
	KOBJMETHOD(vmmfs_node_open,	vmmfs_register_open),
	KOBJMETHOD(vmmfs_node_close,	vmmfs_loader_close),
	KOBJMETHOD(vmmfs_node_access,	vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,	vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,	vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_loader, vmmfs_loader_methods, 0);


/*
 * Resolve the desired loader at start time in the caller's context and require
 * a regular, executable file.  Execution is still future vmm core work.
 */
int
vmmfs_loader_validate(struct vmmfs_machine *m, struct ucred *cred)
{
	struct nlookupdata nd;
	struct vnode *vp = NULL;
	struct vattr va;
	char path[VMMFS_OBUF_MAX];
	size_t n;
	int error;

	n = vmm_loader_path(&m->machine.loader, path, sizeof(path) - 1);
	if (n == 0)
		return EINVAL;
	path[n] = '\0';

	error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW | NLC_LOCKVP);
	if (error == 0)
		error = vn_open(&nd, NULL, FREAD, 0);
	if (error == 0) {
		vp = nd.nl_open_vp;
		nd.nl_open_vp = NULL;
	}
	nlookup_done(&nd);
	if (error)
		return error;

	vn_unlock(vp);
	if (vp->v_type != VREG) {
		vn_close(vp, FREAD, NULL);
		return EACCES;
	}
	vn_lock(vp, LK_SHARED | LK_RETRY);
	error = VOP_GETATTR(vp, &va);
	if (error == 0 && (va.va_mode & 0111) == 0)
		error = EACCES;
	if (error == 0)
		error = VOP_ACCESS(vp, VEXEC, cred);
	vn_unlock(vp);
	vn_close(vp, FREAD, NULL);
	return error;
}
