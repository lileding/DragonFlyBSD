/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs direct boot session node.
 */
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include "vmmfs_node.h"

#include "vmmfs.h"
#include "vmmfs_boot.h"
#include "vmmfs_machine.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"

#define VMMFS_BOOT_MODE 0600

static uint32_t vmmfs_boot_dev_serial;
static int vmmfs_boot_pager_count;

struct vmmfs_boot_session {
	struct vmmfs_boot *boot;
	struct vm_object *pager_object;
	struct vm_object *backing_object;
	vm_size_t size;
	int references;
	bool revoked;
};

static int vmmfs_boot_open(struct vop_open_args *);
static int vmmfs_boot_close(struct vop_close_args *);
static int vmmfs_boot_read(struct vop_read_args *);
static int vmmfs_boot_write(struct vop_write_args *);
static void vmmfs_boot_drop(struct vmmfs_node *);
static int vmmfs_boot_dev_open(struct dev_open_args *);
static int vmmfs_boot_dev_close(struct dev_close_args *);
static int vmmfs_boot_dev_write(struct dev_write_args *);
static int vmmfs_boot_dev_mmap_single(struct dev_mmap_single_args *);
static int vmmfs_boot_pager_ctor(void *, vm_ooffset_t, vm_prot_t,
	vm_ooffset_t, struct ucred *, u_short *);
static void vmmfs_boot_pager_dtor(void *);
static int vmmfs_boot_pager_fault(vm_object_t, vm_ooffset_t, int,
	vm_page_t *);
static void vmmfs_boot_session_revoke(struct vmmfs_boot_session *);
static void vmmfs_boot_session_drop_base(struct vmmfs_boot_session *);
static void vmmfs_boot_session_drop_pager(struct vmmfs_boot_session *);
static struct vmmfs_boot_session *vmmfs_boot_take_session(
	struct vmmfs_boot *, struct vnode **);
static bool vmmfs_boot_cancel_unsubmitted(struct vmmfs_boot *);

int vmmfs_boot_module_fini(void);

struct vop_ops vmmfs_boot_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vmmfs_boot_close,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_boot_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_boot_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_write = vmmfs_boot_write,
};

static struct dev_ops vmmfs_boot_dev_ops = {
	{ "vmmfs_boot", 0, D_MPSAFE },
	.d_open = vmmfs_boot_dev_open,
	.d_close = vmmfs_boot_dev_close,
	.d_write = vmmfs_boot_dev_write,
	.d_mmap_single = vmmfs_boot_dev_mmap_single,
};

static struct cdev_pager_ops vmmfs_boot_pager_ops = {
	.cdev_pg_fault = vmmfs_boot_pager_fault,
	.cdev_pg_ctor = vmmfs_boot_pager_ctor,
	.cdev_pg_dtor = vmmfs_boot_pager_dtor,
};

int
vmmfs_boot_module_fini(void)
{
	if (atomic_fetchadd_int(&vmmfs_boot_pager_count, 0) != 0)
		return (EBUSY);
	return (0);
}

int
vmmfs_boot_init(struct vmmfs_mount *mount, struct vmmfs_branch *parent,
	struct vmmfs_boot *boot,
	struct vnode **vnodep)
{
	struct vmmfs_machine *machine;
	struct vmmfs_root *root;
	uint32_t serial;
	int error;

	if (mount == NULL || parent == NULL || boot == NULL || vnodep == NULL)
		return (EINVAL);
	machine = (struct vmmfs_machine *)parent;
	root = mount->root_vnode == NULL ? NULL : mount->root_vnode->v_data;
	if (root == NULL)
		return (ENXIO);
	*vnodep = NULL;
	bzero(boot, sizeof(*boot));
	if (mount == NULL || mount->boot_vops == NULL)
		return (ENXIO);
	vmmfs_node_setup(&boot->node, parent, vmmfs_boot_drop);
	boot->inode = vmmfs_root_allocate_inode(root);
	vmmfs_node_set_metadata(&boot->node, boot->inode, VMMFS_BOOT_MODE,
	    machine->memory.size);
	serial = atomic_fetchadd_int(&vmmfs_boot_dev_serial, 1);
	boot->dev = make_only_dev(&vmmfs_boot_dev_ops, serial, UID_ROOT,
		GID_WHEEL, VMMFS_BOOT_MODE, "vmmfs_boot%d", serial);
	if (boot->dev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	boot->dev->si_drv1 = boot;
	error = vmmfs_vnode_create_cdev(mount->mount,
	    &mount->boot_vops, boot->dev, &boot->node, vnodep);
	if (error == 0)
		return (0);

fail:
	if (boot->dev != NULL) {
		boot->dev->si_drv1 = NULL;
		destroy_only_dev(boot->dev);
		boot->dev = NULL;
	}
	vmmfs_node_drop(&boot->node);
	return (error);
}

static void
vmmfs_boot_drop(struct vmmfs_node *node)
{
	struct vmmfs_boot *boot;

	boot = (struct vmmfs_boot *)node;
	KKASSERT(boot != NULL);
	if (boot->session != NULL)
		panic("vmmfs_boot_drop: session is still active");
	if (boot->dev != NULL) {
		boot->dev->si_drv1 = NULL;
		destroy_only_dev(boot->dev);
		boot->dev = NULL;
	}
	boot->inode = 0;
	vmmfs_node_parent_put(node);
}


int
vmmfs_boot_arm_locked(struct vmmfs_boot *boot, struct vm_object *object,
	uint64_t size)
{
	struct vmmfs_boot_session *session;
	vm_size_t vm_size;

	if (boot == NULL || vmmfs_boot_machine(boot) == NULL || object == NULL || size == 0 ||
	    (uint64_t)(vm_size_t)size != size)
		return (EINVAL);
	if (boot->session != NULL)
		return (EBUSY);
	vm_size = (vm_size_t)size;
	session = kmalloc(sizeof(*session), M_VMMFS, M_WAITOK | M_ZERO);
	session->boot = boot;
	session->size = vm_size;
	session->references = 1;
	vm_object_reference_quick(object);
	session->backing_object = object;
	session->pager_object = cdev_pager_allocate(session, OBJT_MGTDEVICE,
		&vmmfs_boot_pager_ops, vm_size, VM_PROT_READ | VM_PROT_WRITE, 0,
		proc0.p_ucred);
	if (session->pager_object == NULL) {
		vm_object_deallocate(session->backing_object);
		kfree(session, M_VMMFS);
		return (ENOMEM);
	}
	boot->session = session;
	return (0);
}

bool
vmmfs_boot_is_active(struct vmmfs_boot *boot)
{
	bool active;

	if (boot == NULL || vmmfs_boot_machine(boot) == NULL)
		return (false);
	lwkt_gettoken(&vmmfs_boot_machine(boot)->branch.token);
	active = boot->session != NULL;
	lwkt_reltoken(&vmmfs_boot_machine(boot)->branch.token);
	return (active);
}

void
vmmfs_boot_revoke(struct vmmfs_boot *boot)
{
	struct vmmfs_boot_session *session;
	struct vnode *vnode;

	if (boot == NULL || vmmfs_boot_machine(boot) == NULL)
		return;
	session = vmmfs_boot_take_session(boot, &vnode);
	if (session == NULL) {
		if (vnode != NULL)
			vdrop(vnode);
		return;
	}
	vmmfs_boot_session_revoke(session);
	vmmfs_boot_session_drop_base(session);
	if (vnode != NULL) {
		(void)fdrevoke(vnode, DTYPE_VNODE, proc0.p_ucred);
		vdrop(vnode);
	}
}

static struct vmmfs_boot_session *
vmmfs_boot_take_session(struct vmmfs_boot *boot, struct vnode **vnodep)
{
	struct vmmfs_boot_session *session;

	KKASSERT(boot != NULL && vmmfs_boot_machine(boot) != NULL && vnodep != NULL);
	lwkt_gettoken(&vmmfs_boot_machine(boot)->branch.token);
	session = boot->session;
	boot->session = NULL;
	*vnodep = vmmfs_boot_machine(boot)->boot_vnode;
	if (*vnodep != NULL)
		vhold(*vnodep);
	lwkt_reltoken(&vmmfs_boot_machine(boot)->branch.token);
	return (session);
}

int
vmmfs_boot_submit(struct vmmfs_boot *boot, const struct vmm_cpustate *state)
{
	struct vmmfs_boot_session *session;
	struct vnode *vnode;
	int error;

	if (boot == NULL || vmmfs_boot_machine(boot) == NULL || state == NULL)
		return (EINVAL);
	session = vmmfs_boot_take_session(boot, &vnode);
	if (session == NULL) {
		if (vnode != NULL)
			vdrop(vnode);
		return (EPIPE);
	}
	vmmfs_boot_session_revoke(session);
	vmmfs_boot_session_drop_base(session);
	error = vmmfs_machine_boot_submit(vmmfs_boot_machine(boot), state);
	if (vnode != NULL) {
		(void)fdrevoke(vnode, DTYPE_VNODE, proc0.p_ucred);
		vdrop(vnode);
	}
	return (error);
}

static int
vmmfs_boot_open(struct vop_open_args *ap)
{
	struct vmmfs_boot *boot;
	struct vnode *vnode;
	cdev_t dev;
	int error;

	boot = ap->a_vp->v_data;
	if (boot == NULL || boot->node.dead ||
	    vmmfs_boot_machine(boot) == NULL)
		return (ENOENT);
	if ((ap->a_mode & FWRITE) == 0)
		return (EACCES);
	error = vmmfs_machine_boot_start(vmmfs_boot_machine(boot));
	if (error != 0)
		return (error);
	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	if (dev == NULL) {
		(void)vmmfs_machine_request_stop(vmmfs_boot_machine(boot), "boot-open");
		return (ENXIO);
	}
	vsetflags(vnode, VNOTSEEKABLE);
	vn_unlock(vnode);
	error = dev_dopen(dev, ap->a_mode, S_IFCHR, ap->a_cred, ap->a_fpp,
		vnode);
	vn_lock(vnode, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0) {
		(void)vmmfs_machine_request_stop(vmmfs_boot_machine(boot), "boot-open");
		return (error);
	}
	return (vop_stdopen(ap));
}

static int
vmmfs_boot_close(struct vop_close_args *ap)
{
	struct vnode *vnode;
	cdev_t dev;
	int error;

	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	error = 0;
	if (dev != NULL && vnode->v_opencount <= 1) {
		vn_unlock(vnode);
		error = dev_dclose(dev, ap->a_fflag, S_IFCHR, ap->a_fp);
		vn_lock(vnode, LK_SHARED | LK_RETRY);
	}
	if (vnode->v_opencount > 0)
		vop_stdclose(ap);
	return (error);
}

static int
vmmfs_boot_read(struct vop_read_args *ap)
{
	(void)ap;
	return (EOPNOTSUPP);
}

static int
vmmfs_boot_write(struct vop_write_args *ap)
{
	struct vnode *vnode;
	cdev_t dev;
	int error;

	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	if (dev == NULL)
		return (EBADF);
	vn_unlock(vnode);
	error = dev_dwrite(dev, ap->a_uio, ap->a_ioflag, ap->a_fp);
	vn_lock(vnode, LK_EXCLUSIVE | LK_RETRY);
	return (error);
}


static int
vmmfs_boot_dev_open(struct dev_open_args *ap)
{
	struct vmmfs_boot *boot;
	bool active;

	if (ap == NULL || ap->a_head.a_dev == NULL)
		return (EINVAL);
	boot = ap->a_head.a_dev->si_drv1;
	if (boot == NULL || vmmfs_boot_machine(boot) == NULL)
		return (ENXIO);
	lwkt_gettoken(&vmmfs_boot_machine(boot)->branch.token);
	active = boot->session != NULL && !boot->session->revoked;
	lwkt_reltoken(&vmmfs_boot_machine(boot)->branch.token);
	if (!active)
		return (EPIPE);
	return (0);
}

static int
vmmfs_boot_dev_close(struct dev_close_args *ap)
{
	struct vmmfs_boot *boot;
	int error;

	if (ap == NULL || ap->a_head.a_dev == NULL)
		return (EINVAL);
	boot = ap->a_head.a_dev->si_drv1;
	if (boot == NULL || vmmfs_boot_machine(boot) == NULL)
		return (0);
	if (!vmmfs_boot_cancel_unsubmitted(boot))
		return (0);
	error = vmmfs_machine_boot_abort(vmmfs_boot_machine(boot));
	if (error != 0)
		return (error);
	/* The final boot-session fd closed without a committed BSP state. */
	return (EPIPE);
}

/*
 * Claim and revoke an uncommitted direct-boot session.  A successful
 * cpustate write claims the session first, so a final close can only abort
 * the start when it obtains this session.
 */
static bool
vmmfs_boot_cancel_unsubmitted(struct vmmfs_boot *boot)
{
	struct vmmfs_boot_session *session;
	struct vnode *vnode;

	session = vmmfs_boot_take_session(boot, &vnode);
	if (session == NULL) {
		if (vnode != NULL)
			vdrop(vnode);
		return (false);
	}
	vmmfs_boot_session_revoke(session);
	vmmfs_boot_session_drop_base(session);
	if (vnode != NULL)
		vdrop(vnode);
	return (true);
}

static int
vmmfs_boot_dev_write(struct dev_write_args *ap)
{
	struct vmmfs_boot *boot;
	struct vmm_cpustate state;
	int error;

	if (ap == NULL || ap->a_head.a_dev == NULL || ap->a_uio == NULL)
		return (EINVAL);
	boot = ap->a_head.a_dev->si_drv1;
	if (boot == NULL || ap->a_uio->uio_offset != 0 ||
	    ap->a_uio->uio_resid != sizeof(state))
		return (EINVAL);
	error = uiomove((caddr_t)&state, sizeof(state), ap->a_uio);
	if (error != 0)
		return (error);
	return (vmmfs_boot_submit(boot, &state));
}

static int
vmmfs_boot_dev_mmap_single(struct dev_mmap_single_args *ap)
{
	struct vmmfs_boot *boot;
	struct vmmfs_boot_session *session;
	struct vm_object *object;
	vm_ooffset_t offset;

	if (ap == NULL || ap->a_head.a_dev == NULL)
		return (EINVAL);
	boot = ap->a_head.a_dev->si_drv1;
	if (boot == NULL || vmmfs_boot_machine(boot) == NULL ||
	    (ap->a_nprot & VM_PROT_EXECUTE) != 0)
		return (EINVAL);
	lwkt_gettoken(&vmmfs_boot_machine(boot)->branch.token);
	session = boot->session;
	if (session == NULL || session->pager_object == NULL ||
	    session->revoked) {
		lwkt_reltoken(&vmmfs_boot_machine(boot)->branch.token);
		return (EBADF);
	}
	offset = *ap->a_offset;
	if (offset < 0 || offset > session->size ||
	    ap->a_size > session->size - offset) {
		lwkt_reltoken(&vmmfs_boot_machine(boot)->branch.token);
		return (EINVAL);
	}
	object = session->pager_object;
	VM_OBJECT_LOCK(object);
	if (session->revoked) {
		VM_OBJECT_UNLOCK(object);
		lwkt_reltoken(&vmmfs_boot_machine(boot)->branch.token);
		return (EBADF);
	}
	vm_object_reference_locked(object);
	VM_OBJECT_UNLOCK(object);
	lwkt_reltoken(&vmmfs_boot_machine(boot)->branch.token);
	*ap->a_object = object;
	return (0);
}

static int
vmmfs_boot_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
	vm_ooffset_t offset, struct ucred *cred, u_short *color)
{
	struct vmmfs_boot_session *session;
	struct vmmfs_machine *machine;

	(void)cred;
	session = handle;
	if (session == NULL || color == NULL ||
	    (prot & VM_PROT_EXECUTE) != 0 || offset < 0 ||
	    offset > session->size || size > session->size - offset)
		return (EINVAL);
	machine = vmmfs_boot_machine(session->boot);
	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->branch.token);
	if (session->revoked || session->backing_object == NULL)
		goto fail;
	atomic_add_int(&session->references, 1);
	vmmfs_branch_hold(&machine->branch);
	atomic_add_int(&vmmfs_boot_pager_count, 1);
	lwkt_reltoken(&machine->branch.token);
	*color = 0;
	return (0);

fail:
	lwkt_reltoken(&machine->branch.token);
	return (EINVAL);
}

static void
vmmfs_boot_pager_dtor(void *handle)
{
	struct vmmfs_boot_session *session;

	session = handle;
	if (session != NULL)
		vmmfs_boot_session_drop_pager(session);
}

static int
vmmfs_boot_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot,
	vm_page_t *page_result)
{
	struct vmmfs_boot_session *session;
	struct vm_object *backing_object;
	vm_page_t page;

	session = object->handle;
	if (session == NULL || page_result == NULL || offset < 0 ||
	    offset >= session->size || (prot & VM_PROT_EXECUTE) != 0)
		return (VM_PAGER_ERROR);
	VM_OBJECT_LOCK(object);
	if (session->revoked || session->backing_object == NULL) {
		VM_OBJECT_UNLOCK(object);
		return (VM_PAGER_ERROR);
	}
	backing_object = session->backing_object;
	vm_object_reference_quick(backing_object);
	VM_OBJECT_UNLOCK(object);
	page = vm_page_grab(backing_object, OFF_TO_IDX(offset),
		VM_ALLOC_NORMAL | VM_ALLOC_SYSTEM | VM_ALLOC_ZERO | VM_ALLOC_RETRY);
	vm_object_deallocate(backing_object);
	if (page == NULL)
		return (VM_PAGER_ERROR);
	if (page->valid != VM_PAGE_BITS_ALL)
		vm_page_zero_invalid(page, TRUE);
	*page_result = page;
	return (VM_PAGER_OK);
}

static void
vmmfs_boot_session_revoke(struct vmmfs_boot_session *session)
{
	struct vm_object *object;

	if (session == NULL)
		return;
	object = session->pager_object;
	if (object == NULL)
		return;
	VM_OBJECT_LOCK(object);
	session->revoked = true;
	session->pager_object = NULL;
	VM_OBJECT_UNLOCK(object);
	vm_object_page_remove(object, 0, 0, FALSE);
	vm_object_deallocate(object);
}

static void
vmmfs_boot_session_drop_base(struct vmmfs_boot_session *session)
{
	if (session == NULL)
		return;
	KKASSERT(atomic_fetchadd_int(&session->references, 0) != 0);
	if (atomic_fetchadd_int(&session->references, -1) != 1)
		return;
	if (session->backing_object != NULL)
		vm_object_deallocate(session->backing_object);
	kfree(session, M_VMMFS);
}

static void
vmmfs_boot_session_drop_pager(struct vmmfs_boot_session *session)
{
	if (session == NULL)
		return;
	vmmfs_branch_put(&vmmfs_boot_machine(session->boot)->branch);
	atomic_add_int(&vmmfs_boot_pager_count, -1);
	vmmfs_boot_session_drop_base(session);
}
