#-
# SPDX-License-Identifier: BSD-2-Clause
#
# The vmm node operation interface.  Each filesystem node is a KOBJ instance
# (struct vmmfs_node embeds kobj_ops_t as its first field); the central
# vop_ops handlers in vmmfs.c are thin shims that extract the node and forward
# to these methods, which each object class (vmm_machine, vmm_vcpu, ...)
# implements for the node types it owns.  Every method takes the resolved node
# plus the original vop argument struct.
#
#include <sys/param.h>
#include <sys/kobj.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>

#include "vmm_machine.h"
#include "vmmfs.h"

INTERFACE vmmfs_node;

METHOD int nresolve {
	struct vmmfs_node	*node;
	struct vop_nresolve_args *ap;
};

METHOD int nlookupdotdot {
	struct vmmfs_node	*node;
	struct vop_nlookupdotdot_args *ap;
};

METHOD int nmkdir {
	struct vmmfs_node	*node;
	struct vop_nmkdir_args	*ap;
};

METHOD int ncreate {
	struct vmmfs_node	*node;
	struct vop_ncreate_args	*ap;
};

METHOD int nremove {
	struct vmmfs_node	*node;
	struct vop_nremove_args	*ap;
};

METHOD int nrmdir {
	struct vmmfs_node	*node;
	struct vop_nrmdir_args	*ap;
};

METHOD int nrename {
	struct vmmfs_node	*node;
	struct vop_nrename_args	*ap;
};

METHOD int readlink {
	struct vmmfs_node	*node;
	struct vop_readlink_args *ap;
};

METHOD int open {
	struct vmmfs_node	*node;
	struct vop_open_args	*ap;
};

METHOD int close {
	struct vmmfs_node	*node;
	struct vop_close_args	*ap;
};

METHOD int access {
	struct vmmfs_node	*node;
	struct vop_access_args	*ap;
};

METHOD int getattr {
	struct vmmfs_node	*node;
	struct vop_getattr_args	*ap;
};

METHOD int setattr {
	struct vmmfs_node	*node;
	struct vop_setattr_args	*ap;
};

METHOD int read {
	struct vmmfs_node	*node;
	struct vop_read_args	*ap;
};

METHOD int write {
	struct vmmfs_node	*node;
	struct vop_write_args	*ap;
};

METHOD int ioctl {
	struct vmmfs_node	*node;
	struct vop_ioctl_args	*ap;
};

METHOD int kqfilter {
	struct vmmfs_node	*node;
	struct vop_kqfilter_args *ap;
};

METHOD int readdir {
	struct vmmfs_node	*node;
	struct vop_readdir_args	*ap;
};

METHOD int inactive {
	struct vmmfs_node	*node;
	struct vop_inactive_args *ap;
};

METHOD int reclaim {
	struct vmmfs_node	*node;
	struct vop_reclaim_args	*ap;
};

METHOD int print {
	struct vmmfs_node	*node;
	struct vop_print_args	*ap;
};
