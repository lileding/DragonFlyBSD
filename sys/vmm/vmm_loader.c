/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The loader object -- see vmm_loader.h.
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <sys/devfs.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/fcntl.h>
#include <sys/imgact.h>
#include <sys/kern_syscall.h>
#include <sys/nlookup.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ucred.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/wait.h>
#include <machine/atomic.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>

#include "vmm_parse.h"
#include "vmm_mem.h"
#include "vmm_loader.h"

int
vmm_loader_parse(struct vmm_loader *l, const char *buf, size_t len)
{
	size_t pl;
	const char *p = vmm_trim(buf, len, &pl);

	if (pl == 0 || pl > VMM_LOADER_MAX)
		return 0;
	memcpy(l->path, p, pl);
	l->len = pl;
	return 1;
}

size_t
vmm_loader_format(const struct vmm_loader *l, char *out, size_t cap)
{
	size_t need = l->len + 1;

	if (l->len == 0 || need > cap)
		return 0;
	memcpy(out, l->path, l->len);
	out[l->len] = '\n';
	return need;
}

size_t
vmm_loader_path(const struct vmm_loader *l, char *out, size_t cap)
{
	if (l->len == 0 || l->len > cap)
		return 0;
	memcpy(out, l->path, l->len);
	return l->len;
}

int
vmm_loader_is_set(const struct vmm_loader *l)
{
	return l->len != 0;
}

#define VMM_MANIFEST_MAGIC	"VMMLD0\0\0"
#define VMM_MANIFEST_SIZE	PAGE_SIZE
#define VMM_MANIFEST_ABI	0
#define VMM_MANIFEST_ARCH_X64	1

#define VMM_REC_X64_VCPU_STATE	1
#define VMM_REC_GPA_RANGE	2
#define VMM_REC_F_MANDATORY	1

#define VMM_GPR_RSP		4
#define VMM_GPR_RIP		16
#define VMM_CR_CR3		3
#define VMM_SEG_GDTR		6
#define VMM_SEG_IDTR		7

struct vmm_manifest_header {
	char		magic[8];
	uint16_t	abi_version;
	uint16_t	arch;
	uint32_t	header_size;
	uint32_t	total_size;
	uint32_t	record_count;
	uint64_t	mem_size;
	uint32_t	flags;
	uint32_t	reserved;
} __packed;

struct vmm_manifest_record {
	uint16_t	type;
	uint16_t	flags;
	uint32_t	size;
} __packed;

struct vmm_x64_seg_state {
	uint16_t	selector;
	uint16_t	attrib;
	uint32_t	limit;
	uint64_t	base;
} __packed;

struct vmm_x64_vcpu_state {
	uint32_t	vcpu_id;
	uint32_t	flags;
	uint64_t	runnable;
	uint64_t	gpr[18];
	uint64_t	cr[6];
	uint64_t	msr[11];
	struct vmm_x64_seg_state seg[10];
	uint64_t	intr_flags;
} __packed;

struct vmm_gpa_range {
	uint64_t	start;
	uint64_t	size;
	uint32_t	type;
	uint32_t	flags;
} __packed;

struct vmm_loader_epoch {
	struct file	*mem_fp;
	struct file	*manifest_fp;
	void		*manifest_data;
	uint64_t	 mem_size;
	struct ucred	*cred;
	vmm_loader_cancel_fn *cancel;
	void		*cancel_arg;
	char		 loader_path[VMM_LOADER_MAX + 1];
};

struct vmm_loader_fd {
	cdev_t		dev;
	struct vm_object *object;
	void		*data;
	vm_size_t	size;
	int		buffer;
};

static uint32_t vmm_loader_fd_serial;

static d_open_t		vmm_loader_fd_open;
static d_close_t	vmm_loader_fd_close;
static d_mmap_single_t	vmm_loader_fd_mmap_single;
static int		vmm_loader_fd_uksmap(struct vm_map_backing *ba,
			    int op, cdev_t dev, vm_page_t fake);
static int		vmm_loader_vop_getattr(struct vop_getattr_args *ap);
static int		vmm_loader_fo_readwrite(struct file *fp,
			    struct uio *uio, struct ucred *cred, int flags);
static int		vmm_loader_fo_ioctl(struct file *fp, u_long com,
			    caddr_t data, struct ucred *cred,
			    struct sysmsg *msg);
static int		vmm_loader_fo_kqfilter(struct file *fp,
			    struct knote *kn);
static int		vmm_loader_fo_stat(struct file *fp, struct stat *sb,
			    struct ucred *cred);
static int		vmm_loader_fo_close(struct file *fp);
static int		vmm_loader_fo_seek(struct file *fp, off_t offset,
			    int whence, off_t *res);

static struct dev_ops vmm_loader_object_fd_ops = {
	{ "vmm_loader_object_fd", 0, D_MPSAFE },
	.d_open = vmm_loader_fd_open,
	.d_close = vmm_loader_fd_close,
	.d_mmap_single = vmm_loader_fd_mmap_single,
};

static struct dev_ops vmm_loader_buffer_fd_ops = {
	{ "vmm_loader_buffer_fd", 0, D_MPSAFE },
	.d_open = vmm_loader_fd_open,
	.d_close = vmm_loader_fd_close,
	.d_uksmap = vmm_loader_fd_uksmap,
};

static struct vop_ops vmm_loader_vnode_vops = {
	.vop_default =		vop_defaultop,
	.vop_close =		vop_stdclose,
	.vop_getattr =		vmm_loader_vop_getattr,
	.vop_pathconf =		vop_stdpathconf,
};

static struct vop_ops *vmm_loader_vnode_vops_p = &vmm_loader_vnode_vops;

static struct fileops vmm_loader_fileops = {
	.fo_read =		vmm_loader_fo_readwrite,
	.fo_write =		vmm_loader_fo_readwrite,
	.fo_ioctl =		vmm_loader_fo_ioctl,
	.fo_kqfilter =		vmm_loader_fo_kqfilter,
	.fo_stat =		vmm_loader_fo_stat,
	.fo_close =		vmm_loader_fo_close,
	.fo_shutdown =		nofo_shutdown,
	.fo_seek =		vmm_loader_fo_seek,
};

static int
vmm_loader_fd_open(struct dev_open_args *ap)
{
	(void)ap;
	return 0;
}

static int
vmm_loader_fd_close(struct dev_close_args *ap)
{
	(void)ap;
	return 0;
}

static void
vmm_loader_fill_vattr(struct vattr *vap, vm_size_t size)
{
	VATTR_NULL(vap);
	vap->va_type = VCHR;
	vap->va_mode = 0600;
	vap->va_nlink = 1;
	vap->va_uid = UID_ROOT;
	vap->va_gid = GID_WHEEL;
	vap->va_fileid = 0;
	vap->va_size = (off_t)size;
	vap->va_blocksize = PAGE_SIZE;
	vap->va_bytes = (off_t)round_page(size);
	vap->va_atime.tv_sec = 0;
	vap->va_atime.tv_nsec = 0;
	vap->va_mtime = vap->va_atime;
	vap->va_ctime = vap->va_atime;
	vap->va_flags = 0;
	vap->va_gen = 1;
	vap->va_filerev = 0;
}

static int
vmm_loader_vop_getattr(struct vop_getattr_args *ap)
{
	struct vmm_loader_fd *lfd;
	vm_size_t size = 0;
	int error;

	if (ap->a_fp != NULL) {
		error = devfs_get_cdevpriv(ap->a_fp, (void **)&lfd);
		if (error)
			return error;
		size = lfd->size;
	}
	vmm_loader_fill_vattr(ap->a_vap, size);
	return 0;
}

static int
vmm_loader_fo_readwrite(struct file *fp, struct uio *uio,
    struct ucred *cred, int flags)
{
	(void)fp;
	(void)uio;
	(void)cred;
	(void)flags;
	return EOPNOTSUPP;
}

static int
vmm_loader_fo_ioctl(struct file *fp, u_long com, caddr_t data,
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
vmm_loader_fo_kqfilter(struct file *fp, struct knote *kn)
{
	(void)fp;
	(void)kn;
	return EOPNOTSUPP;
}

static int
vmm_loader_fo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{
	struct vmm_loader_fd *lfd;
	int error;

	(void)cred;
	error = devfs_get_cdevpriv(fp, (void **)&lfd);
	if (error)
		return error;
	bzero(sb, sizeof(*sb));
	sb->st_nlink = 1;
	sb->st_mode = S_IFCHR | 0600;
	sb->st_uid = UID_ROOT;
	sb->st_gid = GID_WHEEL;
	sb->st_size = (off_t)lfd->size;
	sb->st_blocks = howmany(lfd->size, S_BLKSIZE);
	sb->st_blksize = PAGE_SIZE;
	sb->__old_st_blksize = sb->st_blksize;
	return 0;
}

static int
vmm_loader_fo_close(struct file *fp)
{
	struct vnode *vp = fp->f_data;
	int error = 0;

	fp->f_ops = &badfileops;
	if (vp != NULL)
		error = vn_close(vp, fp->f_flag, fp);
	devfs_clear_cdevpriv(fp);
	return error;
}

static int
vmm_loader_fo_seek(struct file *fp, off_t offset, int whence, off_t *res)
{
	(void)fp;
	(void)offset;
	(void)whence;
	(void)res;
	return ESPIPE;
}

static void
vmm_loader_fd_free(void *arg)
{
	struct vmm_loader_fd *lfd = arg;

	if (lfd->dev != NULL) {
		lfd->dev->si_drv1 = NULL;
		destroy_only_dev(lfd->dev);
	}
	kfree(lfd, M_TEMP);
}

static int
vmm_loader_fd_mmap_single(struct dev_mmap_single_args *ap)
{
	struct vmm_loader_fd *lfd;
	vm_ooffset_t off;
	int error;

	error = devfs_get_cdevpriv(ap->a_fp, (void **)&lfd);
	if (error)
		return error;
	if (lfd->buffer || lfd->object == NULL)
		return EINVAL;
	off = *ap->a_offset;
	if (off < 0 || off > lfd->size || ap->a_size > lfd->size - off)
		return EINVAL;
	vm_object_reference_quick(lfd->object);
	*ap->a_object = lfd->object;
	return 0;
}

static int
vmm_loader_fd_uksmap(struct vm_map_backing *ba, int op, cdev_t dev,
    vm_page_t fake)
{
	struct vmm_loader_fd *lfd = dev->si_drv1;
	vm_ooffset_t off;

	if (lfd == NULL || !lfd->buffer || lfd->data == NULL)
		return EINVAL;
	switch (op) {
	case UKSMAPOP_ADD:
	case UKSMAPOP_REM:
		return 0;
	case UKSMAPOP_FAULT:
		off = IDX_TO_OFF(fake->pindex);
		if (off < 0 || off >= lfd->size)
			return EINVAL;
		fake->phys_addr = vtophys((char *)lfd->data + off);
		return 0;
	default:
		(void)ba;
		return EINVAL;
	}
}

static int
vmm_loader_make_vnode(cdev_t dev, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	error = getspecialvnode(VT_NON, NULL, &vmm_loader_vnode_vops_p, &vp,
	    0, 0);
	if (error) {
		*vpp = NULL;
		return error;
	}
	vp->v_type = VCHR;
	error = v_associate_rdev(vp, dev);
	if (error) {
		vx_unlock(vp);
		vrele(vp);
		*vpp = NULL;
		return error;
	}
	vp->v_umajor = dev->si_umajor;
	vp->v_uminor = dev->si_uminor;
	vx_unlock(vp);
	*vpp = vp;
	return 0;
}

static int
vmm_loader_open_fd(struct vmm_loader_fd *lfd, struct file **fpp)
{
	struct vnode *vp;
	struct file *fp;
	int error;

	error = vmm_loader_make_vnode(lfd->dev, &vp);
	if (error)
		goto fail;
	error = falloc(NULL, &fp, NULL);
	if (error) {
		vrele(vp);
		goto fail;
	}
	/*
	 * This is an internal mmap capability, not a normal devfs path.  Build
	 * the file directly so the worker does not need to resolve a devfs
	 * path, and so pathless devfs vnodes do not go through VOP_ACCESS.
	 */
	fsetcred(fp, proc0.p_ucred);
	fp->f_type = DTYPE_VNODE;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &vmm_loader_fileops;
	fp->f_data = vp;
	vref(vp);
	atomic_add_int(&vp->v_opencount, 1);
	atomic_add_int(&vp->v_writecount, 1);
	vrele(vp);
	error = devfs_set_cdevpriv(fp, lfd, vmm_loader_fd_free);
	if (error) {
		fp_close(fp);
		vmm_loader_fd_free(lfd);
		return error;
	}
	*fpp = fp;
	return 0;

fail:
	vmm_loader_fd_free(lfd);
	return error;
}

static int
vmm_loader_open_object_fd(struct vm_object *object, vm_size_t size,
    struct file **fpp)
{
	struct vmm_loader_fd *lfd;
	uint32_t serial;

	if (object == NULL || size == 0)
		return EINVAL;

	lfd = kmalloc(sizeof(*lfd), M_TEMP, M_WAITOK | M_ZERO);
	lfd->object = object;
	lfd->size = round_page(size);
	serial = atomic_fetchadd_int(&vmm_loader_fd_serial, 1);
	lfd->dev = make_only_dev(&vmm_loader_object_fd_ops, serial, UID_ROOT,
	    GID_WHEEL, 0600, "vmmld%d", serial);
	if (lfd->dev == NULL) {
		kfree(lfd, M_TEMP);
		return ENXIO;
	}
	lfd->dev->si_drv1 = lfd;
	return vmm_loader_open_fd(lfd, fpp);
}

static int
vmm_loader_open_buffer_fd(void *data, vm_size_t size, struct file **fpp)
{
	struct vmm_loader_fd *lfd;
	uint32_t serial;

	if (data == NULL || size == 0)
		return EINVAL;

	lfd = kmalloc(sizeof(*lfd), M_TEMP, M_WAITOK | M_ZERO);
	lfd->data = data;
	lfd->size = round_page(size);
	lfd->buffer = 1;
	serial = atomic_fetchadd_int(&vmm_loader_fd_serial, 1);
	lfd->dev = make_only_dev(&vmm_loader_buffer_fd_ops, serial, UID_ROOT,
	    GID_WHEEL, 0600, "vmmld%d", serial);
	if (lfd->dev == NULL) {
		kfree(lfd, M_TEMP);
		return ENXIO;
	}
	lfd->dev->si_drv1 = lfd;
	return vmm_loader_open_fd(lfd, fpp);
}

static size_t
vmm_align8(size_t v)
{
	return (v + 7) & ~(size_t)7;
}

static int
vmm_gpa_inside(uint64_t mem_size, uint64_t start, uint64_t size)
{
	return size != 0 && start < mem_size && size <= mem_size - start;
}

static int
vmm_gpa_addr(uint64_t mem_size, uint64_t addr)
{
	return addr < mem_size;
}

static int
vmm_loader_make_args(const char *path, struct image_args *args)
{
	char *buf;
	size_t len;
	int error;

	bzero(args, sizeof(*args));
	buf = kmalloc(ARG_MAX + PATH_MAX, M_TEMP, M_WAITOK | M_ZERO);
	args->buf = buf;
	args->begin_argv = buf;
	args->endp = buf;
	args->space = ARG_MAX;
	args->fname = buf + ARG_MAX;

	error = copystr(path, args->fname, PATH_MAX, &len);
	if (error)
		goto fail;
	if (len > (size_t)args->space) {
		error = E2BIG;
		goto fail;
	}
	bcopy(args->fname, args->endp, len);
	args->endp += len;
	args->space -= (int)len;
	args->argc = 1;
	args->begin_envv = args->endp;
	args->envc = 0;
	return 0;

fail:
	kfree(args->buf, M_TEMP);
	args->buf = NULL;
	return error;
}

static void
vmm_loader_free_args(struct image_args *args)
{
	if (args->buf != NULL) {
		kfree(args->buf, M_TEMP);
		args->buf = NULL;
	}
}

static int
vmm_loader_install_fd(struct file *fp, int target_fd)
{
	int fd, error;

	error = kern_close(target_fd);
	if (error != 0 && error != EBADF)
		return error;
	error = fdalloc(curproc, target_fd, &fd);
	if (error)
		return error;
	if (fd != target_fd) {
		fsetfd(curproc->p_fd, NULL, fd);
		return EBUSY;
	}
	fsetfd(curproc->p_fd, fp, target_fd);
	return 0;
}

static int
vmm_loader_cancelled(struct vmm_loader_epoch *ep)
{
	return ep->cancel != NULL && ep->cancel(ep->cancel_arg);
}

static void
vmm_loader_child(void *arg, struct trapframe *frame)
{
	struct vmm_loader_epoch *ep = arg;
	struct nlookupdata nd;
	struct image_args args;
	int error;

	(void)frame;
	error = vmm_loader_install_fd(ep->mem_fp, 3);
	if (error == 0)
		error = vmm_loader_install_fd(ep->manifest_fp, 4);
	if (error == 0)
		error = vmm_loader_make_args(ep->loader_path, &args);
	if (error == 0) {
		error = nlookup_init(&nd, ep->loader_path, UIO_SYSSPACE,
		    NLC_FOLLOW);
		if (error == 0) {
			error = kern_execve(&nd, NULL, 0, &args);
			nlookup_done(&nd);
		}
		vmm_loader_free_args(&args);
	}

	if (error < 0)
		exit1(W_EXITCODE(127, SIGABRT));
	if (error != 0)
		exit1(W_EXITCODE(127, 0));
	/* kern_execve() succeeded; return through fork_trampoline to userland. */
}

static void
vmm_loader_set_proc_cred(struct proc *p, struct ucred *cred)
{
	struct lwp *lwp;
	struct ucred *old;

	old = p->p_ucred;
	p->p_ucred = crhold(cred);
	crfree(old);

	lwp = ONLY_LWP_IN_PROC(p);
	old = lwp->lwp_thread->td_ucred;
	lwp->lwp_thread->td_ucred = crhold(cred);
	crfree(old);
}

static int
vmm_loader_wait(struct vmm_loader_epoch *ep)
{
	struct __wrusage wrusage;
	struct proc *child;
	struct lwp *child_lwp;
	pid_t pid;
	int status = 0;
	int result = 0;
	int killed = 0;
	int error;

	error = fork1(curthread->td_lwp, RFFDG | RFPROC | RFPGLOCK, &child);
	if (error)
		return error;

	PHOLD(child);
	pid = child->p_pid;
	child_lwp = ONLY_LWP_IN_PROC(child);
	vmm_loader_set_proc_cred(child, ep->cred);
	cpu_set_fork_handler(child_lwp, vmm_loader_child, ep);
	start_forked_proc(curthread->td_lwp, child);
	PRELE(child);

	for (;;) {
		bzero(&wrusage, sizeof(wrusage));
		status = 0;
		result = 0;
		error = kern_wait(P_PID, pid, &status, WEXITED | WNOHANG,
		    &wrusage, NULL, &result);
		if (error)
			return error;
		if (result != 0)
			break;
		if (vmm_loader_cancelled(ep) && !killed) {
			(void)kern_kill(SIGKILL, pid, -1);
			killed = 1;
		}
		tsleep(ep->cancel_arg != NULL ? ep->cancel_arg : ep, 0,
		    "vmmld", hz / 20 + 1);
	}
	if (killed)
		return EINTR;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return ENOEXEC;
	return 0;
}

static int
vmm_loader_validate_vcpu(uint64_t mem_size,
    const struct vmm_x64_vcpu_state *vcpu)
{
	if (vcpu->vcpu_id != 0 || vcpu->runnable != 1)
		return EINVAL;
	if (!vmm_gpa_addr(mem_size, vcpu->gpr[VMM_GPR_RIP]))
		return EINVAL;
	if (!vmm_gpa_addr(mem_size, vcpu->gpr[VMM_GPR_RSP]))
		return EINVAL;
	if (!vmm_gpa_addr(mem_size, vcpu->cr[VMM_CR_CR3]))
		return EINVAL;
	if ((vcpu->cr[VMM_CR_CR3] & PAGE_MASK) != 0)
		return EINVAL;
	if (!vmm_gpa_addr(mem_size, vcpu->seg[VMM_SEG_GDTR].base))
		return EINVAL;
	if (vcpu->seg[VMM_SEG_IDTR].limit != 0 &&
	    !vmm_gpa_addr(mem_size, vcpu->seg[VMM_SEG_IDTR].base))
		return EINVAL;
	if (vcpu->intr_flags != 0)
		return EINVAL;
	return 0;
}

static int
vmm_loader_validate_ranges(uint64_t mem_size, const uint8_t *payload,
    uint32_t size)
{
	const struct vmm_gpa_range *range;
	uint32_t i, count;

	if (size == 0 || (size % sizeof(*range)) != 0)
		return EINVAL;
	range = (const struct vmm_gpa_range *)payload;
	count = size / sizeof(*range);
	for (i = 0; i < count; i++) {
		if (!vmm_gpa_inside(mem_size, range[i].start, range[i].size))
			return EINVAL;
	}
	return 0;
}

static int
vmm_loader_validate_manifest(uint64_t mem_size, const uint8_t *buf,
    size_t cap)
{
	struct vmm_manifest_header hdr;
	struct vmm_manifest_record rec;
	size_t off;
	uint32_t records = 0;
	int have_vcpu = 0;
	int have_range = 0;
	int error;

	if (cap < sizeof(hdr))
		return EINVAL;
	bcopy(buf, &hdr, sizeof(hdr));
	if (bcmp(hdr.magic, VMM_MANIFEST_MAGIC, sizeof(hdr.magic)) != 0 ||
	    hdr.abi_version != VMM_MANIFEST_ABI ||
	    hdr.arch != VMM_MANIFEST_ARCH_X64 ||
	    hdr.header_size != sizeof(hdr) ||
	    hdr.total_size < hdr.header_size ||
	    hdr.total_size > cap ||
	    hdr.mem_size != mem_size ||
	    hdr.flags != 0 ||
	    hdr.reserved != 0)
		return EINVAL;

	off = hdr.header_size;
	while (off < hdr.total_size) {
		const uint8_t *payload;
		size_t next;

		if (hdr.total_size - off < sizeof(rec)) {
			error = EINVAL;
			return error;
		}
		bcopy(buf + off, &rec, sizeof(rec));
		next = off + vmm_align8(sizeof(rec) + rec.size);
		if (next < off || next > hdr.total_size ||
		    off + sizeof(rec) + rec.size > hdr.total_size) {
			error = EINVAL;
			return error;
		}
		payload = buf + off + sizeof(rec);
		switch (rec.type) {
		case VMM_REC_X64_VCPU_STATE:
			if ((rec.flags & VMM_REC_F_MANDATORY) == 0 ||
			    rec.size != sizeof(struct vmm_x64_vcpu_state) ||
			    have_vcpu) {
				error = EINVAL;
				return error;
			}
			error = vmm_loader_validate_vcpu(mem_size,
			    (const struct vmm_x64_vcpu_state *)payload);
			if (error)
				return error;
			have_vcpu = 1;
			break;
		case VMM_REC_GPA_RANGE:
			if ((rec.flags & VMM_REC_F_MANDATORY) == 0) {
				error = EINVAL;
				return error;
			}
			error = vmm_loader_validate_ranges(mem_size, payload,
			    rec.size);
			if (error)
				return error;
			have_range = 1;
			break;
		default:
			if (rec.flags & VMM_REC_F_MANDATORY) {
				error = EINVAL;
				return error;
			}
			break;
		}
		records++;
		off = next;
	}
	return (records == hdr.record_count && have_vcpu && have_range) ?
	    0 : EINVAL;
}

int
vmm_loader_run(struct vmm_loader *loader, struct vmm_mem *mem,
    struct ucred *cred, vmm_loader_cancel_fn *cancel, void *cancel_arg)
{
	struct vmm_loader_epoch *ep;
	size_t path_len;
	int error;

	if (!vmm_mem_is_set(mem) || vmm_mem_object(mem) == NULL ||
	    !vmm_loader_is_set(loader) || cred == NULL)
		return EINVAL;

	ep = kmalloc(sizeof(*ep), M_TEMP, M_WAITOK | M_ZERO);
	ep->mem_size = mem->bytes;
	ep->cred = cred;
	ep->cancel = cancel;
	ep->cancel_arg = cancel_arg;
	path_len = vmm_loader_path(loader, ep->loader_path,
	    sizeof(ep->loader_path));
	if (path_len == 0) {
		error = EINVAL;
		goto out;
	}
	ep->loader_path[path_len] = '\0';

	if (vmm_loader_cancelled(ep)) {
		error = EINTR;
		goto out;
	}
	error = vmm_loader_open_object_fd(vmm_mem_object(mem),
	    (vm_size_t)ep->mem_size, &ep->mem_fp);
	if (error)
		goto out;
	ep->manifest_data = (void *)kmem_alloc(kernel_map, VMM_MANIFEST_SIZE,
	    VM_SUBSYS_MMAP);
	if (ep->manifest_data == NULL) {
		error = ENOMEM;
		goto out;
	}
	bzero(ep->manifest_data, VMM_MANIFEST_SIZE);
	error = vmm_loader_open_buffer_fd(ep->manifest_data, VMM_MANIFEST_SIZE,
	    &ep->manifest_fp);
	if (error)
		goto out;
	if (vmm_loader_cancelled(ep)) {
		error = EINTR;
		goto out;
	}
	error = vmm_loader_wait(ep);
	if (error)
		goto out;
	if (vmm_loader_cancelled(ep)) {
		error = EINTR;
		goto out;
	}
	error = vmm_loader_validate_manifest(ep->mem_size,
	    ep->manifest_data, VMM_MANIFEST_SIZE);

out:
	if (ep->manifest_fp != NULL)
		fp_close(ep->manifest_fp);
	if (ep->mem_fp != NULL)
		fp_close(ep->mem_fp);
	if (ep->manifest_data != NULL)
		kmem_free(kernel_map, (vm_offset_t)ep->manifest_data,
		    VMM_MANIFEST_SIZE);
	kfree(ep, M_TEMP);
	return error;
}
