/* Minimal lwkt token compatibility for pure vPCIe relation tests. */
#ifndef TEST_VMM_PCIE_COMPAT_SYS_THREAD_H
#define TEST_VMM_PCIE_COMPAT_SYS_THREAD_H

#include <stddef.h>

struct lwkt_token {
	unsigned int opaque;
};

static inline void
lwkt_token_init(struct lwkt_token *token, const char *description)
{
	(void)token;
	(void)description;
}

static inline void
lwkt_gettoken(struct lwkt_token *token)
{
	(void)token;
}

static inline void
lwkt_reltoken(struct lwkt_token *token)
{
	(void)token;
}

#endif /* TEST_VMM_PCIE_COMPAT_SYS_THREAD_H */
