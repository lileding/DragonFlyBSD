#ifndef VMM_TEST_MEM_COMPAT_VM_PMAP_H
#define VMM_TEST_MEM_COMPAT_VM_PMAP_H

#include <stdint.h>

struct pmap {
	int dummy;
};

typedef uintptr_t vm_paddr_t;

struct vmspace;

extern int vmm_test_pmap_maybethreaded_calls;
extern struct pmap *vmm_test_pmap_maybethreaded_pmap;
extern int vmm_test_pmap_del_all_cpus_calls;
extern struct vmspace *vmm_test_pmap_del_all_cpus_vmspace;
extern int vmm_test_pmap_del_all_cpus_before_vmspace_rel;
extern int vmm_test_pmap_del_all_cpus_pending;
extern int vmm_test_pmap_del_all_cpus_bad_order;
extern int vmm_test_vmspace_free_count;

static inline void
pmap_maybethreaded(struct pmap *pmap)
{
	vmm_test_pmap_maybethreaded_calls++;
	vmm_test_pmap_maybethreaded_pmap = pmap;
}

static inline void
pmap_del_all_cpus(struct vmspace *vmspace)
{
	vmm_test_pmap_del_all_cpus_calls++;
	vmm_test_pmap_del_all_cpus_vmspace = vmspace;
	vmm_test_pmap_del_all_cpus_before_vmspace_rel = 1;
	vmm_test_pmap_del_all_cpus_pending = 1;
}

static inline vm_paddr_t
pmap_extract(struct pmap *pmap, vm_offset_t va, void **handlep)
{
	(void)pmap;
	(void)va;
	if (handlep != NULL)
		*handlep = NULL;
	return 0;
}

static inline void
pmap_extract_done(void *handle)
{
	(void)handle;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_PMAP_H */
