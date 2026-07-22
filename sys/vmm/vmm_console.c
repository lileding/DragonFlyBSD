/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Guest serial console core -- see vmm_console.h.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <sys/tty.h>
#include <sys/ttydefaults.h>
#include <sys/thread.h>
#include <sys/thread2.h>
#include <machine/atomic.h>

#include "vmm_console.h"
#include "vmm_machine.h"

static d_open_t		vmm_console_dev_open;
static d_close_t	vmm_console_dev_close;
static d_ioctl_t	vmm_console_dev_ioctl;

static int	vmm_console_tty_param(struct tty *tp, struct termios *tio);
static void	vmm_console_tty_start(struct tty *tp);
static size_t	vmm_console_input_space(struct vmm_console *c);
static size_t	vmm_console_input_write(struct vmm_console *c, const char *buf,
		    size_t len);
static void	vmm_console_tty_kick(struct vmm_console *c);
static void	vmm_console_drain_task(void *arg, int pending);

static uint32_t vmm_console_dev_serial;

static struct dev_ops vmm_console_dev_ops = {
	{ "vmmconsole", 0, D_TTY | D_MPSAFE },
	.d_open =	vmm_console_dev_open,
	.d_close =	vmm_console_dev_close,
	.d_read =	ttyread,
	.d_write =	ttywrite,
	.d_ioctl =	vmm_console_dev_ioctl,
	.d_kqfilter =	ttykqfilter,
	.d_revoke =	ttyrevoke,
};

void
vmm_console_init(struct vmm_console *c)
{
	memset(c, 0, sizeof(*c));
	lwkt_token_init(&c->token_console, "vmmcons");
}

void
vmm_console_attach(struct vmm_console *c, const char *name,
    struct vmm_machine *machine)
{
	struct tty *tp;
	cdev_t dev;
	uint32_t unit;

	KKASSERT(c->own_mut_dev == NULL);
	KKASSERT(c->own_mut_tty == NULL);
	c->own_mut_drain_taskqueue = taskqueue_create("vmm_console", M_WAITOK,
	    taskqueue_thread_enqueue, &c->own_mut_drain_taskqueue);
	KKASSERT(c->own_mut_drain_taskqueue != NULL);
	TASK_INIT(&c->own_mut_drain_task, 0, vmm_console_drain_task, c);
	taskqueue_start_threads(&c->own_mut_drain_taskqueue, 1, TDPRI_KERN_DAEMON,
	    -1, "vmm console");
	c->own_mut_output = kmalloc(VMM_CONSOLE_OUTPUT_SIZE, M_TTYS, M_WAITOK);
	unit = atomic_fetchadd_int(&vmm_console_dev_serial, 1);
	tp = kmalloc(sizeof(*tp), M_TTYS, M_WAITOK | M_ZERO);
	ttyinit(tp);
	ttyregister(tp);
	dev = make_only_dev(&vmm_console_dev_ops, (int)unit, 0, 0, 0600,
	    "vmmconsole/%s", name);
	KKASSERT(dev != NULL);
	dev->si_drv1 = c;
	dev->si_tty = tp;
	tp->t_oproc = vmm_console_tty_start;
	tp->t_param = vmm_console_tty_param;
	tp->t_stop = nottystop;
	tp->t_dev = dev;
	c->own_mut_dev = dev;
	c->own_mut_tty = tp;
	c->borrow_mut_machine = machine;
}

void
vmm_console_detach(struct vmm_console *c)
{
	struct tty *tp = c->own_mut_tty;
	cdev_t dev = c->own_mut_dev;

	if (dev != NULL)
		dev_drevoke(dev);
	atomic_store_rel_int(&c->atomic_mut_open, 0);
	if (c->own_mut_drain_taskqueue != NULL) {
		taskqueue_drain(c->own_mut_drain_taskqueue, &c->own_mut_drain_task);
		taskqueue_free(c->own_mut_drain_taskqueue);
		c->own_mut_drain_taskqueue = NULL;
	}
	if (c->own_mut_output != NULL) {
		kfree(c->own_mut_output, M_TTYS);
		c->own_mut_output = NULL;
	}
	if (tp != NULL) {
		lwkt_gettoken(&tp->t_token);
		if (tp->t_state & TS_ISOPEN) {
			(*linesw[tp->t_line].l_close)(tp, 0);
			ttyclose(tp);
		}
		ttyunregister(tp);
		lwkt_reltoken(&tp->t_token);
	}
	if (dev != NULL) {
		dev->si_drv1 = NULL;
		dev->si_tty = NULL;
		destroy_dev(dev);
		c->own_mut_dev = NULL;
	}
	c->own_mut_tty = NULL;
	c->borrow_mut_machine = NULL;
	if (tp != NULL)
		kfree(tp, M_TTYS);
}

struct cdev *
vmm_console_dev(struct vmm_console *c)
{
	return c->own_mut_dev;
}

void
vmm_console_reset(struct vmm_console *c)
{
	struct tty *tp = c->own_mut_tty;
	int opened = 0;

	if (c->own_mut_drain_taskqueue != NULL)
		taskqueue_drain(c->own_mut_drain_taskqueue, &c->own_mut_drain_task);
	lwkt_gettoken(&c->token_console);
	c->mut_guest_rx_bytes = 0;
	c->mut_guest_drop_bytes = 0;
	c->mut_host_tx_bytes = 0;
	c->mut_host_drop_bytes += c->mut_input_len;
	c->mut_input_start = 0;
	c->mut_input_len = 0;
	c->mut_guest_drop_bytes += c->mut_output_len;
	c->mut_output_start = 0;
	c->mut_output_len = 0;
	lwkt_reltoken(&c->token_console);
	if (tp != NULL) {
		lwkt_gettoken(&tp->t_token);
		opened = (tp->t_state & TS_ISOPEN) != 0;
		lwkt_reltoken(&tp->t_token);
	}
	if (opened)
		ttyflush(tp, FREAD | FWRITE);
}

void
vmm_console_guest_write(struct vmm_console *c, const char *buf, size_t len)
{
	size_t copied = 0;
	int error;
	uint64_t accepted = 0;
	uint64_t dropped = 0;

	if (buf == NULL || len == 0)
		return;
	if (atomic_load_acq_int(&c->atomic_mut_open) == 0) {
		lwkt_gettoken(&c->token_console);
		c->mut_guest_drop_bytes += len;
		lwkt_reltoken(&c->token_console);
		return;
	}
	lwkt_gettoken(&c->token_console);
	while (copied < len && c->mut_output_len < VMM_CONSOLE_OUTPUT_SIZE) {
		size_t pos;

		pos = (c->mut_output_start + c->mut_output_len) %
		    VMM_CONSOLE_OUTPUT_SIZE;
		c->own_mut_output[pos] = buf[copied++];
		c->mut_output_len++;
	}
	accepted = copied;
	dropped = len - copied;
	c->mut_guest_rx_bytes += accepted;
	c->mut_guest_drop_bytes += dropped;
	lwkt_reltoken(&c->token_console);
	if (accepted == 0 || c->own_mut_drain_taskqueue == NULL)
		return;
	error = taskqueue_enqueue(c->own_mut_drain_taskqueue,
	    &c->own_mut_drain_task);
	if (error != 0) {
		lwkt_gettoken(&c->token_console);
		c->mut_guest_drop_bytes += c->mut_output_len;
		c->mut_output_start = 0;
		c->mut_output_len = 0;
		lwkt_reltoken(&c->token_console);
	}
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
	struct tty *tp = c->own_mut_tty;
	int available = 0;

	if (out == NULL)
		return 0;
	lwkt_gettoken(&c->token_console);
	if (c->mut_input_len != 0) {
		*out = c->own_mut_input[c->mut_input_start];
		c->mut_input_start = (c->mut_input_start + 1) %
		    VMM_CONSOLE_INPUT_SIZE;
		c->mut_input_len--;
		available = 1;
	}
	lwkt_reltoken(&c->token_console);
	if (available && tp != NULL)
		ttstart(tp);
	return available;
}

void
vmm_console_guest_reset_input(struct vmm_console *c)
{
	struct tty *tp = c->own_mut_tty;

	lwkt_gettoken(&c->token_console);
	c->mut_host_drop_bytes += c->mut_input_len;
	c->mut_input_start = 0;
	c->mut_input_len = 0;
	lwkt_reltoken(&c->token_console);
	if (tp != NULL)
		ttstart(tp);
}

static int
vmm_console_dev_open(struct dev_open_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct tty *tp = dev->si_tty;
	int error;

	if (dev->si_drv1 == NULL || tp == NULL)
		return ENXIO;
	lwkt_gettoken(&tp->t_token);
	if ((tp->t_state & TS_ISOPEN) == 0) {
		tp->t_state |= TS_CARR_ON | TS_CONNECTED;
		ttychars(tp);
		tp->t_iflag = 0;
		tp->t_oflag = 0;
		tp->t_cflag = CREAD | CS8 | CLOCAL;
		tp->t_lflag = 0;
		tp->t_ispeed = TTYDEF_SPEED;
		tp->t_ospeed = TTYDEF_SPEED;
		ttsetwater(tp);
	}
	error = (*linesw[tp->t_line].l_open)(dev, tp);
	lwkt_reltoken(&tp->t_token);
	if (error == 0)
		atomic_store_rel_int(&((struct vmm_console *)dev->si_drv1)->
		    atomic_mut_open, 1);
	return error;
}

static int
vmm_console_dev_close(struct dev_close_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct tty *tp = dev->si_tty;

	if (tp == NULL)
		return ENXIO;
	lwkt_gettoken(&tp->t_token);
	if (tp->t_state & TS_ISOPEN) {
		(*linesw[tp->t_line].l_close)(tp, ap->a_fflag);
		ttyclose(tp);
	}
	lwkt_reltoken(&tp->t_token);
	if (dev->si_drv1 != NULL)
		atomic_store_rel_int(&((struct vmm_console *)dev->si_drv1)->
		    atomic_mut_open, 0);
	return 0;
}

static int
vmm_console_dev_ioctl(struct dev_ioctl_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct tty *tp = dev->si_tty;
	int error;

	if (tp == NULL)
		return ENXIO;
	lwkt_gettoken(&tp->t_token);
	error = (*linesw[tp->t_line].l_ioctl)(tp, ap->a_cmd, ap->a_data,
	    ap->a_fflag, ap->a_cred);
	if (error == ENOIOCTL)
		error = ttioctl(tp, ap->a_cmd, ap->a_data, ap->a_fflag);
	lwkt_reltoken(&tp->t_token);
	return error == ENOIOCTL ? ENOTTY : error;
}

static int
vmm_console_tty_param(struct tty *tp, struct termios *tio)
{
	lwkt_gettoken(&tp->t_token);
	tp->t_ispeed = tio->c_ispeed;
	tp->t_ospeed = tio->c_ospeed;
	tp->t_cflag = tio->c_cflag;
	lwkt_reltoken(&tp->t_token);
	return 0;
}

static void
vmm_console_tty_start(struct tty *tp)
{
	struct vmm_console *c;
	char buf[64];
	size_t cap;
	int n;
	int kicked = 0;

	lwkt_gettoken(&tp->t_token);
	c = tp->t_dev != NULL ? tp->t_dev->si_drv1 : NULL;
	if (c == NULL || (tp->t_state & (TS_TIMEOUT | TS_TTSTOP)) != 0) {
		lwkt_reltoken(&tp->t_token);
		ttwwakeup(tp);
		return;
	}
	tp->t_state |= TS_BUSY;
	for (;;) {
		cap = vmm_console_input_space(c);
		if (cap == 0)
			break;
		if (cap > sizeof(buf))
			cap = sizeof(buf);
		n = clist_qtob(&tp->t_outq, buf, (int)cap);
		if (n <= 0)
			break;
		if (vmm_console_input_write(c, buf, (size_t)n) != 0)
			kicked = 1;
	}
	tp->t_state &= ~TS_BUSY;
	lwkt_reltoken(&tp->t_token);
	if (kicked)
		vmm_console_tty_kick(c);
	ttwwakeup(tp);
}

static size_t
vmm_console_input_space(struct vmm_console *c)
{
	size_t space;

	lwkt_gettoken(&c->token_console);
	space = VMM_CONSOLE_INPUT_SIZE - c->mut_input_len;
	lwkt_reltoken(&c->token_console);
	return space;
}

static size_t
vmm_console_input_write(struct vmm_console *c, const char *buf, size_t len)
{
	size_t copied = 0;

	lwkt_gettoken(&c->token_console);
	while (copied < len && c->mut_input_len < VMM_CONSOLE_INPUT_SIZE) {
		size_t pos;

		pos = (c->mut_input_start + c->mut_input_len) %
		    VMM_CONSOLE_INPUT_SIZE;
		c->own_mut_input[pos] = buf[copied++];
		c->mut_input_len++;
		c->mut_host_tx_bytes++;
	}
	c->mut_host_drop_bytes += len - copied;
	lwkt_reltoken(&c->token_console);
	return copied;
}

static void
vmm_console_tty_kick(struct vmm_console *c)
{
	struct vmm_machine *m = c->borrow_mut_machine;

	if (m != NULL)
		vmm_machine_console_input(m);
}

static void
vmm_console_drain_task(void *arg, int pending)
{
	struct vmm_console *c = arg;
	struct tty *tp;
	char ch;
	int deliver;

	(void)pending;
	for (;;) {
		if (atomic_load_acq_int(&c->atomic_mut_open) == 0) {
			lwkt_gettoken(&c->token_console);
			c->mut_guest_drop_bytes += c->mut_output_len;
			c->mut_output_start = 0;
			c->mut_output_len = 0;
			lwkt_reltoken(&c->token_console);
			return;
		}
		lwkt_gettoken(&c->token_console);
		if (c->mut_output_len == 0) {
			lwkt_reltoken(&c->token_console);
			return;
		}
		ch = c->own_mut_output[c->mut_output_start];
		c->mut_output_start = (c->mut_output_start + 1) %
		    VMM_CONSOLE_OUTPUT_SIZE;
		c->mut_output_len--;
		lwkt_reltoken(&c->token_console);
		tp = c->own_mut_tty;
		deliver = 0;
		if (tp != NULL) {
			lwkt_gettoken(&tp->t_token);
			if (atomic_load_acq_int(&c->atomic_mut_open) != 0 &&
			    (tp->t_state & TS_ISOPEN) != 0) {
				ttyinput((unsigned char)ch, tp);
				deliver = 1;
			}
			lwkt_reltoken(&tp->t_token);
		}
		if (!deliver) {
			lwkt_gettoken(&c->token_console);
			c->mut_guest_drop_bytes++;
			lwkt_reltoken(&c->token_console);
		}
	}
}
