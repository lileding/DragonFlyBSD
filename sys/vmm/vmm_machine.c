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

#define EV_CREATED	1
#define EV_STARTED	2
#define EV_STOPPED	3
#define EV_DELETED	4

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
ev_push(struct vmm_machine *m, uint8_t code)
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
vmm_machine_init(struct vmm_machine *m)
{
	memset(m, 0, sizeof(*m));
	vmm_console_init(&m->console);
	m->stopped = 1;
	ev_push(m, EV_CREATED);
	ev_push(m, EV_STOPPED);
}

int
vmm_machine_config_complete(const struct vmm_machine *m)
{
	return vmm_vcpu_is_set(&m->vcpu) && vmm_mem_is_set(&m->mem) &&
	    vmm_loader_is_set(&m->loader);
}

int
vmm_machine_is_stopped(const struct vmm_machine *m)
{
	return m->stopped;
}

void
vmm_machine_stop(struct vmm_machine *m, int force)
{
	(void)force;
	if (!m->stopped) {
		m->stopped = 1;
		ev_push(m, EV_STOPPED);
	}
}

void
vmm_machine_start(struct vmm_machine *m)
{
	if (m->stopped) {
		m->stopped = 0;
		ev_push(m, EV_STARTED);
	}
}

int
vmm_machine_is_deleting(const struct vmm_machine *m)
{
	return m->deleting;
}

int
vmm_machine_lease_open(struct vmm_machine *m)
{
	if (m->deleting)
		return 0;
	m->lease_count++;
	m->armed = 1;
	return 1;
}

enum vmm_close_action
vmm_machine_lease_close(struct vmm_machine *m)
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
vmm_machine_begin_delete(struct vmm_machine *m)
{
	if (m->deleting)
		return 0;
	m->deleting = 1;
	ev_push(m, EV_DELETED);
	return 1;
}

int
vmm_machine_events_pending(const struct vmm_machine *m)
{
	return m->ev_count > 0;
}

size_t
vmm_machine_read_events(struct vmm_machine *m, char *out, size_t cap)
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
