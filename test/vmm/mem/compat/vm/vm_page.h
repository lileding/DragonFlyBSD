#ifndef VMM_TEST_MEM_COMPAT_VM_VM_PAGE_H
#define VMM_TEST_MEM_COMPAT_VM_VM_PAGE_H

#include <stdint.h>

#include "vm/vm.h"

#define VM_ALLOC_NORMAL		0x01
#define VM_ALLOC_SYSTEM		0x02
#define VM_ALLOC_ZERO		0x04
#define VM_ALLOC_RETRY		0x08

struct vm_object;

struct vm_page {
	uintptr_t phys;
};

typedef struct vm_page *vm_page_t;

#define VM_PAGE_TO_PHYS(page)	((page)->phys)
#define PHYS_TO_DMAP(pa)	((uintptr_t)(pa))

static inline vm_page_t
vm_page_lookup(struct vm_object *object, vm_pindex_t pindex)
{
	(void)object;
	(void)pindex;
	return NULL;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_PAGE_H */
