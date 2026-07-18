/* DragonFly kernel malloc shim for zstd (FreeBSD-style). */
#ifndef _TARFS_ZSTD_STDLIB_H_
#define _TARFS_ZSTD_STDLIB_H_

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/systm.h>

MALLOC_DECLARE(M_ZSTD);

#undef malloc
#undef free
#undef calloc
#define malloc(x)	kmalloc((x), M_ZSTD, M_WAITOK)
#define free(x)		kfree((x), M_ZSTD)
#define calloc(n, s)	kmalloc((size_t)(n) * (size_t)(s), M_ZSTD, M_WAITOK | M_ZERO)
#endif

#endif
