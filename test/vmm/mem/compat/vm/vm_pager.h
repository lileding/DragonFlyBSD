#ifndef VMM_TEST_MEM_COMPAT_VM_VM_PAGER_H
#define VMM_TEST_MEM_COMPAT_VM_VM_PAGER_H

#include <stdlib.h>

#include "vm/vm.h"
#include "vm/vm_object.h"

extern int vmm_test_default_pager_alloc_fail;
extern vm_size_t vmm_test_default_pager_alloc_size;
extern vm_prot_t vmm_test_default_pager_alloc_prot;
extern int vmm_test_default_pager_alloc_flags;

static inline struct vm_object *
default_pager_alloc(void *handle, vm_size_t size, vm_prot_t prot, int flags)
{
	struct vm_object *object;

	(void)handle;
	if (vmm_test_default_pager_alloc_fail)
		return NULL;
	vmm_test_default_pager_alloc_size = size;
	vmm_test_default_pager_alloc_prot = prot;
	vmm_test_default_pager_alloc_flags = flags;
	object = calloc(1, sizeof(*object));
	if (object != NULL)
		object->refs = 1;
	return object;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_PAGER_H */
