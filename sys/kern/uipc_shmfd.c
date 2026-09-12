/*
 * Copyright (c) 2026 The DragonFly Project.  All rights reserved.
 *
 * Shared-memory file mapping facade.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/file.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/shmfd.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_pager.h>

static int shmfd_map_object(vm_map_t, vm_offset_t *, vm_size_t, vm_prot_t,
	vm_prot_t, int, vm_object_t, vm_ooffset_t);

/*
 * Initialize a caller-owned shmfd and retain one reference to object.
 * The caller keeps the reference supplied to this function and must arrange
 * for shmfd_close() before it releases the enclosing export object.
 */
int
shmfd_init(struct shmfd *shmfd, vm_object_t object, off_t size)
{
	vm_pindex_t psize;
	int error;

	if (shmfd == NULL || object == NULL || size < 0 ||
	    (uintmax_t)size > (uintmax_t)OFF_MAX - PAGE_MASK) {
		return (EINVAL);
	}
	psize = OFF_TO_IDX((vm_ooffset_t)size + PAGE_MASK);
	vm_object_hold_shared(object);
	if ((object->flags & OBJ_DEAD) || object->type == OBJT_DEAD ||
	    psize > object->size) {
		error = EINVAL;
	} else {
		vm_object_reference_quick(object);
		error = 0;
	}
	vm_object_drop(object);
	if (error != 0)
		return (error);

	bzero(shmfd, sizeof(*shmfd));
	lockinit(&shmfd->shmfd_lock, "shmfd", 0, 0);
	shmfd->shmfd_object = object;
	shmfd->shmfd_size = size;
	return (0);
}

/*
 * Release the base facade.  The enclosing object and fileops belong to the
 * caller.  It does not revoke mappings or otherwise change pager state.
 */
void
shmfd_close(struct shmfd *shmfd)
{
	vm_object_t object;

	if (shmfd == NULL)
		return;
	lockmgr(&shmfd->shmfd_lock, LK_EXCLUSIVE);
	object = shmfd->shmfd_object;
	shmfd->shmfd_object = NULL;
	shmfd->shmfd_size = 0;
	lockmgr(&shmfd->shmfd_lock, LK_RELEASE);
	if (object != NULL) {
		vm_object_deallocate(object);
	}
	lockuninit(&shmfd->shmfd_lock);
	bzero(shmfd, sizeof(*shmfd));
}

/*
 * Construct a normal user VM mapping of the object.  This intentionally does
 * not constrain the pager type; the caller's choice of VM object determines
 * the backing semantics.  OBJT_DEVICE revocation excludes MAP_VPAGETABLE,
 * whose page-table ownership cannot be invalidated through the ordinary
 * object backing-list path.
 */
int
shmfd_mmap(struct file *fp, vm_map_t map, vm_offset_t *addr, vm_size_t size,
	vm_prot_t prot, vm_prot_t maxprot_limit, int flags, vm_ooffset_t foff,
	struct thread *td __unused)
{
	struct shmfd *shmfd;
	vm_object_t object;
	vm_prot_t maxprot;
	uint64_t map_size;
	int error;

	if (fp == NULL || fp->f_type != DTYPE_SHM || fp->f_data == NULL ||
	    ((flags & MAP_SHARED) && (flags & (MAP_PRIVATE | MAP_COPY))) ||
	    (flags & (MAP_STACK | MAP_VPAGETABLE)) || foff < 0 ||
	    size == 0 || size > OFF_MAX || foff > OFF_MAX - size) {
		return (EINVAL);
	}
	shmfd = fp->f_data;
	lockmgr(&shmfd->shmfd_lock, LK_SHARED);
	object = shmfd->shmfd_object;
	if (object == NULL) {
		lockmgr(&shmfd->shmfd_lock, LK_RELEASE);
		return (EINVAL);
	}
	vm_object_hold_shared(object);
	if ((object->flags & OBJ_DEAD) || object->type == OBJT_DEAD ||
	    (object->access_state & VM_OBJECT_ACCESS_CLOSED)) {
		vm_object_drop(object);
		lockmgr(&shmfd->shmfd_lock, LK_RELEASE);
		return (EINVAL);
	}
	/*
	 * POSIX shared memory has the ordinary swap-object semantics: a view
	 * may precede a later ftruncate(2), and faults beyond the current EOF
	 * fail until the object is grown.  Other caller-owned pager facades
	 * remain bounded by their declared size.
	 */
	if (object->type != OBJT_SWAP) {
		map_size = (uint64_t)shmfd->shmfd_size;
		if (map_size > UINT64_MAX - PAGE_MASK) {
			vm_object_drop(object);
			lockmgr(&shmfd->shmfd_lock, LK_RELEASE);
			return (EINVAL);
		}
		map_size = (map_size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
		if ((uint64_t)foff > map_size ||
		    size > map_size - (uint64_t)foff) {
			vm_object_drop(object);
			lockmgr(&shmfd->shmfd_lock, LK_RELEASE);
			return (EINVAL);
		}
	}
	vm_object_drop(object);

	maxprot = VM_PROT_NONE;
	if (fp->f_flag & FREAD)
		maxprot |= VM_PROT_READ | VM_PROT_EXECUTE;
	if ((flags & MAP_SHARED) == 0 || (fp->f_flag & FWRITE))
		maxprot |= VM_PROT_WRITE;
	maxprot &= maxprot_limit;
	if ((prot & maxprot) != prot) {
		lockmgr(&shmfd->shmfd_lock, LK_RELEASE);
		return (EACCES);
	}
	error = shmfd_map_object(map, addr, size, prot, maxprot, flags, object,
	    foff);
	lockmgr(&shmfd->shmfd_lock, LK_RELEASE);
	return (error);
}

/*
 * Irreversibly detach an OBJT_DEVICE facade from its provider.
 *
 * The caller remains responsible for fdrevoke() and for the enclosing
 * allocation's lifetime.  Admission closes before PTE removal, so faults
 * started before the close drain before the device pager is destroyed and
 * all later faults fail without entering provider code.
 */
int
shmfd_revoke(struct shmfd *shmfd)
{
	vm_object_t object;
	int error;

	if (shmfd == NULL)
		return (EINVAL);
	lockmgr(&shmfd->shmfd_lock, LK_EXCLUSIVE);
	object = shmfd->shmfd_object;
	if (object == NULL) {
		error = EINVAL;
		goto done;
	}
	vm_object_hold_shared(object);
	if ((object->flags & OBJ_DEAD) || object->type == OBJT_DEAD)
		error = EALREADY;
	else if (object->type != OBJT_DEVICE)
		error = EOPNOTSUPP;
	else
		error = 0;
	vm_object_drop(object);
	if (error != 0)
		goto done;

	if (!vm_object_access_close(object)) {
		error = EALREADY;
		goto done;
	}
	vm_object_access_wait(object, "shmrev");
	vm_object_pip_wait(object, "shmrev");
	vm_object_page_remove(object, 0, 0, FALSE);
	vm_object_pip_wait(object, "shmrev");

	/*
	 * Deallocate as DEVICE while the provider callback and global pager-list
	 * ownership are still valid.  The terminal type then prevents the final
	 * VM-object teardown from re-entering the provider after KLD unload.
	 */
	vm_object_hold(object);
	KASSERT(object->type == OBJT_DEVICE,
	    ("DEVICE shmfd changed type during revoke"));
	vm_pager_deallocate(object);
	object->handle = NULL;
	object->type = OBJT_DEAD;
	vm_object_drop(object);
	done:
	lockmgr(&shmfd->shmfd_lock, LK_RELEASE);
	return (error);
}

static int
shmfd_map_object(vm_map_t map, vm_offset_t *addr, vm_size_t size,
	vm_prot_t prot, vm_prot_t maxprot, int flags, vm_object_t object,
	vm_ooffset_t foff)
{
	vm_size_t align;
	boolean_t fitit;
	int cow;
	int rv;

	if ((foff & PAGE_MASK) != 0)
		return (EINVAL);
	if (flags & MAP_SIZEALIGN) {
		align = size;
		if ((align ^ (align - 1)) != (align << 1) - 1)
			return (EINVAL);
	} else if ((flags & (MAP_FIXED | MAP_TRYFIXED)) == 0 &&
	    ((size & SEG_MASK) == 0 || size > SEG_SIZE * 16)) {
		align = SEG_SIZE;
	} else {
		align = PAGE_SIZE;
	}
	if ((flags & (MAP_FIXED | MAP_TRYFIXED)) == 0) {
		fitit = TRUE;
		*addr = round_page(*addr);
	} else {
		if (*addr != trunc_page(*addr) || *addr + size < *addr)
			return (EINVAL);
		fitit = FALSE;
		if ((flags & MAP_TRYFIXED) == 0)
			vm_map_remove(map, *addr, *addr + size);
	}

	cow = COWF_PREFAULT_PARTIAL;
	if ((flags & MAP_SHARED) == 0)
		cow |= COWF_COPY_ON_WRITE;
	if (flags & MAP_NOSYNC)
		cow |= COWF_DISABLE_SYNCER;
	if (flags & MAP_NOCORE)
		cow |= COWF_DISABLE_COREDUMP;
	if (flags & MAP_32BIT)
		cow |= COWF_32BIT;
	vm_object_reference_quick(object);
	rv = vm_map_find(map, object, NULL, foff, addr, size, align, fitit,
	    VM_MAPTYPE_NORMAL, VM_SUBSYS_MMAP, prot, maxprot, cow);
	if (rv != KERN_SUCCESS) {
		vm_object_deallocate(object);
		return (vm_mmap_to_errno(rv));
	}
	if (flags & (MAP_SHARED | MAP_INHERIT)) {
		rv = vm_map_inherit(map, *addr, *addr + size, VM_INHERIT_SHARE);
		if (rv != KERN_SUCCESS) {
			vm_map_remove(map, *addr, *addr + size);
			return (vm_mmap_to_errno(rv));
		}
	}
	if (map->flags & MAP_WIREFUTURE)
		vm_map_user_wiring(map, *addr, *addr + size, FALSE);
	return (0);
}
