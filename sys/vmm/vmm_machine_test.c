/*-
 * Host unit tests for the VMM core.  Builds and runs on the host (no kernel) --
 * this is the proof that the vmm_ core links with no VFS/KOBJ deps (so a future
 * kvm.ko can reuse it).  The config value objects (vcpu/mem/loader) are tested
 * through their own object API; lifecycle/lease/events through the machine API.
 *   cc vmm_parse.c vmm_vcpu.c vmm_mem.c vmm_loader.c vmm_machine.c \
 *      vmm_machine_test.c -o t && ./t
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
vcpu_s(struct vmm_machine *m)
{
	size_t n = vmm_vcpu_format(&m->vcpu, buf, sizeof(buf) - 1);
	buf[n] = 0;
	return buf;
}
static const char *
mem_s(struct vmm_machine *m)
{
	size_t n = vmm_mem_format(&m->mem, buf, sizeof(buf) - 1);
	buf[n] = 0;
	return buf;
}
static const char *
loader_s(struct vmm_machine *m)
{
	size_t n = vmm_loader_format(&m->loader, buf, sizeof(buf) - 1);
	buf[n] = 0;
	return buf;
}
static const char *
drain(struct vmm_machine *m)
{
	size_t n = vmm_machine_read_events(m, buf, sizeof(buf) - 1);
	buf[n] = 0;
	return buf;
}

int
main(void)
{
	struct vmm_machine m;
	int i;
	size_t n, k;

	/* --- vcpu object: parse + read-back --- */
	vmm_machine_init(&m);
	CK(vmm_vcpu_parse(&m.vcpu, "4\n", 2) == 1, "vcpu 4");
	CKSTR(vcpu_s(&m), "4\n", "vcpu readback 4");
	CK(vmm_vcpu_parse(&m.vcpu, "256", 3) == 1, "vcpu 256");
	CK(vmm_vcpu_parse(&m.vcpu, "257", 3) == 0, "vcpu 257 reject");
	CK(vmm_vcpu_parse(&m.vcpu, "0", 1) == 0, "vcpu 0 reject");
	CK(vmm_vcpu_parse(&m.vcpu, "x", 1) == 0, "vcpu x reject");

	/* --- mem object: parse + read-back --- */
	vmm_machine_init(&m);
	CK(vmm_mem_parse(&m.mem, "512M\n", 5) == 1, "mem 512M");
	CKSTR(mem_s(&m), "536870912\n", "mem readback");
	CK(vmm_mem_parse(&m.mem, "1G", 2) == 1, "mem 1G");
	CK(vmm_mem_parse(&m.mem, "1M", 2) == 0, "mem 1M unaligned reject");
	CK(vmm_mem_parse(&m.mem, "512Q", 4) == 0, "mem 512Q reject");
	CK(vmm_mem_parse(&m.mem, "0", 1) == 0, "mem 0 reject");

	/* --- config registers: unset / set / read-back / completeness --- */
	vmm_machine_init(&m);
	CKSTR(vcpu_s(&m), "", "vcpu unset empty");
	CKSTR(mem_s(&m), "", "mem unset empty");
	CKSTR(loader_s(&m), "", "loader unset empty");
	CK(!vmm_machine_config_complete(&m), "not complete unset");

	vmm_machine_init(&m);
	vmm_vcpu_parse(&m.vcpu, "4", 1);
	vmm_mem_parse(&m.mem, "512M", 4);
	CK(vmm_loader_parse(&m.loader, "/bin/sh\n", 8) == 1, "loader commit");
	CKSTR(loader_s(&m), "/bin/sh\n", "loader readback");
	CK(vmm_machine_config_complete(&m), "complete all three");

	/* invalid commit keeps old */
	vmm_machine_init(&m);
	vmm_vcpu_parse(&m.vcpu, "4", 1);
	CK(vmm_vcpu_parse(&m.vcpu, "0", 1) == 0, "invalid vcpu rejected");
	CKSTR(vcpu_s(&m), "4\n", "invalid vcpu kept old");
	CK(vmm_mem_parse(&m.mem, "3M", 2) == 0, "unaligned mem rejected");
	CKSTR(mem_s(&m), "", "mem never set stays empty");
	CK(vmm_loader_parse(&m.loader, "   ", 3) == 0, "empty loader rejected");

	vmm_machine_init(&m);
	vmm_vcpu_parse(&m.vcpu, "2", 1);
	CK(!vmm_machine_config_complete(&m), "incomplete after vcpu");
	vmm_mem_parse(&m.mem, "2M", 2);
	CK(!vmm_machine_config_complete(&m), "incomplete after mem");
	vmm_loader_parse(&m.loader, "/x", 2);
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
