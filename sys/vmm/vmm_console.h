/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The guest serial console object.  Stub for now: there is no serial ring yet,
 * so output reads return EOF and host input writes are accepted and discarded
 * (but counted).  Pure value + logic, no kernel/VFS deps -- reusable vmm_ core.
 * FS presentation: vmmfs_console.c.  When the ring lands it goes here, not in
 * the fs layer.
 *
 * Types (uint64_t/size_t) come from the includer.
 */
#ifndef VMM_CONSOLE_H
#define VMM_CONSOLE_H

struct vmm_console {
	uint64_t	tx_bytes;	/* host->guest bytes accepted (discarded until the ring exists) */
};

void	vmm_console_init(struct vmm_console *c);
/* Guest output: bytes produced (0 = none/EOF until the ring exists). */
size_t	vmm_console_read(struct vmm_console *c, char *out, size_t cap);
/* Host input to the guest: accepted (and, for now, discarded). */
void	vmm_console_write(struct vmm_console *c, const char *buf, size_t len);

#endif /* VMM_CONSOLE_H */
