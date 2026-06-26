#ifndef VMM_TEST_MEM_COMPAT_VM_PMAP_H
#define VMM_TEST_MEM_COMPAT_VM_PMAP_H

struct pmap {
	int dummy;
};

struct vmspace;

extern int vmm_test_pmap_maybethreaded_calls;
extern struct pmap *vmm_test_pmap_maybethreaded_pmap;
extern int vmm_test_pmap_del_all_cpus_calls;
extern struct vmspace *vmm_test_pmap_del_all_cpus_vmspace;
extern int vmm_test_pmap_del_all_cpus_before_vmspace_rel;
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
	vmm_test_pmap_del_all_cpus_before_vmspace_rel =
	    (vmm_test_vmspace_free_count == 0);
}

#endif /* VMM_TEST_MEM_COMPAT_VM_PMAP_H */
