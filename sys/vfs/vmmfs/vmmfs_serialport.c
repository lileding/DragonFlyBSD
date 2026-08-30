/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs 16550-compatible serial port.
 */
#include <sys/conf.h>
#include <sys/caps.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/tty.h>
#include <sys/ttydefaults.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include "vmmfs.h"
#include "vmmfs_root.h"
#include "vmmfs_parent.h"
#include "vmmfs_machine.h"
#include "vmmfs_node.h"
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
static void vmmfs_serialport_tty_start(struct tty *);
static int vmmfs_serialport_tty_param(struct tty *, struct termios *);

static int vmmfs_serialport_open(struct vop_open_args *);
static int vmmfs_serialport_close(struct vop_close_args *);
static int vmmfs_serialport_read(struct vop_read_args *);
static int vmmfs_serialport_write(struct vop_write_args *);
static int vmmfs_serialport_ioctl(struct vop_ioctl_args *);
static int vmmfs_serialport_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_serialport_read_io(vmm_vcpu_t, void *,
    struct vmm_io_read *);
static int vmmfs_serialport_write_io(vmm_vcpu_t, void *,
    const struct vmm_io_write *);
static void vmmfs_serialport_drop(struct vmmfs_node *);
static void vmmfs_serialport_irq_update(struct vmmfs_serialport *);
static bool vmmfs_serialport_irq_pending_locked(
    const struct vmmfs_serialport *);
static bool vmmfs_serialport_name(const char *, size_t, uint8_t *,
    uint16_t *, uint32_t *);
static size_t vmmfs_serialring_length(const struct vmmfs_serialring *);
static bool vmmfs_serialring_write(struct vmmfs_serialring *, uint8_t);
static size_t vmmfs_serialring_read(struct vmmfs_serialring *, char *,
    size_t);
static void vmmfs_serialring_clear(struct vmmfs_serialring *);

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
    .vop_access = vmmfs_node_access,
    .vop_setattr = (void *)vop_null,
    .vop_close = vmmfs_serialport_close,
    .vop_getattr = vmmfs_node_getattr,
    .vop_getattr_lite = vmmfs_node_getattr_lite,
    .vop_ioctl = vmmfs_serialport_ioctl,
    .vop_kqfilter = vmmfs_serialport_kqfilter,
    .vop_open = vmmfs_serialport_open,
    .vop_pathconf = vop_stdpathconf,
    .vop_read = vmmfs_serialport_read,
    .vop_inactive = vmmfs_node_inactive,
    .vop_reclaim = vmmfs_node_reclaim,
    .vop_write = vmmfs_serialport_write,
};

int
vmmfs_serialport_create(struct vmmfs_serialroot *serialroot,
    const char *name, size_t namelen, struct vmmfs_serialport **portp,
    struct vnode **vnodep)
{
    struct vmmfs_mount *state;
    struct vmmfs_machine *machine;
    struct vmmfs_serialport *port;
    cdev_t dev;
    uint8_t number;
    uint16_t base;
    uint32_t gsi;
    uint32_t unit;
    int error;

    if (serialroot == NULL || vmmfs_serialroot_machine(serialroot) == NULL ||
        portp == NULL || vnodep == NULL ||
        !vmmfs_serialport_name(name, namelen, &number, &base, &gsi))
        return EINVAL;
    machine = vmmfs_serialroot_machine(serialroot);
    state = machine == NULL ? NULL : machine->mount;
    if (state == NULL || state->serialport_vops == NULL)
        return ENXIO;
    *portp = NULL;
    *vnodep = NULL;
    port = kmalloc(sizeof(*port), M_VMMFS, M_WAITOK | M_ZERO);
    port->inode = vmmfs_root_allocate_inode(vmmfs_machine_root(machine));
    bcopy(name, port->name, namelen);
    port->name[namelen] = '\0';
    port->number = number;
    port->base = base;
    port->gsi = gsi;
    lwkt_token_init(&port->token, "vmmfsserial");
    unit = atomic_fetchadd_int(&vmmfs_serialport_dev_serial, 1);
    dev = make_only_dev(&vmmfs_serialport_dev_ops, (int)unit, 0, 0, 0600,
        "vmmfs_serial/%s/%s", vmmfs_serialroot_machine(serialroot)->name,
        port->name);
    if (dev == NULL) {
        error = ENOMEM;
        goto fail_token;
    }
    dev->si_drv1 = port;
    dev->si_tty = &port->tty;
    ttyinit(&port->tty);
    ttyregister(&port->tty);
    port->tty.t_dev = dev;
    port->tty.t_oproc = vmmfs_serialport_tty_start;
    port->tty.t_stop = nottystop;
    port->tty.t_param = vmmfs_serialport_tty_param;
    port->dev = dev;
    vmmfs_node_setup(&port->node, &serialroot->branch,
        vmmfs_serialport_drop);
    vmmfs_node_set_metadata(&port->node, port->inode,
        VMMFS_SERIALPORT_MODE, 0);
    error = vmmfs_vnode_create_cdev(
        state->mount,
        &state->serialport_vops, port->dev, &port->node, vnodep);
    if (error != 0) {
        lwkt_gettoken(&port->token);
        port->destroying = true;
        lwkt_reltoken(&port->token);
        vmmfs_node_drop(&port->node);
        return error;
    }
    *portp = port;
    return 0;

fail_token:
    lwkt_token_uninit(&port->token);
    kfree(port, M_VMMFS);
    return error;
}

static void
vmmfs_serialport_drop(struct vmmfs_node *node)
{
    struct vmmfs_serialport *port;
    cdev_t dev;

    port = (struct vmmfs_serialport *)node;
    KKASSERT(port != NULL);
    lwkt_gettoken(&port->token);
    KKASSERT(port->destroying);
    KKASSERT(port->machine == NULL);
    KKASSERT(!port->stopping);
    port->destroying = true;
    dev = port->dev;
    lwkt_reltoken(&port->token);
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
    vmmfs_serialport_revoke(port);
    lwkt_gettoken(&port->tty.t_token);
    if ((port->tty.t_state & TS_ISOPEN) != 0) {
        (*linesw[port->tty.t_line].l_close)(&port->tty, 0);
        ttyclose(&port->tty);
    }
    ttyunregister(&port->tty);
    lwkt_reltoken(&port->tty.t_token);
    if (dev != NULL) {
        dev->si_tty = NULL;
        dev->si_drv1 = NULL;
        destroy_dev(dev);
    }
    port->dev = NULL;
    lwkt_token_uninit(&port->tty.t_token);
    lwkt_token_uninit(&port->token);
    vmmfs_node_parent_put(node);
    kfree(port, M_VMMFS);
}
void
vmmfs_serialport_revoke(struct vmmfs_serialport *port)
{
    cdev_t dev;
    vmm_machine_t machine;
    bool deassert;

    if (port == NULL)
        return;
    dev = NULL;
    machine = NULL;
    deassert = false;
    lwkt_gettoken(&port->token);
    port->closed = true;
    vmmfs_serialring_clear(&port->host_to_guest);
    port->lsr_overrun = false;
    dev = port->dev;
    if (port->machine != NULL && port->irq_asserted) {
        machine = port->machine;
        port->irq_asserted = false;
        deassert = true;
    }
    lwkt_reltoken(&port->token);
    if (deassert)
        (void)vmm_machine_set_irq(machine, port->gsi, false);
    if (dev != NULL)
        KKASSERT(dev_drevoke(dev) == 0);
}
int
vmmfs_serialport_start(struct vmmfs_serialport *port, vmm_machine_t machine)
{
    int error;

    if (port == NULL || machine == NULL)
        return EINVAL;
    lwkt_gettoken(&port->token);
    if (port->machine != NULL || port->stopping || port->destroying ||
        port->closed) {
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
    vmmfs_serialport_irq_update(port);
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
    if (port->destroying || port->closed) {
        lwkt_reltoken(&port->token);
        return ENXIO;
    }
    ++port->opening_count;
    lwkt_reltoken(&port->token);
    dev = vnode->v_rdev;
    if (dev == NULL) {
        error = ENXIO;
        goto done;
    }
    if (dev->si_iosize_max == 0)
        dev->si_iosize_max = min(MAXPHYS, 64 * 1024);
    vsetflags(vnode, VNOTSEEKABLE);
    vn_unlock(vnode);
    error = dev_dopen(dev, ap->a_mode, S_IFCHR, ap->a_cred, ap->a_fpp,
        vnode);
    vn_lock(vnode, LK_EXCLUSIVE | LK_RETRY);
    if (error == 0) {
        lwkt_gettoken(&port->token);
        if (port->destroying || port->closed) {
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
vmmfs_serialport_read_io(vmm_vcpu_t vcpu, void *argument,
    struct vmm_io_read *read)
{
    struct vmmfs_serialport *port;
    uint64_t reg;
    uint8_t value;
    bool update_irq;

    (void)vcpu;
    port = argument;
    if (port == NULL || read->width != VMM_IO_WIDTH_8 ||
        read->address < port->base || read->address >= port->base + 8)
        return ENOENT;
    reg = read->address - port->base;
    value = 0;
    update_irq = false;
    lwkt_gettoken(&port->token);
    if (port->machine == NULL || port->stopping || port->destroying ||
        port->closed) {
        lwkt_reltoken(&port->token);
        return ENOENT;
    }
    switch (reg) {
    case VMMFS_UART_RBR_THR_DLL:
        if ((port->lcr & VMMFS_UART_LCR_DLAB) != 0) {
            value = port->dll;
        } else if (vmmfs_serialring_length(&port->host_to_guest) != 0) {
            (void)vmmfs_serialring_read(&port->host_to_guest,
                (char *)&value, 1);
            update_irq = true;
        }
        break;
    case VMMFS_UART_IER_DLM:
        value = (port->lcr & VMMFS_UART_LCR_DLAB) != 0 ?
            port->dlm : port->ier;
        break;
    case VMMFS_UART_IIR_FCR:
        if ((port->ier & VMMFS_UART_IER_RDI) != 0 &&
            vmmfs_serialring_length(&port->host_to_guest) != 0) {
            value = VMMFS_UART_IIR_RDI;
        } else if ((port->ier & VMMFS_UART_IER_THRI) != 0 &&
            port->thre_pending) {
            value = VMMFS_UART_IIR_THRI;
            port->thre_pending = false;
            update_irq = true;
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
        if (vmmfs_serialring_length(&port->host_to_guest) != 0)
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
    lwkt_reltoken(&port->token);
    read->value = value;
    if (update_irq)
        vmmfs_serialport_irq_update(port);
    return 0;
}

static int
vmmfs_serialport_write_io(vmm_vcpu_t vcpu, void *argument,
    const struct vmm_io_write *write)
{
    struct vmmfs_serialport *port;
    uint64_t reg;
    uint8_t value;
    bool input_ready;
    bool update_irq;

    (void)vcpu;
    port = argument;
    if (port == NULL || write->width != VMM_IO_WIDTH_8 ||
        write->address < port->base || write->address >= port->base + 8)
        return ENOENT;
    reg = write->address - port->base;
    value = (uint8_t)write->value;
    input_ready = false;
    update_irq = false;
    lwkt_gettoken(&port->token);
    if (port->machine == NULL || port->stopping || port->destroying ||
        port->closed) {
        lwkt_reltoken(&port->token);
        return ENOENT;
    }
    switch (reg) {
    case VMMFS_UART_RBR_THR_DLL:
        if ((port->lcr & VMMFS_UART_LCR_DLAB) != 0) {
            port->dll = value;
        } else {
            port->thre_pending = true;
            input_ready = true;
            update_irq = true;
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
            update_irq = true;
        }
        break;
    case VMMFS_UART_IIR_FCR:
        port->fcr = value & VMMFS_UART_FCR_ENABLE;
        if ((value & VMMFS_UART_FCR_RX_RESET) != 0)
            port->lsr_overrun = false;
        if ((value & VMMFS_UART_FCR_TX_RESET) != 0)
            port->thre_pending = false;
        update_irq = true;
        break;
    case VMMFS_UART_LCR:
        port->lcr = value;
        break;
    case VMMFS_UART_MCR:
        port->mcr = value;
        update_irq = true;
        break;
    case VMMFS_UART_SCR:
        port->scr = value;
        break;
    default:
        lwkt_reltoken(&port->token);
        return ENOENT;
    }
    lwkt_reltoken(&port->token);
    if (input_ready) {
        lwkt_gettoken(&port->tty.t_token);
        if ((port->tty.t_state & TS_ISOPEN) != 0)
            (void)ttyinput(value, &port->tty);
        lwkt_reltoken(&port->tty.t_token);
    }
    if (update_irq)
        vmmfs_serialport_irq_update(port);
    return 0;
}

/*
 * Update the 16550 INT output from the UART state visible to the guest.
 */
static void
vmmfs_serialport_irq_update(struct vmmfs_serialport *port)
{
    vmm_machine_t machine;
    bool asserted;
    bool update;

    update = false;
    machine = NULL;
    asserted = false;
    lwkt_gettoken(&port->token);
    if (port->machine != NULL && !port->stopping && !port->destroying &&
        !port->closed) {
        asserted = vmmfs_serialport_irq_pending_locked(port);
        if (port->irq_asserted != asserted) {
            port->irq_asserted = asserted;
            machine = port->machine;
            update = true;
        }
    }
    lwkt_reltoken(&port->token);
    if (update)
        (void)vmm_machine_set_irq(machine, port->gsi, asserted);
}

static bool
vmmfs_serialport_irq_pending_locked(const struct vmmfs_serialport *port)
{
    if ((port->mcr & VMMFS_UART_MCR_OUT2) == 0)
        return false;
    return ((port->ier & VMMFS_UART_IER_RDI) != 0 &&
        vmmfs_serialring_length(&port->host_to_guest) != 0) ||
        ((port->ier & VMMFS_UART_IER_THRI) != 0 && port->thre_pending);
}

static size_t
vmmfs_serialring_length(const struct vmmfs_serialring *ring)
{
    KKASSERT(ring->write_seq >= ring->read_seq);
    KKASSERT(ring->write_seq - ring->read_seq <=
        VMMFS_SERIALPORT_RING_SIZE);
    return (size_t)(ring->write_seq - ring->read_seq);
}

static bool
vmmfs_serialring_write(struct vmmfs_serialring *ring, uint8_t value)
{
    bool overwritten;

    ring->data[ring->write_seq % VMMFS_SERIALPORT_RING_SIZE] = value;
    ++ring->write_seq;
    overwritten = false;
    if (ring->write_seq - ring->read_seq > VMMFS_SERIALPORT_RING_SIZE) {
        ring->read_seq = ring->write_seq - VMMFS_SERIALPORT_RING_SIZE;
        ++ring->dropped;
        overwritten = true;
    }
    return overwritten;
}

static size_t
vmmfs_serialring_read(struct vmmfs_serialring *ring, char *buffer,
    size_t length)
{
    size_t count;
    size_t index;

    count = vmmfs_serialring_length(ring);
    if (count > length)
        count = length;
    for (index = 0; index < count; ++index)
        buffer[index] = ring->data[(ring->read_seq + index) %
            VMMFS_SERIALPORT_RING_SIZE];
    ring->read_seq += count;
    return count;
}

static void
vmmfs_serialring_clear(struct vmmfs_serialring *ring)
{
    ring->read_seq = 0;
    ring->write_seq = 0;
    ring->dropped = 0;
    bzero(ring->data, sizeof(ring->data));
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
    lwkt_gettoken(&port->token);
    if (port->closed || port->destroying) {
        lwkt_reltoken(&port->token);
        return ENXIO;
    }
    lwkt_reltoken(&port->token);
    lwkt_gettoken(&tty->t_token);
    if ((tty->t_state & TS_ISOPEN) == 0) {
        tty->t_state |= TS_CARR_ON;
        ttychars(tty);
        tty->t_iflag = TTYDEF_IFLAG;
        tty->t_oflag = TTYDEF_OFLAG;
        tty->t_cflag = TTYDEF_CFLAG | CLOCAL;
        tty->t_lflag = TTYDEF_LFLAG;
        tty->t_ispeed = tty->t_ospeed = TTYDEF_SPEED;
        ttsetwater(tty);
    } else if ((tty->t_state & TS_XCLUDE) != 0 &&
        caps_priv_check(ap->a_cred, SYSCAP_RESTRICTEDROOT)) {
        lwkt_reltoken(&tty->t_token);
        return EBUSY;
    }
    error = (*linesw[tty->t_line].l_open)(dev, tty);
    lwkt_reltoken(&tty->t_token);
    return error;
}

static int
vmmfs_serialport_dev_close(struct dev_close_args *ap)
{
    struct tty *tty;
    int error;

    tty = ap->a_head.a_dev->si_tty;
    if (tty == NULL)
        return ENXIO;
    error = 0;
    lwkt_gettoken(&tty->t_token);
    if ((tty->t_state & TS_ISOPEN) != 0) {
        error = (*linesw[tty->t_line].l_close)(tty, ap->a_fflag);
        ttyclose(tty);
    }
    lwkt_reltoken(&tty->t_token);
    return error;
}

static int
vmmfs_serialport_dev_ioctl(struct dev_ioctl_args *ap)
{
    struct tty *tty;
    int error;

    tty = ap->a_head.a_dev->si_tty;
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
    int character;
    bool update_irq;

    update_irq = false;
    lwkt_gettoken(&tty->t_token);
    port = tty->t_dev == NULL ? NULL : tty->t_dev->si_drv1;
    if (port != NULL && (tty->t_state & (TS_TIMEOUT | TS_TTSTOP)) == 0) {
        tty->t_state |= TS_BUSY;
        lwkt_gettoken(&port->token);
        while ((character = clist_getc(&tty->t_outq)) >= 0) {
            if (!port->closed && !port->destroying &&
                vmmfs_serialring_write(&port->host_to_guest,
                (uint8_t)character))
                port->lsr_overrun = true;
        }
        update_irq = !port->closed && !port->destroying &&
            port->machine != NULL && !port->stopping;
        lwkt_reltoken(&port->token);
        tty->t_state &= ~TS_BUSY;
    }
    ttwwakeup(tty);
    lwkt_reltoken(&tty->t_token);
    if (update_irq)
        vmmfs_serialport_irq_update(port);
}
