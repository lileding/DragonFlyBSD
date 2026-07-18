/* public domain */
#ifdef _KERNEL
#include <sys/systm.h>
#define assert(x) KASSERT(x, ("zstd assert"))
#else
#include_next <assert.h>
#endif
