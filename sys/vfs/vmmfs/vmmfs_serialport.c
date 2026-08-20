/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs 16550-compatible serial port.
 */
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <sys/tty.h>
#include <sys/ttydefaults.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include "vmmfs.h"
#include "vmmfs_serialport.h"
#include "vmmfs_serialroot.h"

#define VMMFS_SERIALPORT_MODE 0600

#define VMMFS_UART_RBR_THR_DLL 0
#define VMMFS_UART_IER_DLM 1
#define VMMFS_UART_IIR_FCR 2
#define VMMFS_UART_LCR 3
#define VMMFS_UART_MCR 4
#define VMMFS_UART_LSR 5
#define VMMFS_UART_MSR 6
#define VMMFS_UART_SCR 7

#define VMMFS_UART_IER_RDI 0x01
#define VMMFS_UART_IER_THRI 0x02
#define VMMFS_UART_FCR_ENABLE 0x01
#define VMMFS_UART_FCR_RX_RESET 0x02
#define VMMFS_UART_FCR_TX_RESET 0x04
#define VMMFS_UART_IIR_NOPEND 0x01
#define VMMFS_UART_IIR_THRI 0x02
#define VMMFS_UART_IIR_RDI 0x04
#define VMMFS_UART_LCR_DLAB 0x80
#define VMMFS_UART_MCR_DTR 0x01
#define VMMFS_UART_MCR_RTS 0x02
#define VMMFS_UART_MCR_OUT1 0x04
#define VMMFS_UART_MCR_OUT2 0x08
#define VMMFS_UART_MCR_LOOP 0x10
#define VMMFS_UART_LSR_DR 0x01
#define VMMFS_UART_LSR_OE 0x02
#define VMMFS_UART_LSR_THRE 0x20
#define VMMFS_UART_LSR_TEMT 0x40
#define VMMFS_UART_MSR_CTS 0x10
#define VMMFS_UART_MSR_DSR 0x20
#define VMMFS_UART_MSR_RI 0x40
#define VMMFS_UART_MSR_DCD 0x80

static d_open_t vmmfs_serialport_dev_open;
static d_close_t vmmfs_serialport_dev_close;
static d_ioctl_t vmmfs_serialport_dev_ioctl;

static int vmmfs_serialport_access(struct vop_access_args *);
static int vmmfs_serialport_getattr(struct vop_getattr_args *);
static int vmmfs_serialport_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_serialport_open(struct vop_open_args *);
static int vmmfs_serialport_close(struct vop_close_args *);
static int vmmfs_serialport_read(struct vop_read_args *);
static int vmmfs_serialport_write(struct vop_write_args *);
static int vmmfs_serialport_ioctl(struct vop_ioctl_args *);
static int vmmfs_serialport_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_serialport_reclaim(struct vop_reclaim_args *);
static int vmmfs_serialport_tty_param(struct tty *, struct termios *);
static void vmmfs_serialport_tty_start(struct tty *);
static void vmmfs_serialport_task(void *, int);
static int vmmfs_serialport_read_io(vmm_vcpu_t, void *,
    struct vmm_io_read *);
static int vmmfs_serialport_write_io(vmm_vcpu_t, void *,
    const struct vmm_io_write *);
static int vmmfs_serialport_associate(struct vmmfs_serialport *,
    struct vnode *);
static void vmmfs_serialport_schedule(struct vmmfs_serialport *);
static size_t vmmfs_serialport_input_space(struct vmmfs_serialport *);
static size_t vmmfs_serialport_input_write(struct vmmfs_serialport *,
    const char *, size_t);
static bool vmmfs_serialport_irq_pending_locked(
    const struct vmmfs_serialport *);
static void vmmfs_serialport_drain_output(struct vmmfs_serialport *);
static bool vmmfs_serialport_name(const char *, size_t, uint8_t *,
    uint16_t *, uint32_t *);

static uint32_t vmmfs_serialport_dev_serial;

static struct dev_ops vmmfs_serialport_dev_ops = {
	{ "vmmfs_serial", 0, D_TTY | D_MPSAFE },
	.d_open = vmmfs_serialport_dev_open,
	.d_close = vmmfs_serialport_dev_close,
	.d_read = ttyread,
	.d_write = ttywrite,
	.d_ioctl = vmmfs_serialport_dev_ioctl,
	.d_kqfilter = ttykqfilter,
	.d_revoke = ttyrevoke,
};

struct vop_ops vmmfs_serialport_vops = {
	.vop_default = vop_defaultop,
	.vop_access = vmmfs_serialport_access,
	.vop_close = vmmfs_serialport_close,
	.vop_getattr = vmmfs_serialport_getattr,
	.vop_getattr_lite = vmmfs_serialport_getattr_lite,
	.vop_ioctl = vmmfs_serialport_ioctl,
	.vop_kqfilter = vmmfs_serialport_kqfilter,
	.vop_open = vmmfs_serialport_open,
	.vop_pathconf = vop_stdpathconf,
	.vop_read = vmmfs_serialport_read,
	.vop_reclaim = vmmfs_serialport_reclaim,
	.vop_write = vmmfs_serialport_write,
};

int
vmmfs_serialport_compare(struct vmmfs_serialport *left,
	struct vmmfs_serialport *right)
{
	return strcmp(left->name, right->name);
}

RB_GENERATE(vmmfs_serialport_tree, vmmfs_serialport, entry,
	vmmfs_serialport_compare);

int
vmmfs_serialport_create(struct vmmfs_serialroot *serialroot,
	const char *name, size_t namelen, struct vmmfs_serialport **portp)
{
	struct vmmfs_mount *state;
	struct vmmfs_serialport *port;
	struct tty *tty;
	struct vnode *vnode;
	cdev_t dev;
	uint8_t number;
	uint16_t base;
	uint32_t gsi;
	uint32_t unit;
	int error;

	if (serialroot == NULL || serialroot->machine == NULL || portp == NULL ||
	    !vmmfs_serialport_name(name, namelen, &number, &base, &gsi))
		return EINVAL;
	state = (struct vmmfs_mount *)serialroot->machine->root->mount->mnt_data;
	if (state->serialport_vops == NULL)
		return ENXIO;
	*portp = NULL;
	port = kmalloc(sizeof(*port), M_VMMFS, M_WAITOK | M_ZERO);
	port->serialroot = serialroot;
	port->inode = atomic_fetchadd_int(&state->next_inode, 1);
	bcopy(name, port->name, namelen);
	port->name[namelen] = '\0';
	port->number = number;
	port->base = base;
	port->gsi = gsi;
	lwkt_token_init(&port->token, "vmmfsserial");
	port->output = kmalloc(VMMFS_SERIALPORT_OUTPUT_SIZE, M_TTYS,
	    M_WAITOK | M_ZERO);
	port->taskqueue = taskqueue_create("vmmfs_serial", M_WAITOK,
	    taskqueue_thread_enqueue, &port->taskqueue);
	if (port->taskqueue == NULL) {
		error = ENOMEM;
		goto fail_output;
	}
	TASK_INIT(&port->task, 0, vmmfs_serialport_task, port);
	taskqueue_start_threads(&port->taskqueue, 1, TDPRI_KERN_DAEMON, -1,
	    "vmmfs serial");
	tty = kmalloc(sizeof(*tty), M_TTYS, M_WAITOK | M_ZERO);
	ttyinit(tty);
	ttyregister(tty);
	unit = atomic_fetchadd_int(&vmmfs_serialport_dev_serial, 1);
	dev = make_only_dev(&vmmfs_serialport_dev_ops, (int)unit, 0, 0, 0600,
	    "vmmfs_serial/%s/%s", serialroot->machine->name, port->name);
	if (dev == NULL) {
		error = ENOMEM;
		goto fail_tty;
	}
	dev->si_drv1 = port;
	dev->si_tty = tty;
	tty->t_oproc = vmmfs_serialport_tty_start;
	tty->t_param = vmmfs_serialport_tty_param;
	tty->t_stop = nottystop;
	tty->t_dev = dev;
	port->dev = dev;
	port->tty = tty;
	error = getnewvnode(VT_SYNTH, serialroot->machine->root->mount, &vnode,
	    0, 0);
	if (error != 0)
		goto fail_dev;
	vnode->v_data = port;
	vnode->v_ops = &state->serialport_vops;
	vnode->v_type = VCHR;
	port->vnode = vnode;
	vmmfs_machine_hold(serialroot->machine);
	vx_downgrade(vnode);
	vn_unlock(vnode);
	*portp = port;
	return 0;

fail_dev:
	dev->si_drv1 = NULL;
	dev->si_tty = NULL;
	destroy_dev(dev);
fail_tty:
	ttyunregister(tty);
	kfree(tty, M_TTYS);
	taskqueue_free(port->taskqueue);
fail_output:
	kfree(port->output, M_TTYS);
	lwkt_token_uninit(&port->token);
	kfree(port, M_VMMFS);
	return error;
}

int
vmmfs_serialport_destroy(struct vmmfs_serialport *port)
{
	struct tty *tty;
	struct vnode *vnode;
	cdev_t dev;

	if (port == NULL)
		return EINVAL;
	lwkt_gettoken(&port->token);
	if (port->machine != NULL || port->stopping || port->destroying) {
		lwkt_reltoken(&port->token);
		return EBUSY;
	}
	port->destroying = true;
	dev = port->dev;
	tty = port->tty;
	vnode = port->vnode;
	lwkt_reltoken(&port->token);
	if (vnode != NULL)
		(void)vrevoke(vnode, proc0.p_ucred);
	lwkt_gettoken(&port->token);
	while (port->opening_count != 0) {
		tsleep_interlock(port, 0);
		if (port->opening_count != 0) {
			lwkt_reltoken(&port->token);
			(void)tsleep(port, PINTERLOCKED, "vmmserdestroy", 0);
			lwkt_gettoken(&port->token);
		}
	}
	lwkt_reltoken(&port->token);
	if (port->taskqueue != NULL) {
		taskqueue_drain(port->taskqueue, &port->task);
		taskqueue_free(port->taskqueue);
		port->taskqueue = NULL;
	}
	if (tty != NULL) {
		lwkt_gettoken(&tty->t_token);
		if (tty->t_state & TS_ISOPEN) {
			(*linesw[tty->t_line].l_close)(tty, 0);
			ttyclose(tty);
		}
		ttyunregister(tty);
		lwkt_reltoken(&tty->t_token);
	}
	if (dev != NULL) {
		dev->si_drv1 = NULL;
		dev->si_tty = NULL;
		destroy_dev(dev);
	}
	if (vnode != NULL) {
		vmmfs_vnode_revoke(vnode);
	}
	KKASSERT(port->vnode == NULL);
	port->tty = NULL;
	port->dev = NULL;
	port->serialroot = NULL;
	kfree(port->output, M_TTYS);
	lwkt_token_uninit(&port->token);
	kfree(port, M_VMMFS);
	return 0;
}

int
vmmfs_serialport_start(struct vmmfs_serialport *port, vmm_machine_t machine)
{
	int error;

	if (port == NULL || machine == NULL)
		return EINVAL;
	lwkt_gettoken(&port->token);
	if (port->machine != NULL || port->stopping || port->destroying) {
		lwkt_reltoken(&port->token);
		return EBUSY;
	}
	lwkt_reltoken(&port->token);
	error = vmm_machine_trap_pio_read(machine, port->base, 8,
	    vmmfs_serialport_read_io, port, &port->read_io);
	if (error != 0)
		return error;
	error = vmm_machine_trap_pio_write(machine, port->base, 8,
	    vmmfs_serialport_write_io, port, &port->write_io);
	if (error != 0) {
		(void)vmm_machine_untrap(machine, port->read_io);
		port->read_io = NULL;
		return error;
	}
	lwkt_gettoken(&port->token);
	port->machine = machine;
	lwkt_reltoken(&port->token);
	vmmfs_serialport_schedule(port);
	return 0;
}

int
vmmfs_serialport_stop(struct vmmfs_serialport *port)
{
	vmm_machine_t machine;
	vmm_io_t read_io;
	vmm_io_t write_io;

	if (port == NULL)
		return EINVAL;
	lwkt_gettoken(&port->token);
	if (port->machine == NULL) {
		lwkt_reltoken(&port->token);
		return 0;
	}
	port->stopping = true;
	machine = port->machine;
	lwkt_reltoken(&port->token);
	taskqueue_drain(port->taskqueue, &port->task);
	lwkt_gettoken(&port->token);
	read_io = port->read_io;
	write_io = port->write_io;
	port->read_io = NULL;
	port->write_io = NULL;
	port->machine = NULL;
	port->irq_asserted = false;
	port->stopping = false;
	lwkt_reltoken(&port->token);
	(void)vmm_machine_set_irq(machine, port->gsi, false);
	if (read_io != NULL)
		(void)vmm_machine_untrap(machine, read_io);
	if (write_io != NULL)
		(void)vmm_machine_untrap(machine, write_io);
	return 0;
}

static int
vmmfs_serialport_access(struct vop_access_args *ap)
{
	return vop_helper_access(ap, 0, 0, VMMFS_SERIALPORT_MODE, 0);
}

static int
vmmfs_serialport_getattr(struct vop_getattr_args *ap)
{
	struct vmmfs_serialport *port;
	struct vattr *vattr;
	int error;

	port = ap->a_vp->v_data;
	if (port == NULL)
		return ENOENT;
	error = vmmfs_serialport_associate(port, ap->a_vp);
	if (error != 0)
		return error;
	vattr = ap->a_vap;
	VATTR_NULL(vattr);
	vattr->va_type = VCHR;
	vattr->va_mode = VMMFS_SERIALPORT_MODE;
	vattr->va_nlink = 1;
	vattr->va_uid = 0;
	vattr->va_gid = 0;
	vattr->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];
	vattr->va_fileid = port->inode;
	vattr->va_size = 0;
	vattr->va_blocksize = PAGE_SIZE;
	vattr->va_bytes = 0;
	return 0;
}

static int
vmmfs_serialport_getattr_lite(struct vop_getattr_lite_args *ap)
{
	ap->a_lvap->va_type = VCHR;
	ap->a_lvap->va_mode = VMMFS_SERIALPORT_MODE;
	ap->a_lvap->va_nlink = 1;
	ap->a_lvap->va_uid = 0;
	ap->a_lvap->va_gid = 0;
	ap->a_lvap->va_size = 0;
	ap->a_lvap->va_flags = 0;
	return 0;
}

static int
vmmfs_serialport_open(struct vop_open_args *ap)
{
	struct vmmfs_serialport *port;
	struct vnode *vnode;
	cdev_t dev;
	int error;

	port = ap->a_vp->v_data;
	if (port == NULL)
		return ENOENT;
	vnode = ap->a_vp;
	lwkt_gettoken(&port->token);
	if (port->destroying) {
		lwkt_reltoken(&port->token);
		return ENOENT;
	}
	++port->opening_count;
	lwkt_reltoken(&port->token);
	error = vmmfs_serialport_associate(port, vnode);
	if (error != 0)
		goto done;
	dev = vnode->v_rdev;
	if (dev == NULL) {
		error = ENXIO;
		goto done;
	}
	if (dev->si_iosize_max == 0)
		dev->si_iosize_max = min(MAXPHYS, 64 * 1024);
	vsetflags(vnode, VISTTY | VNOTSEEKABLE);
	vn_unlock(vnode);
	error = dev_dopen(dev, ap->a_mode, S_IFCHR, ap->a_cred, ap->a_fpp,
	    vnode);
	vn_lock(vnode, LK_EXCLUSIVE | LK_RETRY);
	if (error == 0) {
		lwkt_gettoken(&port->token);
		if (port->destroying) {
			lwkt_reltoken(&port->token);
			vn_unlock(vnode);
			(void)dev_dclose(dev, ap->a_mode, S_IFCHR, *ap->a_fpp);
			vn_lock(vnode, LK_EXCLUSIVE | LK_RETRY);
			error = ENXIO;
		} else {
			lwkt_reltoken(&port->token);
			error = vop_stdopen(ap);
		}
	}
done:
	lwkt_gettoken(&port->token);
	KKASSERT(port->opening_count != 0);
	--port->opening_count;
	lwkt_reltoken(&port->token);
	wakeup(port);
	return error;
}

static int
vmmfs_serialport_close(struct vop_close_args *ap)
{
	struct vnode *vnode;
	cdev_t dev;
	int error;

	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	error = 0;
	if (dev != NULL && vnode->v_opencount <= 1) {
		vn_unlock(vnode);
		error = dev_dclose(dev, ap->a_fflag, S_IFCHR, ap->a_fp);
		vn_lock(vnode, LK_SHARED | LK_RETRY);
	}
	if (vnode->v_opencount > 0)
		vop_stdclose(ap);
	return error;
}

static int
vmmfs_serialport_read(struct vop_read_args *ap)
{
	struct vnode *vnode;
	cdev_t dev;
	int error;

	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	if (dev == NULL)
		return EBADF;
	if (ap->a_uio->uio_resid == 0)
		return 0;
	vn_unlock(vnode);
	error = dev_dread(dev, ap->a_uio, ap->a_ioflag, ap->a_fp);
	vn_lock(vnode, LK_SHARED | LK_RETRY);
	return error;
}

static int
vmmfs_serialport_write(struct vop_write_args *ap)
{
	struct vnode *vnode;
	cdev_t dev;
	int error;

	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	if (dev == NULL)
		return EBADF;
	if (ap->a_uio->uio_resid == 0)
		return 0;
	vn_unlock(vnode);
	error = dev_dwrite(dev, ap->a_uio, ap->a_ioflag, ap->a_fp);
	vn_lock(vnode, LK_EXCLUSIVE | LK_RETRY);
	return error;
}

static int
vmmfs_serialport_ioctl(struct vop_ioctl_args *ap)
{
	struct vnode *vnode;
	cdev_t dev;

	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	if (dev == NULL)
		return EBADF;
	return dev_dioctl(dev, ap->a_command, ap->a_data, ap->a_fflag,
	    ap->a_cred, ap->a_sysmsg, NULL);
}

static int
vmmfs_serialport_kqfilter(struct vop_kqfilter_args *ap)
{
	struct vnode *vnode;
	cdev_t dev;

	vnode = ap->a_vp;
	dev = vnode->v_rdev;
	if (dev == NULL)
		return EBADF;
	return dev_dkqfilter(dev, ap->a_kn, NULL);
}

static int
vmmfs_serialport_reclaim(struct vop_reclaim_args *ap)
{
	struct vmmfs_serialport *port;
	struct vmmfs_machine *machine;

	port = ap->a_vp->v_data;
	if (port != NULL) {
		machine = port->serialroot == NULL ? NULL :
		    port->serialroot->machine;
		lwkt_gettoken(&port->token);
		if (port->vnode == ap->a_vp)
			port->vnode = NULL;
		lwkt_reltoken(&port->token);
	} else {
		machine = NULL;
	}
	ap->a_vp->v_data = NULL;
	if (machine != NULL)
		vmmfs_machine_put(machine);
	return 0;
}

static int
vmmfs_serialport_tty_param(struct tty *tty, struct termios *termios)
{
	lwkt_gettoken(&tty->t_token);
	tty->t_ispeed = termios->c_ispeed;
	tty->t_ospeed = termios->c_ospeed;
	tty->t_cflag = termios->c_cflag;
	lwkt_reltoken(&tty->t_token);
	return 0;
}

static void
vmmfs_serialport_tty_start(struct tty *tty)
{
	struct vmmfs_serialport *port;
	char buffer[64];
	size_t capacity;
	int copied;
	bool scheduled;

	lwkt_gettoken(&tty->t_token);
	port = tty->t_dev == NULL ? NULL : tty->t_dev->si_drv1;
	if (port == NULL || (tty->t_state & (TS_TIMEOUT | TS_TTSTOP)) != 0) {
		lwkt_reltoken(&tty->t_token);
		ttwwakeup(tty);
		return;
	}
	tty->t_state |= TS_BUSY;
	scheduled = false;
	for (;;) {
		capacity = vmmfs_serialport_input_space(port);
		if (capacity == 0)
			break;
		if (capacity > sizeof(buffer))
			capacity = sizeof(buffer);
		copied = clist_qtob(&tty->t_outq, buffer, (int)capacity);
		if (copied <= 0)
			break;
		if (vmmfs_serialport_input_write(port, buffer,
		    (size_t)copied) != 0)
			scheduled = true;
	}
	tty->t_state &= ~TS_BUSY;
	lwkt_reltoken(&tty->t_token);
	if (scheduled)
		vmmfs_serialport_schedule(port);
	ttwwakeup(tty);
}

static void
vmmfs_serialport_task(void *argument, int pending)
{
	struct vmmfs_serialport *port;
	vmm_machine_t machine;
	bool asserted;

	(void)pending;
	port = argument;
	vmmfs_serialport_drain_output(port);
	lwkt_gettoken(&port->token);
	if (port->stopping || port->destroying || port->machine == NULL) {
		lwkt_reltoken(&port->token);
		return;
	}
	machine = port->machine;
	asserted = vmmfs_serialport_irq_pending_locked(port);
	if (port->irq_asserted == asserted) {
		lwkt_reltoken(&port->token);
		return;
	}
	port->irq_asserted = asserted;
	lwkt_reltoken(&port->token);
	(void)vmm_machine_set_irq(machine, port->gsi, asserted);
}

static int
vmmfs_serialport_read_io(vmm_vcpu_t vcpu, void *argument,
	struct vmm_io_read *read)
{
	struct vmmfs_serialport *port;
	struct tty *tty;
	uint64_t reg;
	uint8_t value;
	bool wake_tty;
	bool scheduled;

	(void)vcpu;
	port = argument;
	if (port == NULL || read->width != VMM_IO_WIDTH_8 ||
	    read->address < port->base || read->address >= port->base + 8)
		return ENOENT;
	reg = read->address - port->base;
	value = 0;
	wake_tty = false;
	scheduled = false;
	lwkt_gettoken(&port->token);
	if (port->machine == NULL || port->stopping || port->destroying) {
		lwkt_reltoken(&port->token);
		return ENOENT;
	}
	switch (reg) {
	case VMMFS_UART_RBR_THR_DLL:
		if ((port->lcr & VMMFS_UART_LCR_DLAB) != 0) {
			value = port->dll;
		} else if (port->input_length != 0) {
			value = port->input[port->input_start];
			port->input_start = (port->input_start + 1) %
			    VMMFS_SERIALPORT_FIFO_SIZE;
			--port->input_length;
			wake_tty = true;
			scheduled = true;
		}
		break;
	case VMMFS_UART_IER_DLM:
		value = (port->lcr & VMMFS_UART_LCR_DLAB) != 0 ?
		    port->dlm : port->ier;
		break;
	case VMMFS_UART_IIR_FCR:
		if ((port->ier & VMMFS_UART_IER_RDI) != 0 &&
		    port->input_length != 0) {
			value = VMMFS_UART_IIR_RDI;
		} else if ((port->ier & VMMFS_UART_IER_THRI) != 0 &&
		    port->thre_pending) {
			value = VMMFS_UART_IIR_THRI;
			port->thre_pending = false;
			scheduled = true;
		} else {
			value = VMMFS_UART_IIR_NOPEND;
		}
		break;
	case VMMFS_UART_LCR:
		value = port->lcr;
		break;
	case VMMFS_UART_MCR:
		value = port->mcr;
		break;
	case VMMFS_UART_LSR:
		value = VMMFS_UART_LSR_THRE | VMMFS_UART_LSR_TEMT;
		if (port->input_length != 0)
			value |= VMMFS_UART_LSR_DR;
		if (port->lsr_overrun) {
			value |= VMMFS_UART_LSR_OE;
			port->lsr_overrun = false;
		}
		break;
	case VMMFS_UART_MSR:
		if ((port->mcr & VMMFS_UART_MCR_LOOP) != 0) {
			if ((port->mcr & VMMFS_UART_MCR_RTS) != 0)
				value |= VMMFS_UART_MSR_CTS;
			if ((port->mcr & VMMFS_UART_MCR_DTR) != 0)
				value |= VMMFS_UART_MSR_DSR;
			if ((port->mcr & VMMFS_UART_MCR_OUT1) != 0)
				value |= VMMFS_UART_MSR_RI;
			if ((port->mcr & VMMFS_UART_MCR_OUT2) != 0)
				value |= VMMFS_UART_MSR_DCD;
		} else {
			value = VMMFS_UART_MSR_CTS | VMMFS_UART_MSR_DSR |
			    VMMFS_UART_MSR_DCD;
		}
		break;
	case VMMFS_UART_SCR:
		value = port->scr;
		break;
	default:
		lwkt_reltoken(&port->token);
		return ENOENT;
	}
	tty = port->tty;
	lwkt_reltoken(&port->token);
	read->value = value;
	if (wake_tty && tty != NULL)
		ttstart(tty);
	if (scheduled)
		vmmfs_serialport_schedule(port);
	return 0;
}

static int
vmmfs_serialport_write_io(vmm_vcpu_t vcpu, void *argument,
	const struct vmm_io_write *write)
{
	struct vmmfs_serialport *port;
	uint64_t reg;
	uint8_t value;
	bool scheduled;

	(void)vcpu;
	port = argument;
	if (port == NULL || write->width != VMM_IO_WIDTH_8 ||
	    write->address < port->base || write->address >= port->base + 8)
		return ENOENT;
	reg = write->address - port->base;
	value = (uint8_t)write->value;
	scheduled = false;
	lwkt_gettoken(&port->token);
	if (port->machine == NULL || port->stopping || port->destroying) {
		lwkt_reltoken(&port->token);
		return ENOENT;
	}
	switch (reg) {
	case VMMFS_UART_RBR_THR_DLL:
		if ((port->lcr & VMMFS_UART_LCR_DLAB) != 0) {
			port->dll = value;
		} else if (port->output_length < VMMFS_SERIALPORT_OUTPUT_SIZE) {
			port->output[(port->output_start + port->output_length) %
			    VMMFS_SERIALPORT_OUTPUT_SIZE] = value;
			++port->output_length;
			port->thre_pending = true;
			scheduled = true;
		}
		break;
	case VMMFS_UART_IER_DLM:
		if ((port->lcr & VMMFS_UART_LCR_DLAB) != 0) {
			port->dlm = value;
		} else {
			port->ier = value & 0x0f;
			if ((port->ier & VMMFS_UART_IER_THRI) != 0)
				port->thre_pending = true;
			else
				port->thre_pending = false;
			scheduled = true;
		}
		break;
	case VMMFS_UART_IIR_FCR:
		port->fcr = value & VMMFS_UART_FCR_ENABLE;
		if ((value & VMMFS_UART_FCR_RX_RESET) != 0) {
			port->input_start = 0;
			port->input_length = 0;
			port->lsr_overrun = false;
		}
		if ((value & VMMFS_UART_FCR_TX_RESET) != 0)
			port->thre_pending = false;
		scheduled = true;
		break;
	case VMMFS_UART_LCR:
		port->lcr = value;
		break;
	case VMMFS_UART_MCR:
		port->mcr = value;
		scheduled = true;
		break;
	case VMMFS_UART_SCR:
		port->scr = value;
		break;
	default:
		lwkt_reltoken(&port->token);
		return ENOENT;
	}
	lwkt_reltoken(&port->token);
	if (scheduled)
		vmmfs_serialport_schedule(port);
	return 0;
}

static int
vmmfs_serialport_associate(struct vmmfs_serialport *port,
	struct vnode *vnode)
{
	cdev_t dev;
	int error;

	if (vnode->v_rdev != NULL)
		return 0;
	lwkt_gettoken(&port->token);
	dev = port->dev;
	lwkt_reltoken(&port->token);
	if (dev == NULL)
		return ENXIO;
	error = v_associate_rdev(vnode, dev);
	if (error != 0)
		return error;
	vnode->v_umajor = dev->si_umajor;
	vnode->v_uminor = dev->si_uminor;
	return 0;
}

static void
vmmfs_serialport_schedule(struct vmmfs_serialport *port)
{
	struct taskqueue *taskqueue;

	lwkt_gettoken(&port->token);
	taskqueue = !port->stopping && !port->destroying && port->machine != NULL ?
	    port->taskqueue : NULL;
	lwkt_reltoken(&port->token);
	if (taskqueue != NULL)
		(void)taskqueue_enqueue(taskqueue, &port->task);
}

static size_t
vmmfs_serialport_input_space(struct vmmfs_serialport *port)
{
	size_t space;

	lwkt_gettoken(&port->token);
	space = VMMFS_SERIALPORT_FIFO_SIZE - port->input_length;
	lwkt_reltoken(&port->token);
	return space;
}

static size_t
vmmfs_serialport_input_write(struct vmmfs_serialport *port,
	const char *buffer, size_t length)
{
	size_t copied;

	copied = 0;
	lwkt_gettoken(&port->token);
	while (copied < length && port->input_length <
	    VMMFS_SERIALPORT_FIFO_SIZE) {
		port->input[(port->input_start + port->input_length) %
		    VMMFS_SERIALPORT_FIFO_SIZE] = buffer[copied++];
		++port->input_length;
	}
	if (copied != length)
		port->lsr_overrun = true;
	lwkt_reltoken(&port->token);
	return copied;
}

static bool
vmmfs_serialport_irq_pending_locked(const struct vmmfs_serialport *port)
{
	if ((port->mcr & VMMFS_UART_MCR_OUT2) == 0)
		return false;
	return ((port->ier & VMMFS_UART_IER_RDI) != 0 &&
	    port->input_length != 0) ||
	    ((port->ier & VMMFS_UART_IER_THRI) != 0 && port->thre_pending);
}

static void
vmmfs_serialport_drain_output(struct vmmfs_serialport *port)
{
	struct tty *tty;
	uint8_t byte;

	for (;;) {
		if (atomic_load_acq_int(&port->open) == 0)
			return;
		lwkt_gettoken(&port->token);
		if (port->output_length == 0) {
			lwkt_reltoken(&port->token);
			return;
		}
		byte = port->output[port->output_start];
		port->output_start = (port->output_start + 1) %
		    VMMFS_SERIALPORT_OUTPUT_SIZE;
		--port->output_length;
		tty = port->tty;
		lwkt_reltoken(&port->token);
		if (tty == NULL)
			return;
		lwkt_gettoken(&tty->t_token);
		if (atomic_load_acq_int(&port->open) != 0 &&
		    (tty->t_state & TS_ISOPEN) != 0)
			ttyinput(byte, tty);
		lwkt_reltoken(&tty->t_token);
	}
}

static bool
vmmfs_serialport_name(const char *name, size_t namelen, uint8_t *number,
	uint16_t *base, uint32_t *gsi)
{
	static const uint16_t bases[] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8 };
	static const uint32_t gsis[] = { 4, 3, 4, 3 };
	unsigned int index;

	if (name == NULL || namelen != sizeof("com1") - 1 ||
	    bcmp(name, "com", sizeof("com") - 1) != 0 ||
	    name[3] < '1' || name[3] > '4')
		return false;
	index = (unsigned int)(name[3] - '1');
	*number = (uint8_t)(index + 1);
	*base = bases[index];
	*gsi = gsis[index];
	return true;
}

static int
vmmfs_serialport_dev_open(struct dev_open_args *ap)
{
	cdev_t dev;
	struct vmmfs_serialport *port;
	struct tty *tty;
	int error;

	dev = ap->a_head.a_dev;
	port = dev->si_drv1;
	tty = dev->si_tty;
	if (port == NULL || tty == NULL)
		return ENXIO;
	lwkt_gettoken(&tty->t_token);
	if ((tty->t_state & TS_ISOPEN) == 0) {
		tty->t_state |= TS_CARR_ON | TS_CONNECTED;
		ttychars(tty);
		tty->t_iflag = 0;
		tty->t_oflag = 0;
		tty->t_cflag = CREAD | CS8 | CLOCAL;
		tty->t_lflag = 0;
		tty->t_ispeed = TTYDEF_SPEED;
		tty->t_ospeed = TTYDEF_SPEED;
		ttsetwater(tty);
	}
	error = (*linesw[tty->t_line].l_open)(dev, tty);
	lwkt_reltoken(&tty->t_token);
	if (error == 0) {
		atomic_store_rel_int(&port->open, 1);
		vmmfs_serialport_schedule(port);
	}
	return error;
}

static int
vmmfs_serialport_dev_close(struct dev_close_args *ap)
{
	cdev_t dev;
	struct tty *tty;

	dev = ap->a_head.a_dev;
	tty = dev->si_tty;
	if (tty == NULL)
		return ENXIO;
	lwkt_gettoken(&tty->t_token);
	if (tty->t_state & TS_ISOPEN) {
		(*linesw[tty->t_line].l_close)(tty, ap->a_fflag);
		ttyclose(tty);
	}
	lwkt_reltoken(&tty->t_token);
	if (dev->si_drv1 != NULL)
		atomic_store_rel_int(&((struct vmmfs_serialport *)dev->si_drv1)->
		    open, 0);
	return 0;
}

static int
vmmfs_serialport_dev_ioctl(struct dev_ioctl_args *ap)
{
	cdev_t dev;
	struct tty *tty;
	int error;

	dev = ap->a_head.a_dev;
	tty = dev->si_tty;
	if (tty == NULL)
		return ENXIO;
	lwkt_gettoken(&tty->t_token);
	error = (*linesw[tty->t_line].l_ioctl)(tty, ap->a_cmd, ap->a_data,
	    ap->a_fflag, ap->a_cred);
	if (error == ENOIOCTL)
		error = ttioctl(tty, ap->a_cmd, ap->a_data, ap->a_fflag);
	lwkt_reltoken(&tty->t_token);
	return error == ENOIOCTL ? ENOTTY : error;
}
