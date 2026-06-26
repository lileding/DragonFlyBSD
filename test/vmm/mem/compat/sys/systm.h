#ifndef VMM_TEST_MEM_COMPAT_SYS_SYSTM_H
#define VMM_TEST_MEM_COMPAT_SYS_SYSTM_H

#include <string.h>

#define bcopy(src, dst, len)	memcpy((dst), (src), (len))
#define bzero(ptr, len)		memset((ptr), 0, (len))
#define bcmp(a, b, len)		memcmp((a), (b), (len))

#endif /* VMM_TEST_MEM_COMPAT_SYS_SYSTM_H */
