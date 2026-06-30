/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The guest serial console object.  It owns a tty-backed cdev exposed by
 * vmmfs as machines/<name>/console.  Guest output enters the tty line
 * discipline; host output is staged in a small FIFO consumed by the guest's
 * emulated serial device.
 *
 * Types (uint64_t/size_t) and struct lwkt_token come from the includer.
 */
#ifndef VMM_CONSOLE_H
#define VMM_CONSOLE_H

#define VMM_CONSOLE_INPUT_SIZE	1024

struct cdev;
struct tty;
struct vmm_machine;

struct vmm_console {
	/*
	 * Lock map:
	 * token_console protects the guest RX FIFO and byte counters.  The tty
	 * object is protected by its own t_token.  Paths that need both locks
	 * take the tty token first, then token_console.
	 */
	struct cdev	*own_mut_dev;
	struct tty	*own_mut_tty;
	struct vmm_machine *borrow_mut_machine;
	struct lwkt_token token_console;
	uint64_t	mut_host_tx_bytes;
	uint64_t	mut_host_drop_bytes;
	uint64_t	mut_guest_rx_bytes;
	uint64_t	mut_guest_drop_bytes;
	size_t		mut_input_start;
	size_t		mut_input_len;
	char		mut_input[VMM_CONSOLE_INPUT_SIZE];
};

void	vmm_console_init(struct vmm_console *c);
void	vmm_console_attach(struct vmm_console *c, const char *name,
	    struct vmm_machine *machine);
void	vmm_console_detach(struct vmm_console *c);
struct cdev *vmm_console_dev(struct vmm_console *c);
/* Clear guest output for a new execution epoch. */
void	vmm_console_reset(struct vmm_console *c);
/* Bytes produced by the guest's emulated console device. */
void	vmm_console_guest_write(struct vmm_console *c, const char *buf,
	    size_t len);
/* Bytes available for the guest's emulated console device. */
size_t	vmm_console_guest_pending(struct vmm_console *c);
/* Consume one host-provided byte from the guest's emulated console device. */
int	vmm_console_guest_read(struct vmm_console *c, char *out);
/* Drop host-provided input, used by the guest UART FIFO reset bit. */
void	vmm_console_guest_reset_input(struct vmm_console *c);

#endif /* VMM_CONSOLE_H */
