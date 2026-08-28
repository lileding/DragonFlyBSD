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

static int vmmfs_boot_access(struct vop_access_args *);
static int vmmfs_boot_getattr(struct vop_getattr_args *);
static int vmmfs_boot_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_boot_open(struct vop_open_args *);
static int vmmfs_boot_close(struct vop_close_args *);
static int vmmfs_boot_read(struct vop_read_args *);
static int vmmfs_boot_write(struct vop_write_args *);
static int vmmfs_boot_inactive(struct vop_inactive_args *);
static int vmmfs_boot_reclaim(struct vop_reclaim_args *);
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

struct vop_ops vmmfs_boot_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_boot_access,
	.vop_close = vmmfs_boot_close,
	.vop_getattr = vmmfs_boot_getattr,
	.vop_getattr_lite = vmmfs_boot_getattr_lite,
	.vop_open = vmmfs_boot_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_boot_read,
	.vop_inactive = vmmfs_boot_inactive,
	.vop_reclaim = vmmfs_boot_reclaim,
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
vmmfs_boot_init(struct vmmfs_machine *machine, struct vmmfs_boot *boot)
{
	struct vmmfs_mount *mount;
	uint32_t serial;
	int error;

	if (machine == NULL || boot == NULL || machine->root == NULL)
		return (EINVAL);
	bzero(boot, sizeof(*boot));
	mount = (struct vmmfs_mount *)machine->root->mount->mnt_data;
	if (mount == NULL || mount->boot_vops == NULL)
		return (ENXIO);
	boot->machine = machine;
	vmmfs_machine_hold(machine);
	boot->inode = atomic_fetchadd_int(&mount->next_inode, 1);
	serial = atomic_fetchadd_int(&vmmfs_boot_dev_serial, 1);
	boot->dev = make_only_dev(&vmmfs_boot_dev_ops, serial, UID_ROOT,
		GID_WHEEL, VMMFS_BOOT_MODE, "vmmfs_boot%d", serial);
	if (boot->dev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	boot->dev->si_drv1 = boot;
	error = vmmfs_node_init_cdev(&boot->node, machine->root->mount,
		&mount->boot_vops, boot->dev, boot);
	if (error != 0)
		goto fail;
	return (0);

fail:
	vmmfs_node_abort(&boot->node);
	if (boot->dev != NULL) {
		boot->dev->si_drv1 = NULL;
		destroy_only_dev(boot->dev);
		boot->dev = NULL;
	}
	boot->machine = NULL;
	boot->inode = 0;
	vmmfs_machine_put(machine);
	return (error);
}

void
vmmfs_boot_fini(struct vmmfs_boot *boot)
{
	struct vmmfs_machine *machine;

	if (boot == NULL)
		return;
	machine = boot->machine;
	if (machine == NULL)
		return;
	if (boot->node.published || boot->session != NULL)
		panic("vmmfs_boot_fini: node or session is still active");
	vmmfs_node_abort(&boot->node);
	if (boot->dev != NULL) {
		boot->dev->si_drv1 = NULL;
		destroy_only_dev(boot->dev);
		boot->dev = NULL;
	}
	boot->machine = NULL;
	boot->inode = 0;
	vmmfs_machine_put(machine);
	return;
}

int
vmmfs_boot_arm_locked(struct vmmfs_boot *boot, struct vm_object *object,
	uint64_t size)
{
	struct vmmfs_boot_session *session;
	vm_size_t vm_size;

	if (boot == NULL || boot->machine == NULL || object == NULL || size == 0 ||
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

	if (boot == NULL || boot->machine == NULL)
		return (false);
	lwkt_gettoken(&boot->machine->token);
	active = boot->session != NULL;
	lwkt_reltoken(&boot->machine->token);
	return (active);
}

void
vmmfs_boot_revoke(struct vmmfs_boot *boot)
{
	struct vmmfs_boot_session *session;
	struct vnode *vnode;

	if (boot == NULL || boot->machine == NULL)
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

	KKASSERT(boot != NULL && boot->machine != NULL && vnodep != NULL);
	lwkt_gettoken(&boot->machine->token);
	session = boot->session;
	boot->session = NULL;
	*vnodep = boot->node.vnode;
	if (*vnodep != NULL)
		vhold(*vnodep);
	lwkt_reltoken(&boot->machine->token);
	return (session);
}

int
vmmfs_boot_submit(struct vmmfs_boot *boot, const struct vmm_cpustate *state)
{
	struct vmmfs_boot_session *session;
	struct vnode *vnode;
	int error;

	if (boot == NULL || boot->machine == NULL || state == NULL)
		return (EINVAL);
	session = vmmfs_boot_take_session(boot, &vnode);
	if (session == NULL) {
		if (vnode != NULL)
			vdrop(vnode);
		return (EPIPE);
	}
	vmmfs_boot_session_revoke(session);
	vmmfs_boot_session_drop_base(session);
	error = vmmfs_machine_boot_submit(boot->machine, state);
	if (vnode != NULL) {
		(void)fdrevoke(vnode, DTYPE_VNODE, proc0.p_ucred);
		vdrop(vnode);
	}
	return (error);
}

static int
vmmfs_boot_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_BOOT_MODE, 0));
}

static int
vmmfs_boot_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_boot *boot;
	struct vattr *vattr;
	uint64_t size;

	boot = ap->a_vp->v_data;
	if (boot == NULL || boot->machine == NULL ||
	    vmmfs_machine_is_dead(boot->machine))
		return (ENOENT);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VCHR;
	vattr->va_mode = VMMFS_BOOT_MODE;
	vattr->va_flags = 0;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = boot->inode;
	lwkt_gettoken(&boot->machine->token);
	size = boot->machine->memory.size;
	lwkt_reltoken(&boot->machine->token);
	vattr->va_size = size;
	vattr->va_blocksize = PAGE_SIZE;
	return (0);
}

static int
vmmfs_boot_getattr_lite(struct vop_getattr_lite_args *ap)
{
	struct vmmfs_boot *boot;
	uint64_t size;

	boot = ap->a_vp->v_data;
	if (boot == NULL || boot->machine == NULL)
		return (ENOENT);
	lwkt_gettoken(&boot->machine->token);
	size = boot->machine->memory.size;
	lwkt_reltoken(&boot->machine->token);
	ap->a_lvap->va_type = VCHR;
	ap->a_lvap->va_mode = VMMFS_BOOT_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = size;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_boot_open(struct vop_open_args *ap)
{
	struct vmmfs_boot *boot;
	struct vnode *vnode;
	cdev_t dev;
	int error;

	boot = ap->a_vp->v_data;
	if (boot == NULL || boot->machine == NULL)
		return (ENOENT);
	if ((ap->a_mode & FWRITE) == 0)
		return (EACCES);
	error = vmmfs_machine_boot_start(boot->machine);
	if (error != 0)
		return (error);
	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	if (dev == NULL) {
		(void)vmmfs_machine_stop_request(boot->machine, "boot-open");
		return (ENXIO);
	}
	vsetflags(vnode, VNOTSEEKABLE);
	vn_unlock(vnode);
	error = dev_dopen(dev, ap->a_mode, S_IFCHR, ap->a_cred, ap->a_fpp,
		vnode);
	vn_lock(vnode, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0) {
		(void)vmmfs_machine_stop_request(boot->machine, "boot-open");
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
vmmfs_boot_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_boot *boot;
	struct vmmfs_machine *machine;

	boot = ap->a_vp->v_data;
	if (boot == NULL || boot->machine == NULL)
		return (0);
	machine = boot->machine;
	if (!vmmfs_machine_is_dead(machine))
		return (0);
	vmmfs_node_inactive(&boot->node, ap->a_vp);
	return (0);
}

static int
vmmfs_boot_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_boot *boot;
	struct vmmfs_machine *machine;
	bool reclaim;

	boot = ap->a_vp->v_data;
	if (boot == NULL || boot->machine == NULL)
		return (0);
	machine = boot->machine;
	lwkt_gettoken(&machine->token);
	reclaim = vmmfs_node_reclaim(&boot->node, ap->a_vp);
	lwkt_reltoken(&machine->token);
	if (reclaim)
		vmmfs_boot_fini(boot);
	return (0);
}

static int
vmmfs_boot_dev_open(struct dev_open_args *ap)
{
	struct vmmfs_boot *boot;
	bool active;

	if (ap == NULL || ap->a_head.a_dev == NULL)
		return (EINVAL);
	boot = ap->a_head.a_dev->si_drv1;
	if (boot == NULL || boot->machine == NULL)
		return (ENXIO);
	lwkt_gettoken(&boot->machine->token);
	active = boot->session != NULL && !boot->session->revoked;
	lwkt_reltoken(&boot->machine->token);
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
	if (boot == NULL || boot->machine == NULL)
		return (0);
	if (!vmmfs_boot_cancel_unsubmitted(boot))
		return (0);
	error = vmmfs_machine_boot_abort(boot->machine);
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
	if (boot == NULL || boot->machine == NULL ||
	    (ap->a_nprot & VM_PROT_EXECUTE) != 0)
		return (EINVAL);
	lwkt_gettoken(&boot->machine->token);
	session = boot->session;
	if (session == NULL || session->pager_object == NULL ||
	    session->revoked) {
		lwkt_reltoken(&boot->machine->token);
		return (EBADF);
	}
	offset = *ap->a_offset;
	if (offset < 0 || offset > session->size ||
	    ap->a_size > session->size - offset) {
		lwkt_reltoken(&boot->machine->token);
		return (EINVAL);
	}
	object = session->pager_object;
	VM_OBJECT_LOCK(object);
	if (session->revoked) {
		VM_OBJECT_UNLOCK(object);
		lwkt_reltoken(&boot->machine->token);
		return (EBADF);
	}
	vm_object_reference_locked(object);
	VM_OBJECT_UNLOCK(object);
	lwkt_reltoken(&boot->machine->token);
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
	machine = session->boot->machine;
	if (machine == NULL)
		return (EINVAL);
	lwkt_gettoken(&machine->token);
	if (session->revoked || session->backing_object == NULL)
		goto fail;
	atomic_add_int(&session->references, 1);
	vmmfs_machine_hold(machine);
	atomic_add_int(&vmmfs_boot_pager_count, 1);
	lwkt_reltoken(&machine->token);
	*color = 0;
	return (0);

fail:
	lwkt_reltoken(&machine->token);
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
	vmmfs_machine_put(session->boot->machine);
	atomic_add_int(&vmmfs_boot_pager_count, -1);
	vmmfs_boot_session_drop_base(session);
}
