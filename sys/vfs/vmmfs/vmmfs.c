/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vmmfs - DragonFlyBSD system VMM control filesystem.
 *
 * A machine is created with `mkdir machines/<name>`: it always starts stopped
 * with empty config.  The config files vcpu/mem/loader behave like hardware
 * registers — open() yields a per-open buffer holding the current DESIRED value
 * as text; read/write/seek act on that buffer; only close() atomically parses
 * the whole buffer and updates the desired value (invalid input is a no-op, the
 * old value is kept).  A successful write does NOT mean the value updated; only
 * reading it back is authoritative.  `rm stopped` starts the machine after
 * validating the config is complete and the loader path is executable in the
 * caller's context; `echo apic|force > stopped` stops it; `rmdir` deletes a
 * stopped machine.  Every operation is atomic or idempotent — no intermediate
 * state.  The loader is not yet executed (that is "vmm core").
 *
 * C owns the VFS/namecache plumbing, the per-open buffers, the loader-path
 * validation, and the fixed machine registry; Rust owns the config parsing,
 * desired-state registers, and the lifecycle/lease/event state machine.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namecache.h>
#include <sys/nlookup.h>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/queue.h>

extern int	vmmfs_rust_init(void);
extern void	vmmfs_rust_fini(void);

/* Rust-owned per-machine state object (config registers + lifecycle). */
struct vmmfs_machine_state;
extern struct vmmfs_machine_state *vmmfs_machine_new(void);
extern void	vmmfs_machine_free(struct vmmfs_machine_state *m);
extern int	vmmfs_machine_commit_vcpu(struct vmmfs_machine_state *m,
		    const uint8_t *buf, size_t len);
extern int	vmmfs_machine_commit_mem(struct vmmfs_machine_state *m,
		    const uint8_t *buf, size_t len);
extern int	vmmfs_machine_commit_loader(struct vmmfs_machine_state *m,
		    const uint8_t *buf, size_t len);
extern size_t	vmmfs_machine_vcpu_text(struct vmmfs_machine_state *m,
		    uint8_t *buf, size_t cap);
extern size_t	vmmfs_machine_mem_text(struct vmmfs_machine_state *m,
		    uint8_t *buf, size_t cap);
extern size_t	vmmfs_machine_loader_text(struct vmmfs_machine_state *m,
		    uint8_t *buf, size_t cap);
extern size_t	vmmfs_machine_loader_path(struct vmmfs_machine_state *m,
		    uint8_t *buf, size_t cap);
extern int	vmmfs_machine_config_complete(struct vmmfs_machine_state *m);
extern int	vmmfs_machine_is_stopped(struct vmmfs_machine_state *m);
extern void	vmmfs_machine_stop(struct vmmfs_machine_state *m, int force);
extern void	vmmfs_machine_start(struct vmmfs_machine_state *m);
extern int	vmmfs_machine_is_deleting(struct vmmfs_machine_state *m);
extern int	vmmfs_machine_lease_open(struct vmmfs_machine_state *m);
extern int	vmmfs_machine_lease_close(struct vmmfs_machine_state *m);
extern int	vmmfs_machine_begin_delete(struct vmmfs_machine_state *m);
extern int	vmmfs_machine_events_pending(struct vmmfs_machine_state *m);
extern size_t	vmmfs_machine_read_events(struct vmmfs_machine_state *m,
		    uint8_t *buf, size_t cap);

MALLOC_DEFINE(M_VMMFS, "vmmfs", "vmmfs mount structures");

/* kmalloc wrappers backing the Rust side's raw state objects. */
void *vmmfs_kalloc(size_t size);
void vmmfs_kfree(void *ptr);

void *
vmmfs_kalloc(size_t size)
{
	return kmalloc(size, M_VMMFS, M_WAITOK | M_ZERO);
}

void
vmmfs_kfree(void *ptr)
{
	kfree(ptr, M_VMMFS);
}

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

static const char *const vmmfs_cfg_name[VMMFS_NCFG] = {
	[VMMFS_CFG_VCPU] =	"vcpu",
	[VMMFS_CFG_MEM] =	"mem",
	[VMMFS_CFG_LOADER] =	"loader",
	[VMMFS_CFG_LEASE] =	"lease",
	[VMMFS_CFG_EVENTS] =	"events",
	[VMMFS_CFG_CONSOLE] =	"console",
	[VMMFS_CFG_STATUS] =	"status.tar.gz",
	[VMMFS_CFG_STOPPED] =	"stopped",
};

/* The three desired-state registers use the per-open buffer + commit-on-close. */
static int
vmmfs_cfg_is_register(enum vmmfs_cfg cfg)
{
	return cfg == VMMFS_CFG_VCPU || cfg == VMMFS_CFG_MEM ||
	    cfg == VMMFS_CFG_LOADER;
}

static mode_t
vmmfs_cfg_mode(enum vmmfs_cfg cfg)
{
	switch (cfg) {
	case VMMFS_CFG_VCPU:
	case VMMFS_CFG_MEM:
	case VMMFS_CFG_LOADER:
	case VMMFS_CFG_CONSOLE:
	case VMMFS_CFG_STOPPED:
		return 0644;
	case VMMFS_CFG_LEASE:
	case VMMFS_CFG_EVENTS:
	case VMMFS_CFG_STATUS:
	default:
		return 0444;
	}
}

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
	int				in_use;
	char				name[VMMFS_NAME_MAX + 1];
	struct vmmfs_machine_state     *rust;	/* config + lifecycle (Rust) */
	struct vmmfs_node		node;
	struct vmmfs_node		cfg[VMMFS_NCFG];
	struct vmmfs_node		vn_devices;	/* this machine's devices/ */
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

static int	vmmfs_statfs(struct mount *mp, struct statfs *sbp,
		    struct ucred *cred);
static size_t	vmmfs_cfg_text(struct vmmfs_node *node, uint8_t *buf,
		    size_t cap);

static struct vop_ops vmmfs_vnode_vops;

/* --------------------------------------------------------------------- */

static void
vmmfs_node_init(struct vmmfs_node *node, enum vmmfs_ntype type, ino_t ino,
    struct vmmfs_node *parent, struct vmmfs_machine *machine,
    enum vmmfs_cfg cfg)
{
	node->vn_type = type;
	node->vn_cfg = cfg;
	node->vn_ino = ino;
	if (type == VMMFS_NCONFIG)
		node->vn_mode = vmmfs_cfg_mode(cfg);
	else if (type == VMMFS_NDEVICE)
		node->vn_mode = 0444;
	else if (type == VMMFS_NDEVLINK)
		node->vn_mode = 0777;
	else
		node->vn_mode = VMMFS_DIR_MODE;
	node->vn_owner = VMMFS_OWNER_HOST;
	node->vn_parent = parent;
	node->vn_machine = machine;
	node->vn_vnode = NULL;
	lockinit(&node->vn_interlock, "vmmfs node", 0, 0);
	SLIST_INIT(&node->vn_obufs);
}

/* Free any lingering per-open buffers (teardown only; no commit). */
static void
vmmfs_obuf_drain(struct vmmfs_node *node)
{
	struct vmmfs_openbuf *ob;

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	while ((ob = SLIST_FIRST(&node->vn_obufs)) != NULL) {
		SLIST_REMOVE_HEAD(&node->vn_obufs, ob_link);
		kfree(ob->ob_data, M_VMMFS);
		kfree(ob, M_VMMFS);
	}
	lockmgr(&node->vn_interlock, LK_RELEASE);
}

static void
vmmfs_node_uninit(struct vmmfs_node *node)
{
	vmmfs_obuf_drain(node);
	lockuninit(&node->vn_interlock);
}

static ino_t
vmmfs_parent_ino(struct vmmfs_node *node)
{
	return (node->vn_parent != NULL) ? node->vn_parent->vn_ino :
	    node->vn_ino;
}

static int
vmmfs_cfg_present(struct vmmfs_machine *m, enum vmmfs_cfg cfg)
{
	if (cfg == VMMFS_CFG_STOPPED)
		return vmmfs_machine_is_stopped(m->rust);
	return 1;
}

/*
 * Bind a vnode to the given node, caching it.  Mirrors the interlocked
 * tmpfs_alloc_vp() normal path; vx_downgrade() after getnewvnode() is mandatory
 * so vflush() does not trip the v_spin assertion on unmount.
 */
static int
vmmfs_alloc_vp(struct mount *mp, struct vmmfs_node *node, int lkflag,
    struct vnode **vpp)
{
	struct vnode *vp;
	enum vtype vtype;
	int error = 0;

	if (node->vn_type == VMMFS_NCONFIG || node->vn_type == VMMFS_NDEVICE)
		vtype = VREG;
	else if (node->vn_type == VMMFS_NDEVLINK)
		vtype = VLNK;
	else
		vtype = VDIR;

loop:
	vp = NULL;
	if (node->vn_vnode == NULL) {
		error = getnewvnode(VMMFS_VTAG, mp, &vp, VLKTIMEOUT,
		    LK_CANRECURSE);
		if (error)
			goto out;
	}

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	if (node->vn_vnode != NULL) {
		struct vnode *ovp = node->vn_vnode;

		vhold(ovp);
		lockmgr(&node->vn_interlock, LK_RELEASE);
		if (vp != NULL) {
			vp->v_type = VBAD;
			vx_put(vp);
			vp = NULL;
		}
		if (vget(ovp, (lkflag & ~LK_RETRY) | LK_EXCLUSIVE) != 0) {
			vdrop(ovp);
			goto loop;
		}
		if (node->vn_vnode != ovp) {
			vput(ovp);
			vdrop(ovp);
			goto loop;
		}
		vdrop(ovp);
		vp = ovp;
		goto out;
	}

	if (vp == NULL) {
		lockmgr(&node->vn_interlock, LK_RELEASE);
		goto loop;
	}

	vp->v_data = node;
	vp->v_type = vtype;
	node->vn_vnode = vp;
	lockmgr(&node->vn_interlock, LK_RELEASE);

	vx_downgrade(vp);

out:
	*vpp = vp;
	return error;
}

/* --------------------------------------------------------------------- */
/* Per-open config-register buffers (PCIe-register semantics).           */

static struct vmmfs_openbuf *
vmmfs_obuf_get(struct vmmfs_node *node, struct file *fp, int create)
{
	struct vmmfs_openbuf *ob;

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	SLIST_FOREACH(ob, &node->vn_obufs, ob_link) {
		if (ob->ob_fp == fp) {
			lockmgr(&node->vn_interlock, LK_RELEASE);
			return ob;
		}
	}
	lockmgr(&node->vn_interlock, LK_RELEASE);
	if (!create)
		return NULL;

	ob = kmalloc(sizeof(*ob), M_VMMFS, M_WAITOK | M_ZERO);
	ob->ob_fp = fp;
	ob->ob_cap = 64;
	ob->ob_data = kmalloc(ob->ob_cap, M_VMMFS, M_WAITOK);
	ob->ob_len = 0;
	ob->ob_written = 0;

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	{
		struct vmmfs_openbuf *ex;

		SLIST_FOREACH(ex, &node->vn_obufs, ob_link) {
			if (ex->ob_fp == fp) {
				lockmgr(&node->vn_interlock, LK_RELEASE);
				kfree(ob->ob_data, M_VMMFS);
				kfree(ob, M_VMMFS);
				return ex;
			}
		}
	}
	SLIST_INSERT_HEAD(&node->vn_obufs, ob, ob_link);
	lockmgr(&node->vn_interlock, LK_RELEASE);
	return ob;
}

/* Read a register: the open buffer if it was written, else the current value. */
static int
vmmfs_obuf_read(struct vmmfs_node *node, struct file *fp, struct uio *uio)
{
	struct vmmfs_openbuf *ob;
	uint8_t tmp[300];
	char *data;
	int len;
	off_t off;

	if (uio->uio_offset < 0)
		return EINVAL;
	ob = vmmfs_obuf_get(node, fp, 0);
	if (ob != NULL && ob->ob_written) {
		data = ob->ob_data;
		len = ob->ob_len;
	} else {
		len = (int)vmmfs_cfg_text(node, tmp, sizeof(tmp));
		data = (char *)tmp;
	}
	off = uio->uio_offset;
	if (off >= len)
		return 0;
	return uiomove(data + off, (size_t)(len - off), uio);
}

/* Write into a register's open buffer (created on demand). */
static int
vmmfs_obuf_write(struct vmmfs_node *node, struct file *fp, struct uio *uio)
{
	struct vmmfs_openbuf *ob;
	off_t off;
	size_t need, resid0, wrote;
	int error;

	if (uio->uio_offset < 0)
		return EINVAL;
	ob = vmmfs_obuf_get(node, fp, 1);
	if (ob == NULL)
		return ENOMEM;

	off = uio->uio_offset;
	need = (size_t)off + (size_t)uio->uio_resid;
	if (need > VMMFS_OBUF_MAX)
		return EFBIG;
	if ((int)need > ob->ob_cap) {
		int ncap = ob->ob_cap;
		char *nd;

		while (ncap < (int)need)
			ncap *= 2;
		nd = kmalloc(ncap, M_VMMFS, M_WAITOK);
		bcopy(ob->ob_data, nd, ob->ob_len);
		kfree(ob->ob_data, M_VMMFS);
		ob->ob_data = nd;
		ob->ob_cap = ncap;
	}
	if (off > ob->ob_len)
		bzero(ob->ob_data + ob->ob_len, (size_t)(off - ob->ob_len));

	resid0 = uio->uio_resid;
	error = uiomove(ob->ob_data + off, (size_t)uio->uio_resid, uio);
	if (error)
		return error;
	wrote = resid0 - uio->uio_resid;
	ob->ob_written = 1;
	if ((int)(off + wrote) > ob->ob_len)
		ob->ob_len = (int)(off + wrote);
	return 0;
}

/* Commit (parse + update desired) on close, then free the buffer. */
static void
vmmfs_cfg_commit(struct vmmfs_node *node, const uint8_t *buf, size_t len);

static void
vmmfs_obuf_commit_close(struct vmmfs_node *node, struct file *fp)
{
	struct vmmfs_openbuf *ob = NULL, *it;

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	SLIST_FOREACH(it, &node->vn_obufs, ob_link) {
		if (it->ob_fp == fp) {
			ob = it;
			SLIST_REMOVE(&node->vn_obufs, it, vmmfs_openbuf,
			    ob_link);
			break;
		}
	}
	lockmgr(&node->vn_interlock, LK_RELEASE);
	if (ob == NULL)
		return;
	if (ob->ob_written)
		vmmfs_cfg_commit(node, (const uint8_t *)ob->ob_data,
		    (size_t)ob->ob_len);
	kfree(ob->ob_data, M_VMMFS);
	kfree(ob, M_VMMFS);
}

/* Current desired value of a register, serialized as text. */
static size_t
vmmfs_cfg_text(struct vmmfs_node *node, uint8_t *buf, size_t cap)
{
	struct vmmfs_machine *m = node->vn_machine;

	switch (node->vn_cfg) {
	case VMMFS_CFG_VCPU:
		return vmmfs_machine_vcpu_text(m->rust, buf, cap);
	case VMMFS_CFG_MEM:
		return vmmfs_machine_mem_text(m->rust, buf, cap);
	case VMMFS_CFG_LOADER:
		return vmmfs_machine_loader_text(m->rust, buf, cap);
	default:
		return 0;
	}
}

static void
vmmfs_cfg_commit(struct vmmfs_node *node, const uint8_t *buf, size_t len)
{
	struct vmmfs_machine *m = node->vn_machine;

	switch (node->vn_cfg) {
	case VMMFS_CFG_VCPU:
		(void)vmmfs_machine_commit_vcpu(m->rust, buf, len);
		break;
	case VMMFS_CFG_MEM:
		(void)vmmfs_machine_commit_mem(m->rust, buf, len);
		break;
	case VMMFS_CFG_LOADER:
		(void)vmmfs_machine_commit_loader(m->rust, buf, len);
		break;
	default:
		break;
	}
}

/*
 * Validate the desired loader at start time: resolve the path in the caller's
 * context and require a regular, executable file.  No execution yet (vmm core).
 */
static int
vmmfs_validate_loader(struct vmmfs_machine *m, struct ucred *cred)
{
	struct nlookupdata nd;
	struct vnode *vp = NULL;
	struct vattr va;
	char path[VMMFS_OBUF_MAX];
	size_t n;
	int error;

	n = vmmfs_machine_loader_path(m->rust, (uint8_t *)path,
	    sizeof(path) - 1);
	if (n == 0)
		return EINVAL;
	path[n] = '\0';

	error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW | NLC_LOCKVP);
	if (error == 0)
		error = vn_open(&nd, NULL, FREAD, 0);
	if (error == 0) {
		vp = nd.nl_open_vp;
		nd.nl_open_vp = NULL;
	}
	nlookup_done(&nd);
	if (error)
		return error;

	vn_unlock(vp);
	if (vp->v_type != VREG) {
		vn_close(vp, FREAD, NULL);
		return EACCES;
	}
	vn_lock(vp, LK_SHARED | LK_RETRY);
	error = VOP_GETATTR(vp, &va);
	if (error == 0 && (va.va_mode & 0111) == 0)
		error = EACCES;
	if (error == 0)
		error = VOP_ACCESS(vp, VEXEC, cred);
	vn_unlock(vp);
	vn_close(vp, FREAD, NULL);
	return error;
}

/* --------------------------------------------------------------------- */
/* Machine registry (guarded by vm_lock).                                */

static struct vmmfs_machine *
vmmfs_find_machine(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	int i;

	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];

		if (m->in_use && (int)strlen(m->name) == nlen &&
		    bcmp(m->name, name, nlen) == 0)
			return m;
	}
	return NULL;
}

/*
 * Find a reusable slot: not in_use and with no lingering cached vnode (machine
 * dir or any config file).  Frees any deferred Rust state before reuse, the
 * safe point to free it: by here no vnode can still reference the machine.
 */
static struct vmmfs_machine *
vmmfs_alloc_slot(struct vmmfs_mount *vmp)
{
	int i, j;

	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];
		int busy = 0;

		if (m->in_use || m->node.vn_vnode != NULL ||
		    m->vn_devices.vn_vnode != NULL)
			continue;
		for (j = 0; j < VMMFS_NCFG; j++) {
			if (m->cfg[j].vn_vnode != NULL) {
				busy = 1;
				break;
			}
		}
		if (busy)
			continue;
		if (m->rust != NULL) {
			vmmfs_machine_free(m->rust);
			m->rust = NULL;
		}
		return m;
	}
	return NULL;
}

/*
 * Mark a machine deleted: enter the deletion flow (so the lease can no longer
 * be opened) and drop it from the namespace.  The Rust state and the slot are
 * reclaimed lazily (vmmfs_alloc_slot / unmount), so any still-open fds keep
 * working.  Callers that hold the machine's directory vnode additionally
 * cache_inval_vp() it for an immediate vanish.
 */
static void
vmmfs_machine_mark_deleted(struct vmmfs_mount *vmp, struct vmmfs_machine *m)
{
	int idx = (int)(m - vmp->vm_mach);
	int i;

	vmmfs_machine_begin_delete(m->rust);
	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m->in_use = 0;
	/* Any device bound to this machine: host devices return to the host
	 * pool, user backends are unloaded. */
	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		struct vmmfs_device *d = &vmp->vm_dev[i];

		if (d->in_use && d->owner == idx) {
			if (d->is_host)
				d->owner = VMMFS_OWNER_HOST;
			else
				d->in_use = 0;
		}
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);
}

/* --------------------------------------------------------------------- */
/* Device pool (guarded by vm_lock).                                     */

static int
vmmfs_device_format(struct vmmfs_device *d, char *buf, size_t bufsize)
{
	return ksnprintf(buf, bufsize, "%s\n", d->bdf);
}

static struct vmmfs_device *
vmmfs_find_device(struct vmmfs_mount *vmp, int owner, const char *name, int nlen)
{
	int i;

	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		struct vmmfs_device *d = &vmp->vm_dev[i];

		if (d->in_use && d->owner == owner &&
		    (int)strlen(d->bdf) == nlen && bcmp(d->bdf, name, nlen) == 0)
			return d;
	}
	return NULL;
}

/* Find any in_use device by bdf, regardless of owner (for the index). */
static struct vmmfs_device *
vmmfs_find_device_any(struct vmmfs_mount *vmp, const char *name, int nlen)
{
	int i;

	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		struct vmmfs_device *d = &vmp->vm_dev[i];

		if (d->in_use && (int)strlen(d->bdf) == nlen &&
		    bcmp(d->bdf, name, nlen) == 0)
			return d;
	}
	return NULL;
}

/* Owner display name: "host" or the owning machine's name. */
static const char *
vmmfs_owner_name(struct vmmfs_mount *vmp, int owner)
{
	if (owner == VMMFS_OWNER_HOST)
		return "host";
	if (owner >= 0 && owner < VMMFS_MAX_MACHINES &&
	    vmp->vm_mach[owner].in_use)
		return vmp->vm_mach[owner].name;
	return NULL;
}

/* Relative symlink target for a device in the /vmm/devices/ index. */
static int
vmmfs_devlink_target(struct vmmfs_mount *vmp, struct vmmfs_device *d,
    char *buf, size_t bufsize)
{
	const char *owner = vmmfs_owner_name(vmp, d->owner);

	if (owner == NULL)
		return -1;
	return ksnprintf(buf, bufsize, "../machines/%s/devices/%s", owner,
	    d->bdf);
}

/* --------------------------------------------------------------------- */

static int
vmmfs_nresolve(struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
	struct vmmfs_node *child = NULL;
	struct vnode *vp = NULL;
	int error;

	if (dnode->vn_type == VMMFS_NROOT) {
		if (ncp->nc_nlen == 8 && bcmp(ncp->nc_name, "machines", 8) == 0)
			child = &vmp->vm_machines;
		else if (ncp->nc_nlen == 7 &&
		    bcmp(ncp->nc_name, "devices", 7) == 0)
			child = &vmp->vm_devroot;
	} else if (dnode->vn_type == VMMFS_NMACHINES) {
		if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0) {
			child = &vmp->vm_host;
		} else {
			struct vmmfs_machine *m;

			lockmgr(&vmp->vm_lock, LK_SHARED);
			m = vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen);
			if (m != NULL)
				child = &m->node;
			lockmgr(&vmp->vm_lock, LK_RELEASE);
		}
	} else if (dnode->vn_type == VMMFS_NHOST) {
		if (ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "devices", 7) == 0)
			child = &vmp->vm_host_devices;
	} else if (dnode->vn_type == VMMFS_NMACHINE) {
		struct vmmfs_machine *m = dnode->vn_machine;
		int i;

		if (ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "devices", 7) == 0) {
			child = &m->vn_devices;
		} else {
			for (i = 0; i < VMMFS_NCFG; i++) {
				if (!vmmfs_cfg_present(m, i))
					continue;
				if ((int)strlen(vmmfs_cfg_name[i]) ==
				    ncp->nc_nlen &&
				    bcmp(vmmfs_cfg_name[i], ncp->nc_name,
				    ncp->nc_nlen) == 0) {
					child = &m->cfg[i];
					break;
				}
			}
		}
	} else if (dnode->vn_type == VMMFS_NDEVICES) {
		struct vmmfs_device *d;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		d = vmmfs_find_device(vmp, dnode->vn_owner, ncp->nc_name,
		    ncp->nc_nlen);
		if (d != NULL)
			child = &d->node;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
	} else if (dnode->vn_type == VMMFS_NDEVROOT) {
		struct vmmfs_device *d;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		d = vmmfs_find_device_any(vmp, ncp->nc_name, ncp->nc_nlen);
		if (d != NULL)
			child = &d->link;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
	}

	if (child == NULL) {
		cache_setvp(ap->a_nch, NULL);
		return ENOENT;
	}

	error = vmmfs_alloc_vp(dvp->v_mount, child, LK_EXCLUSIVE | LK_RETRY,
	    &vp);
	if (error)
		return error;

	vn_unlock(vp);
	cache_setvp(ap->a_nch, vp);
	vrele(vp);
	return 0;
}

static int
vmmfs_nlookupdotdot(struct vop_nlookupdotdot_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
	int error;

	*vpp = NULL;

	error = VOP_ACCESS(dvp, VEXEC, ap->a_cred);
	if (error)
		return error;

	if (dnode->vn_parent != NULL) {
		error = vmmfs_alloc_vp(dvp->v_mount, dnode->vn_parent,
		    LK_EXCLUSIVE | LK_RETRY, vpp);
		if (*vpp != NULL)
			vn_unlock(*vpp);
	}

	return (*vpp == NULL) ? ENOENT : 0;
}

/*
 * `mkdir machines/<name>` creates a machine: always stopped, empty config.
 * The user then writes vcpu/mem/loader and `rm stopped` to start.
 */
static int
vmmfs_nmkdir(struct vop_nmkdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
	struct vmmfs_machine *m;
	struct vmmfs_machine_state *rust;
	struct vnode *vp;
	int error;

	if (dnode->vn_type != VMMFS_NMACHINES)
		return EPERM;
	if (ncp->nc_nlen == 0 || ncp->nc_nlen > VMMFS_NAME_MAX)
		return ENAMETOOLONG;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0)
		return EEXIST;	/* host is reserved */

	rust = vmmfs_machine_new();
	if (rust == NULL)
		return ENOMEM;

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	if (vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen) != NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vmmfs_machine_free(rust);
		return EEXIST;
	}
	m = vmmfs_alloc_slot(vmp);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vmmfs_machine_free(rust);
		return ENOSPC;
	}
	bcopy(ncp->nc_name, m->name, ncp->nc_nlen);
	m->name[ncp->nc_nlen] = '\0';
	m->rust = rust;
	m->in_use = 1;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	error = vmmfs_alloc_vp(dvp->v_mount, &m->node, LK_EXCLUSIVE | LK_RETRY,
	    &vp);
	if (error) {
		lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
		m->in_use = 0;
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return error;
	}

	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	return 0;
}

/*
 * `rmdir machines/<name>` removes a stopped machine (source 1): it deletes
 * regardless of leases.  The Rust state is freed lazily (vmmfs_alloc_slot) so
 * any still-open fds keep working until reclaimed.
 */
static int
vmmfs_nrmdir(struct vop_nrmdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(dvp->v_mount);
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	if (dnode->vn_type != VMMFS_NMACHINES)
		return EINVAL;
	if (ncp->nc_nlen == 4 && bcmp(ncp->nc_name, "host", 4) == 0)
		return EPERM;	/* host is not removable */

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	m = vmmfs_find_machine(vmp, ncp->nc_name, ncp->nc_nlen);
	if (m == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	if (!vmmfs_machine_is_stopped(m->rust)) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return EBUSY;
	}
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	vmmfs_machine_mark_deleted(vmp, m);
	cache_inval_vp(vp, CINV_DESTROY | CINV_CHILDREN);
	vrele(vp);
	return 0;
}

/*
 * Create "stopped" under a machine directory: an atomic, idempotent request to
 * stop the machine.  `echo apic > stopped` opens with O_CREAT.
 */
static int
vmmfs_ncreate(struct vop_ncreate_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	if (dnode->vn_type != VMMFS_NMACHINE)
		return EPERM;
	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;

	m = dnode->vn_machine;
	vmmfs_machine_stop(m->rust, 0);

	error = vmmfs_alloc_vp(dvp->v_mount, &m->cfg[VMMFS_CFG_STOPPED],
	    LK_EXCLUSIVE | LK_RETRY, &vp);
	if (error)
		return error;

	*ap->a_vpp = vp;
	cache_setunresolved(ap->a_nch);
	cache_setvp(ap->a_nch, vp);
	return 0;
}

/*
 * `rm <name>/devices/<dev>` unbinds a device.  A host device returns to the
 * host pool; a user backend is deleted (unloaded).  Removing from host/devices/
 * itself is refused (the host pool is fixed).
 */
static int
vmmfs_nremove_device(struct vop_nremove_args *ap, struct vmmfs_node *dnode)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_dvp->v_mount);
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_device *d;
	struct vnode *vp;
	int error;

	if (dnode->vn_owner == VMMFS_OWNER_HOST)
		return EPERM;

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	d = vmmfs_find_device(vmp, dnode->vn_owner, ncp->nc_name, ncp->nc_nlen);
	if (d == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vrele(vp);
		return ENOENT;
	}
	if (d->is_host)
		d->owner = VMMFS_OWNER_HOST;	/* unbind: back to host pool */
	else
		d->in_use = 0;			/* backend: unload */
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	cache_unlink(ap->a_nch);
	vrele(vp);
	return 0;
}

/*
 * `rm machines/<name>/stopped` is an atomic request to start the machine.  The
 * config must be complete and the loader path executable; otherwise the start
 * fails and the machine stays stopped.  Only "stopped" is removable.
 */
static int
vmmfs_nremove(struct vop_nremove_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_node *dnode = VP_TO_VMMFS(dvp);
	struct vmmfs_machine *m;
	struct vnode *vp;
	int error;

	if (dnode->vn_type == VMMFS_NDEVICES)
		return vmmfs_nremove_device(ap, dnode);
	if (dnode->vn_type != VMMFS_NMACHINE)
		return EPERM;
	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;

	m = dnode->vn_machine;
	if (!vmmfs_machine_is_stopped(m->rust))
		return ENOENT;

	if (!vmmfs_machine_config_complete(m->rust))
		return EINVAL;
	error = vmmfs_validate_loader(m, ap->a_cred);
	if (error)
		return error;

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	vmmfs_machine_start(m->rust);

	cache_unlink(ap->a_nch);
	vrele(vp);
	return 0;
}

/*
 * `mv <devices>/<dev> <devices>/` rebinds a device: it changes which machine
 * owns it.  Both sides must be devices/ directories; the BDF name is unchanged.
 * Desired-state semantics.  (cp is impossible: devices/ rejects file creation.)
 */
static int
vmmfs_nrename(struct vop_nrename_args *ap)
{
	struct namecache *fncp = ap->a_fnch->ncp;
	struct namecache *tncp = ap->a_tnch->ncp;
	struct vmmfs_node *fdnode = VP_TO_VMMFS(ap->a_fdvp);
	struct vmmfs_node *tdnode = VP_TO_VMMFS(ap->a_tdvp);
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(ap->a_fdvp->v_mount);
	struct vmmfs_device *d;

	if (fdnode->vn_type != VMMFS_NDEVICES ||
	    tdnode->vn_type != VMMFS_NDEVICES)
		return EXDEV;
	if (fncp->nc_nlen != tncp->nc_nlen ||
	    bcmp(fncp->nc_name, tncp->nc_name, fncp->nc_nlen) != 0)
		return EINVAL;	/* a device keeps its BDF name */

	lockmgr(&vmp->vm_lock, LK_EXCLUSIVE);
	d = vmmfs_find_device(vmp, fdnode->vn_owner, fncp->nc_name,
	    fncp->nc_nlen);
	if (d == NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return ENOENT;
	}
	if (fdnode->vn_owner == tdnode->vn_owner) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return 0;	/* no-op rebind */
	}
	if (vmmfs_find_device(vmp, tdnode->vn_owner, tncp->nc_name,
	    tncp->nc_nlen) != NULL) {
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		return EEXIST;
	}
	d->owner = tdnode->vn_owner;
	lockmgr(&vmp->vm_lock, LK_RELEASE);

	cache_rename(ap->a_fnch, ap->a_tnch);
	return 0;
}

static int
vmmfs_open(struct vop_open_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);

	/*
	 * Opening the lease takes a reference; refuse once deletion has begun.
	 * Register files need no per-open work here: the scratch buffer is
	 * created lazily on first write, and reads fall back to the current
	 * value, so a read-only open allocates nothing.
	 */
	if (node->vn_type == VMMFS_NCONFIG && node->vn_cfg == VMMFS_CFG_LEASE) {
		if (vmmfs_machine_lease_open(node->vn_machine->rust) == 0)
			return ENXIO;
	}
	return vop_stdopen(ap);
}

static int
vmmfs_close(struct vop_close_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);
	int error;

	/* Commit a register's open buffer before the fd goes away. */
	if (node->vn_type == VMMFS_NCONFIG &&
	    vmmfs_cfg_is_register(node->vn_cfg))
		vmmfs_obuf_commit_close(node, ap->a_fp);

	error = vop_stdclose(ap);

	/* Releasing the last lease of an armed machine destroys it (source 3). */
	if (node->vn_type == VMMFS_NCONFIG && node->vn_cfg == VMMFS_CFG_LEASE) {
		if (vmmfs_machine_lease_close(node->vn_machine->rust)) {
			struct vmmfs_mount *vmp =
			    VFS_TO_VMMFS(ap->a_vp->v_mount);

			vmmfs_machine_mark_deleted(vmp, node->vn_machine);
		}
	}
	return error;
}

static int
vmmfs_access(struct vop_access_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);

	return vop_helper_access(ap, 0, 0, node->vn_mode, 0);
}

static int
vmmfs_getattr(struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);
	int is_file = (node->vn_type == VMMFS_NCONFIG ||
	    node->vn_type == VMMFS_NDEVICE);
	int is_link = (node->vn_type == VMMFS_NDEVLINK);
	uint8_t tmp[300];

	vap->va_type = is_link ? VLNK : (is_file ? VREG : VDIR);
	vap->va_mode = node->vn_mode;
	vap->va_nlink = (is_file || is_link) ? 1 :
	    ((node->vn_type == VMMFS_NROOT) ? 3 : 2);
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = node->vn_ino;
	if (node->vn_type == VMMFS_NCONFIG &&
	    vmmfs_cfg_is_register(node->vn_cfg)) {
		vap->va_size = vmmfs_cfg_text(node, tmp, sizeof(tmp));
	} else if (node->vn_type == VMMFS_NDEVICE) {
		vap->va_size = vmmfs_device_format(VMMFS_DEV_OF_NODE(node),
		    (char *)tmp, sizeof(tmp));
	} else if (is_link) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
		int len;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		len = vmmfs_devlink_target(vmp, VMMFS_DEV_OF_LINK(node),
		    (char *)tmp, sizeof(tmp));
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		vap->va_size = (len < 0) ? 0 : len;
	} else {
		vap->va_size = 0;
	}
	vap->va_blocksize = PAGE_SIZE;
	vap->va_atime.tv_sec = 0;
	vap->va_atime.tv_nsec = 0;
	vap->va_mtime = vap->va_atime;
	vap->va_ctime = vap->va_atime;
	vap->va_gen = 1;
	vap->va_flags = 0;
	vap->va_bytes = 0;
	vap->va_filerev = 0;

	return 0;
}

/*
 * Accept no-op size changes (the O_TRUNC from `echo ... > file`) on writable
 * config files; everything else is read-only.
 */
static int
vmmfs_setattr(struct vop_setattr_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);

	if (node->vn_type == VMMFS_NCONFIG && (node->vn_mode & 0200))
		return 0;
	return EPERM;
}

static int
vmmfs_read(struct vop_read_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);

	if (vp->v_type != VREG)
		return EINVAL;

	if (node->vn_type == VMMFS_NDEVICE) {
		struct vmmfs_device *d = VMMFS_DEV_OF_NODE(node);
		char dbuf[64];
		int len;
		off_t off;

		if (uio->uio_offset < 0)
			return EINVAL;
		len = vmmfs_device_format(d, dbuf, sizeof(dbuf));
		off = uio->uio_offset;
		if (off >= len)
			return 0;
		return uiomove(dbuf + off, (size_t)(len - off), uio);
	}

	if (node->vn_type != VMMFS_NCONFIG)
		return EINVAL;

	/*
	 * events is a stream: each read drains and consumes the queued event
	 * lines (shared one-shot cursor).  Non-blocking; blocking read and
	 * kqueue are a later step.
	 */
	if (node->vn_cfg == VMMFS_CFG_EVENTS) {
		uint8_t ebuf[256];
		size_t n;

		n = vmmfs_machine_read_events(node->vn_machine->rust, ebuf,
		    sizeof(ebuf));
		if (n == 0)
			return 0;
		return uiomove(ebuf, n, uio);
	}

	if (vmmfs_cfg_is_register(node->vn_cfg))
		return vmmfs_obuf_read(node, ap->a_fp, uio);

	/* console / status.tar.gz / lease / stopped: empty for now. */
	return 0;
}

static int
vmmfs_write(struct vop_write_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);
	char buf[16];
	size_t take;
	int error, force;

	if (node->vn_type != VMMFS_NCONFIG)
		return EPERM;

	if (vmmfs_cfg_is_register(node->vn_cfg))
		return vmmfs_obuf_write(node, ap->a_fp, uio);

	if (node->vn_cfg == VMMFS_CFG_CONSOLE) {
		/* Console input is a stub: accept and discard. */
		while (uio->uio_resid > 0) {
			char dump[64];
			size_t d = (uio->uio_resid < (int)sizeof(dump)) ?
			    (size_t)uio->uio_resid : sizeof(dump);

			error = uiomove(dump, d, uio);
			if (error)
				return error;
		}
		return 0;
	}

	if (node->vn_cfg != VMMFS_CFG_STOPPED)
		return EPERM;

	/*
	 * Writing the stopped control file selects the stop method (apic|force)
	 * and (re)applies the stop.  Idempotent.
	 */
	take = (uio->uio_resid < (int)(sizeof(buf) - 1)) ?
	    (size_t)uio->uio_resid : sizeof(buf) - 1;
	error = uiomove(buf, take, uio);
	if (error)
		return error;
	buf[take] = '\0';
	force = (take >= 5 && strncmp(buf, "force", 5) == 0);

	while (uio->uio_resid > 0) {
		char dump[32];
		size_t d = (uio->uio_resid < (int)sizeof(dump)) ?
		    (size_t)uio->uio_resid : sizeof(dump);

		error = uiomove(dump, d, uio);
		if (error)
			return error;
	}

	vmmfs_machine_stop(node->vn_machine->rust, force);
	return 0;
}

/* A device-index symlink resolves to its owner's devices/ entry. */
static int
vmmfs_readlink(struct vop_readlink_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
	char buf[128];
	int len;

	if (node->vn_type != VMMFS_NDEVLINK)
		return EINVAL;
	lockmgr(&vmp->vm_lock, LK_SHARED);
	len = vmmfs_devlink_target(vmp, VMMFS_DEV_OF_LINK(node), buf,
	    sizeof(buf));
	lockmgr(&vmp->vm_lock, LK_RELEASE);
	if (len < 0)
		return ENOENT;
	return uiomove(buf, (size_t)len, ap->a_uio);
}

static int
vmmfs_readdir(struct vop_readdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);
	int error = 0;
	int r;
	int full = 0;
	off_t off;

	if (vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0)
		return EINVAL;

	off = uio->uio_offset;

	if (off == 0) {
		r = vop_write_dirent(&error, uio, node->vn_ino, DT_DIR, 1, ".");
		if (r) {
			full = 1;
			goto done;
		}
		off = 1;
	}
	if (off == 1) {
		r = vop_write_dirent(&error, uio, vmmfs_parent_ino(node),
		    DT_DIR, 2, "..");
		if (r) {
			full = 1;
			goto done;
		}
		off = 2;
	}

	if (node->vn_type == VMMFS_NROOT) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);

		if (off == 2) {
			r = vop_write_dirent(&error, uio,
			    vmp->vm_machines.vn_ino, DT_DIR, 8, "machines");
			if (r) {
				full = 1;
				goto done;
			}
			off = 3;
		}
		if (off == 3) {
			r = vop_write_dirent(&error, uio, vmp->vm_devroot.vn_ino,
			    DT_DIR, 7, "devices");
			if (r) {
				full = 1;
				goto done;
			}
			off = 4;
		}
	} else if (node->vn_type == VMMFS_NMACHINES) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
		int i;

		/* host is always the first entry. */
		if (off == 2) {
			r = vop_write_dirent(&error, uio, vmp->vm_host.vn_ino,
			    DT_DIR, 4, "host");
			if (r) {
				full = 1;
				goto done;
			}
			off = 3;
		}
		lockmgr(&vmp->vm_lock, LK_SHARED);
		for (i = (int)off - 3; i < VMMFS_MAX_MACHINES; i++) {
			struct vmmfs_machine *m = &vmp->vm_mach[i];

			if (!m->in_use)
				continue;
			r = vop_write_dirent(&error, uio, m->node.vn_ino,
			    DT_DIR, (uint16_t)strlen(m->name), m->name);
			if (r) {
				off = 3 + i;
				full = 1;
				break;
			}
			off = 3 + i + 1;
		}
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		if (!full && off < 3 + VMMFS_MAX_MACHINES)
			off = 3 + VMMFS_MAX_MACHINES;
	} else if (node->vn_type == VMMFS_NHOST) {
		if (off == 2) {
			struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);

			r = vop_write_dirent(&error, uio,
			    vmp->vm_host_devices.vn_ino, DT_DIR, 7, "devices");
			if (r) {
				full = 1;
				goto done;
			}
			off = 3;
		}
	} else if (node->vn_type == VMMFS_NDEVICES) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
		int i;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		for (i = (int)off - 2; i < VMMFS_MAX_DEVICES; i++) {
			struct vmmfs_device *d = &vmp->vm_dev[i];

			if (!d->in_use || d->owner != node->vn_owner)
				continue;
			r = vop_write_dirent(&error, uio, d->node.vn_ino,
			    DT_REG, (uint16_t)strlen(d->bdf), d->bdf);
			if (r) {
				off = 2 + i;
				full = 1;
				break;
			}
			off = 2 + i + 1;
		}
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		if (!full && off < 2 + VMMFS_MAX_DEVICES)
			off = 2 + VMMFS_MAX_DEVICES;
	} else if (node->vn_type == VMMFS_NDEVROOT) {
		struct vmmfs_mount *vmp = VFS_TO_VMMFS(vp->v_mount);
		int i;

		lockmgr(&vmp->vm_lock, LK_SHARED);
		for (i = (int)off - 2; i < VMMFS_MAX_DEVICES; i++) {
			struct vmmfs_device *d = &vmp->vm_dev[i];

			if (!d->in_use)
				continue;
			r = vop_write_dirent(&error, uio, d->link.vn_ino,
			    DT_LNK, (uint16_t)strlen(d->bdf), d->bdf);
			if (r) {
				off = 2 + i;
				full = 1;
				break;
			}
			off = 2 + i + 1;
		}
		lockmgr(&vmp->vm_lock, LK_RELEASE);
		if (!full && off < 2 + VMMFS_MAX_DEVICES)
			off = 2 + VMMFS_MAX_DEVICES;
	} else if (node->vn_type == VMMFS_NMACHINE) {
		struct vmmfs_machine *m = node->vn_machine;
		int i;

		for (i = (int)off - 2; i < VMMFS_NCFG; i++) {
			if (!vmmfs_cfg_present(m, i))
				continue;
			r = vop_write_dirent(&error, uio, m->cfg[i].vn_ino,
			    DT_REG, (uint16_t)strlen(vmmfs_cfg_name[i]),
			    vmmfs_cfg_name[i]);
			if (r) {
				off = 2 + i;
				full = 1;
				break;
			}
			off = 2 + i + 1;
		}
		if (!full && off < 2 + VMMFS_NCFG)
			off = 2 + VMMFS_NCFG;
		/* devices/ follows the config files. */
		if (!full && off == 2 + VMMFS_NCFG) {
			r = vop_write_dirent(&error, uio, m->vn_devices.vn_ino,
			    DT_DIR, 7, "devices");
			if (r)
				full = 1;
			else
				off = 2 + VMMFS_NCFG + 1;
		}
	}

done:
	uio->uio_offset = off;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = !full;
	if (ap->a_ncookies != NULL) {
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	return error;
}

static int
vmmfs_inactive(struct vop_inactive_args *ap)
{
	return 0;
}

static int
vmmfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vmmfs_node *node = VP_TO_VMMFS(vp);

	/* No open fd can remain here, but free any stray buffers defensively. */
	vmmfs_obuf_drain(node);

	lockmgr(&node->vn_interlock, LK_EXCLUSIVE);
	KKASSERT(node->vn_vnode == vp);
	node->vn_vnode = NULL;
	vp->v_data = NULL;
	lockmgr(&node->vn_interlock, LK_RELEASE);

	return 0;
}

static int
vmmfs_print(struct vop_print_args *ap)
{
	struct vmmfs_node *node = VP_TO_VMMFS(ap->a_vp);

	kprintf("\tvmmfs_node %p ino %ju type %d\n", node,
	    (uintmax_t)(node != NULL ? node->vn_ino : 0),
	    node != NULL ? (int)node->vn_type : -1);
	return 0;
}

static struct vop_ops vmmfs_vnode_vops = {
	.vop_default =		vop_defaultop,
	.vop_nresolve =		vmmfs_nresolve,
	.vop_nlookupdotdot =	vmmfs_nlookupdotdot,
	.vop_nmkdir =		vmmfs_nmkdir,
	.vop_ncreate =		vmmfs_ncreate,
	.vop_nremove =		vmmfs_nremove,
	.vop_nrmdir =		vmmfs_nrmdir,
	.vop_nrename =		vmmfs_nrename,
	.vop_readlink =		vmmfs_readlink,
	.vop_open =		vmmfs_open,
	.vop_close =		vmmfs_close,
	.vop_access =		vmmfs_access,
	.vop_getattr =		vmmfs_getattr,
	.vop_setattr =		vmmfs_setattr,
	.vop_read =		vmmfs_read,
	.vop_write =		vmmfs_write,
	.vop_readdir =		vmmfs_readdir,
	.vop_inactive =		vmmfs_inactive,
	.vop_reclaim =		vmmfs_reclaim,
	.vop_print =		vmmfs_print,
};

/* --------------------------------------------------------------------- */

static int
vmmfs_mount(struct mount *mp, char *path, caddr_t data, struct ucred *cred)
{
	struct vmmfs_mount *vmp;
	size_t size;
	int i;

	if (mp->mnt_flag & MNT_UPDATE)
		return EOPNOTSUPP;

	vmp = kmalloc(sizeof(*vmp), M_VMMFS, M_WAITOK | M_ZERO);
	vmp->vm_mp = mp;
	lockinit(&vmp->vm_lock, "vmmfs registry", 0, 0);
	vmmfs_node_init(&vmp->vm_root, VMMFS_NROOT, VMMFS_ROOT_INO, NULL, NULL,
	    0);
	vmmfs_node_init(&vmp->vm_machines, VMMFS_NMACHINES, VMMFS_MACHINES_INO,
	    &vmp->vm_root, NULL, 0);
	vmmfs_node_init(&vmp->vm_host, VMMFS_NHOST, VMMFS_HOST_INO,
	    &vmp->vm_machines, NULL, 0);
	vmmfs_node_init(&vmp->vm_host_devices, VMMFS_NDEVICES,
	    VMMFS_HOST_DEV_INO, &vmp->vm_host, NULL, 0);
	vmp->vm_host_devices.vn_owner = VMMFS_OWNER_HOST;
	vmmfs_node_init(&vmp->vm_devroot, VMMFS_NDEVROOT, VMMFS_DEVROOT_INO,
	    &vmp->vm_root, NULL, 0);
	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];
		ino_t base = VMMFS_MACHINE_INO_BASE +
		    (ino_t)i * VMMFS_MACHINE_INO_STRIDE;
		int j;

		vmmfs_node_init(&m->node, VMMFS_NMACHINE, base,
		    &vmp->vm_machines, m, 0);
		for (j = 0; j < VMMFS_NCFG; j++)
			vmmfs_node_init(&m->cfg[j], VMMFS_NCONFIG, base + 1 + j,
			    &m->node, m, j);
		vmmfs_node_init(&m->vn_devices, VMMFS_NDEVICES,
		    base + VMMFS_MACHINE_DEV_OFF, &m->node, m, 0);
		m->vn_devices.vn_owner = i;
		m->in_use = 0;
		m->rust = NULL;
	}
	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		struct vmmfs_device *d = &vmp->vm_dev[i];

		d->in_use = 0;
		d->owner = VMMFS_OWNER_HOST;
		d->is_host = 1;
		vmmfs_node_init(&d->node, VMMFS_NDEVICE,
		    VMMFS_DEV_INO_BASE + (ino_t)i, &vmp->vm_host_devices, NULL,
		    0);
		vmmfs_node_init(&d->link, VMMFS_NDEVLINK,
		    VMMFS_DEVLINK_INO_BASE + (ino_t)i, &vmp->vm_devroot, NULL,
		    0);
	}
	/* Stub host PCIe device pool: a few fixed BDFs, all owned by host. */
	{
		static const char *const stub_bdf[] = {
			"0000:00:02.0", "0000:00:03.0", "0000:00:04.0",
		};
		int n = (int)(sizeof(stub_bdf) / sizeof(stub_bdf[0]));

		for (i = 0; i < n && i < VMMFS_MAX_DEVICES; i++) {
			struct vmmfs_device *d = &vmp->vm_dev[i];

			d->in_use = 1;
			strlcpy(d->bdf, stub_bdf[i], sizeof(d->bdf));
		}
	}

	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_ALL_MPSAFE;
	mp->mnt_kern_flag |= MNTK_NOMSYNC;
	mp->mnt_data = (qaddr_t)vmp;
	mp->mnt_iosize_max = MAXBSIZE;
	vfs_getnewfsid(mp);

	vfs_add_vnodeops(mp, &vmmfs_vnode_vops, &mp->mnt_vn_norm_ops);

	copystr("vmmfs", mp->mnt_stat.f_mntfromname, MNAMELEN - 1, &size);
	bzero(mp->mnt_stat.f_mntfromname + size, MNAMELEN - size);
	bzero(mp->mnt_stat.f_mntonname, sizeof(mp->mnt_stat.f_mntonname));
	copyinstr(path, mp->mnt_stat.f_mntonname,
	    sizeof(mp->mnt_stat.f_mntonname) - 1, &size);

	vmmfs_statfs(mp, &mp->mnt_stat, cred);
	return 0;
}

static int
vmmfs_unmount(struct mount *mp, int mntflags)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(mp);
	int flags = 0;
	int error;
	int i;

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	error = vflush(mp, 0, flags);
	if (error)
		return error;

	for (i = 0; i < VMMFS_MAX_MACHINES; i++) {
		struct vmmfs_machine *m = &vmp->vm_mach[i];
		int j;

		if (m->rust != NULL)
			vmmfs_machine_free(m->rust);
		for (j = 0; j < VMMFS_NCFG; j++)
			vmmfs_node_uninit(&m->cfg[j]);
		vmmfs_node_uninit(&m->vn_devices);
		vmmfs_node_uninit(&m->node);
	}
	for (i = 0; i < VMMFS_MAX_DEVICES; i++) {
		vmmfs_node_uninit(&vmp->vm_dev[i].node);
		vmmfs_node_uninit(&vmp->vm_dev[i].link);
	}
	vmmfs_node_uninit(&vmp->vm_devroot);
	vmmfs_node_uninit(&vmp->vm_host_devices);
	vmmfs_node_uninit(&vmp->vm_host);
	vmmfs_node_uninit(&vmp->vm_machines);
	vmmfs_node_uninit(&vmp->vm_root);
	lockuninit(&vmp->vm_lock);
	mp->mnt_data = NULL;
	kfree(vmp, M_VMMFS);
	return 0;
}

static int
vmmfs_root(struct mount *mp, struct vnode **vpp)
{
	struct vmmfs_mount *vmp = VFS_TO_VMMFS(mp);
	int error;

	error = vmmfs_alloc_vp(mp, &vmp->vm_root, LK_EXCLUSIVE | LK_RETRY, vpp);
	if (error)
		return error;
	(*vpp)->v_flag |= VROOT;
	(*vpp)->v_type = VDIR;
	return 0;
}

static int
vmmfs_statfs(struct mount *mp, struct statfs *sbp, struct ucred *cred)
{
	sbp->f_iosize = PAGE_SIZE;
	sbp->f_bsize = PAGE_SIZE;
	sbp->f_blocks = 1;
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = VMMFS_MAX_MACHINES;
	sbp->f_ffree = 0;
	return 0;
}

static int
vmmfs_vfs_init(struct vfsconf *conf)
{
	return vmmfs_rust_init();
}

static int
vmmfs_vfs_uninit(struct vfsconf *conf)
{
	vmmfs_rust_fini();
	return 0;
}

static struct vfsops vmmfs_vfsops = {
	.vfs_flags =		0,
	.vfs_mount =		vmmfs_mount,
	.vfs_unmount =		vmmfs_unmount,
	.vfs_root =		vmmfs_root,
	.vfs_statfs =		vmmfs_statfs,
	.vfs_init =		vmmfs_vfs_init,
	.vfs_uninit =		vmmfs_vfs_uninit,
};

VFS_SET(vmmfs_vfsops, vmmfs, VFCF_MPSAFE);
MODULE_VERSION(vmmfs, 1);
