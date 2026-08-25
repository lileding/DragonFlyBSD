/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DragonFly vmmfs 16550-compatible serial port.
 */
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include "vmmfs.h"
#include "vmmfs_serialport.h"
#include "vmmfs_serialroot.h"

#define VMMFS_SERIALPORT_MODE 0600
#define VMMFS_SERIALPORT_READ_SIZE 256

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
static d_read_t vmmfs_serialport_dev_read;
static d_write_t vmmfs_serialport_dev_write;
static d_ioctl_t vmmfs_serialport_dev_ioctl;
static d_kqfilter_t vmmfs_serialport_dev_kqfilter;
static d_revoke_t vmmfs_serialport_dev_revoke;

static int vmmfs_serialport_access(struct vop_access_args *);
static int vmmfs_serialport_getattr(struct vop_getattr_args *);
static int vmmfs_serialport_getattr_lite(struct vop_getattr_lite_args *);
static int vmmfs_serialport_open(struct vop_open_args *);
static int vmmfs_serialport_close(struct vop_close_args *);
static int vmmfs_serialport_read(struct vop_read_args *);
static int vmmfs_serialport_write(struct vop_write_args *);
static int vmmfs_serialport_ioctl(struct vop_ioctl_args *);
static int vmmfs_serialport_kqfilter(struct vop_kqfilter_args *);
static int vmmfs_serialport_inactive(struct vop_inactive_args *);
static int vmmfs_serialport_reclaim(struct vop_reclaim_args *);
static int vmmfs_serialport_read_io(vmm_vcpu_t, void *,
    struct vmm_io_read *);
static int vmmfs_serialport_write_io(vmm_vcpu_t, void *,
    const struct vmm_io_write *);
static int vmmfs_serialport_associate(struct vmmfs_serialport *,
    struct vnode *);
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
static void vmmfs_serialport_filter_detach(struct knote *);
static int vmmfs_serialport_filter_read(struct knote *, long);
static int vmmfs_serialport_filter_write(struct knote *, long);

static uint32_t vmmfs_serialport_dev_serial;

static struct filterops vmmfs_serialport_read_filterops = {
    FILTEROP_ISFD | FILTEROP_MPSAFE,
    NULL,
    vmmfs_serialport_filter_detach,
    vmmfs_serialport_filter_read,
};

static struct filterops vmmfs_serialport_write_filterops = {
    FILTEROP_ISFD | FILTEROP_MPSAFE,
    NULL,
    vmmfs_serialport_filter_detach,
    vmmfs_serialport_filter_write,
};

static struct dev_ops vmmfs_serialport_dev_ops = {
    { "vmmfs_serial", 0, D_MPSAFE },
    .d_open = vmmfs_serialport_dev_open,
    .d_close = vmmfs_serialport_dev_close,
    .d_read = vmmfs_serialport_dev_read,
    .d_write = vmmfs_serialport_dev_write,
    .d_ioctl = vmmfs_serialport_dev_ioctl,
    .d_kqfilter = vmmfs_serialport_dev_kqfilter,
    .d_revoke = vmmfs_serialport_dev_revoke,
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
    .vop_inactive = vmmfs_serialport_inactive,
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
    SLIST_INIT(&port->kq.ki_note);
    unit = atomic_fetchadd_int(&vmmfs_serialport_dev_serial, 1);
    dev = make_only_dev(&vmmfs_serialport_dev_ops, (int)unit, 0, 0, 0600,
        "vmmfs_serial/%s/%s", serialroot->machine->name, port->name);
    if (dev == NULL) {
        error = ENOMEM;
        goto fail_token;
    }
    dev->si_drv1 = port;
    port->dev = dev;
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
    destroy_dev(dev);
fail_token:
    lwkt_token_uninit(&port->token);
    kfree(port, M_VMMFS);
    return error;
}

int
vmmfs_serialport_destroy(struct vmmfs_serialport *port)
{
    cdev_t dev;

    if (port == NULL)
        return EINVAL;
    lwkt_gettoken(&port->token);
    if (port->machine != NULL || port->stopping || port->vnode != NULL) {
        lwkt_reltoken(&port->token);
        return EBUSY;
    }
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
    if (dev != NULL) {
        dev->si_drv1 = NULL;
        destroy_dev(dev);
    }
    port->dev = NULL;
    port->serialroot = NULL;
    lwkt_token_uninit(&port->token);
    kfree(port, M_VMMFS);
    return 0;
}

void
vmmfs_serialport_revoke(struct vmmfs_serialport *port)
{
    vmm_machine_t machine;
    bool deassert;

    if (port == NULL)
        return;
    machine = NULL;
    deassert = false;
    lwkt_gettoken(&port->token);
    port->closed = true;
    vmmfs_serialring_clear(&port->host_to_guest);
    vmmfs_serialring_clear(&port->guest_to_host);
    port->lsr_overrun = false;
    if (port->machine != NULL && port->irq_asserted) {
        machine = port->machine;
        port->irq_asserted = false;
        deassert = true;
    }
    lwkt_reltoken(&port->token);
    if (deassert)
        (void)vmm_machine_set_irq(machine, port->gsi, false);
    wakeup(&port->host_to_guest);
    wakeup(&port->guest_to_host);
    KNOTE(&port->kq.ki_note, 0);
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
    vattr->va_flags = 0;
    vattr->va_filerev = 0;
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
    if (port->destroying || port->closed) {
        lwkt_reltoken(&port->token);
        return ENXIO;
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
vmmfs_serialport_inactive(struct vop_inactive_args *ap)
{
    struct vmmfs_serialport *port;
    struct vmmfs_machine *machine;
    bool destroying;
    int error;

    port = ap->a_vp->v_data;
    if (port == NULL || port->serialroot == NULL)
        return 0;
    machine = port->serialroot->machine;
    lwkt_gettoken(&port->token);
    destroying = port->destroying && port->vnode == ap->a_vp;
    if (destroying)
        port->vnode = NULL;
    lwkt_reltoken(&port->token);
    if (destroying) {
        ap->a_vp->v_data = NULL;
        error = vmmfs_serialport_destroy(port);
        KKASSERT(error == 0);
        vmmfs_machine_put(machine);
        vrecycle(ap->a_vp);
        return 0;
    }
    if (!vmmfs_machine_vnode_detach(machine, &port->vnode, ap->a_vp))
        return 0;
    ap->a_vp->v_data = NULL;
    vmmfs_machine_put(machine);
    vrecycle(ap->a_vp);
    return 0;
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
    bool output_ready;
    bool update_irq;

    (void)vcpu;
    port = argument;
    if (port == NULL || write->width != VMM_IO_WIDTH_8 ||
        write->address < port->base || write->address >= port->base + 8)
        return ENOENT;
    reg = write->address - port->base;
    value = (uint8_t)write->value;
    output_ready = false;
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
            (void)vmmfs_serialring_write(&port->guest_to_host, value);
            port->thre_pending = true;
            output_ready = true;
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
    if (output_ready) {
        wakeup(&port->guest_to_host);
        KNOTE(&port->kq.ki_note, 0);
    }
    if (update_irq)
        vmmfs_serialport_irq_update(port);
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
    dev = port->closed ? NULL : port->dev;
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
    struct vmmfs_serialport *port;

    port = ap->a_head.a_dev->si_drv1;
    if (port == NULL)
        return ENXIO;
    lwkt_gettoken(&port->token);
    if (port->closed || port->destroying) {
        lwkt_reltoken(&port->token);
        return ENXIO;
    }
    lwkt_reltoken(&port->token);
    return 0;
}

static int
vmmfs_serialport_dev_close(struct dev_close_args *ap)
{
    (void)ap;
    return 0;
}

static int
vmmfs_serialport_dev_read(struct dev_read_args *ap)
{
    struct vmmfs_serialport *port;
    char buffer[VMMFS_SERIALPORT_READ_SIZE];
    size_t length;
    int error;

    port = ap->a_head.a_dev->si_drv1;
    if (port == NULL)
        return ENXIO;
    if (ap->a_uio->uio_resid == 0)
        return 0;
    for (;;) {
        lwkt_gettoken(&port->token);
        if (vmmfs_serialring_length(&port->guest_to_host) != 0)
            break;
        if (port->closed) {
            lwkt_reltoken(&port->token);
            return ENXIO;
        }
        if ((ap->a_ioflag & IO_NDELAY) != 0) {
            lwkt_reltoken(&port->token);
            return EWOULDBLOCK;
        }
        tsleep_interlock(&port->guest_to_host, PCATCH);
        lwkt_reltoken(&port->token);
        error = tsleep(&port->guest_to_host, PINTERLOCKED | PCATCH,
            "vmmserread", 0);
        if (error != 0)
            return error;
    }
    length = vmmfs_serialring_length(&port->guest_to_host);
    if (length > sizeof(buffer))
        length = sizeof(buffer);
    if (length > (size_t)ap->a_uio->uio_resid)
        length = (size_t)ap->a_uio->uio_resid;
    (void)vmmfs_serialring_read(&port->guest_to_host, buffer, length);
    lwkt_reltoken(&port->token);
    return uiomove(buffer, length, ap->a_uio);
}

static int
vmmfs_serialport_dev_write(struct dev_write_args *ap)
{
    struct vmmfs_serialport *port;
    char buffer[VMMFS_SERIALPORT_READ_SIZE];
    size_t length;
    size_t index;
    bool update_irq;
    int error;

    port = ap->a_head.a_dev->si_drv1;
    if (port == NULL)
        return ENXIO;
    while (ap->a_uio->uio_resid != 0) {
        length = min((size_t)ap->a_uio->uio_resid, sizeof(buffer));
        error = uiomove(buffer, length, ap->a_uio);
        if (error != 0)
            return error;
        update_irq = false;
        lwkt_gettoken(&port->token);
        if (port->closed) {
            lwkt_reltoken(&port->token);
            return ENXIO;
        }
        for (index = 0; index < length; ++index) {
            if (vmmfs_serialring_write(&port->host_to_guest,
                (uint8_t)buffer[index]))
                port->lsr_overrun = true;
        }
        update_irq = port->machine != NULL && !port->stopping &&
            !port->destroying;
        lwkt_reltoken(&port->token);
        if (update_irq)
            vmmfs_serialport_irq_update(port);
    }
    return 0;
}

static int
vmmfs_serialport_dev_ioctl(struct dev_ioctl_args *ap)
{
    (void)ap;
    return ENOTTY;
}

static int
vmmfs_serialport_dev_kqfilter(struct dev_kqfilter_args *ap)
{
    struct vmmfs_serialport *port;

    port = ap->a_head.a_dev->si_drv1;
    if (port == NULL)
        return ENXIO;
    ap->a_result = 0;
    switch (ap->a_kn->kn_filter) {
    case EVFILT_READ:
        ap->a_kn->kn_fop = &vmmfs_serialport_read_filterops;
        break;
    case EVFILT_WRITE:
        ap->a_kn->kn_fop = &vmmfs_serialport_write_filterops;
        break;
    default:
        ap->a_result = EOPNOTSUPP;
        return 0;
    }
    lwkt_gettoken(&port->token);
    ap->a_kn->kn_hook = (caddr_t)port;
    knote_insert(&port->kq.ki_note, ap->a_kn);
    lwkt_reltoken(&port->token);
    return 0;
}

static int
vmmfs_serialport_dev_revoke(struct dev_revoke_args *ap)
{
    vmmfs_serialport_revoke(ap->a_head.a_dev->si_drv1);
    return 0;
}

static void
vmmfs_serialport_filter_detach(struct knote *knote)
{
    struct vmmfs_serialport *port;

    port = (struct vmmfs_serialport *)knote->kn_hook;
    if (port == NULL)
        return;
    lwkt_gettoken(&port->token);
    knote_remove(&port->kq.ki_note, knote);
    lwkt_reltoken(&port->token);
}

static int
vmmfs_serialport_filter_read(struct knote *knote, long hint)
{
    struct vmmfs_serialport *port;

    (void)hint;
    port = (struct vmmfs_serialport *)knote->kn_hook;
    if (port == NULL)
        return 0;
    lwkt_gettoken(&port->token);
    if (port->closed) {
        knote->kn_data = 0;
        knote->kn_flags |= EV_EOF | EV_NODATA;
    } else {
        knote->kn_data = vmmfs_serialring_length(&port->guest_to_host);
    }
    lwkt_reltoken(&port->token);
    return knote->kn_data != 0 || (knote->kn_flags & EV_EOF) != 0;
}

static int
vmmfs_serialport_filter_write(struct knote *knote, long hint)
{
    struct vmmfs_serialport *port;

    (void)hint;
    port = (struct vmmfs_serialport *)knote->kn_hook;
    if (port == NULL)
        return 0;
    lwkt_gettoken(&port->token);
    if (port->closed) {
        knote->kn_data = 0;
        knote->kn_flags |= EV_EOF;
    } else {
        knote->kn_data = VMMFS_SERIALPORT_RING_SIZE;
    }
    lwkt_reltoken(&port->token);
    return knote->kn_data != 0 || (knote->kn_flags & EV_EOF) != 0;
}
