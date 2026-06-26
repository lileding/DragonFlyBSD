#ifndef VMM_TEST_MEM_COMPAT_VM_VM_PAGER_H
#define VMM_TEST_MEM_COMPAT_VM_VM_PAGER_H

#include <stdlib.h>

#include "vm/vm.h"
#include "vm/vm_object.h"

static inline struct vm_object *
default_pager_alloc(void *handle, vm_size_t size, vm_prot_t prot, int flags)
{
	(void)handle;
	(void)size;
	(void)prot;
	(void)flags;
	return calloc(1, sizeof(struct vm_object));
}

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_PAGER_H */
