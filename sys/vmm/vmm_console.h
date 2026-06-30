/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The guest serial console object.  It exposes machines/<name>/console as a
 * terminal stream: guest output is retained in a bounded sequence ring, and
 * host input is a bounded FIFO consumed by the emulated serial device.  FS
 * presentation: vmmfs_console.c.
 *
 * Types (off_t/uint64_t/size_t) and struct lwkt_token come from the includer.
 */
#ifndef VMM_CONSOLE_H
#define VMM_CONSOLE_H

#define VMM_CONSOLE_RING_SIZE	8192
#define VMM_CONSOLE_INPUT_SIZE	1024

struct vmm_console {
	/*
	 * Lock map:
	 * token_console protects all mut_ fields below.  Blocking readers sleep
	 * on mut_output_tail_seq.  Blocking host writers sleep on mut_input_len.
	 * Guest output and input consumption happen from the vCPU thread,
	 * independent of the machine lifecycle token.
	 */
	struct lwkt_token token_console;
	uint64_t	mut_host_tx_bytes;
	uint64_t	mut_host_drop_bytes;
	uint64_t	mut_guest_rx_bytes;
	uint64_t	mut_guest_drop_bytes;
	uint64_t	mut_output_head_seq;
	uint64_t	mut_output_tail_seq;
	char		mut_ring[VMM_CONSOLE_RING_SIZE];
	size_t		mut_input_start;
	size_t		mut_input_len;
	char		mut_input[VMM_CONSOLE_INPUT_SIZE];
};

void	vmm_console_init(struct vmm_console *c);
/* Clear guest output for a new execution epoch. */
void	vmm_console_reset(struct vmm_console *c);
/* Guest output stream exposed through machines/<name>/console. */
int	vmm_console_read(struct vmm_console *c, off_t *offp, char *out,
	    size_t cap, int nonblock, size_t *copiedp);
int	vmm_console_wait_output(struct vmm_console *c, off_t off);
/* Bytes produced by the guest's emulated console device. */
void	vmm_console_guest_write(struct vmm_console *c, const char *buf,
	    size_t len);
/* Host input to the guest. */
int	vmm_console_write(struct vmm_console *c, const char *buf, size_t len,
	    int nonblock, size_t *copiedp);
/* Bytes available for the guest's emulated console device. */
size_t	vmm_console_guest_pending(struct vmm_console *c);
/* Consume one host-provided byte from the guest's emulated console device. */
int	vmm_console_guest_read(struct vmm_console *c, char *out);
/* Drop host-provided input, used by the guest UART FIFO reset bit. */
void	vmm_console_guest_reset_input(struct vmm_console *c);

#endif /* VMM_CONSOLE_H */
