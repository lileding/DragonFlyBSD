/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Internal interface shared between the vmmfs control plane (vmmfs.c -- the
 * mount, the machine/device registry, vnode allocation, per-open buffers) and
 * the vnode operations (vmmfs_vnode.c -- the vop_ops handlers + table).
 *
 * Include the kernel headers (sys/param.h, vnode.h, lock.h, namecache.h,
 * uio.h, queue.h, malloc.h) and vmm_machine.h before this file.
 */
#ifndef VMMFS_H
#define VMMFS_H

MALLOC_DECLARE(M_VMMFS);

/*
 * Informational vnode tag.  vmmfs has no dedicated VT_ enum slot yet; VT_UNUSED7
 * is a reserved-unused value already present in the running kernel.
 */
#define VMMFS_VTAG		VT_UNUSED7

#define VMMFS_ROOT_INO		1
#define VMMFS_MACHINES_INO	2
#define VMMFS_MACHINE_INO_BASE	3
#define VMMFS_MACHINE_INO_STRIDE 16	/* room for the machine dir + configs */
#define VMMFS_MACHINE_DEV_OFF	9	/* devices/ ino = machine base + 9 */
#define VMMFS_HOST_INO		0x10000
#define VMMFS_HOST_DEV_INO	0x10001
#define VMMFS_DEVROOT_INO	0x10002
#define VMMFS_DEV_INO_BASE	0x20000	/* device i -> base + i */
#define VMMFS_DEVLINK_INO_BASE	0x30000	/* device i symlink -> base + i */

#define VMMFS_DIR_MODE		0555
#define VMMFS_MAX_MACHINES	64
#define VMMFS_NAME_MAX		63
#define VMMFS_OBUF_MAX		4096	/* a config register can't exceed this */

/* PCIe device passthrough (stub): a fixed pool of host devices, each owned by
 * a machine (or the host).  Binding is `mv` between devices/ dirs. */
#define VMMFS_OWNER_HOST	(-1)
#define VMMFS_MAX_DEVICES	8
#define VMMFS_BDF_MAX		31

enum vmmfs_ntype {
	VMMFS_NROOT,
	VMMFS_NMACHINES,
	VMMFS_NMACHINE,
	VMMFS_NCONFIG,
	VMMFS_NHOST,		/* machines/host/ (the physical host machine) */
	VMMFS_NDEVICES,		/* a devices/ directory */
	VMMFS_NDEVICE,		/* a device file (one PCIe BDF) */
	VMMFS_NDEVROOT,		/* /dev/vmm/devices/ (symlink index) */
	VMMFS_NDEVLINK,		/* a symlink in the index -> the owner's device */
};

/* Config files presented under a machine directory. */
enum vmmfs_cfg {
	VMMFS_CFG_VCPU,
	VMMFS_CFG_MEM,
	VMMFS_CFG_LOADER,
	VMMFS_CFG_LEASE,
	VMMFS_CFG_EVENTS,
	VMMFS_CFG_CONSOLE,
	VMMFS_CFG_STATUS,
	VMMFS_CFG_STOPPED,
	VMMFS_NCFG,
};

/* A per-open scratch buffer for a config register, keyed by struct file. */
struct vmmfs_openbuf {
	struct file		*ob_fp;
	char			*ob_data;
	int			 ob_len;
	int			 ob_cap;
	int			 ob_written;
	SLIST_ENTRY(vmmfs_openbuf) ob_link;
};

struct vmmfs_machine;

struct vmmfs_node {
	kobj_ops_t		ops;		/* KOBJ dispatch table; must be first */
	enum vmmfs_ntype	vn_type;
	enum vmmfs_cfg		vn_cfg;		/* valid for VMMFS_NCONFIG */
	ino_t			vn_ino;
	mode_t			vn_mode;
	int			vn_owner;	/* for NDEVICES: owner id it lists */
	struct vmmfs_node      *vn_parent;
	struct vmmfs_machine   *vn_machine;
	struct vnode	       *vn_vnode;
	struct lock		vn_interlock;
	SLIST_HEAD(, vmmfs_openbuf) vn_obufs;	/* register open buffers */
};

/* A PCIe device in the (stub) pool.  `owner` is the machine slot index it is
 * currently bound to, or VMMFS_OWNER_HOST. */
struct vmmfs_device {
	int			in_use;
	int			owner;
	int			is_host;	/* rm returns it to host vs deletes */
	char			bdf[VMMFS_BDF_MAX + 1];
	struct vmmfs_node	node;		/* the NDEVICE file */
	struct vmmfs_node	link;		/* its NDEVLINK in /vmm/devices/ */
};

#define VMMFS_DEV_OF_NODE(n) \
	((struct vmmfs_device *)((char *)(n) - __offsetof(struct vmmfs_device, node)))
#define VMMFS_DEV_OF_LINK(n) \
	((struct vmmfs_device *)((char *)(n) - __offsetof(struct vmmfs_device, link)))

struct vmmfs_machine {
	int			in_use;
	char			name[VMMFS_NAME_MAX + 1];
	struct vmm_machine	state;		/* config + lifecycle (vmm core) */
	struct vmmfs_node	node;
	struct vmmfs_node	cfg[VMMFS_NCFG];
	struct vmmfs_node	vn_devices;	/* this machine's devices/ */
};

struct vmmfs_mount {
	struct mount	       *vm_mp;
	struct vmmfs_node	vm_root;
	struct vmmfs_node	vm_machines;
	struct vmmfs_node	vm_host;	/* machines/host/ */
	struct vmmfs_node	vm_host_devices; /* machines/host/devices/ */
	struct vmmfs_node	vm_devroot;	/* /dev/vmm/devices/ symlink index */
	struct lock		vm_lock;
	struct vmmfs_machine	vm_mach[VMMFS_MAX_MACHINES];
	struct vmmfs_device	vm_dev[VMMFS_MAX_DEVICES];
};

#define VFS_TO_VMMFS(mp)	((struct vmmfs_mount *)((mp)->mnt_data))
#define VP_TO_VMMFS(vp)		((struct vmmfs_node *)((vp)->v_data))

extern const char *const vmmfs_cfg_name[VMMFS_NCFG];
extern struct vop_ops vmmfs_vnode_vops;

/*
 * Each node binds to a KOBJ class chosen by vmmfs_class_for().  vmm_legacy is
 * the catch-all for node types not yet split into their own object module.
 */
DECLARE_CLASS(vmm_base_class);		/* fallback commons (vmmfs_vnode.c) */
DECLARE_CLASS(vmm_root_class);		/* NROOT    (vmmfs.c) */
DECLARE_CLASS(vmm_device_class);	/* NDEVICE  (vmm_device.c) */
DECLARE_CLASS(vmm_devlink_class);	/* NDEVLINK (vmm_device.c) */
DECLARE_CLASS(vmm_machine_class);	/* NMACHINE  (vmm_machine.c) */
DECLARE_CLASS(vmm_machines_class);	/* NMACHINES (vmmfs_machines.c) */
DECLARE_CLASS(vmm_host_class);		/* NHOST    (vmm_host.c) */
DECLARE_CLASS(vmm_devices_class);	/* NDEVICES (vmmfs_devices.c) */
DECLARE_CLASS(vmm_devroot_class);	/* NDEVROOT (vmmfs_devices.c) */
DECLARE_CLASS(vmm_vcpu_class);		/* NCONFIG vcpu    (vmm_vcpu.c) */
DECLARE_CLASS(vmm_mem_class);		/* NCONFIG mem     (vmm_mem.c) */
DECLARE_CLASS(vmm_loader_class);	/* NCONFIG loader  (vmm_loader.c) */
DECLARE_CLASS(vmm_console_class);	/* NCONFIG console (vmm_console.c) */
DECLARE_CLASS(vmm_lease_class);		/* NCONFIG lease   (vmm_machine.c) */
DECLARE_CLASS(vmm_events_class);	/* NCONFIG events  (vmm_machine.c) */
DECLARE_CLASS(vmm_status_class);	/* NCONFIG status  (vmm_machine.c) */
DECLARE_CLASS(vmm_stopped_class);	/* NCONFIG stopped (vmm_machine.c) */
kobj_class_t vmmfs_class_for(enum vmmfs_ntype type, enum vmmfs_cfg cfg);

/*
 * A config "register" file serializes one machine setting as text.  Each
 * register object (vcpu/mem/loader) supplies its own text + commit functions
 * to the shared register vops below; the open buffer machinery is generic.
 */
typedef size_t (*vmm_text_fn)(const struct vmm_machine *m, char *out, size_t cap);
typedef int (*vmm_commit_fn)(struct vmm_machine *m, const char *buf, size_t len);
int	vmmfs_register_getattr(struct vmmfs_node *node,
	    struct vop_getattr_args *ap, vmm_text_fn text);
int	vmmfs_register_read(struct vmmfs_node *node, struct vop_read_args *ap,
	    vmm_text_fn text);
int	vmmfs_register_write(struct vmmfs_node *node, struct vop_write_args *ap);
int	vmmfs_register_open(struct vmmfs_node *node, struct vop_open_args *ap);
int	vmmfs_register_close(struct vmmfs_node *node, struct vop_close_args *ap,
	    vmm_commit_fn commit);
int	vmmfs_zero_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap);
int	vmmfs_zero_read(struct vmmfs_node *node, struct vop_read_args *ap);

/* Common vops shared across node classes (vmmfs_vnode.c for now). */
int	vmmnode_access(struct vmmfs_node *node, struct vop_access_args *ap);
int	vmmnode_setattr(struct vmmfs_node *node, struct vop_setattr_args *ap);
int	vmmnode_open(struct vmmfs_node *node, struct vop_open_args *ap);
int	vmmnode_close(struct vmmfs_node *node, struct vop_close_args *ap);
int	vmmnode_inactive(struct vmmfs_node *node, struct vop_inactive_args *ap);
int	vmmnode_reclaim(struct vmmfs_node *node, struct vop_reclaim_args *ap);
int	vmmnode_print(struct vmmfs_node *node, struct vop_print_args *ap);
int	vmmnode_nlookupdotdot(struct vmmfs_node *node,
	    struct vop_nlookupdotdot_args *ap);

/* Shared node attribute / directory helpers (vmmfs.c). */
void	vmmfs_fill_attr(struct vmmfs_node *node, struct vattr *vap,
	    enum vtype type, int nlink, off_t size);
int	vmmfs_dir_getattr(struct vmmfs_node *node, struct vop_getattr_args *ap);
int	vmmfs_nresolve_finish(struct vnode *dvp, struct vmmfs_node *child,
	    struct nchandle *nch);
int	vmmfs_readdir_dots(struct vop_readdir_args *ap, struct vmmfs_node *node,
	    off_t *offp, int *fullp);
int	vmmfs_readdir_end(struct vop_readdir_args *ap, off_t off, int full,
	    int error);

/* vmmfs.c (control plane / registry / vnode binding / per-open buffers),
 * called by the vnode operations in vmmfs_vnode.c. */
int	vmmfs_cfg_present(struct vmmfs_machine *m, enum vmmfs_cfg cfg);
ino_t	vmmfs_parent_ino(struct vmmfs_node *node);
int	vmmfs_alloc_vp(struct mount *mp, struct vmmfs_node *node, int lkflag,
	    struct vnode **vpp);
int	vmmfs_obuf_write(struct vmmfs_node *node, struct file *fp,
	    struct uio *uio);
void	vmmfs_obuf_drain(struct vmmfs_node *node);
void	vmmfs_machine_mark_deleted(struct vmmfs_mount *vmp,
	    struct vmmfs_machine *m);
int	vmmfs_validate_loader(struct vmmfs_machine *m, struct ucred *cred);
struct vmmfs_device *vmmfs_find_device(struct vmmfs_mount *vmp, int owner,
	    const char *name, int nlen);
struct vmmfs_device *vmmfs_find_device_any(struct vmmfs_mount *vmp,
	    const char *name, int nlen);
int	vmmfs_devlink_target(struct vmmfs_mount *vmp, struct vmmfs_device *d,
	    char *buf, size_t bufsize);
int	vmmfs_device_format(struct vmmfs_device *d, char *buf, size_t bufsize);

#endif /* VMMFS_H */
