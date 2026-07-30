#ifndef VMM_PCIE_TEST_COMPAT_SYS_MALLOC_H
#define VMM_PCIE_TEST_COMPAT_SYS_MALLOC_H

#include <stddef.h>
#include <stdlib.h>

#define M_TEMP		0
#define M_WAITOK	0
#define M_ZERO		1

static inline void *
kmalloc(size_t size, int type, int flags)
{

	(void)type;
	return (flags & M_ZERO) != 0 ? calloc(1, size) : malloc(size);
}

static inline void
kfree(void *ptr, int type)
{

	(void)type;
	free(ptr);
}

#endif /* VMM_PCIE_TEST_COMPAT_SYS_MALLOC_H */
