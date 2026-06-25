/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Guest serial console core -- see vmm_console.h.
 */
#include <sys/types.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <sys/systm.h>

#include "vmm_console.h"

static size_t
vmm_console_min(size_t a, size_t b)
{
	return a < b ? a : b;
}

void
vmm_console_init(struct vmm_console *c)
{
	memset(c, 0, sizeof(*c));
	lwkt_token_init(&c->token_console, "vmmcons");
}

void
vmm_console_reset(struct vmm_console *c)
{
	lwkt_gettoken(&c->token_console);
	c->mut_guest_rx_bytes = 0;
	c->mut_guest_drop_bytes = 0;
	c->mut_ring_start = 0;
	c->mut_ring_len = 0;
	lwkt_reltoken(&c->token_console);
}

size_t
vmm_console_read(struct vmm_console *c, off_t off, char *out, size_t cap)
{
	size_t copied = 0;
	size_t pos;
	size_t first;

	if (cap == 0 || off < 0)
		return 0;
	lwkt_gettoken(&c->token_console);
	if ((uint64_t)off >= c->mut_ring_len)
		goto out;
	copied = vmm_console_min(cap, c->mut_ring_len - (size_t)off);
	pos = (c->mut_ring_start + (size_t)off) % VMM_CONSOLE_RING_SIZE;
	first = vmm_console_min(copied, VMM_CONSOLE_RING_SIZE - pos);
	memcpy(out, c->mut_ring + pos, first);
	if (first < copied)
		memcpy(out + first, c->mut_ring, copied - first);
out:
	lwkt_reltoken(&c->token_console);
	return copied;
}

void
vmm_console_guest_write(struct vmm_console *c, const char *buf, size_t len)
{
	size_t i;

	if (len == 0)
		return;
	lwkt_gettoken(&c->token_console);
	for (i = 0; i < len; i++) {
		size_t pos;

		if (c->mut_ring_len < VMM_CONSOLE_RING_SIZE) {
			pos = (c->mut_ring_start + c->mut_ring_len) %
			    VMM_CONSOLE_RING_SIZE;
			c->mut_ring_len++;
		} else {
			pos = c->mut_ring_start;
			c->mut_ring_start = (c->mut_ring_start + 1) %
			    VMM_CONSOLE_RING_SIZE;
			c->mut_guest_drop_bytes++;
		}
		c->mut_ring[pos] = buf[i];
		c->mut_guest_rx_bytes++;
	}
	lwkt_reltoken(&c->token_console);
	wakeup(c);
}

void
vmm_console_write(struct vmm_console *c, const char *buf, size_t len)
{
	(void)buf;
	lwkt_gettoken(&c->token_console);
	c->mut_host_tx_bytes += len;
	lwkt_reltoken(&c->token_console);
}
