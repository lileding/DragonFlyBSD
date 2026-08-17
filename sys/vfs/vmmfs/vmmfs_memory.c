/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs machine vCPU declaration node.
 */
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_pager.h>

#include "vmmfs.h"

#define VMMFS_MEMORY_MODE 0644

static int vmmfs_memory_access(struct vop_access_args *);
static int vmmfs_memory_getattr(struct vop_getattr_args *);
static int vmmfs_memory_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_memory_open(struct vop_open_args *);
static int vmmfs_memory_read(struct vop_read_args *);
static int vmmfs_memory_setattr(struct vop_setattr_args *);
static int vmmfs_memory_write(struct vop_write_args *);
static int vmmfs_memory_reclaim(struct vop_reclaim_args *);
static int vmmfs_memory_map_object(struct vmspace *, struct vm_object *,
	uint64_t);
static void vmmfs_memory_object_reference(struct vm_object *);

struct vop_ops vmmfs_memory_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_memory_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_memory_getattr,
	.vop_getattr_lite = vmmfs_memory_getattr_lite,
	.vop_open = vmmfs_memory_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_memory_read,
	.vop_reclaim = vmmfs_memory_reclaim,
	.vop_setattr = vmmfs_memory_setattr,
	.vop_write = vmmfs_memory_write,
};

static int
vmmfs_memory_load(struct vmmfs_memory *memory, char *buffer, size_t capacity,
	size_t *length)
{
	uint64_t size;
	int result;

	size = memory->machine->spec.memory.size;
	result = ksnprintf(buffer, capacity, "%llu\n", (unsigned long long)size);
	if (result < 0 || (size_t)result >= capacity)
		return (EOVERFLOW);
	*length = (size_t)result;
	return (0);
}

static int
vmmfs_memory_store(struct vmmfs_memory *memory, const char *buffer, size_t length)
{
	uint64_t value;
	size_t index;
	unsigned int digit;

	if (length == 0)
		return (EINVAL);
	if (buffer[length - 1] == '\n')
		--length;
	if (length == 0)
		return (EINVAL);

	value = 0;
	for (index = 0; index < length; ++index) {
		if (buffer[index] < '0' || buffer[index] > '9')
			return (EINVAL);
		digit = (unsigned int)(buffer[index] - '0');
		if (value > (UINT64_MAX - digit) / 10)
			return (ERANGE);
		value = value * 10 + digit;
	}

	lwkt_gettoken(&memory->machine->token);
	if (!memory->machine->stopped.expect_stopped ||
	    memory->machine->machine != NULL) {
		lwkt_reltoken(&memory->machine->token);
		return (EBUSY);
	}
	memory->machine->spec.memory.size = (uint64_t)value;
	lwkt_reltoken(&memory->machine->token);
	return (0);
}

int
vmmfs_memory_create(struct vmmfs_machine *machine, struct vmmfs_memory *memory)
{
	struct vmmfs_mount *state;
	struct vnode *vnode;
	int error;

	bzero(memory, sizeof(*memory));
	memory->machine = machine;
	state = (struct vmmfs_mount *)machine->root->mount->mnt_data;
    memory->inode = atomic_fetchadd_int(&state->next_inode, 1);
	if (state->memory_vops == NULL)
		return (ENXIO);

	error = getnewvnode(VT_SYNTH, machine->root->mount, &vnode, 0, 0);
	if (error != 0)
		return (error);
	vnode->v_data = memory;
	vnode->v_ops = &state->memory_vops;
	vnode->v_type = VREG;
	memory->vnode = vnode;
	vx_downgrade(vnode);
	vn_unlock(vnode);
	return (0);
}

int
vmmfs_memory_destroy(struct vmmfs_memory *memory)
{
	struct vnode *vnode;

	if (memory == NULL)
		return (EINVAL);
	if (memory->object != NULL || memory->boot_vmspace != NULL || memory->run_vmspace != NULL)
		return (EBUSY);
	vnode = memory->vnode;
	if (vnode != NULL) {
		vx_get(vnode);
		vgone_vxlocked(vnode);
		vx_put(vnode);
		vrele(vnode);
	}
	KKASSERT(memory->vnode == NULL);
	memory->machine = NULL;
	return (0);
}

int
vmmfs_memory_prepare(struct vmmfs_memory *memory)
{
	struct vm_object *object;
	struct vmspace *vmspace;
	uint64_t size;
	int error;

	if (memory == NULL || memory->machine == NULL)
		return (EINVAL);
	if (memory->object != NULL || memory->boot_vmspace != NULL ||
	    memory->run_vmspace != NULL)
		return (EBUSY);
	size = memory->machine->spec.memory.size;
	if (size == 0 || (size & PAGE_MASK) != 0)
		return (EINVAL);
	object = default_pager_alloc(NULL, round_page64(size), VM_PROT_DEFAULT,
	    0);
	if (object == NULL)
		return (ENOMEM);
	vm_object_set_flag(object, OBJ_NOSPLIT);
	vmspace = vmspace_alloc(0, (vm_offset_t)size);
	if (vmspace == NULL) {
		vm_object_deallocate(object);
		return (ENOMEM);
	}
	pmap_maybethreaded(vmspace_pmap(vmspace));
	error = vmmfs_memory_map_object(vmspace, object, size);
	if (error != 0) {
		vmspace_rel(vmspace);
		vm_object_deallocate(object);
		return (error);
	}
	memory->object = object;
	memory->run_vmspace = vmspace;
	return (0);
}

int
vmmfs_memory_snapshot(struct vmmfs_memory *memory)
{
	struct vmspace *boot_vmspace;

	if (memory == NULL || memory->object == NULL ||
	    memory->run_vmspace == NULL || memory->boot_vmspace != NULL)
		return (EINVAL);
	boot_vmspace = vmspace_fork(memory->run_vmspace, NULL, NULL);
	if (boot_vmspace == NULL)
		return (ENOMEM);
	pmap_pinit2(vmspace_pmap(boot_vmspace));
	memory->boot_vmspace = boot_vmspace;
	return (0);
}

void
vmmfs_memory_release(struct vmmfs_memory *memory)
{
	struct vm_object *object;
	struct vmspace *boot_vmspace;
	struct vmspace *run_vmspace;

	if (memory == NULL)
		return;
	object = memory->object;
	boot_vmspace = memory->boot_vmspace;
	run_vmspace = memory->run_vmspace;
	memory->object = NULL;
	memory->boot_vmspace = NULL;
	memory->run_vmspace = NULL;
	if (run_vmspace != NULL)
		vmspace_rel(run_vmspace);
	if (boot_vmspace != NULL)
		vmspace_rel(boot_vmspace);
	if (object != NULL)
		vm_object_deallocate(object);
}

static int
vmmfs_memory_map_object(struct vmspace *vmspace, struct vm_object *object,
	uint64_t size)
{
	vm_map_t map;
	vm_prot_t prot;
	int count;
	int error;

	map = &vmspace->vm_map;
	prot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
	count = vm_map_entry_reserve(MAP_RESERVE_COUNT);
	vm_map_lock(map);
	vmmfs_memory_object_reference(object);
	vm_object_hold(object);
	error = vm_map_insert(map, &count, object, NULL, 0, NULL, 0,
	    round_page64(size), VM_MAPTYPE_NORMAL, VM_SUBSYS_MMAP, prot, prot,
	    0);
	vm_object_drop(object);
	vm_map_unlock(map);
	vm_map_entry_release(count);
	if (error != 0) {
		vm_object_deallocate(object);
		return (ENOMEM);
	}
	return (0);
}

static void
vmmfs_memory_object_reference(struct vm_object *object)
{
	vm_object_hold(object);
	vm_object_reference_locked(object);
	vm_object_drop(object);
}

static int
vmmfs_memory_access(struct vop_access_args *ap)
{
	return (vop_helper_access(ap, 0, 0, VMMFS_MEMORY_MODE, 0));
}

static int
vmmfs_memory_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_memory *memory;
	struct vattr *vattr;
	char buffer[32];
	size_t length;
	int error;

	memory = ap->a_vp->v_data;
	if (memory == NULL)
		return (ENOENT);
	error = vmmfs_memory_load(memory, buffer, sizeof(buffer), &length);
	if (error != 0)
		return (error);
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VREG;
	vattr->va_mode = VMMFS_MEMORY_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = memory->inode;
	vattr->va_size = length;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = length;
	return (0);
}

static int
vmmfs_memory_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VREG;
	ap->a_lvap->va_mode = VMMFS_MEMORY_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return (0);
}

static int
vmmfs_memory_open(struct vop_open_args *ap)
{
	return (vop_stdopen(ap));
}

static int
vmmfs_memory_read(struct vop_read_args *ap)
{
	struct vmmfs_memory *memory;
	struct uio *uio;
	char buffer[32];
	size_t length;
	off_t offset;
	int error;

	memory = ap->a_vp->v_data;
	if (memory == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset < 0)
		return (EINVAL);
	error = vmmfs_memory_load(memory, buffer, sizeof(buffer), &length);
	if (error != 0)
		return (error);
	offset = uio->uio_offset;
	if ((size_t)offset >= length)
		return (0);
	return (uiomove(buffer + offset, length - (size_t)offset, uio));
}

static int
vmmfs_memory_setattr(struct vop_setattr_args *ap)
{
	/* Accept the O_TRUNC size update performed before a control write. */
	(void)ap;
	return (0);
}

static int
vmmfs_memory_write(struct vop_write_args *ap)
{
	struct vmmfs_memory *memory;
	struct uio *uio;
	char buffer[32];
	size_t length;
	int error;

	memory = ap->a_vp->v_data;
	if (memory == NULL)
		return (ENOENT);
	uio = ap->a_uio;
	if (uio->uio_offset != 0 || uio->uio_resid == 0 ||
	    (size_t)uio->uio_resid >= sizeof(buffer))
		return (EINVAL);
	length = (size_t)uio->uio_resid;
	error = uiomove(buffer, length, uio);
	if (error != 0)
		return (error);
	return (vmmfs_memory_store(memory, buffer, length));
}

static int
vmmfs_memory_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_memory *memory;

	memory = ap->a_vp->v_data;
	if (memory != NULL && memory->vnode == ap->a_vp)
		memory->vnode = NULL;
	ap->a_vp->v_data = NULL;
	return (0);
}
