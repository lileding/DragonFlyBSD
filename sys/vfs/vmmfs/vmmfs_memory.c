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
#include "vmmfs_machine.h"
#include "vmmfs_memory.h"
#include "vmmfs_parent.h"
#include "vmmfs_root.h"
#include "vmmfs_pciroot.h"

#define VMMFS_MEMORY_MODE 0644
#define VMMFS_GPA_MAX ((vm_offset_t)127 * 1024 * 1024 * 1024 * 1024)

static int vmmfs_memory_map_object(struct vmmfs_memory *, struct vm_object *,
	uint64_t, uint64_t, uint64_t, vm_prot_t);
static int vmmfs_memory_map_vmspace(struct vmspace *, struct vm_object *,
	uint64_t, uint64_t, uint64_t, vm_prot_t);
static void vmmfs_memory_drop(struct vmmfs_node *);
static void vmmfs_memory_object_reference(struct vm_object *);
static int vmmfs_memory_node_load(struct vmmfs_node *, char *, size_t, size_t *);
static int vmmfs_memory_node_store(struct vmmfs_node *, const char *, size_t);

struct vop_ops vmmfs_memory_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_node_access,
	.vop_close = vop_stdclose,
	.vop_getattr = vmmfs_node_getattr,
	.vop_getattr_lite = vmmfs_node_getattr_lite,
	.vop_open = vmmfs_node_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_node_read,
	.vop_inactive = vmmfs_node_inactive,
	.vop_reclaim = vmmfs_node_reclaim,
	.vop_setattr = vmmfs_node_setattr,
	.vop_write = vmmfs_node_write,
};

static int
vmmfs_memory_load(struct vmmfs_memory *memory, char *buffer, size_t capacity,
	size_t *length)
{
	uint64_t size;
	int result;

	if (memory == NULL || memory->node.dead)
		return (ENOENT);
	lwkt_gettoken(&vmmfs_memory_machine(memory)->branch.token);
	size = memory->size;
	lwkt_reltoken(&vmmfs_memory_machine(memory)->branch.token);
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

	if (memory->node.dead)
		return (ENOENT);
	lwkt_gettoken(&vmmfs_memory_machine(memory)->branch.token);
	if (vmmfs_memory_machine(memory)->machine != NULL) {
		lwkt_reltoken(&vmmfs_memory_machine(memory)->branch.token);
		return (EBUSY);
	}
	memory->size = value;
	memory->node.size = vmmfs_node_decimal_size(value);
	vmmfs_memory_machine(memory)->boot.node.size = (off_t)value;
	lwkt_reltoken(&vmmfs_memory_machine(memory)->branch.token);
	return (0);
}

static int
vmmfs_memory_node_load(struct vmmfs_node *node, char *buffer,
	size_t capacity, size_t *length)
{
	return (vmmfs_memory_load((struct vmmfs_memory *)node, buffer,
	    capacity, length));
}

static int
vmmfs_memory_node_store(struct vmmfs_node *node, const char *buffer,
	size_t length)
{
	return (vmmfs_memory_store((struct vmmfs_memory *)node, buffer, length));
}

int
vmmfs_memory_init(struct vmmfs_mount *mount, struct vmmfs_branch *parent,
	struct vmmfs_memory *memory, struct vnode **vnodep)
{
	struct vmmfs_root *root;
	int error;

	if (mount == NULL || parent == NULL || memory == NULL || vnodep == NULL)
		return (EINVAL);
	root = mount->root_vnode == NULL ? NULL : mount->root_vnode->v_data;
	if (root == NULL)
		return (ENXIO);
	*vnodep = NULL;
	bzero(memory, sizeof(*memory));
	memory->node.parent = parent;
	memory->node.dead = false;
	memory->node.deactivate = vmmfs_node_default_deactivate;
	memory->node.drop = vmmfs_memory_drop;
	if (parent != NULL)
		vmmfs_branch_hold(parent);
	memory->node.load_limit = 32;
	memory->node.store_limit = 31;
	memory->node.load = vmmfs_memory_node_load;
	memory->node.store = vmmfs_memory_node_store;
	memory->node.inode = vmmfs_root_allocate_inode(root);
	memory->node.mode = VMMFS_MEMORY_MODE;
	memory->node.size = vmmfs_node_decimal_size(memory->size);
	if (mount->memory_vops == NULL) {
		error = ENXIO;
		goto fail;
	}
	error = vmmfs_vnode_create_regular(mount->mount,
	    &mount->memory_vops, VREG, &memory->node, vnodep);
	if (error == 0)
		return (0);

fail:
	vmmfs_node_drop(&memory->node);
	return (error);
}

static void
vmmfs_memory_drop(struct vmmfs_node *node)
{
	struct vmmfs_memory *memory;

	memory = (struct vmmfs_memory *)node;
	KKASSERT(memory != NULL);
	if (memory->object != NULL || memory->boot_vmspace != NULL || memory->run_vmspace != NULL)
		panic("vmmfs_memory_drop: runtime memory is still active");
	memory->node.inode = 0;
	vmmfs_node_parent_put(node);
}


int
vmmfs_memory_prepare(struct vmmfs_memory *memory, uint64_t size)
{
	struct vm_object *object;
	struct vmspace *vmspace;

	if (memory == NULL || vmmfs_memory_machine(memory) == NULL)
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

static int
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
