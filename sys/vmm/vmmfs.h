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

#include "vmm_device.h"		/* struct vmm_device (wrapped by vmmfs_device) */
#include "vmm_host.h"		/* struct vmm_host (the device pool source) */

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
#define VMMFS_NAME_MAX		63
#define VMMFS_OBUF_MAX		4096	/* a config register can't exceed this */

/*
 * Nodes carry no type tag.  Each binds a KOBJ class (its behavior), an
 * enum vtype (VDIR/VREG/VLNK), and a mode at creation time; dispatch is the
 * class, not a switch.  A machine's config files come from a descriptor table
 * in vmmfs_machines.c.
 */

/* A per-open scratch buffer for a config register, keyed by struct file. */
struct vmmfs_openbuf {
	struct file		*ob_fp;
	char			*ob_data;
	int			 ob_len;
	int			 ob_cap;
	int			 ob_written;
	SLIST_ENTRY(vmmfs_openbuf) ob_link;
};

struct vmmfs_machines;

struct vmmfs_node {
	kobj_ops_t		ops;		/* KOBJ dispatch table; must be first */
	enum vtype		vn_vtype;	/* VDIR / VREG / VLNK */
	ino_t			vn_ino;
	mode_t			vn_mode;
	struct vmmfs_node      *vn_parent;
	struct vmmfs_machines   *vn_machine;
	struct vnode	       *vn_vnode;
	struct lock		vn_interlock;
	SLIST_HEAD(, vmmfs_openbuf) vn_obufs;	/* register open buffers */
};

/*
 * A PCIe device in the (stub) pool, allocated on demand and kept in the
 * per-mount SLIST.  `owner` is the machine it is currently bound to, or NULL
 * for the host pool.  A host device is permanent (rm returns it to the pool);
 * a user backend is freed on rm.
 */
struct vmmfs_device {
	SLIST_ENTRY(vmmfs_device) dv_link;
	struct vmm_device	dev;		/* core: bdf, owner (vmm_machine*), is_host */
	struct vmmfs_node	node;		/* the NDEVICE file */
	struct vmmfs_node	link;		/* its NDEVLINK in /vmm/devices/ */
};

SLIST_HEAD(vmmfs_devlist, vmmfs_device);

#define VMMFS_DEV_OF_NODE(n) \
	((struct vmmfs_device *)((char *)(n) - __offsetof(struct vmmfs_device, node)))
#define VMMFS_DEV_OF_LINK(n) \
	((struct vmmfs_device *)((char *)(n) - __offsetof(struct vmmfs_device, link)))

/* Recover the fs slot from a core VM pointer (a device's owner is vmm_machine*). */
#define VMMFS_MACHINES_OF_STATE(s) \
	((struct vmmfs_machines *)((char *)(s) - __offsetof(struct vmmfs_machines, state)))
/* The core VM of an fs slot, or NULL for the host pool (NULL slot). */
#define VMMFS_STATE_OF(m)	((m) != NULL ? &(m)->state : NULL)

/*
 * A user VM: allocated on demand and kept in the per-mount RB tree keyed by
 * name.  vm_refs is the lifetime count: 1 while in the tree, plus one per live
 * vnode bound to any of its nodes.  rmdir drops the tree reference; the struct
 * is freed when vm_refs reaches 0, so a machine held by an open fd (e.g. a
 * lease) survives rmdir until its last vnode is reclaimed.
 */
struct vmmfs_machines {
	RB_ENTRY(vmmfs_machines)	vm_link;
	char			name[VMMFS_NAME_MAX + 1];
	int			vm_refs;
	int			vm_in_tree;	/* guards a single RB_REMOVE */
	struct vmm_machine	state;		/* config + lifecycle (vmm core) */
	struct vmmfs_node	node;		/* the machine directory */
	/* config files (one per cfg_table row in vmmfs_machines.c) */
	struct vmmfs_node	n_vcpu, n_mem, n_loader, n_console;
	struct vmmfs_node	n_lease, n_events, n_status, n_stopped;
	struct vmmfs_node	vn_devices;	/* this machine's devices/ */
};

RB_HEAD(vmmfs_machtree, vmmfs_machines);	/* ops: vmmfs_machines.h */

struct vmmfs_mount {
	struct mount	       *vm_mp;
	struct vmmfs_node	vm_root;
	struct vmmfs_node	vm_machines;
	struct vmmfs_node	vm_host;	/* machines/host/ */
	struct vmmfs_node	vm_host_devices; /* machines/host/devices/ */
	struct vmmfs_node	vm_devroot;	/* /dev/vmm/devices/ symlink index */
	struct lock		vm_lock;
	struct vmmfs_machtree	vm_machtree;	/* user VMs, keyed by name */
	ino_t			vm_next_ino;	/* monotonic machine ino allocator */
	struct vmm_host		host;		/* the physical host (device pool source) */
	struct vmmfs_devlist	vm_devs;	/* PCIe device pool (fs nodes) */
	int			vm_next_dev;	/* monotonic device ino index */
};

#define VFS_TO_VMMFS(mp)	((struct vmmfs_mount *)((mp)->mnt_data))
#define VP_TO_VMMFS(vp)		((struct vmmfs_node *)((vp)->v_data))

extern struct vop_ops vmmfs_vnode_vops;

/*
 * Each node binds a KOBJ class -- its behavior and vop dispatch -- chosen at
 * creation, not via a type switch.  The class IS the node's type.
 */
DECLARE_CLASS(vmmfs_base_class);		/* fallback commons (vmmfs_vnode.c) */
DECLARE_CLASS(vmmfs_root_class);		/* root dir       (vmmfs.c) */
DECLARE_CLASS(vmmfs_device_class);	/* device file    (vmmfs_device.c) */
DECLARE_CLASS(vmmfs_devlink_class);	/* device symlink (vmmfs_device.c) */
DECLARE_CLASS(vmmfs_machine_class);	/* a machine dir  (vmmfs_machines.c) */
DECLARE_CLASS(vmmfs_machines_class);	/* machines/      (vmmfs_machines.c) */
DECLARE_CLASS(vmmfs_host_class);		/* machines/host/ (vmmfs_host.c) */
DECLARE_CLASS(vmmfs_devices_class);	/* a devices/ dir (vmmfs_devices.c) */
DECLARE_CLASS(vmmfs_devroot_class);	/* /vmm/devices/  (vmmfs_devices.c) */
DECLARE_CLASS(vmmfs_vcpu_class);		/* vcpu file      (vmmfs_vcpu.c) */
DECLARE_CLASS(vmmfs_mem_class);		/* mem file       (vmmfs_mem.c) */
DECLARE_CLASS(vmmfs_loader_class);	/* loader file    (vmmfs_loader.c) */
DECLARE_CLASS(vmmfs_console_class);	/* console file   (vmmfs_console.c) */
DECLARE_CLASS(vmmfs_lease_class);		/* lease file     (vmmfs_machines.c) */
DECLARE_CLASS(vmmfs_events_class);	/* events file    (vmmfs_machines.c) */
DECLARE_CLASS(vmmfs_status_class);	/* status file    (vmmfs_machines.c) */
DECLARE_CLASS(vmmfs_stopped_class);	/* stopped file   (vmmfs_machines.c) */

/* Class identity test: is this node an instance of the given class? */
#define VMMFS_NODE_IS(node, classname)	((node)->ops == (classname).ops)

/*
 * A config "register" file serializes one machine setting as text.  Each
 * register object (vcpu/mem/loader) supplies its own text + commit functions
 * to the shared register vops below; the open buffer machinery is generic.
 */
typedef size_t (*vmmfs_text_fn)(const struct vmm_machine *m, char *out, size_t cap);
typedef int (*vmmfs_commit_fn)(struct vmm_machine *m, const char *buf, size_t len);
int	vmmfs_register_getattr(struct vmmfs_node *node,
	    struct vop_getattr_args *ap, vmmfs_text_fn text);
int	vmmfs_register_read(struct vmmfs_node *node, struct vop_read_args *ap,
	    vmmfs_text_fn text);
int	vmmfs_register_write(struct vmmfs_node *node, struct vop_write_args *ap);
int	vmmfs_register_open(struct vmmfs_node *node, struct vop_open_args *ap);
int	vmmfs_register_close(struct vmmfs_node *node, struct vop_close_args *ap,
	    vmmfs_commit_fn commit);
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
ino_t	vmmfs_parent_ino(struct vmmfs_node *node);
void	vmmfs_node_init(struct vmmfs_node *node, kobj_class_t class,
	    enum vtype vtype, mode_t mode, ino_t ino, struct vmmfs_node *parent,
	    struct vmmfs_machines *machine);
void	vmmfs_node_uninit(struct vmmfs_node *node);
int	vmmfs_alloc_vp(struct mount *mp, struct vmmfs_node *node, int lkflag,
	    struct vnode **vpp);
int	vmmfs_obuf_write(struct vmmfs_node *node, struct file *fp,
	    struct uio *uio);
void	vmmfs_obuf_drain(struct vmmfs_node *node);

/* Per-module interfaces (the machines registry, the device pool) live in their
 * own headers: vmmfs_machines.h and vmmfs_device.h. */

#endif /* VMMFS_H */
