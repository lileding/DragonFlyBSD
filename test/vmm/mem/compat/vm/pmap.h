#ifndef VMM_TEST_MEM_COMPAT_VM_PMAP_H
#define VMM_TEST_MEM_COMPAT_VM_PMAP_H

struct pmap {
	int dummy;
};

struct vmspace;

extern int vmm_test_pmap_maybethreaded_calls;
extern struct pmap *vmm_test_pmap_maybethreaded_pmap;

static inline void
pmap_maybethreaded(struct pmap *pmap)
{
	vmm_test_pmap_maybethreaded_calls++;
	vmm_test_pmap_maybethreaded_pmap = pmap;
}

static inline void
pmap_del_all_cpus(struct vmspace *vmspace)
{
	(void)vmspace;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_PMAP_H */
