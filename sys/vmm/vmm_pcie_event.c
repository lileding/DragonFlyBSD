/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * One private, bidirectional vPCIe function event capability.
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/event.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/sysmsg.h>
#include <sys/ucred.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <machine/atomic.h>

#include "vmm_pcie.h"
#include "vmm_pcie_event.h"

struct vmm_pcie_event {
	/* token protects mut_pending, mut_revoked, mut_sequence and mut_read_kq. */
	struct lwkt_token	token;
	struct kqinfo		mut_read_kq;
	struct vmm_pcie	*borrow_imm_pcie;
	struct vmm_pcie_user	*borrow_imm_provider;
	uint64_t		imm_device_id;
	struct file		*own_mut_fp;
	struct vnode		*own_mut_vnode;
	cdev_t			own_mut_dev;
	uint64_t		mut_sequence;
	int			mut_pending;
	int			mut_revoked;
	int			atomic_mut_refs;
};

static uint32_t vmm_pcie_event_serial;
static int vmm_pcie_event_capability_count;

static d_open_t	vmm_pcie_event_fd_open;
static d_close_t	vmm_pcie_event_fd_close;
static int	vmm_pcie_event_vop_getattr(struct vop_getattr_args *);
static int	vmm_pcie_event_fo_read(struct file *, struct uio *,
		    struct ucred *, int);
static int	vmm_pcie_event_fo_write(struct file *, struct uio *,
		    struct ucred *, int);
static int	vmm_pcie_event_fo_ioctl(struct file *, u_long, caddr_t,
		    struct ucred *, struct sysmsg *);
static int	vmm_pcie_event_fo_kqfilter(struct file *, struct knote *);
static int	vmm_pcie_event_fo_stat(struct file *, struct stat *,
		    struct ucred *);
static int	vmm_pcie_event_fo_close(struct file *);
static int	vmm_pcie_event_fo_seek(struct file *, off_t, int, off_t *);
static void	vmm_pcie_event_fd_free(void *);
static int	vmm_pcie_event_open_fd(struct vmm_pcie_event *, struct file **);
static int	vmm_pcie_event_make_vnode(cdev_t, struct vnode **);
static void	vmm_pcie_event_disarm_fp(struct file *);
static void	vmm_pcie_event_put(struct vmm_pcie_event *);
static void	vmm_pcie_event_filter_detach(struct knote *);
static int	vmm_pcie_event_filter_read(struct knote *, long);

static struct dev_ops vmm_pcie_event_fd_ops = {
	{ "vmm_pcie_event_fd", 0, D_MPSAFE },
	.d_open = vmm_pcie_event_fd_open,
	.d_close = vmm_pcie_event_fd_close,
};

static struct vop_ops vmm_pcie_event_vnode_vops = {
	.vop_default =	vop_defaultop,
	.vop_close =	vop_stdclose,
	.vop_getattr =	vmm_pcie_event_vop_getattr,
	.vop_advlock =	(void *)vop_null,
	.vop_inactive =	(void *)vop_null,
	.vop_reclaim =	(void *)vop_null,
	.vop_pathconf =	vop_stdpathconf,
};

static struct vop_ops *vmm_pcie_event_vnode_vops_p =
	&vmm_pcie_event_vnode_vops;

static struct fileops vmm_pcie_event_fileops = {
	.fo_read =	vmm_pcie_event_fo_read,
	.fo_write =	vmm_pcie_event_fo_write,
	.fo_ioctl =	vmm_pcie_event_fo_ioctl,
	.fo_kqfilter =	vmm_pcie_event_fo_kqfilter,
	.fo_stat =	vmm_pcie_event_fo_stat,
	.fo_close =	vmm_pcie_event_fo_close,
	.fo_shutdown =	nofo_shutdown,
	.fo_seek =	vmm_pcie_event_fo_seek,
};

static struct filterops vmm_pcie_event_read_filterops = {
	FILTEROP_ISFD | FILTEROP_MPSAFE,
	NULL,
	vmm_pcie_event_filter_detach,
	vmm_pcie_event_filter_read,
};

int
vmm_pcie_event_create(struct vmm_pcie_event **eventp,
	struct vmm_pcie *pcie, struct vmm_pcie_user *provider, uint64_t device_id)
{
	struct vmm_pcie_event *event;
	uint32_t serial;
	int error;

	if (eventp == NULL || pcie == NULL || provider == NULL || device_id == 0)
		return EINVAL;
	*eventp = NULL;
	event = kmalloc(sizeof(*event), M_TEMP, M_WAITOK | M_ZERO);
	lwkt_token_init(&event->token, "vmmpcevt");
	SLIST_INIT(&event->mut_read_kq.ki_note);
	event->borrow_imm_pcie = pcie;
	event->borrow_imm_provider = provider;
	event->imm_device_id = device_id;
	event->atomic_mut_refs = 1;
	atomic_add_int(&vmm_pcie_event_capability_count, 1);
	serial = atomic_fetchadd_int(&vmm_pcie_event_serial, 1);
	event->own_mut_dev = make_only_dev(&vmm_pcie_event_fd_ops, serial,
	    UID_ROOT, GID_WHEEL, 0600, "vmmpe%d", serial);
	if (event->own_mut_dev == NULL) {
		vmm_pcie_event_put(event);
		return ENXIO;
	}
	event->own_mut_dev->si_drv1 = event;
	error = vmm_pcie_event_open_fd(event, &event->own_mut_fp);
	if (error != 0) {
		vmm_pcie_event_fd_free(event);
		return error;
	}
	*eventp = event;
	return 0;
}

struct file *
vmm_pcie_event_file_hold(struct vmm_pcie_event *event)
{

	if (event == NULL || event->own_mut_fp == NULL)
		return NULL;
	fhold(event->own_mut_fp);
	return event->own_mut_fp;
}

void
vmm_pcie_event_hold(struct vmm_pcie_event *event)
{

	if (event != NULL)
		atomic_add_int(&event->atomic_mut_refs, 1);
}

void
vmm_pcie_event_release(struct vmm_pcie_event *event)
{

	if (event != NULL)
		vmm_pcie_event_put(event);
}

void
vmm_pcie_event_signal(struct vmm_pcie_event *event)
{

	if (event == NULL)
		return;
	lwkt_gettoken(&event->token);
	if (!event->mut_revoked && !event->mut_pending) {
		event->mut_pending = 1;
		event->mut_sequence++;
		wakeup(event);
		KNOTE(&event->mut_read_kq.ki_note, 0);
	}
	lwkt_reltoken(&event->token);
}

void
vmm_pcie_event_revoke(struct vmm_pcie_event *event)
{

	if (event == NULL)
		return;
	lwkt_gettoken(&event->token);
	if (!event->mut_revoked) {
		event->mut_revoked = 1;
		event->mut_pending = 0;
		wakeup(event);
		KNOTE(&event->mut_read_kq.ki_note, 0);
	}
	lwkt_reltoken(&event->token);
}

void
vmm_pcie_event_destroy(struct vmm_pcie_event *event)
{
	struct file *fp;

	if (event == NULL)
		return;
	vmm_pcie_event_revoke(event);
	fp = event->own_mut_fp;
	event->own_mut_fp = NULL;
	if (fp != NULL)
		fp_close(fp);
}

int
vmm_pcie_event_active(void)
{

	return atomic_fetchadd_int(&vmm_pcie_event_capability_count, 0) != 0;
}

struct vmm_pcie *
vmm_pcie_event_pcie(const struct vmm_pcie_event *event)
{

	return event != NULL ? event->borrow_imm_pcie : NULL;
}

struct vmm_pcie_user *
vmm_pcie_event_provider(const struct vmm_pcie_event *event)
{

	return event != NULL ? event->borrow_imm_provider : NULL;
}

uint64_t
vmm_pcie_event_device_id(const struct vmm_pcie_event *event)
{

	return event != NULL ? event->imm_device_id : 0;
}

static int
vmm_pcie_event_fd_open(struct dev_open_args *ap)
{

	(void)ap;
	return 0;
}

static int
vmm_pcie_event_fd_close(struct dev_close_args *ap)
{

	(void)ap;
	return 0;
}

static int
vmm_pcie_event_vop_getattr(struct vop_getattr_args *ap)
{

	(void)ap->a_fp;
	bzero(ap->a_vap, sizeof(*ap->a_vap));
	ap->a_vap->va_type = VCHR;
	ap->a_vap->va_mode = 0600;
	ap->a_vap->va_uid = UID_ROOT;
	ap->a_vap->va_gid = GID_WHEEL;
	ap->a_vap->va_nlink = 1;
	ap->a_vap->va_blocksize = PAGE_SIZE;
	return 0;
}

static int
vmm_pcie_event_fo_read(struct file *fp, struct uio *uio,
	struct ucred *cred, int flags)
{
	struct vmm_pcie_event *event;
	uint64_t sequence;
	int error;

	(void)cred;
	(void)flags;
	if (uio->uio_resid != sizeof(sequence))
		return EINVAL;
	error = devfs_get_cdevpriv(fp, (void **)&event);
	if (error != 0)
		return error;
	lwkt_gettoken(&event->token);
	if (event->mut_revoked) {
		error = ENXIO;
	} else if (!event->mut_pending) {
		error = EWOULDBLOCK;
	} else {
		sequence = event->mut_sequence;
		event->mut_pending = 0;
		error = 0;
	}
	lwkt_reltoken(&event->token);
	if (error != 0)
		return error;
	return uiomove((caddr_t)&sequence, sizeof(sequence), uio);
}

static int
vmm_pcie_event_fo_write(struct file *fp, struct uio *uio,
	struct ucred *cred, int flags)
{
	struct vmm_pcie_abi_msix message;
	struct vmm_pcie_event *event;
	int error;

	(void)cred;
	(void)flags;
	if (uio->uio_resid != sizeof(message))
		return EINVAL;
	error = devfs_get_cdevpriv(fp, (void **)&event);
	if (error != 0)
		return error;
	error = uiomove((caddr_t)&message, sizeof(message), uio);
	if (error != 0)
		return error;
	if (vmm_pcie_abi_validate(&message, sizeof(message)) != 0 ||
	    le16toh(message.header.le_type) != VMM_PCIE_ABI_MSG_MSIX)
		return EINVAL;
	lwkt_gettoken(&event->token);
	if (event->mut_revoked)
		error = ENXIO;
	else
		error = vmm_pcie_device_event_msix(event, &message);
	lwkt_reltoken(&event->token);
	return error;
}

static int
vmm_pcie_event_fo_ioctl(struct file *fp, u_long com, caddr_t data,
	struct ucred *cred, struct sysmsg *msg)
{

	(void)fp;
	(void)com;
	(void)data;
	(void)cred;
	(void)msg;
	return EOPNOTSUPP;
}

static int
vmm_pcie_event_fo_kqfilter(struct file *fp, struct knote *kn)
{
	struct vmm_pcie_event *event;
	int error;

	error = devfs_get_cdevpriv(fp, (void **)&event);
	if (error != 0)
		return error;
	if (kn->kn_filter != EVFILT_READ)
		return EOPNOTSUPP;
	lwkt_gettoken(&event->token);
	kn->kn_fop = &vmm_pcie_event_read_filterops;
	kn->kn_hook = (caddr_t)event;
	knote_insert(&event->mut_read_kq.ki_note, kn);
	lwkt_reltoken(&event->token);
	return 0;
}

static int
vmm_pcie_event_fo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{

	(void)fp;
	(void)cred;
	bzero(sb, sizeof(*sb));
	sb->st_nlink = 1;
	sb->st_mode = S_IFCHR | 0600;
	sb->st_uid = UID_ROOT;
	sb->st_gid = GID_WHEEL;
	sb->st_blksize = PAGE_SIZE;
	sb->__old_st_blksize = sb->st_blksize;
	return 0;
}

static int
vmm_pcie_event_fo_close(struct file *fp)
{

	vmm_pcie_event_disarm_fp(fp);
	return 0;
}

static int
vmm_pcie_event_fo_seek(struct file *fp, off_t offset, int whence, off_t *res)
{

	(void)fp;
	(void)offset;
	(void)whence;
	(void)res;
	return ESPIPE;
}

static int
vmm_pcie_event_open_fd(struct vmm_pcie_event *event, struct file **fpp)
{
	struct vnode *vp;
	struct file *fp;
	int error;

	error = vmm_pcie_event_make_vnode(event->own_mut_dev, &vp);
	if (error != 0)
		return error;
	event->own_mut_vnode = vp;
	error = falloc(NULL, &fp, NULL);
	if (error != 0)
		return error;
	fsetcred(fp, proc0.p_ucred);
	fp->f_type = DTYPE_VNODE;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &vmm_pcie_event_fileops;
	fp->f_data = vp;
	vref(vp);
	atomic_add_int(&vp->v_opencount, 1);
	atomic_add_int(&vp->v_writecount, 1);
	error = devfs_set_cdevpriv(fp, event, vmm_pcie_event_fd_free);
	if (error != 0) {
		fp_close(fp);
		return error;
	}
	*fpp = fp;
	return 0;
}

static int
vmm_pcie_event_make_vnode(cdev_t dev, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	error = getspecialvnode(VT_NON, NULL, &vmm_pcie_event_vnode_vops_p, &vp,
	    0, 0);
	if (error != 0) {
		*vpp = NULL;
		return error;
	}
	vp->v_type = VCHR;
	error = v_associate_rdev(vp, dev);
	if (error != 0) {
		vgone_vxlocked(vp);
		vx_put(vp);
		*vpp = NULL;
		return error;
	}
	vp->v_umajor = dev->si_umajor;
	vp->v_uminor = dev->si_uminor;
	vx_unlock(vp);
	*vpp = vp;
	return 0;
}

static void
vmm_pcie_event_disarm_fp(struct file *fp)
{
	struct vnode *vp;

	if (fp == NULL || fp->f_ops == &badfileops)
		return;
	vp = fp->f_data;
	fp->f_data = NULL;
	atomic_clear_int(&fp->f_flag, FHASLOCK);
	fp->f_ops = &badfileops;
	if (vp != NULL)
		(void)vn_close(vp, fp->f_flag, fp);
	devfs_clear_cdevpriv(fp);
}

static void
vmm_pcie_event_put(struct vmm_pcie_event *event)
{

	if (atomic_fetchadd_int(&event->atomic_mut_refs, -1) == 1) {
		atomic_add_int(&vmm_pcie_event_capability_count, -1);
		kfree(event, M_TEMP);
	}
}

static void
vmm_pcie_event_filter_detach(struct knote *kn)
{
	struct vmm_pcie_event *event;

	event = (struct vmm_pcie_event *)kn->kn_hook;
	lwkt_gettoken(&event->token);
	knote_remove(&event->mut_read_kq.ki_note, kn);
	lwkt_reltoken(&event->token);
}

static int
vmm_pcie_event_filter_read(struct knote *kn, long hint)
{
	struct vmm_pcie_event *event;
	int ready;

	(void)hint;
	event = (struct vmm_pcie_event *)kn->kn_hook;
	lwkt_gettoken(&event->token);
	ready = event->mut_pending || event->mut_revoked;
	if (event->mut_revoked)
		kn->kn_flags |= EV_EOF;
	else
		kn->kn_data = event->mut_sequence;
	lwkt_reltoken(&event->token);
	return ready;
}

static void
vmm_pcie_event_fd_free(void *arg)
{
	struct vmm_pcie_event *event;
	struct vnode *vp;

	event = arg;
	if (event == NULL)
		return;
	vmm_pcie_event_revoke(event);
	vp = event->own_mut_vnode;
	if (vp != NULL) {
		event->own_mut_vnode = NULL;
		vx_get(vp);
		vgone_vxlocked(vp);
		vx_put(vp);
		vrele(vp);
	}
	if (event->own_mut_dev != NULL) {
		event->own_mut_dev->si_drv1 = NULL;
		destroy_only_dev(event->own_mut_dev);
		event->own_mut_dev = NULL;
	}
	vmm_pcie_event_put(event);
}
