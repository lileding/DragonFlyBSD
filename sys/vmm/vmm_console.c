/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Guest serial console core -- see vmm_console.h.
 */
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <sys/systm.h>

#include "vmm_console.h"

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
	c->mut_host_tx_bytes = 0;
	c->mut_host_drop_bytes = 0;
	c->mut_output_head_seq = c->mut_output_tail_seq;
	c->mut_input_start = 0;
	c->mut_input_len = 0;
	lwkt_reltoken(&c->token_console);
	wakeup(&c->mut_output_tail_seq);
	wakeup(&c->mut_input_len);
}

int
vmm_console_read(struct vmm_console *c, off_t *offp, char *out, size_t cap,
    int nonblock, size_t *copiedp)
{
	uint64_t off;
	uint64_t available;
	size_t pos;
	size_t first;
	size_t copied;
	int error;

	*copiedp = 0;
	if (cap == 0)
		return 0;
	for (;;) {
		lwkt_gettoken(&c->token_console);
		off = *offp < 0 ? c->mut_output_head_seq : (uint64_t)*offp;
		if (off < c->mut_output_head_seq)
			off = c->mut_output_head_seq;
		if (off < c->mut_output_tail_seq)
			break;
		if (nonblock) {
			lwkt_reltoken(&c->token_console);
			return EWOULDBLOCK;
		}
		tsleep_interlock(&c->mut_output_tail_seq, PCATCH);
		lwkt_reltoken(&c->token_console);
		error = tsleep(&c->mut_output_tail_seq,
		    PCATCH | PINTERLOCKED, "vmmconr", 0);
		if (error)
			return error;
	}
	available = c->mut_output_tail_seq - off;
	copied = (available < cap) ? (size_t)available : cap;
	pos = off % VMM_CONSOLE_RING_SIZE;
	first = copied;
	if (first > VMM_CONSOLE_RING_SIZE - pos)
		first = VMM_CONSOLE_RING_SIZE - pos;
	memcpy(out, c->mut_ring + pos, first);
	if (first < copied)
		memcpy(out + first, c->mut_ring, copied - first);
	*offp = (off_t)off;
	*copiedp = copied;
	lwkt_reltoken(&c->token_console);
	return 0;
}

int
vmm_console_wait_output(struct vmm_console *c, off_t wait_off)
{
	uint64_t off;
	int error;

	lwkt_gettoken(&c->token_console);
	off = wait_off < 0 ? c->mut_output_head_seq : (uint64_t)wait_off;
	if (off < c->mut_output_head_seq)
		off = c->mut_output_head_seq;
	if (off < c->mut_output_tail_seq) {
		lwkt_reltoken(&c->token_console);
		return 0;
	}
	tsleep_interlock(&c->mut_output_tail_seq, PCATCH);
	lwkt_reltoken(&c->token_console);
	error = tsleep(&c->mut_output_tail_seq, PCATCH | PINTERLOCKED,
	    "vmmconr", 0);
	return error;
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

		if (c->mut_output_tail_seq - c->mut_output_head_seq >=
		    VMM_CONSOLE_RING_SIZE) {
			c->mut_output_head_seq++;
			c->mut_guest_drop_bytes++;
		}
		pos = c->mut_output_tail_seq % VMM_CONSOLE_RING_SIZE;
		c->mut_ring[pos] = buf[i];
		c->mut_output_tail_seq++;
		c->mut_guest_rx_bytes++;
	}
	lwkt_reltoken(&c->token_console);
	wakeup(&c->mut_output_tail_seq);
}

int
vmm_console_write(struct vmm_console *c, const char *buf, size_t len,
    int nonblock, size_t *copiedp)
{
	size_t copied = 0;
	int error;

	*copiedp = 0;
	while (copied < len) {
		lwkt_gettoken(&c->token_console);
		while (c->mut_input_len == VMM_CONSOLE_INPUT_SIZE) {
			if (nonblock) {
				lwkt_reltoken(&c->token_console);
				*copiedp = copied;
				return copied == 0 ? EWOULDBLOCK : 0;
			}
			tsleep_interlock(&c->mut_input_len, PCATCH);
			lwkt_reltoken(&c->token_console);
			error = tsleep(&c->mut_input_len,
			    PCATCH | PINTERLOCKED, "vmmconw", 0);
			if (error) {
				*copiedp = copied;
				return copied == 0 ? error : 0;
			}
			lwkt_gettoken(&c->token_console);
		}
		while (copied < len &&
		    c->mut_input_len < VMM_CONSOLE_INPUT_SIZE) {
			size_t pos;

			pos = (c->mut_input_start + c->mut_input_len) %
			    VMM_CONSOLE_INPUT_SIZE;
			c->mut_input_len++;
			c->mut_input[pos] = buf[copied++];
			c->mut_host_tx_bytes++;
		}
		lwkt_reltoken(&c->token_console);
	}
	*copiedp = copied;
	return 0;
}

size_t
vmm_console_guest_pending(struct vmm_console *c)
{
	size_t pending;

	lwkt_gettoken(&c->token_console);
	pending = c->mut_input_len;
	lwkt_reltoken(&c->token_console);
	return pending;
}

int
vmm_console_guest_read(struct vmm_console *c, char *out)
{
	int available = 0;

	if (out == NULL)
		return 0;
	lwkt_gettoken(&c->token_console);
	if (c->mut_input_len != 0) {
		*out = c->mut_input[c->mut_input_start];
		c->mut_input_start = (c->mut_input_start + 1) %
		    VMM_CONSOLE_INPUT_SIZE;
		c->mut_input_len--;
		available = 1;
	}
	lwkt_reltoken(&c->token_console);
	if (available)
		wakeup(&c->mut_input_len);
	return available;
}

void
vmm_console_guest_reset_input(struct vmm_console *c)
{

	lwkt_gettoken(&c->token_console);
	c->mut_host_drop_bytes += c->mut_input_len;
	c->mut_input_start = 0;
	c->mut_input_len = 0;
	lwkt_reltoken(&c->token_console);
	wakeup(&c->mut_input_len);
}
