#ifndef VMM_TEST_MEM_COMPAT_VM_VM_EXTERN_H
#define VMM_TEST_MEM_COMPAT_VM_VM_EXTERN_H

#include <stdlib.h>

#include "vm/vm.h"
#include "vm/vm_map.h"

static inline struct vmspace *
vmspace_alloc(vm_offset_t min, vm_offset_t max)
{
	struct vmspace *vmspace;

	(void)min;
	(void)max;
	vmspace = calloc(1, sizeof(*vmspace));
	if (vmspace != NULL)
		vmspace->vm_map.pmap = &vmspace->vm_pmap;
	return vmspace;
}

static inline void
vmspace_rel(struct vmspace *vmspace)
{
	free(vmspace);
}

static inline int
vm_fault(vm_map_t map, vm_offset_t addr, vm_prot_t prot, int flags)
{
	(void)map;
	(void)addr;
	(void)prot;
	(void)flags;
	return 0;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_EXTERN_H */
