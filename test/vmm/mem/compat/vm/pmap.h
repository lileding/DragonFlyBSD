#ifndef VMM_TEST_MEM_COMPAT_VM_PMAP_H
#define VMM_TEST_MEM_COMPAT_VM_PMAP_H

struct pmap {
	int dummy;
};

struct vmspace;

static inline void
pmap_maybethreaded(struct pmap *pmap)
{
	(void)pmap;
}

static inline void
pmap_del_all_cpus(struct vmspace *vmspace)
{
	(void)vmspace;
}

#endif /* VMM_TEST_MEM_COMPAT_VM_PMAP_H */
