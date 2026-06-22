/*-
 * Host unit tests for the VMM core machine model.  Builds and runs on the host
 * (no kernel) -- the C replacement for machine.rs's #[cfg(test)] tests.
 *   cc vmm_machine.c vmm_machine_test.c -o t && ./t
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "vmm_machine.h"

static int passed, failed;
#define CK(cond, name) \
	do { if (cond) passed++; else { failed++; printf("FAIL %s\n", name); } } while (0)
#define CKSTR(a, b, name) CK(strcmp((a), (b)) == 0, name)

static char buf[1024];

static const char *
vcpu_s(struct vmm_machine_state *m)
{
	size_t n = vmm_machine_vcpu_text(m, buf, sizeof(buf) - 1);
	buf[n] = 0;
	return buf;
}
static const char *
mem_s(struct vmm_machine_state *m)
{
	size_t n = vmm_machine_mem_text(m, buf, sizeof(buf) - 1);
	buf[n] = 0;
	return buf;
}
static const char *
loader_s(struct vmm_machine_state *m)
{
	size_t n = vmm_machine_loader_text(m, buf, sizeof(buf) - 1);
	buf[n] = 0;
	return buf;
}
static const char *
drain(struct vmm_machine_state *m)
{
	size_t n = vmm_machine_read_events(m, buf, sizeof(buf) - 1);
	buf[n] = 0;
	return buf;
}

int
main(void)
{
	struct vmm_machine_state m;
	int i;
	size_t n, k;

	/* --- parsing (via commit + read-back) --- */
	vmm_machine_init(&m);
	CK(vmm_machine_commit_vcpu(&m, "4\n", 2) == 1, "vcpu 4");
	CKSTR(vcpu_s(&m), "4\n", "vcpu readback 4");
	CK(vmm_machine_commit_vcpu(&m, "256", 3) == 1, "vcpu 256");
	CK(vmm_machine_commit_vcpu(&m, "257", 3) == 0, "vcpu 257 reject");
	CK(vmm_machine_commit_vcpu(&m, "0", 1) == 0, "vcpu 0 reject");
	CK(vmm_machine_commit_vcpu(&m, "x", 1) == 0, "vcpu x reject");

	vmm_machine_init(&m);
	CK(vmm_machine_commit_mem(&m, "512M\n", 5) == 1, "mem 512M");
	CKSTR(mem_s(&m), "536870912\n", "mem readback");
	CK(vmm_machine_commit_mem(&m, "1G", 2) == 1, "mem 1G");
	CK(vmm_machine_commit_mem(&m, "1M", 2) == 0, "mem 1M unaligned reject");
	CK(vmm_machine_commit_mem(&m, "512Q", 4) == 0, "mem 512Q reject");
	CK(vmm_machine_commit_mem(&m, "0", 1) == 0, "mem 0 reject");

	/* --- config registers: unset / commit / read-back --- */
	vmm_machine_init(&m);
	CKSTR(vcpu_s(&m), "", "vcpu unset empty");
	CKSTR(mem_s(&m), "", "mem unset empty");
	CKSTR(loader_s(&m), "", "loader unset empty");
	CK(!vmm_machine_config_complete(&m), "not complete unset");

	vmm_machine_init(&m);
	vmm_machine_commit_vcpu(&m, "4", 1);
	vmm_machine_commit_mem(&m, "512M", 4);
	CK(vmm_machine_commit_loader(&m, "/bin/sh\n", 8) == 1, "loader commit");
	CKSTR(loader_s(&m), "/bin/sh\n", "loader readback");
	CK(vmm_machine_config_complete(&m), "complete all three");

	/* invalid commit keeps old */
	vmm_machine_init(&m);
	vmm_machine_commit_vcpu(&m, "4", 1);
	CK(vmm_machine_commit_vcpu(&m, "0", 1) == 0, "invalid vcpu rejected");
	CKSTR(vcpu_s(&m), "4\n", "invalid vcpu kept old");
	CK(vmm_machine_commit_mem(&m, "3M", 2) == 0, "unaligned mem rejected");
	CKSTR(mem_s(&m), "", "mem never set stays empty");
	CK(vmm_machine_commit_loader(&m, "   ", 3) == 0, "empty loader rejected");

	vmm_machine_init(&m);
	vmm_machine_commit_vcpu(&m, "2", 1);
	CK(!vmm_machine_config_complete(&m), "incomplete after vcpu");
	vmm_machine_commit_mem(&m, "2M", 2);
	CK(!vmm_machine_config_complete(&m), "incomplete after mem");
	vmm_machine_commit_loader(&m, "/x", 2);
	CK(vmm_machine_config_complete(&m), "complete after loader");

	/* --- lifecycle --- */
	vmm_machine_init(&m);
	CK(vmm_machine_is_stopped(&m), "new is stopped");
	vmm_machine_start(&m);
	CK(!vmm_machine_is_stopped(&m), "started");
	vmm_machine_stop(&m, 0);
	CK(vmm_machine_is_stopped(&m), "stopped");

	vmm_machine_init(&m);
	vmm_machine_start(&m);
	vmm_machine_start(&m);
	CK(!vmm_machine_is_stopped(&m), "start idempotent");
	vmm_machine_stop(&m, 0);
	vmm_machine_stop(&m, 1);
	CK(vmm_machine_is_stopped(&m), "stop idempotent");

	/* --- lease --- */
	vmm_machine_init(&m);
	CK(vmm_machine_lease_open(&m) == 1, "lease open");
	CK(vmm_machine_lease_close(&m) == VMM_CLOSE_DELETE, "lease close deletes");
	CK(vmm_machine_is_deleting(&m), "deleting after close");

	vmm_machine_init(&m);
	vmm_machine_lease_open(&m);
	vmm_machine_lease_open(&m);
	CK(vmm_machine_lease_close(&m) == VMM_CLOSE_NONE, "refcount 1 of 2");
	CK(vmm_machine_lease_close(&m) == VMM_CLOSE_DELETE, "refcount 0 deletes");

	vmm_machine_init(&m);
	CK(vmm_machine_lease_close(&m) == VMM_CLOSE_NONE, "never opened no delete");
	CK(!vmm_machine_is_deleting(&m), "not deleting");

	vmm_machine_init(&m);
	CK(vmm_machine_begin_delete(&m) == 1, "begin delete");
	CK(vmm_machine_lease_open(&m) == 0, "lease open fails while deleting");

	vmm_machine_init(&m);
	vmm_machine_lease_open(&m);
	vmm_machine_begin_delete(&m);
	CK(vmm_machine_lease_close(&m) == VMM_CLOSE_NONE, "no double delete");

	/* --- events --- */
	vmm_machine_init(&m);
	CKSTR(drain(&m), "created\nstopped\n", "events on create");

	vmm_machine_init(&m);
	drain(&m);
	CK(!vmm_machine_events_pending(&m), "drained no pending");
	vmm_machine_start(&m);
	vmm_machine_stop(&m, 0);
	CKSTR(drain(&m), "started\nstopped\n", "events lifecycle one-shot");

	vmm_machine_init(&m);
	drain(&m);
	vmm_machine_stop(&m, 0);
	CK(!vmm_machine_events_pending(&m), "idempotent stop no dup event");

	vmm_machine_init(&m);
	drain(&m);
	vmm_machine_begin_delete(&m);
	CKSTR(drain(&m), "deleted\n", "event delete");

	vmm_machine_init(&m);
	drain(&m);
	for (i = 0; i < 100; i++) {
		vmm_machine_start(&m);
		vmm_machine_stop(&m, 0);
	}
	n = vmm_machine_read_events(&m, buf, sizeof(buf) - 1);
	{
		int lines = 0;
		for (k = 0; k < n; k++)
			if (buf[k] == '\n')
				lines++;
		CK(lines > 0 && lines <= VMM_EVENT_CAP, "events lossy overflow bounded");
	}

	printf("VMM-UNIT: %d passed %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
