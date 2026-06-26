#ifndef VMM_TEST_MEM_COMPAT_VM_VM_H
#define VMM_TEST_MEM_COMPAT_VM_VM_H

#include <stdint.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE	4096
#endif
#ifndef PAGE_MASK
#define PAGE_MASK	(PAGE_SIZE - 1)
#endif

typedef uint64_t vm_offset_t;
typedef uint64_t vm_ooffset_t;
typedef uint64_t vm_pindex_t;
typedef uint64_t vm_size_t;
typedef int vm_prot_t;

#define VM_PROT_READ		0x01
#define VM_PROT_WRITE		0x02
#define VM_PROT_EXECUTE		0x04
#define VM_PROT_DEFAULT		(VM_PROT_READ | VM_PROT_WRITE)

#define VM_FAULT_NORMAL		0
#define VM_FAULT_DIRTY		1

static inline uint64_t
trunc_page(uint64_t value)
{
	return value & ~(uint64_t)PAGE_MASK;
}

static inline uint64_t
round_page64(uint64_t value)
{
	return (value + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
}

#define OFF_TO_IDX(offset)	((vm_pindex_t)((offset) / PAGE_SIZE))

#endif /* VMM_TEST_MEM_COMPAT_VM_VM_H */
