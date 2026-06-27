#ifndef VMM_TEST_MEM_COMPAT_VM_VM_MAP_H
#define VMM_TEST_MEM_COMPAT_VM_VM_MAP_H

#include "vm/vm.h"
#include "vm/pmap.h"

#define MAP_RESERVE_COUNT	1
#define VM_MAPTYPE_NORMAL	0
#define VM_SUBSYS_MMAP		0
#define VMM_TEST_VM_MAP_INSERT_MAX 4

#include "vm/vm_object.h"

struct vm_map {
	struct pmap *pmap;
	struct vm_object *mapped_object;
	vm_offset_t mapped_start;
	vm_offset_t mapped_end;
	vm_ooffset_t mapped_offset;
	vm_prot_t mapped_prot;
	vm_prot_t mapped_maxprot;
	int mapped_count;
	struct vm_object *mapped_objects[VMM_TEST_VM_MAP_INSERT_MAX];
	vm_offset_t mapped_starts[VMM_TEST_VM_MAP_INSERT_MAX];
	vm_offset_t mapped_ends[VMM_TEST_VM_MAP_INSERT_MAX];
	vm_ooffset_t mapped_offsets[VMM_TEST_VM_MAP_INSERT_MAX];
};

typedef struct vm_map *vm_map_t;

extern int vmm_test_vm_map_insert_result;
extern int vmm_test_vm_map_insert_calls;

struct vmspace {
	struct vm_map vm_map;
	struct pmap vm_pmap;
};

static inline struct pmap *
vmspace_pmap(struct vmspace *vmspace)
{
	return &vmspace->vm_pmap;
}

static inline int
vm_map_entry_reserve(int count)
{
	return count;
}

static inline void
vm_map_entry_release(int count)
{
	(void)count;
}

static inline void
vm_map_lock(vm_map_t map)
{
	(void)map;
}

static inline void
vm_map_unlock(vm_map_t map)
{
	(void)map;
}

static inline int
vm_map_insert(vm_map_t map, int *count, struct vm_object *object,
    void *unused1, vm_ooffset_t offset, void *unused2, vm_offset_t start,
    vm_offset_t end, int maptype, int subsystem, vm_prot_t prot,
    vm_prot_t maxprot, int cow)
{
	(void)map;
	(void)count;
	(void)object;
	(void)unused1;
	(void)unused2;
	(void)maptype;
	(void)subsystem;
	(void)cow;
	vmm_test_vm_map_insert_calls++;
	if (vmm_test_vm_map_insert_result != 0)
		return vmm_test_vm_map_insert_result;
	map->mapped_object = object;
	map->mapped_start = start;
	map->mapped_end = end;
	map->mapped_offset = offset;
	map->mapped_prot = prot;
	map->mapped_maxprot = maxprot;
	if (map->mapped_count < VMM_TEST_VM_MAP_INSERT_MAX) {
		int idx = map->mapped_count;

		map->mapped_objects[idx] = object;
		map->mapped_starts[idx] = start;
		map->mapped_ends[idx] = end;
		map->mapped_offsets[idx] = offset;
	}
	map->mapped_count++;
	return 0;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_MAP_H */
