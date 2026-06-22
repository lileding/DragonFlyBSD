/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMM core machine model -- see vmm_machine.h.  Pure C, no kernel calls beyond
 * memcpy/memset (which libkern and the host C library both provide), so it
 * builds for the kernel module and for host unit tests.
 */
#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

#include "vmm_machine.h"

#define VMM_VCPU_MAX	256u
#define VMM_MEM_ALIGN	(2ull * 1024 * 1024)	/* large-page granularity */

#define EV_CREATED	1
#define EV_STARTED	2
#define EV_STOPPED	3
#define EV_DELETED	4

/* --------------------------------------------------------------------- */
/* Config value parsing.                                                 */

static int
is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Trim leading/trailing whitespace; returns the start, sets *outlen. */
static const char *
trim(const char *b, size_t len, size_t *outlen)
{
	size_t s = 0, e = len;

	while (s < e && is_ws(b[s]))
		s++;
	while (e > s && is_ws(b[e - 1]))
		e--;
	*outlen = e - s;
	return b + s;
}

/* Parse a non-empty decimal with overflow check; 1 on success. */
static int
parse_decimal(const char *b, size_t len, uint64_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (len == 0)
		return 0;
	for (i = 0; i < len; i++) {
		char c = b[i];

		if (c < '0' || c > '9')
			return 0;
		if (v > ((uint64_t)-1 - (uint64_t)(c - '0')) / 10)
			return 0;
		v = v * 10 + (uint64_t)(c - '0');
	}
	*out = v;
	return 1;
}

/* vcpu: decimal 1..=VMM_VCPU_MAX. */
static int
parse_vcpu(const char *b, size_t len, uint32_t *out)
{
	size_t tl;
	const char *t = trim(b, len, &tl);
	uint64_t v;

	if (!parse_decimal(t, tl, &v) || v < 1 || v > VMM_VCPU_MAX)
		return 0;
	*out = (uint32_t)v;
	return 1;
}

/* mem: number[KkMmGg], > 0, VMM_MEM_ALIGN-aligned. */
static int
parse_mem(const char *b, size_t len, uint64_t *out)
{
	size_t tl;
	const char *t = trim(b, len, &tl);
	uint64_t mult, v;
	size_t dlen;
	char last;

	if (tl == 0)
		return 0;
	last = t[tl - 1];
	if (last == 'K' || last == 'k') {
		mult = 1024;
		dlen = tl - 1;
	} else if (last == 'M' || last == 'm') {
		mult = 1024 * 1024;
		dlen = tl - 1;
	} else if (last == 'G' || last == 'g') {
		mult = 1024 * 1024 * 1024;
		dlen = tl - 1;
	} else if (last >= '0' && last <= '9') {
		mult = 1;
		dlen = tl;
	} else {
		return 0;
	}
	if (!parse_decimal(t, dlen, &v))
		return 0;
	if (v > ((uint64_t)-1) / mult)
		return 0;
	v *= mult;
	if (v == 0 || (v % VMM_MEM_ALIGN) != 0)
		return 0;
	*out = v;
	return 1;
}

/* Write a decimal + '\n'; returns bytes written (0 if it does not fit). */
static size_t
write_decimal(uint64_t v, char *out, size_t cap)
{
	char tmp[20];
	size_t i = sizeof(tmp), digits, need;

	do {
		tmp[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v != 0);
	digits = sizeof(tmp) - i;
	need = digits + 1;
	if (need > cap)
		return 0;
	memcpy(out, tmp + i, digits);
	out[need - 1] = '\n';
	return need;
}

/* --------------------------------------------------------------------- */
/* Event ring.                                                           */

static const char *
event_text(uint8_t code, size_t *len)
{
	switch (code) {
	case EV_CREATED: *len = 8; return "created\n";
	case EV_STARTED: *len = 8; return "started\n";
	case EV_STOPPED: *len = 8; return "stopped\n";
	case EV_DELETED: *len = 8; return "deleted\n";
	default:	 *len = 0; return "";
	}
}

static void
ev_push(struct vmm_machine *m, uint8_t code)
{
	size_t slot = (m->ev_tail + m->ev_count) % VMM_EVENT_CAP;

	m->ev_codes[slot] = code;
	if (m->ev_count < VMM_EVENT_CAP)
		m->ev_count++;
	else
		m->ev_tail = (m->ev_tail + 1) % VMM_EVENT_CAP;
}

/* --------------------------------------------------------------------- */
/* Machine model.                                                        */

void
vmm_machine_init(struct vmm_machine *m)
{
	memset(m, 0, sizeof(*m));
	m->stopped = 1;
	ev_push(m, EV_CREATED);
	ev_push(m, EV_STOPPED);
}

int
vmm_machine_commit_vcpu(struct vmm_machine *m, const char *buf, size_t len)
{
	uint32_t v;

	if (!parse_vcpu(buf, len, &v))
		return 0;
	m->vcpu = v;
	return 1;
}

int
vmm_machine_commit_mem(struct vmm_machine *m, const char *buf, size_t len)
{
	uint64_t v;

	if (!parse_mem(buf, len, &v))
		return 0;
	m->mem = v;
	return 1;
}

int
vmm_machine_commit_loader(struct vmm_machine *m, const char *buf,
    size_t len)
{
	size_t pl;
	const char *p = trim(buf, len, &pl);

	if (pl == 0 || pl > VMM_LOADER_MAX)
		return 0;
	memcpy(m->loader, p, pl);
	m->loader_len = pl;
	return 1;
}

size_t
vmm_machine_vcpu_text(const struct vmm_machine *m, char *out, size_t cap)
{
	return (m->vcpu == 0) ? 0 : write_decimal(m->vcpu, out, cap);
}

size_t
vmm_machine_mem_text(const struct vmm_machine *m, char *out, size_t cap)
{
	return (m->mem == 0) ? 0 : write_decimal(m->mem, out, cap);
}

size_t
vmm_machine_loader_text(const struct vmm_machine *m, char *out, size_t cap)
{
	size_t need = m->loader_len + 1;

	if (m->loader_len == 0 || need > cap)
		return 0;
	memcpy(out, m->loader, m->loader_len);
	out[m->loader_len] = '\n';
	return need;
}

size_t
vmm_machine_loader_path(const struct vmm_machine *m, char *out, size_t cap)
{
	if (m->loader_len == 0 || m->loader_len > cap)
		return 0;
	memcpy(out, m->loader, m->loader_len);
	return m->loader_len;
}

int
vmm_machine_config_complete(const struct vmm_machine *m)
{
	return m->vcpu != 0 && m->mem != 0 && m->loader_len != 0;
}

int
vmm_machine_is_stopped(const struct vmm_machine *m)
{
	return m->stopped;
}

void
vmm_machine_stop(struct vmm_machine *m, int force)
{
	(void)force;
	if (!m->stopped) {
		m->stopped = 1;
		ev_push(m, EV_STOPPED);
	}
}

void
vmm_machine_start(struct vmm_machine *m)
{
	if (m->stopped) {
		m->stopped = 0;
		ev_push(m, EV_STARTED);
	}
}

int
vmm_machine_is_deleting(const struct vmm_machine *m)
{
	return m->deleting;
}

int
vmm_machine_lease_open(struct vmm_machine *m)
{
	if (m->deleting)
		return 0;
	m->lease_count++;
	m->armed = 1;
	return 1;
}

enum vmm_close_action
vmm_machine_lease_close(struct vmm_machine *m)
{
	if (m->lease_count > 0)
		m->lease_count--;
	if (m->armed && m->lease_count == 0 && !m->deleting) {
		m->deleting = 1;
		ev_push(m, EV_DELETED);
		return VMM_CLOSE_DELETE;
	}
	return VMM_CLOSE_NONE;
}

int
vmm_machine_begin_delete(struct vmm_machine *m)
{
	if (m->deleting)
		return 0;
	m->deleting = 1;
	ev_push(m, EV_DELETED);
	return 1;
}

int
vmm_machine_events_pending(const struct vmm_machine *m)
{
	return m->ev_count > 0;
}

size_t
vmm_machine_read_events(struct vmm_machine *m, char *out, size_t cap)
{
	size_t n = 0;

	while (m->ev_count > 0) {
		size_t tlen;
		const char *t = event_text(m->ev_codes[m->ev_tail], &tlen);

		if (n + tlen > cap)
			break;
		memcpy(out + n, t, tlen);
		n += tlen;
		m->ev_tail = (m->ev_tail + 1) % VMM_EVENT_CAP;
		m->ev_count--;
	}
	return n;
}

/* --------------------------------------------------------------------- */
/* The machine directory's vops (kernel only; the object owns its node).  */
#ifdef _KERNEL
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namecache.h>
#include <sys/dirent.h>
#include <sys/uio.h>
#include <sys/kobj.h>

#include "vmmfs.h"
#include "vmm_node_if.h"

static int
vmm_machine_nresolve(struct vmmfs_node *dnode, struct vop_nresolve_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machine *m = dnode->vn_machine;
	struct vmmfs_node *child = NULL;
	int i;

	if (ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "devices", 7) == 0) {
		child = &m->vn_devices;
	} else {
		for (i = 0; i < VMMFS_NCFG; i++) {
			if (!vmmfs_cfg_present(m, i))
				continue;
			if ((int)strlen(vmmfs_cfg_name[i]) == ncp->nc_nlen &&
			    bcmp(vmmfs_cfg_name[i], ncp->nc_name,
			    ncp->nc_nlen) == 0) {
				child = &m->cfg[i];
				break;
			}
		}
	}
	return vmmfs_nresolve_finish(dvp, child, ap->a_nch);
}

static int
vmm_machine_readdir(struct vmmfs_node *node, struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct vmmfs_machine *m = node->vn_machine;
	off_t off;
	int full, error, i;

	error = vmmfs_readdir_dots(ap, node, &off, &full);
	if (error || full)
		goto out;
	for (i = (int)off - 2; i < VMMFS_NCFG; i++) {
		if (!vmmfs_cfg_present(m, i))
			continue;
		if (vop_write_dirent(&error, uio, m->cfg[i].vn_ino, DT_REG,
		    (uint16_t)strlen(vmmfs_cfg_name[i]), vmmfs_cfg_name[i])) {
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
		if (vop_write_dirent(&error, uio, m->vn_devices.vn_ino, DT_DIR,
		    7, "devices"))
			full = 1;
		else
			off = 2 + VMMFS_NCFG + 1;
	}
out:
	return vmmfs_readdir_end(ap, off, full, error);
}

/*
 * Create "stopped" under a machine: an atomic, idempotent request to stop it.
 * `echo apic > stopped` opens with O_CREAT.
 */
static int
vmm_machine_ncreate(struct vmmfs_node *dnode, struct vop_ncreate_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machine *m = dnode->vn_machine;
	struct vnode *vp;
	int error;

	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;

	vmm_machine_stop(&m->state, 0);
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
 * `rm machines/<name>/stopped` requests start: config must be complete and the
 * loader executable, else the start fails and the machine stays stopped.
 */
static int
vmm_machine_nremove(struct vmmfs_node *dnode, struct vop_nremove_args *ap)
{
	struct namecache *ncp = ap->a_nch->ncp;
	struct vmmfs_machine *m = dnode->vn_machine;
	struct vnode *vp;
	int error;

	if (!(ncp->nc_nlen == 7 && bcmp(ncp->nc_name, "stopped", 7) == 0))
		return EPERM;
	if (!vmm_machine_is_stopped(&m->state))
		return ENOENT;
	if (!vmm_machine_config_complete(&m->state))
		return EINVAL;
	error = vmmfs_validate_loader(m, ap->a_cred);
	if (error)
		return error;

	error = cache_vget(ap->a_nch, ap->a_cred, LK_SHARED, &vp);
	if (error)
		return error;
	vn_unlock(vp);

	vmm_machine_start(&m->state);
	cache_unlink(ap->a_nch);
	vrele(vp);
	return 0;
}

static kobj_method_t vmm_machine_methods[] = {
	KOBJMETHOD(vmm_node_nresolve,		vmm_machine_nresolve),
	KOBJMETHOD(vmm_node_readdir,		vmm_machine_readdir),
	KOBJMETHOD(vmm_node_ncreate,		vmm_machine_ncreate),
	KOBJMETHOD(vmm_node_nremove,		vmm_machine_nremove),
	KOBJMETHOD(vmm_node_getattr,		vmmfs_dir_getattr),
	KOBJMETHOD(vmm_node_nlookupdotdot,	vmmnode_nlookupdotdot),
	KOBJMETHOD(vmm_node_access,		vmmnode_access),
	KOBJMETHOD(vmm_node_setattr,		vmmnode_setattr),
	KOBJMETHOD(vmm_node_open,		vmmnode_open),
	KOBJMETHOD(vmm_node_close,		vmmnode_close),
	KOBJMETHOD(vmm_node_inactive,		vmmnode_inactive),
	KOBJMETHOD(vmm_node_reclaim,		vmmnode_reclaim),
	KOBJMETHOD(vmm_node_print,		vmmnode_print),
	KOBJMETHOD_END
};
DEFINE_CLASS(vmm_machine, vmm_machine_methods, 0);
#endif /* _KERNEL */
