#include <sys/param.h>
#include <sys/systm.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/refcount.h>
#include <sys/shmfd.h>
#include <sys/stat.h>
#include <sys/fnv_hash.h>
#include <sys/jail.h>
#include <sys/thread.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/vnode.h>
#include <sys/syslimits.h>
#include <sys/sysmsg.h>
#include <sys/sysent.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_page2.h>
#include <vm/vm_pager.h>
#include <vm/swap_pager.h>
#include <vm/vm_param.h>

#include <cpu/lwbuf.h>
#include <machine/limits.h>

struct shm_mapping {
	char			*sm_path;
	Fnv32_t			sm_fnv;
	struct prison		*sm_prison;
	struct posix_shmfd		*sm_shmfd;
	LIST_ENTRY(shm_mapping)	sm_link;
};

struct posix_shmfd {
	struct shmfd		pshmfd_base;
	int		shm_grow_on_write;
	char		shm_name[NAME_MAX + 1];
	volatile u_int	shm_refs;
	uid_t		shm_uid;
	gid_t		shm_gid;
	mode_t		shm_mode;
	ino_t		shm_ino;
	struct timespec	shm_atim;
	struct timespec	shm_mtim;
	struct timespec	shm_ctim;
};

static MALLOC_DEFINE(M_SHMFD, "shmfd", "anonymous shared memory files");
static LIST_HEAD(, shm_mapping) *shm_dictionary;
static struct lock posix_shmfd_lock;
static u_long shm_hash;
static volatile uint64_t shm_next_ino = 1;

#define SHM_HASH(fnv) (&shm_dictionary[(fnv) & shm_hash])

static int	posix_shmfd_read(struct file *, struct uio *, struct ucred *, int);
static int	posix_shmfd_write(struct file *, struct uio *, struct ucred *, int);
static int	posix_shmfd_stat(struct file *, struct stat *, struct ucred *);
static int	posix_shmfd_close(struct file *);
static int	posix_shmfd_seek(struct file *, off_t, int, off_t *);
static int	posix_shmfd_mmap(struct file *, vm_map_t, vm_offset_t *,
			vm_size_t, vm_prot_t, vm_prot_t, int, vm_ooffset_t,
			struct thread *);
static void	posix_shmfd_init(void *);
static void	posix_shmfd_set_metadata(struct posix_shmfd *, struct ucred *, mode_t);
static int	posix_shmfd_copyin_path(const char *, char **);
static struct posix_shmfd	*posix_shmfd_lookup_locked(const char *, Fnv32_t,
			struct prison *);
static int	posix_shmfd_access_locked(struct posix_shmfd *, struct ucred *, int);
static void	posix_shmfd_insert_locked(char *, Fnv32_t, struct prison *,
			struct posix_shmfd *);
static struct shm_mapping *posix_shmfd_remove_locked(const char *, Fnv32_t,
			struct prison *, struct ucred *, int *);
static void	posix_shmfd_mapping_free(struct shm_mapping *);
static struct posix_shmfd *posix_shmfd_hold(struct posix_shmfd *);
static void	posix_shmfd_drop(struct posix_shmfd *);
static int	posix_shmfd_zero_partial(vm_object_t, vm_pindex_t, int, int);
int		posix_shmfd_truncate(struct file *, off_t);

static __inline struct posix_shmfd *
posix_shmfd_from_file(struct file *fp)
{
	return (__containerof(fp->f_data, struct posix_shmfd, pshmfd_base));
}

struct fileops posix_shmfd_fileops = {
	.fo_read = posix_shmfd_read,
	.fo_write = posix_shmfd_write,
	.fo_ioctl = badfo_ioctl,
	.fo_kqfilter = badfo_kqfilter,
	.fo_stat = posix_shmfd_stat,
	.fo_close = posix_shmfd_close,
	.fo_shutdown = badfo_shutdown,
	.fo_seek = posix_shmfd_seek,
	.fo_mmap = posix_shmfd_mmap
};
static void
posix_shmfd_init(void *arg __unused)
{
	lockinit(&posix_shmfd_lock, "shmfd", 0, 0);
	shm_dictionary = hashinit(64, M_SHMFD, &shm_hash);
}
SYSINIT(shmfd, SI_SUB_PRE_DRIVERS, SI_ORDER_ANY, posix_shmfd_init, NULL);

static void
posix_shmfd_set_metadata(struct posix_shmfd *shmfd, struct ucred *cred, mode_t mode)
{
	struct timespec now;

	getnanotime(&now);
	shmfd->shm_uid = cred->cr_uid;
	shmfd->shm_gid = cred->cr_gid;
	shmfd->shm_mode = mode & ACCESSPERMS;
	shmfd->shm_ino = atomic_fetchadd_64(&shm_next_ino, 1);
	shmfd->shm_atim = now;
	shmfd->shm_mtim = now;
	shmfd->shm_ctim = now;
}

static int
posix_shmfd_copyin_path(const char *userpath, char **pathp)
{
	char *path;
	int error;

	path = kmalloc(NAME_MAX + 2, M_SHMFD, M_WAITOK | M_ZERO);
	error = copyinstr(userpath, path, NAME_MAX + 2, NULL);
	if (error == 0 && path[0] != '/')
		error = EINVAL;
	if (error != 0) {
		if (path != NULL)
			kfree(path, M_SHMFD);
		return (error);
	}
	*pathp = path;
	return (0);
}

static struct posix_shmfd *
posix_shmfd_lookup_locked(const char *path, Fnv32_t fnv, struct prison *prison)
{
	struct shm_mapping *map;

	KKASSERT(lockstatus(&posix_shmfd_lock, curthread));
	LIST_FOREACH(map, SHM_HASH(fnv), sm_link) {
		if (map->sm_fnv == fnv && map->sm_prison == prison &&
		    strcmp(map->sm_path, path) == 0)
			return (map->sm_shmfd);
	}
	return (NULL);
}

static int
posix_shmfd_access_locked(struct posix_shmfd *shmfd, struct ucred *cred, int flags)
{
	mode_t accmode;

	KKASSERT(lockstatus(&posix_shmfd_lock, curthread));
	accmode = 0;
	if (flags & FREAD)
		accmode |= VREAD;
	if (flags & FWRITE)
		accmode |= VWRITE;
	return (vaccess(VREG, shmfd->shm_mode, shmfd->shm_uid,
	    shmfd->shm_gid, accmode, cred));
}

static void
posix_shmfd_insert_locked(char *path, Fnv32_t fnv, struct prison *prison,
    struct posix_shmfd *shmfd)
{
	struct shm_mapping *map;

	KKASSERT(lockstatus(&posix_shmfd_lock, curthread));
	map = kmalloc(sizeof(*map), M_SHMFD, M_WAITOK | M_ZERO);
	map->sm_path = path;
	map->sm_fnv = fnv;
	map->sm_prison = prison;
	if (prison != NULL)
		prison_hold(prison);
	map->sm_shmfd = posix_shmfd_hold(shmfd);
	LIST_INSERT_HEAD(SHM_HASH(fnv), map, sm_link);
}

static struct shm_mapping *
posix_shmfd_remove_locked(const char *path, Fnv32_t fnv, struct prison *prison,
    struct ucred *cred, int *errorp)
{
	struct shm_mapping *map;

	KKASSERT(lockstatus(&posix_shmfd_lock, curthread));
	LIST_FOREACH(map, SHM_HASH(fnv), sm_link) {
		if (map->sm_fnv != fnv || map->sm_prison != prison ||
		    strcmp(map->sm_path, path) != 0)
			continue;
		*errorp = posix_shmfd_access_locked(map->sm_shmfd, cred, FWRITE);
		if (*errorp != 0)
			return (NULL);
		LIST_REMOVE(map, sm_link);
		return (map);
	}
	*errorp = ENOENT;
	return (NULL);
}

static void
posix_shmfd_mapping_free(struct shm_mapping *map)
{
	if (map == NULL)
		return;
	posix_shmfd_drop(map->sm_shmfd);
	if (map->sm_prison != NULL)
		prison_free(map->sm_prison);
	kfree(map->sm_path, M_SHMFD);
	kfree(map, M_SHMFD);
}

static struct posix_shmfd *
posix_shmfd_alloc(off_t size)
{
	struct posix_shmfd *shmfd;
	vm_object_t object;

	if (size < 0 || (uintmax_t)size > (uintmax_t)OFF_MAX - PAGE_MASK)
		return (NULL);
	object = swap_pager_alloc(NULL, size, VM_PROT_DEFAULT, 0);
	if (object == NULL)
		return (NULL);
	shmfd = kmalloc(sizeof(*shmfd), M_SHMFD, M_WAITOK | M_ZERO);
	if (shmfd_init(&shmfd->pshmfd_base, object, size) != 0) {
		vm_object_deallocate(object);
		kfree(shmfd, M_SHMFD);
		return (NULL);
	}
	vm_object_deallocate(object);
	refcount_init(&shmfd->shm_refs, 1);
	return (shmfd);
}

static struct posix_shmfd *
posix_shmfd_hold(struct posix_shmfd *shmfd)
{
	if (shmfd != NULL)
		refcount_acquire(&shmfd->shm_refs);
	return (shmfd);
}

static void
posix_shmfd_drop(struct posix_shmfd *shmfd)
{
	if (shmfd == NULL || !refcount_release(&shmfd->shm_refs))
		return;
	shmfd_close(&shmfd->pshmfd_base);
	kfree(shmfd, M_SHMFD);
}

/*
 * The caller holds object exclusively.  Preserve that state while paging in
 * a partially retained tail page, then zero the discarded bytes.
 */
static int
posix_shmfd_zero_partial(vm_object_t object, vm_pindex_t pindex, int base,
    int end)
{
	vm_page_t m;
	int rv;

	KASSERT(base >= 0 && end >= base && end <= PAGE_SIZE,
	    ("posix_shmfd_zero_partial: invalid range"));
	m = vm_page_lookup_busy_wait(object, pindex, TRUE, "shmtrunc");
	if (m == NULL) {
		if (!vm_pager_has_page(object, pindex))
			return (0);
		vm_object_drop(object);
		m = vm_page_grab(object, pindex,
		    VM_ALLOC_NORMAL | VM_ALLOC_RETRY);
		if (m == NULL) {
			vm_object_hold(object);
			return (ENOMEM);
		}
		rv = vm_pager_get_page(object, pindex, &m, 1);
		vm_object_hold(object);
		if (rv == VM_PAGER_FAIL) {
			pmap_zero_page(VM_PAGE_TO_PHYS(m));
			m->valid = VM_PAGE_BITS_ALL;
		} else if (rv != VM_PAGER_OK) {
			vm_page_wakeup(m);
			return (EIO);
		}
	} else if (m->valid != VM_PAGE_BITS_ALL) {
		vm_object_drop(object);
		rv = vm_pager_get_page(object, pindex, &m, 1);
		vm_object_hold(object);
		if (rv == VM_PAGER_FAIL) {
			pmap_zero_page(VM_PAGE_TO_PHYS(m));
			m->valid = VM_PAGE_BITS_ALL;
		} else if (rv != VM_PAGER_OK) {
			vm_page_wakeup(m);
			return (EIO);
		}
	}
	pmap_zero_page_area(VM_PAGE_TO_PHYS(m), base, end - base);
	vm_page_set_validdirty(m, base, end - base);
	vm_page_wakeup(m);
	return (0);
}

static int
posix_shmfd_uiomove_page(vm_object_t object, size_t len, struct uio *uio)
{
	vm_page_t m;
	vm_pindex_t pindex;
	vm_offset_t offset;
	ssize_t oldresid;
	int error;
	int rv;

	pindex = OFF_TO_IDX(uio->uio_offset);
	offset = uio->uio_offset & PAGE_MASK;
	len = MIN(len, PAGE_SIZE - offset);
	oldresid = uio->uio_resid;
	vm_object_hold_shared(object);
	if ((object->flags & OBJ_DEAD) || object->type == OBJT_DEAD) {
		vm_object_drop(object);
		return (EIO);
	}
	vm_object_drop(object);
	m = vm_page_grab(object, pindex, VM_ALLOC_NORMAL | VM_ALLOC_RETRY);
	if (m == NULL)
		return (ENOMEM);
	if (m->valid != VM_PAGE_BITS_ALL) {
		/*
		 * Keep the object lock order consistent with vm_fault: the
		 * page is busy before the pager is entered, and the pager may
		 * acquire the object token recursively.  Revoke takes this
		 * token before waiting for busy pages, so dropping it here
		 * would allow revoke and page-in to wait on each other.
		 * The lock covers only page-in; uiomove below remains concurrent.
		 */
		vm_object_hold(object);
		if ((object->flags & OBJ_DEAD) || object->type == OBJT_DEAD) {
			vm_object_drop(object);
			vm_page_wakeup(m);
			return (EIO);
		}
		rv = vm_pager_get_page(object, pindex, &m, 1);
		vm_object_drop(object);
		if (rv == VM_PAGER_FAIL) {
			pmap_zero_page(VM_PAGE_TO_PHYS(m));
			m->valid = VM_PAGE_BITS_ALL;
		} else if (rv != VM_PAGER_OK) {
			vm_page_wakeup(m);
			return (rv == VM_PAGER_AGAIN ? ENOSPC : EIO);
		}
	}

	error = uiomove_fromphys(&m, offset, len, uio);
	if (uio->uio_rw == UIO_WRITE && uio->uio_resid != oldresid)
		vm_page_dirty(m);
	vm_page_activate(m);
	vm_page_wakeup(m);
	return (error);
}

/*
 * The caller holds shm_control shared or exclusive and the object token.
 * Recheck size under the token: another shared writer may already have grown
 * it.  This path must not sleep between the size comparison and publication.
 * No page allocation or user copy is done here.
 */
static int
posix_shmfd_grow_locked(struct posix_shmfd *shmfd, off_t length)
{
	struct timespec now;

	if ((uintmax_t)length > (uintmax_t)OFF_MAX - PAGE_MASK)
		return (EFBIG);
	if ((uint64_t)length > shmfd->pshmfd_base.shmfd_size) {
		shmfd->pshmfd_base.shmfd_object->size = OFF_TO_IDX(length + PAGE_MASK);
		shmfd->pshmfd_base.shmfd_size = length;
		getnanotime(&now);
		shmfd->shm_mtim = now;
		shmfd->shm_ctim = now;
	}
	return (0);
}

static int
posix_shmfd_uiomove(struct posix_shmfd *shmfd, struct uio *uio)
{
	vm_object_t object;
	ssize_t oldresid;
	size_t len;
	off_t size;
	int admitted;
	int error;

	/*
	 * The control lock serializes size publication, not the transfer.
	 * An admitted I/O holds the backing stable through its final page use;
	 * distinct reads and writes remain concurrent after the object token drops.
	 */
	admitted = 0;
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_SHARED);
	object = shmfd->pshmfd_base.shmfd_object;
	vm_object_hold(object);
	if ((object->flags & OBJ_DEAD) || object->type == OBJT_DEAD ||
	    !vm_object_access_enter(object)) {
		error = EIO;
		goto out_object;
	}
	admitted = 1;
	if (object->type != OBJT_SWAP && object->type != OBJT_DEFAULT) {
		error = EOPNOTSUPP;
		goto out_object;
	}
	if (uio->uio_offset < 0) {
		error = EINVAL;
		goto out_object;
	}
	error = 0;
	if (uio->uio_rw == UIO_WRITE && shmfd->shm_grow_on_write) {
		if (uio->uio_resid > OFF_MAX - uio->uio_offset) {
			error = EFBIG;
			goto out_object;
		}
		error = posix_shmfd_grow_locked(shmfd,
		    uio->uio_offset + uio->uio_resid);
		if (error != 0)
			goto out_object;
	}
	size = (off_t)shmfd->pshmfd_base.shmfd_size;
	vm_object_drop(object);
	while (uio->uio_resid != 0 && uio->uio_offset < size) {
		oldresid = uio->uio_resid;
		len = MIN((off_t)uio->uio_resid, size - uio->uio_offset);
		if (len == 0)
			break;
		error = posix_shmfd_uiomove_page(object, len, uio);
		if (error != 0 || uio->uio_resid == oldresid)
			break;
	}
	goto out;
out_object:
	vm_object_drop(object);
out:
	if (admitted)
		vm_object_access_exit(object);
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_RELEASE);
	return (error);
}

static int
posix_shmfd_read(struct file *fp, struct uio *uio, struct ucred *cred __unused,
	    int flags)
{
	int error;

	if ((flags & O_FOFFSET) == 0)
		uio->uio_offset = fp->f_offset;
	error = posix_shmfd_uiomove(posix_shmfd_from_file(fp), uio);
	if ((flags & O_FOFFSET) == 0)
		fp->f_offset = uio->uio_offset;
	return (error);
}

static int
posix_shmfd_write(struct file *fp, struct uio *uio, struct ucred *cred __unused,
	    int flags)
{
	int error;

	if ((flags & O_FOFFSET) == 0)
		uio->uio_offset = fp->f_offset;
	error = posix_shmfd_uiomove(posix_shmfd_from_file(fp), uio);
	if ((flags & O_FOFFSET) == 0)
		fp->f_offset = uio->uio_offset;
	return (error);
}

static int
posix_shmfd_stat(struct file *fp, struct stat *sb, struct ucred *cred __unused)
{
	struct posix_shmfd *shmfd;

	shmfd = posix_shmfd_from_file(fp);
	lockmgr(&posix_shmfd_lock, LK_SHARED | LK_RETRY);
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_SHARED);
	vm_object_hold_shared(shmfd->pshmfd_base.shmfd_object);
	bzero(sb, sizeof(*sb));
	sb->st_size = (off_t)shmfd->pshmfd_base.shmfd_size;
	sb->st_blksize = PAGE_SIZE;
	sb->st_blocks = howmany(sb->st_size, DEV_BSIZE);
	sb->st_nlink = 1;
	sb->st_mode = S_IFREG | shmfd->shm_mode;
	sb->st_uid = shmfd->shm_uid;
	sb->st_gid = shmfd->shm_gid;
	sb->st_ino = shmfd->shm_ino;
	sb->st_atim = shmfd->shm_atim;
	sb->st_mtim = shmfd->shm_mtim;
	sb->st_ctim = shmfd->shm_ctim;
	vm_object_drop(shmfd->pshmfd_base.shmfd_object);
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_RELEASE);
	lockmgr(&posix_shmfd_lock, LK_RELEASE);
	return (0);
}

static int
posix_shmfd_seek(struct file *fp, off_t offset, int whence, off_t *res)
{
	struct posix_shmfd *shmfd;
	off_t base;
	off_t new_offset;
	int error;

	shmfd = posix_shmfd_from_file(fp);
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_SHARED);
	switch (whence) {
	case L_SET:
		base = 0;
		break;
	case L_INCR:
		base = fp->f_offset;
		break;
	case L_XTND:
		base = (off_t)shmfd->pshmfd_base.shmfd_size;
		break;
	default:
		error = EINVAL;
		goto out;
	}
	if (offset > 0 && base > OFF_MAX - offset) {
		error = EOVERFLOW;
		goto out;
	}
	if (offset < 0 && base < OFF_MIN - offset) {
		error = EOVERFLOW;
		goto out;
	}
	new_offset = base + offset;
	if (new_offset < 0) {
		error = EINVAL;
		goto out;
	}
	fp->f_offset = new_offset;
	*res = new_offset;
	error = 0;
out:
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_RELEASE);
	return (error);
}

static int
posix_shmfd_mmap(struct file *fp, vm_map_t map, vm_offset_t *addr,
    vm_size_t size, vm_prot_t prot, vm_prot_t maxprot_limit, int flags,
    vm_ooffset_t foff, struct thread *td)
{
	struct posix_shmfd *shmfd;
	int error;

	shmfd = posix_shmfd_from_file(fp);
	error = shmfd_mmap(fp, map, addr, size, prot, maxprot_limit, flags,
	    foff, td);
	if (error == 0) {
		lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_SHARED);
		getnanotime(&shmfd->shm_atim);
		lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_RELEASE);
	}
	return (error);
}

static int
posix_shmfd_truncate_locked(struct posix_shmfd *shmfd, off_t length)
{
	vm_object_t object;
	vm_pindex_t new_size;
	vm_pindex_t old_size;
	int error;

	if (length < 0)
		return (EINVAL);
	if ((uintmax_t)length > (uintmax_t)OFF_MAX - PAGE_MASK)
		return (EFBIG);
	object = shmfd->pshmfd_base.shmfd_object;
	if ((object->flags & OBJ_DEAD) || object->type == OBJT_DEAD)
		return (EINVAL);
	if (object->type != OBJT_SWAP && object->type != OBJT_DEFAULT)
		return (EOPNOTSUPP);
	if ((uint64_t)length >= shmfd->pshmfd_base.shmfd_size)
		return (posix_shmfd_grow_locked(shmfd, length));
	new_size = OFF_TO_IDX(length + PAGE_MASK);
	old_size = object->size;
	if ((uint64_t)length < shmfd->pshmfd_base.shmfd_size) {
		if ((length & PAGE_MASK) != 0) {
			error = posix_shmfd_zero_partial(object, OFF_TO_IDX(length),
			    length & PAGE_MASK, PAGE_SIZE);
			if (error != 0)
				return (error);
			swap_pager_freespace(object, OFF_TO_IDX(length), 1);
		}
		if (new_size < old_size)
			vm_object_page_remove(object, new_size, old_size, FALSE);
	}
	object->size = new_size;
	shmfd->pshmfd_base.shmfd_size = length;
	getnanotime(&shmfd->shm_ctim);
	shmfd->shm_mtim = shmfd->shm_ctim;
	return (0);
}

static int
posix_shmfd_dotruncate(struct posix_shmfd *shmfd, off_t length)
{
	int error;

	if (shmfd == NULL)
		return (EINVAL);
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_EXCLUSIVE);
	vm_object_hold(shmfd->pshmfd_base.shmfd_object);
	error = posix_shmfd_truncate_locked(shmfd, length);
	vm_object_drop(shmfd->pshmfd_base.shmfd_object);
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_RELEASE);
	return (error);
}

int
posix_shmfd_truncate(struct file *fp, off_t length)
{
	struct posix_shmfd *shmfd;
	int error;

	if (fp->f_type != DTYPE_SHM)
		return (EINVAL);
	shmfd = posix_shmfd_from_file(fp);
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_EXCLUSIVE);
	vm_object_hold(shmfd->pshmfd_base.shmfd_object);
	error = posix_shmfd_truncate_locked(shmfd, length);
	vm_object_drop(shmfd->pshmfd_base.shmfd_object);
	lockmgr(&shmfd->pshmfd_base.shmfd_lock, LK_RELEASE);
	return (error);
}

static int
posix_shmfd_close(struct file *fp)
{
	struct posix_shmfd *shmfd;

	shmfd = posix_shmfd_from_file(fp);
	fp->f_data = NULL;
	fp->f_ops = &badfileops;
	posix_shmfd_drop(shmfd);
	return (0);
}

int
sys_memfd_create(struct sysmsg *sysmsg, const struct memfd_create_args *uap)
{
	struct file *fp;
	struct filedesc *fdp;
	struct posix_shmfd *shmfd;
	struct thread *td;
	char name[NAME_MAX + 1];
	int fd;
	int error;

	error = copyinstr(uap->name, name, sizeof(name), NULL);
	if (error == ENAMETOOLONG)
		error = EINVAL;
	if (error != 0)
		return (error);
	if ((uap->flags & ~(MFD_CLOEXEC)) != 0)
		return ((uap->flags & (MFD_ALLOW_SEALING | MFD_HUGETLB)) != 0 ?
		    EOPNOTSUPP : EINVAL);
	td = curthread;
	error = falloc(td->td_lwp, &fp, &fd);
	if (error != 0)
		return (error);
	shmfd = posix_shmfd_alloc(0);
	if (shmfd == NULL) {
		fsetfd(td->td_proc->p_fd, NULL, fd);
		fp->f_ops = &badfileops;
		fdrop(fp);
		return (ENOMEM);
	}
	shmfd->shm_grow_on_write = 1;
	bcopy(name, shmfd->shm_name, sizeof(shmfd->shm_name));
	posix_shmfd_set_metadata(shmfd, td->td_ucred, 0600);
	fp->f_type = DTYPE_SHM;
	fp->f_flag = FREAD | FWRITE;
	fp->f_ops = &posix_shmfd_fileops;
	fp->f_data = &shmfd->pshmfd_base;
	fdp = td->td_proc->p_fd;
	fsetfd_flags(fdp, fp, fd,
	    (uap->flags & MFD_CLOEXEC) ? UF_EXCLOSE : 0);
	sysmsg->sysmsg_result = fd;
	fdrop(fp);
	return (0);
}

int
sys_shm_open2(struct sysmsg *sysmsg, const struct shm_open2_args *uap)
{
	struct filedesc *fdp;
	struct file *fp;
	struct posix_shmfd *shmfd;
	struct thread *td;
	char *path;
	char name[NAME_MAX + 1];
	Fnv32_t fnv;
	mode_t cmode;
	int flags;
	int fd;
	int error;
	int file_ref;

	if (uap->shmflags != 0)
		return (EOPNOTSUPP);
	if ((uap->flags & O_ACCMODE) != O_RDONLY &&
	    (uap->flags & O_ACCMODE) != O_RDWR)
		return (EINVAL);
	if ((uap->flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC |
	    O_CLOEXEC | O_CLOFORK)) != 0)
		return (EINVAL);

	td = curthread;
	fdp = td->td_proc->p_fd;
	error = falloc(td->td_lwp, &fp, &fd);
	if (error != 0)
		return (error);
	path = NULL;
	shmfd = NULL;
	file_ref = 0;
	flags = FFLAGS(uap->flags & O_ACCMODE) & (FREAD | FWRITE);

	if (uap->path == SHM_ANON) {
		bzero(name, sizeof(name));
		if (uap->name != NULL) {
			error = copyinstr(uap->name, name, sizeof(name), NULL);
			if (error == ENAMETOOLONG)
				error = EINVAL;
			if (error != 0)
				goto fail;
		}
		if ((uap->flags & O_ACCMODE) == O_RDONLY) {
			error = EINVAL;
			goto fail;
		}
		shmfd = posix_shmfd_alloc(0);
		if (shmfd == NULL) {
			error = ENOMEM;
			goto fail;
		}
		posix_shmfd_set_metadata(shmfd, td->td_ucred, 0600);
		bcopy(name, shmfd->shm_name, sizeof(shmfd->shm_name));
		file_ref = 1;
	} else {
		error = posix_shmfd_copyin_path(uap->path, &path);
		if (error != 0)
			goto fail;
		fnv = fnv_32_buf(path, strlen(path), FNV1_32_INIT);
		cmode = (uap->mode & ~fdp->fd_cmask) & ACCESSPERMS;
		lockmgr(&posix_shmfd_lock, LK_EXCLUSIVE | LK_RETRY);
		shmfd = posix_shmfd_lookup_locked(path, fnv, td->td_ucred->cr_prison);
		if (shmfd == NULL) {
			if ((uap->flags & O_CREAT) == 0) {
				error = ENOENT;
				lockmgr(&posix_shmfd_lock, LK_RELEASE);
				goto fail;
			}
			shmfd = posix_shmfd_alloc(0);
			if (shmfd == NULL) {
				error = ENOMEM;
				lockmgr(&posix_shmfd_lock, LK_RELEASE);
				goto fail;
			}
			posix_shmfd_set_metadata(shmfd, td->td_ucred, cmode);
			posix_shmfd_insert_locked(path, fnv, td->td_ucred->cr_prison,
			    shmfd);
			path = NULL;
			file_ref = 1;
		} else {
			if ((uap->flags & (O_CREAT | O_EXCL)) ==
			    (O_CREAT | O_EXCL)) {
				error = EEXIST;
			} else {
				error = posix_shmfd_access_locked(shmfd, td->td_ucred,
				    flags);
			}
			if (error == 0 && (uap->flags & O_TRUNC) != 0 &&
			    (uap->flags & O_ACCMODE) == O_RDWR)
				error = posix_shmfd_dotruncate(shmfd, 0);
			if (error == 0)
				posix_shmfd_hold(shmfd);
			if (error == 0)
				file_ref = 1;
		}
		lockmgr(&posix_shmfd_lock, LK_RELEASE);
		if (error != 0)
			goto fail;
	}

	fp->f_type = DTYPE_SHM;
	fp->f_flag = flags;
	fp->f_ops = &posix_shmfd_fileops;
	fp->f_data = &shmfd->pshmfd_base;
	fsetfd_flags(fdp, fp, fd,
	    ((uap->flags & O_CLOEXEC) ? UF_EXCLOSE : 0) |
	    ((uap->flags & O_CLOFORK) ? UF_FOCLOSE : 0));
	sysmsg->sysmsg_result = fd;
	fdrop(fp);
	if (path != NULL)
		kfree(path, M_SHMFD);
	return (0);

fail:
	if (shmfd != NULL && file_ref)
		posix_shmfd_drop(shmfd);
	fsetfd(fdp, NULL, fd);
	fp->f_ops = &badfileops;
	fdrop(fp);
	if (path != NULL)
		kfree(path, M_SHMFD);
	return (error);
}

int
sys_shm_unlink(struct sysmsg *sysmsg __unused,
    const struct shm_unlink_args *uap)
{
	struct shm_mapping *map;
	struct thread *td;
	char *path;
	Fnv32_t fnv;
	int error;

	td = curthread;
	error = posix_shmfd_copyin_path(uap->path, &path);
	if (error != 0)
		return (error);
	fnv = fnv_32_buf(path, strlen(path), FNV1_32_INIT);
	lockmgr(&posix_shmfd_lock, LK_EXCLUSIVE | LK_RETRY);
	map = posix_shmfd_remove_locked(path, fnv, td->td_ucred->cr_prison,
	    td->td_ucred, &error);
	lockmgr(&posix_shmfd_lock, LK_RELEASE);
	if (map != NULL)
		posix_shmfd_mapping_free(map);
	if (path != NULL)
		kfree(path, M_SHMFD);
	return (error);
}
