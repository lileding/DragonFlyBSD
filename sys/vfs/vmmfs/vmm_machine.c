/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine model -- see vmm_machine.h.  Pure C, no kernel calls beyond
 * memcpy/memset (which libkern and the host C library both provide), so it
 * builds for the kernel module and for host unit tests.
 */
#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

#include "vmm_machine.h"

#define VMM_VCPU_MAX	256u
#define VMM_MEM_ALIGN	(2ull * 1024 * 1024)	/* large-page granularity */

#define EV_CREATED	1
#define EV_STARTED	2
#define EV_STOPPED	3
#define EV_DELETED	4

/* --------------------------------------------------------------------- */
/* Config value parsing.                                                 */

static int
is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Trim leading/trailing whitespace; returns the start, sets *outlen. */
static const char *
trim(const char *b, size_t len, size_t *outlen)
{
	size_t s = 0, e = len;

	while (s < e && is_ws(b[s]))
		s++;
	while (e > s && is_ws(b[e - 1]))
		e--;
	*outlen = e - s;
	return b + s;
}

/* Parse a non-empty decimal with overflow check; 1 on success. */
static int
parse_decimal(const char *b, size_t len, uint64_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (len == 0)
		return 0;
	for (i = 0; i < len; i++) {
		char c = b[i];

		if (c < '0' || c > '9')
			return 0;
		if (v > ((uint64_t)-1 - (uint64_t)(c - '0')) / 10)
			return 0;
		v = v * 10 + (uint64_t)(c - '0');
	}
	*out = v;
	return 1;
}

/* vcpu: decimal 1..=VMM_VCPU_MAX. */
static int
parse_vcpu(const char *b, size_t len, uint32_t *out)
{
	size_t tl;
	const char *t = trim(b, len, &tl);
	uint64_t v;

	if (!parse_decimal(t, tl, &v) || v < 1 || v > VMM_VCPU_MAX)
		return 0;
	*out = (uint32_t)v;
	return 1;
}

/* mem: number[KkMmGg], > 0, VMM_MEM_ALIGN-aligned. */
static int
parse_mem(const char *b, size_t len, uint64_t *out)
{
	size_t tl;
	const char *t = trim(b, len, &tl);
	uint64_t mult, v;
	size_t dlen;
	char last;

	if (tl == 0)
		return 0;
	last = t[tl - 1];
	if (last == 'K' || last == 'k') {
		mult = 1024;
		dlen = tl - 1;
	} else if (last == 'M' || last == 'm') {
		mult = 1024 * 1024;
		dlen = tl - 1;
	} else if (last == 'G' || last == 'g') {
		mult = 1024 * 1024 * 1024;
		dlen = tl - 1;
	} else if (last >= '0' && last <= '9') {
		mult = 1;
		dlen = tl;
	} else {
		return 0;
	}
	if (!parse_decimal(t, dlen, &v))
		return 0;
	if (v > ((uint64_t)-1) / mult)
		return 0;
	v *= mult;
	if (v == 0 || (v % VMM_MEM_ALIGN) != 0)
		return 0;
	*out = v;
	return 1;
}

/* Write a decimal + '\n'; returns bytes written (0 if it does not fit). */
static size_t
write_decimal(uint64_t v, char *out, size_t cap)
{
	char tmp[20];
	size_t i = sizeof(tmp), digits, need;

	do {
		tmp[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v != 0);
	digits = sizeof(tmp) - i;
	need = digits + 1;
	if (need > cap)
		return 0;
	memcpy(out, tmp + i, digits);
	out[need - 1] = '\n';
	return need;
}

/* --------------------------------------------------------------------- */
/* Event ring.                                                           */

static const char *
event_text(uint8_t code, size_t *len)
{
	switch (code) {
	case EV_CREATED: *len = 8; return "created\n";
	case EV_STARTED: *len = 8; return "started\n";
	case EV_STOPPED: *len = 8; return "stopped\n";
	case EV_DELETED: *len = 8; return "deleted\n";
	default:	 *len = 0; return "";
	}
}

static void
ev_push(struct vmm_machine_state *m, uint8_t code)
{
	size_t slot = (m->ev_tail + m->ev_count) % VMM_EVENT_CAP;

	m->ev_codes[slot] = code;
	if (m->ev_count < VMM_EVENT_CAP)
		m->ev_count++;
	else
		m->ev_tail = (m->ev_tail + 1) % VMM_EVENT_CAP;
}

/* --------------------------------------------------------------------- */
/* Machine model.                                                        */

void
vmm_machine_init(struct vmm_machine_state *m)
{
	memset(m, 0, sizeof(*m));
	m->stopped = 1;
	ev_push(m, EV_CREATED);
	ev_push(m, EV_STOPPED);
}

int
vmm_machine_commit_vcpu(struct vmm_machine_state *m, const char *buf, size_t len)
{
	uint32_t v;

	if (!parse_vcpu(buf, len, &v))
		return 0;
	m->vcpu = v;
	return 1;
}

int
vmm_machine_commit_mem(struct vmm_machine_state *m, const char *buf, size_t len)
{
	uint64_t v;

	if (!parse_mem(buf, len, &v))
		return 0;
	m->mem = v;
	return 1;
}

int
vmm_machine_commit_loader(struct vmm_machine_state *m, const char *buf,
    size_t len)
{
	size_t pl;
	const char *p = trim(buf, len, &pl);

	if (pl == 0 || pl > VMM_LOADER_MAX)
		return 0;
	memcpy(m->loader, p, pl);
	m->loader_len = pl;
	return 1;
}

size_t
vmm_machine_vcpu_text(const struct vmm_machine_state *m, char *out, size_t cap)
{
	return (m->vcpu == 0) ? 0 : write_decimal(m->vcpu, out, cap);
}

size_t
vmm_machine_mem_text(const struct vmm_machine_state *m, char *out, size_t cap)
{
	return (m->mem == 0) ? 0 : write_decimal(m->mem, out, cap);
}

size_t
vmm_machine_loader_text(const struct vmm_machine_state *m, char *out, size_t cap)
{
	size_t need = m->loader_len + 1;

	if (m->loader_len == 0 || need > cap)
		return 0;
	memcpy(out, m->loader, m->loader_len);
	out[m->loader_len] = '\n';
	return need;
}

size_t
vmm_machine_loader_path(const struct vmm_machine_state *m, char *out, size_t cap)
{
	if (m->loader_len == 0 || m->loader_len > cap)
		return 0;
	memcpy(out, m->loader, m->loader_len);
	return m->loader_len;
}

int
vmm_machine_config_complete(const struct vmm_machine_state *m)
{
	return m->vcpu != 0 && m->mem != 0 && m->loader_len != 0;
}

int
vmm_machine_is_stopped(const struct vmm_machine_state *m)
{
	return m->stopped;
}

void
vmm_machine_stop(struct vmm_machine_state *m, int force)
{
	(void)force;
	if (!m->stopped) {
		m->stopped = 1;
		ev_push(m, EV_STOPPED);
	}
}

void
vmm_machine_start(struct vmm_machine_state *m)
{
	if (m->stopped) {
		m->stopped = 0;
		ev_push(m, EV_STARTED);
	}
}

int
vmm_machine_is_deleting(const struct vmm_machine_state *m)
{
	return m->deleting;
}

int
vmm_machine_lease_open(struct vmm_machine_state *m)
{
	if (m->deleting)
		return 0;
	m->lease_count++;
	m->armed = 1;
	return 1;
}

enum vmm_close_action
vmm_machine_lease_close(struct vmm_machine_state *m)
{
	if (m->lease_count > 0)
		m->lease_count--;
	if (m->armed && m->lease_count == 0 && !m->deleting) {
		m->deleting = 1;
		ev_push(m, EV_DELETED);
		return VMM_CLOSE_DELETE;
	}
	return VMM_CLOSE_NONE;
}

int
vmm_machine_begin_delete(struct vmm_machine_state *m)
{
	if (m->deleting)
		return 0;
	m->deleting = 1;
	ev_push(m, EV_DELETED);
	return 1;
}

int
vmm_machine_events_pending(const struct vmm_machine_state *m)
{
	return m->ev_count > 0;
}

size_t
vmm_machine_read_events(struct vmm_machine_state *m, char *out, size_t cap)
{
	size_t n = 0;

	while (m->ev_count > 0) {
		size_t tlen;
		const char *t = event_text(m->ev_codes[m->ev_tail], &tlen);

		if (n + tlen > cap)
			break;
		memcpy(out + n, t, tlen);
		n += tlen;
		m->ev_tail = (m->ev_tail + 1) % VMM_EVENT_CAP;
		m->ev_count--;
	}
	return n;
}
