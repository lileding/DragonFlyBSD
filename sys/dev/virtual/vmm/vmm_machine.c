/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine model -- see vmm_machine.h.
 */
#include <sys/kernel.h>
#include <sys/errno.h>
#include <sys/malloc.h>

#include <machine/vmparam.h>

#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>

#include "vmm_backend.h"
#include "vmm_machine.h"

MALLOC_DEFINE(M_VMM, "vmm", "vmm runtime objects");

struct vmm_memory_snapshot {
	struct vm_object *object;
	uint64_t offset;
	uint64_t gpa_base;
	uint64_t gpa_size;
};

static int vmm_machine_remap(struct vmm_machine *, uint64_t, uint64_t,
	struct vm_object *, uint64_t);
static int vmm_machine_vmspace_create(struct vmm_machine *,
	struct vmm_memory_mapping_list *, struct vmspace **);
static int vmm_machine_vmspace_map(struct vmspace *,
	const struct vmm_memory_mapping *);
static void vmm_machine_vmspace_destroy(struct vmspace *);
static int vmm_memory_snapshot_take(struct vmm_machine *,
	struct vmm_memory_snapshot **, unsigned int *, uint64_t *);
static void vmm_memory_snapshot_release(struct vmm_memory_snapshot *,
	unsigned int);
static int vmm_memory_build(struct vmm_memory_snapshot *, unsigned int,
	uint64_t, uint64_t, struct vm_object *, uint64_t,
	struct vmm_memory_mapping_list *, int *);
static int vmm_memory_mapping_create(struct vm_object *, uint64_t, uint64_t,
	uint64_t, struct vmm_memory_mapping **);
static void vmm_memory_release(struct vmm_memory_mapping_list *);

int
vmm_machine_create(vmm_machine_t *machine)
{
	struct vmm_machine *m;
	const struct vmm_backend_ops *backend;
	int error;

	if (machine == NULL)
		return EINVAL;

	*machine = NULL;
	backend = vmm_backend_machine_acquire();
	if (backend == NULL)
		return ENXIO;
	m = kmalloc(sizeof(*m), M_VMM, M_WAITOK | M_ZERO);
	if (m == NULL) {
		vmm_backend_machine_release(backend);
		return ENOMEM;
	}

	lwkt_token_init(&m->token, "vmmmach");
	TAILQ_INIT(&m->memory);
	m->memory_generation = 1;
	m->backend = backend;
	error = backend->machine_create(m);
	if (error != 0) {
		kfree(m, M_VMM);
		vmm_backend_machine_release(backend);
		return error;
	}
	error = vmm_machine_vmspace_create(m, &m->memory, &m->vmspace);
	if (error != 0) {
		backend->machine_destroy(m);
		kfree(m, M_VMM);
		vmm_backend_machine_release(backend);
		return error;
	}
	*machine = m;
	return 0;
}

int
vmm_machine_map(vmm_machine_t machine, uint64_t gpa_base,
	uint64_t gpa_size, struct vm_object *object, uint64_t offset)
{
	uint64_t object_size;

	if (machine == NULL || object == NULL || gpa_size == 0 ||
	    (offset & PAGE_MASK) != 0 || (gpa_base & PAGE_MASK) != 0 ||
	    (gpa_size & PAGE_MASK) != 0)
		return EINVAL;
	if (gpa_base + gpa_size < gpa_base)
		return EINVAL;

	vm_object_hold(object);
	object_size = IDX_TO_OFF(object->size);
	vm_object_drop(object);
	if (offset > object_size || gpa_size > object_size - offset)
		return EINVAL;

	return vmm_machine_remap(machine, gpa_base, gpa_size, object, offset);
}

int
vmm_machine_unmap(vmm_machine_t machine, uint64_t gpa_base,
	uint64_t gpa_size)
{
	if (machine == NULL || gpa_size == 0 ||
	    (gpa_base & PAGE_MASK) != 0 || (gpa_size & PAGE_MASK) != 0)
		return EINVAL;
	if (gpa_base + gpa_size < gpa_base)
		return EINVAL;

	return vmm_machine_remap(machine, gpa_base, gpa_size, NULL, 0);
}

int
vmm_machine_destroy(vmm_machine_t machine)
{
	struct vmm_memory_mapping_list memory;
	struct vmspace *vmspace;

	if (machine == NULL)
		return EINVAL;

	lwkt_gettoken(&machine->token);
	if (machine->vcpu_count != 0 || machine->run_count != 0) {
		lwkt_reltoken(&machine->token);
		return EBUSY;
	}
	memory = machine->memory;
	TAILQ_INIT(&machine->memory);
	vmspace = machine->vmspace;
	machine->vmspace = NULL;
	lwkt_reltoken(&machine->token);

	machine->backend->machine_destroy(machine);
	vmm_machine_vmspace_destroy(vmspace);
	vmm_memory_release(&memory);
	vmm_backend_machine_release(machine->backend);
	kfree(machine, M_VMM);
	return 0;
}

static int
vmm_machine_remap(struct vmm_machine *machine, uint64_t gpa_base,
	uint64_t gpa_size, struct vm_object *object, uint64_t offset)
{
	struct vmm_memory_mapping_list memory;
	struct vmm_memory_snapshot *snapshot;
	struct vmspace *vmspace;
	struct vmspace *old_vmspace;
	uint64_t generation;
	unsigned int count;
	int changed;
	int error;

	for (;;) {
		error = vmm_memory_snapshot_take(machine, &snapshot, &count,
		    &generation);
		if (error != 0)
			return error;
		TAILQ_INIT(&memory);
		error = vmm_memory_build(snapshot, count, gpa_base, gpa_size,
		    object, offset, &memory, &changed);
		vmm_memory_snapshot_release(snapshot, count);
		if (error != 0) {
			vmm_memory_release(&memory);
			return error;
		}
		if (!changed) {
			vmm_memory_release(&memory);
			return 0;
		}
		error = vmm_machine_vmspace_create(machine, &memory, &vmspace);
		if (error != 0) {
			vmm_memory_release(&memory);
			return error;
		}

		lwkt_gettoken(&machine->token);
		if (machine->run_count != 0) {
			lwkt_reltoken(&machine->token);
			vmm_machine_vmspace_destroy(vmspace);
			vmm_memory_release(&memory);
			return EBUSY;
		}
		if (machine->memory_generation != generation) {
			lwkt_reltoken(&machine->token);
			vmm_machine_vmspace_destroy(vmspace);
			vmm_memory_release(&memory);
			continue;
		}
		{
			struct vmm_memory_mapping_list old_memory;

			old_memory = machine->memory;
			machine->memory = memory;
			TAILQ_INIT(&memory);
			old_vmspace = machine->vmspace;
			machine->vmspace = vmspace;
			++machine->memory_generation;
			lwkt_reltoken(&machine->token);
			vmm_machine_vmspace_destroy(old_vmspace);
			vmm_memory_release(&old_memory);
		}
		return 0;
	}
}

static int
vmm_machine_vmspace_create(struct vmm_machine *machine,
	struct vmm_memory_mapping_list *memory, struct vmspace **vmspacep)
{
	struct vmm_memory_mapping *mapping;
	struct vmspace *vmspace;
	int error;

	*vmspacep = NULL;
	vmspace = vmspace_alloc(VM_MIN_USER_ADDRESS, VM_MAX_USER_ADDRESS);
	if (vmspace == NULL)
		return ENOMEM;
	pmap_maybethreaded(vmspace_pmap(vmspace));
	error = machine->backend->machine_pmap_init(machine,
		vmspace_pmap(vmspace));
	if (error != 0)
		goto fail;
	TAILQ_FOREACH(mapping, memory, entry) {
		error = vmm_machine_vmspace_map(vmspace, mapping);
		if (error != 0)
			goto fail;
	}
	*vmspacep = vmspace;
	return 0;

fail:
	vmm_machine_vmspace_destroy(vmspace);
	return error;
}

static int
vmm_machine_vmspace_map(struct vmspace *vmspace,
	const struct vmm_memory_mapping *mapping)
{
	struct vm_map *map;
	int count;
	int error;

	map = &vmspace->vm_map;
	count = vm_map_entry_reserve(MAP_RESERVE_COUNT);
	vm_map_lock(map);
	vm_object_hold(mapping->object);
	vm_object_reference_locked(mapping->object);
	vm_object_drop(mapping->object);
	vm_object_hold(mapping->object);
	error = vm_map_insert(map, &count, mapping->object, NULL,
		mapping->offset, NULL, mapping->gpa_base,
		mapping->gpa_base + mapping->gpa_size, VM_MAPTYPE_NORMAL,
		VM_SUBSYS_MMAP, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE,
		VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE, 0);
	vm_object_drop(mapping->object);
	vm_map_unlock(map);
	vm_map_entry_release(count);
	if (error != 0) {
		vm_object_deallocate(mapping->object);
		return ENOMEM;
	}
	return 0;
}

static void
vmm_machine_vmspace_destroy(struct vmspace *vmspace)
{
	if (vmspace == NULL)
		return;
	pmap_del_all_cpus(vmspace);
	vmspace_rel(vmspace);
}

static int
vmm_memory_snapshot_take(struct vmm_machine *machine,
	struct vmm_memory_snapshot **snapshotp, unsigned int *countp,
	uint64_t *generationp)
{
	struct vmm_memory_mapping *mapping;
	struct vmm_memory_snapshot *snapshot;
	uint64_t generation;
	unsigned int count;
	unsigned int index;

	for (;;) {
		count = 0;
		lwkt_gettoken(&machine->token);
		if (machine->run_count != 0) {
			lwkt_reltoken(&machine->token);
			return EBUSY;
		}
		TAILQ_FOREACH(mapping, &machine->memory, entry)
			++count;
		generation = machine->memory_generation;
		lwkt_reltoken(&machine->token);

		snapshot = NULL;
		if (count != 0) {
			snapshot = kmalloc(sizeof(*snapshot) * count, M_VMM,
			    M_WAITOK | M_ZERO);
			if (snapshot == NULL)
				return ENOMEM;
		}

		index = 0;
		lwkt_gettoken(&machine->token);
		if (machine->run_count == 0 &&
		    machine->memory_generation == generation) {
			TAILQ_FOREACH(mapping, &machine->memory, entry) {
				snapshot[index].object = mapping->object;
				snapshot[index].offset = mapping->offset;
				snapshot[index].gpa_base = mapping->gpa_base;
				snapshot[index].gpa_size = mapping->gpa_size;
				vm_object_reference_quick(mapping->object);
				++index;
			}
			lwkt_reltoken(&machine->token);
			*snapshotp = snapshot;
			*countp = count;
			*generationp = generation;
			return 0;
		}
		lwkt_reltoken(&machine->token);
		if (snapshot != NULL)
			kfree(snapshot, M_VMM);
	}
}

static void
vmm_memory_snapshot_release(struct vmm_memory_snapshot *snapshot,
	unsigned int count)
{
	unsigned int index;

	for (index = 0; index < count; ++index)
		vm_object_deallocate(snapshot[index].object);
	if (snapshot != NULL)
		kfree(snapshot, M_VMM);
}

static int
vmm_memory_build(struct vmm_memory_snapshot *snapshot, unsigned int count,
	uint64_t gpa_base, uint64_t gpa_size, struct vm_object *object,
	uint64_t offset, struct vmm_memory_mapping_list *memory, int *changedp)
{
	struct vmm_memory_mapping *mapping;
	struct vmm_memory_mapping *next;
	uint64_t gpa_end;
	uint64_t mapping_end;
	unsigned int index;
	int error;

	gpa_end = gpa_base + gpa_size;
	*changedp = object != NULL;
	for (index = 0; index < count; ++index) {
		mapping_end = snapshot[index].gpa_base + snapshot[index].gpa_size;
		if (mapping_end <= gpa_base || gpa_end <= snapshot[index].gpa_base) {
			error = vmm_memory_mapping_create(snapshot[index].object,
			    snapshot[index].offset, snapshot[index].gpa_base,
			    snapshot[index].gpa_size, &mapping);
			if (error != 0)
				goto fail;
			TAILQ_INSERT_TAIL(memory, mapping, entry);
			continue;
		}
		*changedp = 1;
		if (snapshot[index].gpa_base < gpa_base) {
			error = vmm_memory_mapping_create(snapshot[index].object,
			    snapshot[index].offset, snapshot[index].gpa_base,
			    gpa_base - snapshot[index].gpa_base, &mapping);
			if (error != 0)
				goto fail;
			TAILQ_INSERT_TAIL(memory, mapping, entry);
		}
		if (gpa_end < mapping_end) {
			error = vmm_memory_mapping_create(snapshot[index].object,
			    snapshot[index].offset + gpa_end - snapshot[index].gpa_base,
			    gpa_end, mapping_end - gpa_end, &mapping);
			if (error != 0)
				goto fail;
			TAILQ_INSERT_TAIL(memory, mapping, entry);
		}
	}
	if (object == NULL)
		return 0;

	error = vmm_memory_mapping_create(object, offset, gpa_base, gpa_size,
	    &mapping);
	if (error != 0)
		goto fail;
	TAILQ_FOREACH(next, memory, entry) {
		if (gpa_base < next->gpa_base) {
			TAILQ_INSERT_BEFORE(next, mapping, entry);
			return 0;
		}
	}
	TAILQ_INSERT_TAIL(memory, mapping, entry);
	return 0;

fail:
	return error;
}

static int
vmm_memory_mapping_create(struct vm_object *object, uint64_t offset,
	uint64_t gpa_base, uint64_t gpa_size,
	struct vmm_memory_mapping **mappingp)
{
	struct vmm_memory_mapping *mapping;

	mapping = kmalloc(sizeof(*mapping), M_VMM, M_WAITOK | M_ZERO);
	if (mapping == NULL)
		return ENOMEM;
	vm_object_hold(object);
	vm_object_reference_locked(object);
	vm_object_drop(object);
	mapping->object = object;
	mapping->offset = offset;
	mapping->gpa_base = gpa_base;
	mapping->gpa_size = gpa_size;
	*mappingp = mapping;
	return 0;
}

static void
vmm_memory_release(struct vmm_memory_mapping_list *memory)
{
	struct vmm_memory_mapping *mapping;

	while ((mapping = TAILQ_FIRST(memory)) != NULL) {
		TAILQ_REMOVE(memory, mapping, entry);
		vm_object_deallocate(mapping->object);
		kfree(mapping, M_VMM);
	}
}
