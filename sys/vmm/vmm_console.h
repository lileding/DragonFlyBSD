/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The guest serial console object.  It retains a bounded snapshot of guest
 * output for the filesystem console file.  Host input writes are accepted and
 * counted, but not yet delivered to the guest.  FS presentation:
 * vmmfs_console.c.
 *
 * Types (off_t/uint64_t/size_t) and struct lwkt_token come from the includer.
 */
#ifndef VMM_CONSOLE_H
#define VMM_CONSOLE_H

#define VMM_CONSOLE_RING_SIZE	8192

struct vmm_console {
	/*
	 * Lock map:
	 * token_console protects all mut_ fields below.  Readers copy a
	 * bounded snapshot while holding the token, then vmmfs performs
	 * uiomove after the token is released.  Guest output appends from the
	 * vCPU thread, independent of the machine lifecycle token.
	 */
	struct lwkt_token token_console;
	uint64_t	mut_host_tx_bytes;
	uint64_t	mut_guest_rx_bytes;
	uint64_t	mut_guest_drop_bytes;
	size_t		mut_ring_start;
	size_t		mut_ring_len;
	char		mut_ring[VMM_CONSOLE_RING_SIZE];
};

void	vmm_console_init(struct vmm_console *c);
/* Clear guest output for a new execution epoch. */
void	vmm_console_reset(struct vmm_console *c);
/* Guest output snapshot exposed through machines/<name>/console. */
size_t	vmm_console_read(struct vmm_console *c, off_t off, char *out,
	    size_t cap);
/* Bytes produced by the guest's emulated console device. */
void	vmm_console_guest_write(struct vmm_console *c, const char *buf,
	    size_t len);
/* Host input to the guest: accepted and counted, not yet delivered. */
void	vmm_console_write(struct vmm_console *c, const char *buf, size_t len);

#endif /* VMM_CONSOLE_H */
