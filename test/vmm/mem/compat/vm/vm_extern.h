#ifndef VMM_TEST_MEM_COMPAT_VM_VM_EXTERN_H
#define VMM_TEST_MEM_COMPAT_VM_VM_EXTERN_H

#include <stdlib.h>

#include "vm/vm.h"
#include "vm/vm_map.h"
#include "vm/vm_object.h"

extern int vmm_test_vm_fault_calls;
extern vm_map_t vmm_test_vm_fault_map;
extern vm_offset_t vmm_test_vm_fault_addr;
extern vm_prot_t vmm_test_vm_fault_prot;
extern int vmm_test_vm_fault_flags;
extern int vmm_test_vm_fault_result;
extern int vmm_test_vmspace_alloc_fail;
extern int vmm_test_vmspace_free_count;
extern vm_offset_t vmm_test_vmspace_alloc_min;
extern vm_offset_t vmm_test_vmspace_alloc_max;

static inline struct vmspace *
vmspace_alloc(vm_offset_t min, vm_offset_t max)
{
	struct vmspace *vmspace;

	if (vmm_test_vmspace_alloc_fail)
		return NULL;
	vmm_test_vmspace_alloc_min = min;
	vmm_test_vmspace_alloc_max = max;
	vmspace = calloc(1, sizeof(*vmspace));
	if (vmspace != NULL)
		vmspace->vm_map.pmap = &vmspace->vm_pmap;
	return vmspace;
}

static inline void
vmspace_rel(struct vmspace *vmspace)
{
	if (vmspace != NULL) {
		int i;
		int n = vmspace->vm_map.mapped_count;

		if (n > VMM_TEST_VM_MAP_INSERT_MAX)
			n = VMM_TEST_VM_MAP_INSERT_MAX;
		for (i = 0; i < n; i++)
			vm_object_deallocate(vmspace->vm_map.mapped_objects[i]);
		vmm_test_vmspace_free_count++;
	}
	free(vmspace);
}

static inline int
vm_fault(vm_map_t map, vm_offset_t addr, vm_prot_t prot, int flags)
{
	vmm_test_vm_fault_calls++;
	vmm_test_vm_fault_map = map;
	vmm_test_vm_fault_addr = addr;
	vmm_test_vm_fault_prot = prot;
	vmm_test_vm_fault_flags = flags;
	return vmm_test_vm_fault_result;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_EXTERN_H */
