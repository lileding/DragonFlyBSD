/*
 * Copyright (c) 2026
 *
 * Native asynchronous I/O control plane: ioport(2) / ioevent(2).
 *
 * Phase 1 stubs: the syscalls are registered and the ABI (record layouts and
 * opcode namespace) is frozen in <sys/ioport.h>, but the completion-port
 * implementation arrives in later phases.  Until then the syscalls fail with
 * ENOSYS so that a registered-but-unimplemented call is distinguishable from
 * an unregistered syscall number (which raises SIGSYS via sys_nosys()).
 *
 * See ioport-liburing-design.md for the design baseline.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysmsg.h>
#include <sys/sysproto.h>
#include <sys/ioport.h>

/*
 * ioport(2) -- create a completion port (pfd) with nworkers worker LWKTs.
 *
 * Phase 2 will create the per-port taskqueue and workers, allocate the
 * completion queue, and install the pfd fileops/kqfilter.
 */
int
sys_ioport(struct sysmsg *sysmsg, const struct ioport_args *uap)
{
	(void)sysmsg;
	(void)uap;
	return (ENOSYS);
}

/*
 * ioevent(2) -- submit a batch of requests and/or reap a batch of
 * completions, optionally waiting.
 *
 * Phase 3 will implement submit/reap; IO_CANCEL is a submit-array control
 * marker handled there.
 */
int
sys_ioevent(struct sysmsg *sysmsg, const struct ioevent_args *uap)
{
	(void)sysmsg;
	(void)uap;
	return (ENOSYS);
}
