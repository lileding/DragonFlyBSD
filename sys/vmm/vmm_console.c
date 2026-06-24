/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Guest serial console core -- see vmm_console.h.
 */
#include <sys/types.h>
#include <sys/systm.h>

#include "vmm_console.h"

void
vmm_console_init(struct vmm_console *c)
{
	c->tx_bytes = 0;
}

size_t
vmm_console_read(struct vmm_console *c, char *out, size_t cap)
{
	(void)c;
	(void)out;
	(void)cap;
	return 0;		/* no serial ring yet: EOF */
}

void
vmm_console_write(struct vmm_console *c, const char *buf, size_t len)
{
	(void)buf;
	c->tx_bytes += len;	/* discarded, but accounted */
}
