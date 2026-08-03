/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Filesystem presentation of one vPCIe function.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/file.h>
#include <sys/dirent.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namecache.h>
#include <sys/socketvar.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/kobj.h>

#include "vmm_machine.h"
#include "vmm_pcie.h"
#include "vmm_pcie_user.h"
#include "vmmfs.h"
#include "vmmfs_device.h"
#include "vmmfs_node_if.h"

extern struct fileops socketops;

struct vmmfs_device_leaf_desc {
	const char		*imm_name;
	uint16_t		imm_name_len;
	kobj_class_t		imm_class;
	enum vmmfs_device_leaf_kind imm_kind;
	mode_t			imm_mode;
};

static int	vmmfs_device_nresolve(struct vmmfs_node *,
		    struct vop_nresolve_args *);
static int	vmmfs_device_readdir(struct vmmfs_node *,
		    struct vop_readdir_args *);
static int	vmmfs_device_session_open(struct vmmfs_node *,
		    struct vop_open_args *);
static void	vmmfs_device_session_release(void *);
static int	vmmfs_device_info_getattr(struct vmmfs_node *,
		    struct vop_getattr_args *);
static int	vmmfs_device_info_read(struct vmmfs_node *,
		    struct vop_read_args *);
static size_t	vmmfs_device_info_format(struct vmmfs_device_leaf *, char *,
		    size_t);

static const struct vmmfs_device_leaf_desc
	vmmfs_device_leaf_descs[VMMFS_DEVICE_LEAF_COUNT] = {
	[VMMFS_DEVICE_LEAF_PROVIDER] = {
		.imm_name = "provider",
		.imm_name_len = sizeof("provider") - 1,
		.imm_class = &vmmfs_device_session_class,
		.imm_kind = VMMFS_DEVICE_LEAF_PROVIDER,
		.imm_mode = 0600,
	},
	[VMMFS_DEVICE_LEAF_CONSUMER] = {
		.imm_name = "consumer",
		.imm_name_len = sizeof("consumer") - 1,
		.imm_class = &vmmfs_device_session_class,
		.imm_kind = VMMFS_DEVICE_LEAF_CONSUMER,
		.imm_mode = 0600,
	},
	[VMMFS_DEVICE_LEAF_STATE] = {
		.imm_name = "state",
		.imm_name_len = sizeof("state") - 1,
		.imm_class = &vmmfs_device_info_class,
		.imm_kind = VMMFS_DEVICE_LEAF_STATE,
		.imm_mode = 0444,
	},
	[VMMFS_DEVICE_LEAF_BDF] = {
		.imm_name = "bdf",
		.imm_name_len = sizeof("bdf") - 1,
		.imm_class = &vmmfs_device_info_class,
		.imm_kind = VMMFS_DEVICE_LEAF_BDF,
		.imm_mode = 0444,
	},
};

struct vmmfs_device *
vmmfs_find_device(struct vmmfs_mount *vmp, struct vmmfs_machine *owner,
    const char *name, int nlen)
{
	struct vmm_pcie_root *root;
	struct vmm_device *device;

	root = owner != NULL ? &owner->machine.own_mut_pcie_root :
	    vmm_pcie_host_root(&vmp->own_mut_pcie);
	device = vmm_pcie_device_find(&vmp->own_mut_pcie, root, name, nlen);
	return device != NULL ? VMMFS_DEV_OF_CORE(device) : NULL;
}

void
vmmfs_device_init(struct vmmfs_device *d, struct vmmfs_node *parent,
    ino_t index)
{
	unsigned int i;

	vmmfs_node_init(&d->node, &vmmfs_device_class, VDIR, VMMFS_DIR_MODE,
	    VMMFS_DEV_INO_BASE + index * VMMFS_DEV_INO_STRIDE, parent, NULL);
	for (i = 0; i < VMMFS_DEVICE_LEAF_COUNT; i++) {
		struct vmmfs_device_leaf *leaf = &d->own_mut_leaves[i];
		const struct vmmfs_device_leaf_desc *desc =
		    &vmmfs_device_leaf_descs[i];

		leaf->borrow_imm_device = d;
		leaf->imm_kind = desc->imm_kind;
		vmmfs_node_init(&leaf->node, desc->imm_class, VREG,
		    desc->imm_mode, d->node.vn_ino + i + 1, &d->node, NULL);
	}
}

void
vmmfs_device_uninit(struct vmmfs_device *d)
{
	unsigned int i;

	if (d == NULL)
		return;
	for (i = 0; i < VMMFS_DEVICE_LEAF_COUNT; i++)
		vmmfs_node_uninit(&d->own_mut_leaves[i].node);
	vmmfs_node_uninit(&d->node);
}

/*
 * A device view owns vnode-private memory.  Reclaim every leaf before the
 * view is uninitialized so a cached vnode can never retain a stale v_data
 * pointer after rmdir, machine deletion, or unmount.
 */
void
vmmfs_device_revoke(struct vmmfs_device *d)
{
	unsigned int i;

	vmmfs_node_begin_revoke(&d->node);
	for (i = 0; i < VMMFS_DEVICE_LEAF_COUNT; i++)
		vmmfs_node_begin_revoke(&d->own_mut_leaves[i].node);
	for (i = 0; i < VMMFS_DEVICE_LEAF_COUNT; i++)
		vmmfs_node_revoke(&d->own_mut_leaves[i].node);
	vmmfs_node_revoke(&d->node);
}

int
vmmfs_device_destroy_owner_locked(struct vmmfs_mount *vmp,
    struct vmm_machine *owner)
{
	struct vmmfs_device *d;
	struct vmm_pcie_root *root;
	int error;

	root = &owner->own_mut_pcie_root;
	for (;;) {
		d = NULL;
		SLIST_FOREACH(d, &vmp->vm_device_views, dv_view_link) {
			if (vmm_pcie_device_at_root(&d->own_mut_device, root))
				break;
		}
		if (d == NULL)
			return 0;
		/* Machine deletion is provider removal, not an EBUSY condition. */
		vmm_pcie_device_force_close(&d->own_mut_device);
		error = vmm_pcie_device_destroy(&vmp->own_mut_pcie,
		    &d->own_mut_device);
		if (error != 0)
			return error;
		SLIST_REMOVE(&vmp->vm_device_views, d, vmmfs_device,
		    dv_view_link);
		vmmfs_device_revoke(d);
		vmmfs_device_uninit(d);
		kfree(d, M_VMMFS);
	}
}

void
vmmfs_device_destroy_all(struct vmmfs_mount *vmp)
{
	struct vmmfs_device *d;
	int error;

	while (!SLIST_EMPTY(&vmp->vm_device_views)) {
		d = SLIST_FIRST(&vmp->vm_device_views);
		SLIST_REMOVE_HEAD(&vmp->vm_device_views, dv_view_link);
		error = vmm_pcie_device_destroy(&vmp->own_mut_pcie,
		    &d->own_mut_device);
		KKASSERT(error == 0);
		vmmfs_device_revoke(d);
		vmmfs_device_uninit(d);
		kfree(d, M_VMMFS);
	}
}

static int
vmmfs_device_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct namecache *ncp;
	struct vmmfs_device *d;
	struct vmmfs_node *child;
	unsigned int i;

	d = VMMFS_DEV_OF_NODE(dnode);
	ncp = ap->a_nch->ncp;
	child = NULL;
	for (i = 0; i < VMMFS_DEVICE_LEAF_COUNT; i++) {
		const struct vmmfs_device_leaf_desc *desc =
		    &vmmfs_device_leaf_descs[i];

		if (ncp->nc_nlen == desc->imm_name_len &&
		    bcmp(ncp->nc_name, desc->imm_name, desc->imm_name_len) == 0) {
			child = &d->own_mut_leaves[i].node;
			break;
		}
	}
	return vmmfs_nresolve_finish(ap->a_dvp, child, ap->a_nch);
}

static int
vmmfs_device_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio;
	off_t off;
	int error;
	int full;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error != 0 || full)
		return vmmfs_readdir_end(ap, off, full, error);
	uio = ap->a_uio;
	while (off - 2 < VMMFS_DEVICE_LEAF_COUNT) {
		const struct vmmfs_device_leaf_desc *desc =
		    &vmmfs_device_leaf_descs[off - 2];
		struct vmmfs_device *d = VMMFS_DEV_OF_NODE(node);

		if (vop_write_dirent(&error, uio,
		    d->own_mut_leaves[off - 2].node.vn_ino, DT_REG,
		    desc->imm_name_len, desc->imm_name)) {
			full = 1;
			break;
		}
		off++;
	}
	return vmmfs_readdir_end(ap, off, full, error);
}

static int
vmmfs_device_session_open(struct vmmfs_node *node, struct vop_open_args *ap)
{
	struct vmmfs_device_leaf *leaf;
	struct socket *user_socket;
	struct file *fp;
	enum vmm_pcie_user_role role;
	int error;

	if (ap->a_fpp == NULL || (ap->a_mode & (FREAD | FWRITE)) !=
	    (FREAD | FWRITE))
		return EACCES;
	leaf = VMMFS_DEVICE_LEAF_OF_NODE(node);
	role = leaf->imm_kind == VMMFS_DEVICE_LEAF_PROVIDER ?
	    VMM_PCIE_USER_PROVIDER : VMM_PCIE_USER_CONSUMER;
	error = vmmfs_node_enter(node);
	if (error != 0)
		return error;
	error = vmm_pcie_user_open(&leaf->borrow_imm_device->own_mut_device,
	    role, ap->a_cred, node, vmmfs_device_session_release, &user_socket);
	if (error != 0) {
		vmmfs_node_leave(node);
		return error;
	}
	fp = *ap->a_fpp;
	fp->f_type = DTYPE_SOCKET;
	fp->f_flag = (fp->f_flag & ~FMASK) | FREAD | FWRITE;
	fp->f_ops = &socketops;
	fp->f_data = user_socket;
	return 0;
}

static void
vmmfs_device_session_release(void *arg)
{
	vmmfs_node_leave(arg);
}

static void
vmmfs_device_session_revoke(struct vmmfs_node *node)
{
	struct vmmfs_device_leaf *leaf = VMMFS_DEVICE_LEAF_OF_NODE(node);

	vmm_pcie_device_force_close(&leaf->borrow_imm_device->own_mut_device);
}

static int
vmmfs_device_info_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap)
{
	struct vmmfs_device_leaf *leaf;
	char buf[128];
	size_t size;

	leaf = VMMFS_DEVICE_LEAF_OF_NODE(node);
	size = vmmfs_device_info_format(leaf, buf, sizeof(buf));
	vmmfs_fill_attr(node, ap->a_vap, VREG, 1, (off_t)size);
	return 0;
}

static int
vmmfs_device_info_read(struct vmmfs_node *node, struct vop_read_args *ap)
{
	struct vmmfs_device_leaf *leaf;
	char buf[128];
	size_t size;
	off_t offset;

	if (ap->a_uio->uio_offset < 0)
		return EINVAL;
	leaf = VMMFS_DEVICE_LEAF_OF_NODE(node);
	size = vmmfs_device_info_format(leaf, buf, sizeof(buf));
	offset = ap->a_uio->uio_offset;
	if ((uint64_t)offset >= size)
		return 0;
	return uiomove(buf + offset, size - (size_t)offset, ap->a_uio);
}

static kobj_method_t vmmfs_device_methods[] = {
	KOBJMETHOD(vmmfs_node_nresolve,		vmmfs_device_nresolve),
	KOBJMETHOD(vmmfs_node_readdir,		vmmfs_device_readdir),
	KOBJMETHOD(vmmfs_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmmfs_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmmfs_node_access,		vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_open,		vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,		vmmnode_close),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_device, vmmfs_device_methods, 0);

static kobj_method_t vmmfs_device_session_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,		vmmfs_zero_getattr),
	KOBJMETHOD(vmmfs_node_read,		vmmfs_zero_read),
	KOBJMETHOD(vmmfs_node_open,		vmmfs_device_session_open),
	KOBJMETHOD(vmmfs_node_revoke,		vmmfs_device_session_revoke),
	KOBJMETHOD(vmmfs_node_access,		vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_device_session, vmmfs_device_session_methods, 0);

static kobj_method_t vmmfs_device_info_methods[] = {
	KOBJMETHOD(vmmfs_node_getattr,		vmmfs_device_info_getattr),
	KOBJMETHOD(vmmfs_node_read,		vmmfs_device_info_read),
	KOBJMETHOD(vmmfs_node_open,		vmmnode_open),
	KOBJMETHOD(vmmfs_node_close,		vmmnode_close),
	KOBJMETHOD(vmmfs_node_access,		vmmnode_access),
	KOBJMETHOD(vmmfs_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmmfs_node_inactive,	vmmnode_inactive),
	KOBJMETHOD(vmmfs_node_reclaim,	vmmnode_reclaim),
	KOBJMETHOD(vmmfs_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmmfs_device_info, vmmfs_device_info_methods, 0);

static size_t
vmmfs_device_info_format(struct vmmfs_device_leaf *leaf, char *buf,
    size_t cap)
{
	struct vmm_device *device;
	struct vmm_pcie *pcie;
	const char *provider;
	const char *consumer;
	int n;

	if (leaf == NULL || buf == NULL || cap == 0 ||
	    leaf->borrow_imm_device == NULL)
		return 0;
	device = &leaf->borrow_imm_device->own_mut_device;
	pcie = device->borrow_imm_pcie;
	if (pcie == NULL)
		return 0;
	lwkt_gettoken(&pcie->token_registry);
	if (leaf->imm_kind == VMMFS_DEVICE_LEAF_STATE) {
		if (device->borrow_mut_provider == NULL)
			provider = "detached";
		else if (device->mut_registered)
			provider = "registered";
		else
			provider = "pending";
		consumer = device->borrow_mut_offload != NULL ? "offloaded" :
		    "root";
		n = ksnprintf(buf, cap, "provider=%s\nconsumer=%s\n", provider,
		    consumer);
	} else {
		n = ksnprintf(buf, cap, "%#x\n", device->mut_bdf);
	}
	lwkt_reltoken(&pcie->token_registry);
	if (n < 0 || (size_t)n >= cap)
		return 0;
	return (size_t)n;
}
