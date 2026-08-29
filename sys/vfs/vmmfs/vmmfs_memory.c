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
#include "vmmfs_pciroot.h"

#define VMMFS_MEMORY_MODE 0644
#define VMMFS_GPA_MAX ((vm_offset_t)127 * 1024 * 1024 * 1024 * 1024)

static int vmmfs_memory_access(struct vop_access_args *);
static int vmmfs_memory_getattr(struct vop_getattr_args *);
static int vmmfs_memory_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_memory_open(struct vop_open_args *);
static int vmmfs_memory_read(struct vop_read_args *);
static int vmmfs_memory_setattr(struct vop_setattr_args *);
static int vmmfs_memory_write(struct vop_write_args *);
static int vmmfs_memory_inactive(struct vop_inactive_args *);
static int vmmfs_memory_reclaim(struct vop_reclaim_args *);
static int vmmfs_memory_map_vmspace(struct vmspace *, struct vm_object *,
	uint64_t, uint64_t, uint64_t, vm_prot_t);
static void vmmfs_memory_drop(struct vmmfs_node *);
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
	.vop_inactive = vmmfs_memory_inactive,
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

	if (memory == NULL || vmmfs_machine_is_dead(memory->machine))
		return (ENOENT);
	lwkt_gettoken(&memory->machine->token);
	size = memory->size;
	lwkt_reltoken(&memory->machine->token);
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
	if (memory->machine->dead) {
		lwkt_reltoken(&memory->machine->token);
		return (ENOENT);
	}
	if (memory->machine->machine != NULL) {
		lwkt_reltoken(&memory->machine->token);
		return (EBUSY);
	}
	memory->size = value;
	lwkt_reltoken(&memory->machine->token);
	return (0);
}

int
vmmfs_memory_init(struct vmmfs_machine *machine, struct vmmfs_memory *memory)
{
	struct vmmfs_mount *state;
	int error;

	if (machine == NULL || memory == NULL)
		return (EINVAL);
	bzero(memory, sizeof(*memory));
	memory->machine = machine;
	vmmfs_node_setup(&memory->node, &machine->branch.node, vmmfs_memory_drop, NULL);
	vmmfs_machine_hold(machine);
	state = (struct vmmfs_mount *)machine->root->mount->mnt_data;
	memory->inode = atomic_fetchadd_int(&state->next_inode, 1);
	if (state->memory_vops == NULL) {
		error = ENXIO;
		goto fail;
	}

	return (0);

fail:
	memory->machine = NULL;
	memory->inode = 0;
	KKASSERT(vmmfs_node_detach_parent(&memory->node) ==
	    &machine->branch.node);
	vmmfs_machine_put(machine);
	return (error);
}

void
vmmfs_memory_fini(struct vmmfs_memory *memory)
{
	struct vmmfs_machine *machine;

	if (memory == NULL)
		return;
	machine = memory->machine;
	if (machine == NULL)
		return;
	if (memory->object != NULL || memory->boot_vmspace != NULL || memory->run_vmspace != NULL)
		panic("vmmfs_memory_fini: runtime memory is still active");
	if (memory->node.vnode != NULL)
		panic("vmmfs_memory_fini: vnode is still published");
	KKASSERT(vmmfs_node_detach_parent(&memory->node) ==
	    &machine->branch.node);
	memory->machine = NULL;
	memory->inode = 0;
	vmmfs_machine_put(machine);
	return;
}

int
vmmfs_memory_publish(struct vmmfs_memory *memory)
{
	struct vmmfs_mount *state;

	if (memory == NULL || memory->machine == NULL)
		return (EINVAL);
	state = (struct vmmfs_mount *)memory->machine->root->mount->mnt_data;
	if (state == NULL || state->memory_vops == NULL)
		return (ENXIO);
	return (vmmfs_node_publish_regular(&memory->node,
	    memory->machine->root->mount, &state->memory_vops, VREG, memory));
}

static void
vmmfs_memory_drop(struct vmmfs_node *node)
{
	vmmfs_memory_fini((struct vmmfs_memory *)node);
}

int
vmmfs_memory_prepare(struct vmmfs_memory *memory, uint64_t size)
{
	struct vm_object *object;
	struct vmspace *vmspace;

	if (memory == NULL || memory->machine == NULL)
		return (EINVAL);
	if (memory->object != NULL || memory->boot_vmspace != NULL ||
	    memory->run_vmspace != NULL)
		return (EBUSY);
	if (size == 0 || (size & PAGE_MASK) != 0)
		return (EINVAL);
	object = default_pager_alloc(NULL, round_page64(size), VM_PROT_DEFAULT,
	    0);
	if (object == NULL)
		return (ENOMEM);
	vm_object_set_flag(object, OBJ_NOSPLIT);
	/*
	 * Keep the complete GPA namespace available.  RAM is mapped later around
	 * the fixed VMMFS x64 PCI hole.
	 */
	vmspace = vmspace_alloc(VM_MIN_USER_ADDRESS, VMMFS_GPA_MAX);
	if (vmspace == NULL) {
		vm_object_deallocate(object);
		return (ENOMEM);
	}
	/* VMM transforms this empty pmap to NPT before RAM is mapped into it. */
	pmap_maybethreaded(vmspace_pmap(vmspace));
	memory->object = object;
	memory->run_vmspace = vmspace;
	return (0);
}

int
vmmfs_memory_map(struct vmmfs_memory *memory)
{
	uint64_t size;
	int error;

	if (memory == NULL || memory->object == NULL ||
	    memory->run_vmspace == NULL || memory->mapped)
		return (EINVAL);
	size = memory->size;
	if (size <= VMMFS_PCI_MMIO_GPA)
		error = vmmfs_memory_map_object(memory, memory->object, 0, 0,
		    size, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
	else
		error = vmmfs_memory_map_object(memory, memory->object, 0, 0,
		    VMMFS_PCI_MMIO_GPA,
		    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
	if (error != 0)
		return (error);
	if (size > VMMFS_PCI_HOLE_END) {
		error = vmmfs_memory_map_object(memory, memory->object,
		    VMMFS_PCI_HOLE_END, VMMFS_PCI_HOLE_END,
		    size - VMMFS_PCI_HOLE_END,
		    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
		if (error != 0)
			return (error);
	}
	memory->mapped = true;
	return (0);
}

int
vmmfs_memory_snapshot(struct vmmfs_memory *memory)
{
	struct vmspace *boot_vmspace;

	if (memory == NULL || memory->object == NULL ||
	    memory->run_vmspace == NULL || !memory->mapped ||
	    memory->boot_vmspace != NULL)
		return (EINVAL);
	boot_vmspace = vmspace_fork(memory->run_vmspace, NULL, NULL);
	if (boot_vmspace == NULL)
		return (ENOMEM);
	pmap_pinit2(vmspace_pmap(boot_vmspace));
	memory->boot_vmspace = boot_vmspace;
	return (0);
}

int
vmmfs_memory_reset_begin(struct vmmfs_memory *memory,
	struct vmspace **old_vmspacep)
{
	struct vmspace *old_vmspace;
	struct vmspace *run_vmspace;

	if (memory == NULL || old_vmspacep == NULL ||
	    memory->boot_vmspace == NULL || memory->run_vmspace == NULL)
		return (EINVAL);
	run_vmspace = vmspace_fork(memory->boot_vmspace, NULL, NULL);
	if (run_vmspace == NULL)
		return (ENOMEM);
	pmap_maybethreaded(vmspace_pmap(run_vmspace));
	old_vmspace = memory->run_vmspace;
	memory->run_vmspace = run_vmspace;
	*old_vmspacep = old_vmspace;
	return (0);
}

void
vmmfs_memory_reset_abort(struct vmmfs_memory *memory,
	struct vmspace *old_vmspace)
{
	struct vmspace *run_vmspace;

	KKASSERT(memory != NULL);
	KKASSERT(old_vmspace != NULL);
	run_vmspace = memory->run_vmspace;
	KKASSERT(run_vmspace != NULL && run_vmspace != old_vmspace);
	memory->run_vmspace = old_vmspace;
	pmap_del_all_cpus(run_vmspace);
	vmspace_rel(run_vmspace);
}

void
vmmfs_memory_reset_commit(struct vmmfs_memory *memory,
	struct vmspace *old_vmspace)
{
	KKASSERT(memory != NULL);
	KKASSERT(memory->run_vmspace != NULL);
	KKASSERT(old_vmspace != NULL && memory->run_vmspace != old_vmspace);
	pmap_del_all_cpus(old_vmspace);
	vmspace_rel(old_vmspace);
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
	memory->mapped = false;
	if (run_vmspace != NULL) {
		pmap_del_all_cpus(run_vmspace);
		vmspace_rel(run_vmspace);
	}
	if (boot_vmspace != NULL) {
		pmap_del_all_cpus(boot_vmspace);
		vmspace_rel(boot_vmspace);
	}
	if (object != NULL)
		vm_object_deallocate(object);
}

int
vmmfs_memory_map_object(struct vmmfs_memory *memory,
	struct vm_object *object, uint64_t gpa, uint64_t offset, uint64_t size,
	vm_prot_t prot)
{
	if (memory == NULL || memory->run_vmspace == NULL || object == NULL ||
	    size == 0 || (gpa & PAGE_MASK) != 0 || (offset & PAGE_MASK) != 0 ||
	    (size & PAGE_MASK) != 0 || gpa > VMMFS_GPA_MAX - size)
		return (EINVAL);
	return (vmmfs_memory_map_vmspace(memory->run_vmspace, object, gpa,
	    offset, size, prot));
}

void
vmmfs_memory_unmap(struct vmmfs_memory *memory, uint64_t gpa, uint64_t size)
{
	if (memory == NULL || memory->run_vmspace == NULL || size == 0 ||
	    (gpa & PAGE_MASK) != 0 || (size & PAGE_MASK) != 0 ||
	    gpa > VMMFS_GPA_MAX - size)
		return;
	(void)vm_map_remove(&memory->run_vmspace->vm_map, gpa, gpa + size);
}

static int
vmmfs_memory_map_vmspace(struct vmspace *vmspace, struct vm_object *object,
	uint64_t gpa, uint64_t offset, uint64_t size, vm_prot_t prot)
{
	vm_map_t map;
	int count;
	int error;

	map = &vmspace->vm_map;
	count = vm_map_entry_reserve(MAP_RESERVE_COUNT);
	vm_map_lock(map);
	vmmfs_memory_object_reference(object);
	vm_object_hold(object);
	error = vm_map_insert(map, &count, object, NULL, offset, NULL, gpa,
	    gpa + round_page64(size), VM_MAPTYPE_NORMAL, VM_SUBSYS_MMAP, prot,
	    prot, 0);
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
vmmfs_memory_inactive(struct vop_inactive_args *ap)
{
	struct vmmfs_memory *memory;
	struct vmmfs_machine *machine;

	memory = ap->a_vp->v_data;
	if (memory == NULL)
		return (0);
	machine = memory->machine;
	if (!vmmfs_machine_is_dead(machine))
		return (0);
	vmmfs_node_inactive(&memory->node, ap->a_vp);
	return (0);
}

static int
vmmfs_memory_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_memory *memory;
	struct vmmfs_machine *machine;
	bool reclaim;

	memory = ap->a_vp->v_data;
	if (memory == NULL || memory->machine == NULL)
		return (0);
	machine = memory->machine;
	lwkt_gettoken(&machine->token);
	reclaim = vmmfs_node_reclaim(&memory->node, ap->a_vp);
	lwkt_reltoken(&machine->token);
	if (reclaim)
		vmmfs_node_drop(&memory->node);
	return (0);
}
