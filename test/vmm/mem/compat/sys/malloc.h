#ifndef VMM_TEST_MEM_COMPAT_SYS_MALLOC_H
#define VMM_TEST_MEM_COMPAT_SYS_MALLOC_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define M_TEMP		0
#define M_WAITOK	0x01
#define M_ZERO		0x02

static inline void *
kmalloc(size_t size, int type, int flags)
{
	void *ptr;

	(void)type;
	ptr = malloc(size);
	if (ptr != NULL && (flags & M_ZERO) != 0)
		memset(ptr, 0, size);
	return ptr;
}

static inline void
kfree(void *ptr, int type)
{
	(void)type;
	free(ptr);
}

#endif /* VMM_TEST_MEM_COMPAT_SYS_MALLOC_H */
