#ifndef VMM_TEST_MEM_COMPAT_VM_VM_MAP_H
#define VMM_TEST_MEM_COMPAT_VM_VM_MAP_H

#include "vm/vm.h"
#include "vm/pmap.h"

#define MAP_RESERVE_COUNT	1
#define VM_MAPTYPE_NORMAL	0
#define VM_SUBSYS_MMAP		0

struct vm_object;

struct vm_map {
	struct pmap *pmap;
};

typedef struct vm_map *vm_map_t;

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
	(void)offset;
	(void)unused2;
	(void)start;
	(void)end;
	(void)maptype;
	(void)subsystem;
	(void)prot;
	(void)maxprot;
	(void)cow;
	return 0;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_MAP_H */
