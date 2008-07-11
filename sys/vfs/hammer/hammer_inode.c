/*
 * Copyright (c) 2007-2008 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * $DragonFly: src/sys/vfs/hammer/hammer_inode.c,v 1.99 2008/07/11 01:22:29 dillon Exp $
 */

#include "hammer.h"
#include <vm/vm_extern.h>
#include <sys/buf.h>
#include <sys/buf2.h>

static int	hammer_unload_inode(struct hammer_inode *ip);
static void	hammer_free_inode(hammer_inode_t ip);
static void	hammer_flush_inode_core(hammer_inode_t ip, int flags);
static int	hammer_setup_child_callback(hammer_record_t rec, void *data);
static int	hammer_syncgrp_child_callback(hammer_record_t rec, void *data);
static int	hammer_setup_parent_inodes(hammer_inode_t ip);
static int	hammer_setup_parent_inodes_helper(hammer_record_t record);
static void	hammer_inode_wakereclaims(hammer_inode_t ip);

#ifdef DEBUG_TRUNCATE
extern struct hammer_inode *HammerTruncIp;
#endif

/*
 * RB-Tree support for inode structures
 */
int
hammer_ino_rb_compare(hammer_inode_t ip1, hammer_inode_t ip2)
{
	if (ip1->obj_localization < ip2->obj_localization)
		return(-1);
	if (ip1->obj_localization > ip2->obj_localization)
		return(1);
	if (ip1->obj_id < ip2->obj_id)
		return(-1);
	if (ip1->obj_id > ip2->obj_id)
		return(1);
	if (ip1->obj_asof < ip2->obj_asof)
		return(-1);
	if (ip1->obj_asof > ip2->obj_asof)
		return(1);
	return(0);
}

/*
 * RB-Tree support for inode structures / special LOOKUP_INFO
 */
static int
hammer_inode_info_cmp(hammer_inode_info_t info, hammer_inode_t ip)
{
	if (info->obj_localization < ip->obj_localization)
		return(-1);
	if (info->obj_localization > ip->obj_localization)
		return(1);
	if (info->obj_id < ip->obj_id)
		return(-1);
	if (info->obj_id > ip->obj_id)
		return(1);
	if (info->obj_asof < ip->obj_asof)
		return(-1);
	if (info->obj_asof > ip->obj_asof)
		return(1);
	return(0);
}

/*
 * Used by hammer_scan_inode_snapshots() to locate all of an object's
 * snapshots.  Note that the asof field is not tested, which we can get
 * away with because it is the lowest-priority field.
 */
static int
hammer_inode_info_cmp_all_history(hammer_inode_t ip, void *data)
{
	hammer_inode_info_t info = data;

	if (ip->obj_localization > info->obj_localization)
		return(1);
	if (ip->obj_localization < info->obj_localization)
		return(-1);
	if (ip->obj_id > info->obj_id)
		return(1);
	if (ip->obj_id < info->obj_id)
		return(-1);
	return(0);
}

/*
 * RB-Tree support for pseudofs structures
 */
static int
hammer_pfs_rb_compare(hammer_pseudofs_inmem_t p1, hammer_pseudofs_inmem_t p2)
{
	if (p1->localization < p2->localization)
		return(-1);
	if (p1->localization > p2->localization)
		return(1);
	return(0);
}


RB_GENERATE(hammer_ino_rb_tree, hammer_inode, rb_node, hammer_ino_rb_compare);
RB_GENERATE_XLOOKUP(hammer_ino_rb_tree, INFO, hammer_inode, rb_node,
		hammer_inode_info_cmp, hammer_inode_info_t);
RB_GENERATE2(hammer_pfs_rb_tree, hammer_pseudofs_inmem, rb_node,
             hammer_pfs_rb_compare, u_int32_t, localization);

/*
 * The kernel is not actively referencing this vnode but is still holding
 * it cached.
 *
 * This is called from the frontend.
 */
int
hammer_vop_inactive(struct vop_inactive_args *ap)
{
	struct hammer_inode *ip = VTOI(ap->a_vp);

	/*
	 * Degenerate case
	 */
	if (ip == NULL) {
		vrecycle(ap->a_vp);
		return(0);
	}

	/*
	 * If the inode no longer has visibility in the filesystem try to
	 * recycle it immediately, even if the inode is dirty.  Recycling
	 * it quickly allows the system to reclaim buffer cache and VM
	 * resources which can matter a lot in a heavily loaded system.
	 *
	 * This can deadlock in vfsync() if we aren't careful.
	 * 
	 * Do not queue the inode to the flusher if we still have visibility,
	 * otherwise namespace calls such as chmod will unnecessarily generate
	 * multiple inode updates.
	 */
	hammer_inode_unloadable_check(ip, 0);
	if (ip->ino_data.nlinks == 0) {
		if (ip->flags & HAMMER_INODE_MODMASK)
			hammer_flush_inode(ip, 0);
		vrecycle(ap->a_vp);
	}
	return(0);
}

/*
 * Release the vnode association.  This is typically (but not always)
 * the last reference on the inode.
 *
 * Once the association is lost we are on our own with regards to
 * flushing the inode.
 */
int
hammer_vop_reclaim(struct vop_reclaim_args *ap)
{
	struct hammer_inode *ip;
	hammer_mount_t hmp;
	struct vnode *vp;

	vp = ap->a_vp;

	if ((ip = vp->v_data) != NULL) {
		hmp = ip->hmp;
		vp->v_data = NULL;
		ip->vp = NULL;

		if ((ip->flags & HAMMER_INODE_RECLAIM) == 0) {
			++hammer_count_reclaiming;
			++hmp->inode_reclaims;
			ip->flags |= HAMMER_INODE_RECLAIM;
			if (hmp->inode_reclaims > HAMMER_RECLAIM_FLUSH &&
			    (hmp->inode_reclaims & 255) == 0) {
				hammer_flusher_async(hmp);
			}
		}
		hammer_rel_inode(ip, 1);
	}
	return(0);
}

/*
 * Return a locked vnode for the specified inode.  The inode must be
 * referenced but NOT LOCKED on entry and will remain referenced on
 * return.
 *
 * Called from the frontend.
 */
int
hammer_get_vnode(struct hammer_inode *ip, struct vnode **vpp)
{
	hammer_mount_t hmp;
	struct vnode *vp;
	int error = 0;
	u_int8_t obj_type;

	hmp = ip->hmp;

	for (;;) {
		if ((vp = ip->vp) == NULL) {
			error = getnewvnode(VT_HAMMER, hmp->mp, vpp, 0, 0);
			if (error)
				break;
			hammer_lock_ex(&ip->lock);
			if (ip->vp != NULL) {
				hammer_unlock(&ip->lock);
				vp->v_type = VBAD;
				vx_put(vp);
				continue;
			}
			hammer_ref(&ip->lock);
			vp = *vpp;
			ip->vp = vp;

			obj_type = ip->ino_data.obj_type;
			vp->v_type = hammer_get_vnode_type(obj_type);

			hammer_inode_wakereclaims(ip);

			switch(ip->ino_data.obj_type) {
			case HAMMER_OBJTYPE_CDEV:
			case HAMMER_OBJTYPE_BDEV:
				vp->v_ops = &hmp->mp->mnt_vn_spec_ops;
				addaliasu(vp, ip->ino_data.rmajor,
					  ip->ino_data.rminor);
				break;
			case HAMMER_OBJTYPE_FIFO:
				vp->v_ops = &hmp->mp->mnt_vn_fifo_ops;
				break;
			default:
				break;
			}

			/*
			 * Only mark as the root vnode if the ip is not
			 * historical, otherwise the VFS cache will get
			 * confused.  The other half of the special handling
			 * is in hammer_vop_nlookupdotdot().
			 *
			 * Pseudo-filesystem roots also do not count.
			 */
			if (ip->obj_id == HAMMER_OBJID_ROOT &&
			    ip->obj_asof == hmp->asof &&
			    ip->obj_localization == 0) {
				vp->v_flag |= VROOT;
			}

			vp->v_data = (void *)ip;
			/* vnode locked by getnewvnode() */
			/* make related vnode dirty if inode dirty? */
			hammer_unlock(&ip->lock);
			if (vp->v_type == VREG)
				vinitvmio(vp, ip->ino_data.size);
			break;
		}

		/*
		 * loop if the vget fails (aka races), or if the vp
		 * no longer matches ip->vp.
		 */
		if (vget(vp, LK_EXCLUSIVE) == 0) {
			if (vp == ip->vp)
				break;
			vput(vp);
		}
	}
	*vpp = vp;
	return(error);
}

/*
 * Locate all copies of the inode for obj_id compatible with the specified
 * asof, reference, and issue the related call-back.  This routine is used
 * for direct-io invalidation and does not create any new inodes.
 */
void
hammer_scan_inode_snapshots(hammer_mount_t hmp, hammer_inode_info_t iinfo,
		            int (*callback)(hammer_inode_t ip, void *data),
			    void *data)
{
	hammer_ino_rb_tree_RB_SCAN(&hmp->rb_inos_root,
				   hammer_inode_info_cmp_all_history,
				   callback, iinfo);
}

/*
 * Acquire a HAMMER inode.  The returned inode is not locked.  These functions
 * do not attach or detach the related vnode (use hammer_get_vnode() for
 * that).
 *
 * The flags argument is only applied for newly created inodes, and only
 * certain flags are inherited.
 *
 * Called from the frontend.
 */
struct hammer_inode *
hammer_get_inode(hammer_transaction_t trans, hammer_inode_t dip,
		 int64_t obj_id, hammer_tid_t asof, u_int32_t localization,
		 int flags, int *errorp)
{
	hammer_mount_t hmp = trans->hmp;
	struct hammer_inode_info iinfo;
	struct hammer_cursor cursor;
	struct hammer_inode *ip;


	/*
	 * Determine if we already have an inode cached.  If we do then
	 * we are golden.
	 */
	iinfo.obj_id = obj_id;
	iinfo.obj_asof = asof;
	iinfo.obj_localization = localization;
loop:
	ip = hammer_ino_rb_tree_RB_LOOKUP_INFO(&hmp->rb_inos_root, &iinfo);
	if (ip) {
		hammer_ref(&ip->lock);
		*errorp = 0;
		return(ip);
	}

	/*
	 * Allocate a new inode structure and deal with races later.
	 */
	ip = kmalloc(sizeof(*ip), M_HAMMER, M_WAITOK|M_ZERO);
	++hammer_count_inodes;
	++hmp->count_inodes;
	ip->obj_id = obj_id;
	ip->obj_asof = iinfo.obj_asof;
	ip->obj_localization = localization;
	ip->hmp = hmp;
	ip->flags = flags & HAMMER_INODE_RO;
	ip->cache[0].ip = ip;
	ip->cache[1].ip = ip;
	if (hmp->ronly)
		ip->flags |= HAMMER_INODE_RO;
	ip->sync_trunc_off = ip->trunc_off = ip->save_trunc_off =
		0x7FFFFFFFFFFFFFFFLL;
	RB_INIT(&ip->rec_tree);
	TAILQ_INIT(&ip->target_list);
	hammer_ref(&ip->lock);

	/*
	 * Locate the on-disk inode.  If this is a PFS root we always
	 * access the current version of the root inode and (if it is not
	 * a master) always access information under it with a snapshot
	 * TID.
	 */
retry:
	hammer_init_cursor(trans, &cursor, (dip ? &dip->cache[0] : NULL), NULL);
	cursor.key_beg.localization = localization + HAMMER_LOCALIZE_INODE;
	cursor.key_beg.obj_id = ip->obj_id;
	cursor.key_beg.key = 0;
	cursor.key_beg.create_tid = 0;
	cursor.key_beg.delete_tid = 0;
	cursor.key_beg.rec_type = HAMMER_RECTYPE_INODE;
	cursor.key_beg.obj_type = 0;

	cursor.asof = iinfo.obj_asof;
	cursor.flags = HAMMER_CURSOR_GET_LEAF | HAMMER_CURSOR_GET_DATA |
		       HAMMER_CURSOR_ASOF;

	*errorp = hammer_btree_lookup(&cursor);
	if (*errorp == EDEADLK) {
		hammer_done_cursor(&cursor);
		goto retry;
	}

	/*
	 * On success the B-Tree lookup will hold the appropriate
	 * buffer cache buffers and provide a pointer to the requested
	 * information.  Copy the information to the in-memory inode
	 * and cache the B-Tree node to improve future operations.
	 */
	if (*errorp == 0) {
		ip->ino_leaf = cursor.node->ondisk->elms[cursor.index].leaf;
		ip->ino_data = cursor.data->inode;

		/*
		 * cache[0] tries to cache the location of the object inode.
		 * The assumption is that it is near the directory inode.
		 *
		 * cache[1] tries to cache the location of the object data.
		 * The assumption is that it is near the directory data.
		 */
		hammer_cache_node(&ip->cache[0], cursor.node);
		if (dip && dip->cache[1].node)
			hammer_cache_node(&ip->cache[1], dip->cache[1].node);

		/*
		 * The file should not contain any data past the file size
		 * stored in the inode.  Setting save_trunc_off to the
		 * file size instead of max reduces B-Tree lookup overheads
		 * on append by allowing the flusher to avoid checking for
		 * record overwrites.
		 */
		ip->save_trunc_off = ip->ino_data.size;

		/*
		 * Locate and assign the pseudofs management structure to
		 * the inode.
		 */
		if (dip && dip->obj_localization == ip->obj_localization) {
			ip->pfsm = dip->pfsm;
			hammer_ref(&ip->pfsm->lock);
		} else {
			ip->pfsm = hammer_load_pseudofs(trans,
							ip->obj_localization,
							errorp);
			*errorp = 0;	/* ignore ENOENT */
		}
	}

	/*
	 * The inode is placed on the red-black tree and will be synced to
	 * the media when flushed or by the filesystem sync.  If this races
	 * another instantiation/lookup the insertion will fail.
	 */
	if (*errorp == 0) {
		if (RB_INSERT(hammer_ino_rb_tree, &hmp->rb_inos_root, ip)) {
			hammer_free_inode(ip);
			hammer_done_cursor(&cursor);
			goto loop;
		}
		ip->flags |= HAMMER_INODE_ONDISK;
	} else {
		if (ip->flags & HAMMER_INODE_RSV_INODES) {
			ip->flags &= ~HAMMER_INODE_RSV_INODES; /* sanity */
			--hmp->rsv_inodes;
		}

		hammer_free_inode(ip);
		ip = NULL;
	}
	hammer_done_cursor(&cursor);
	return (ip);
}

/*
 * Create a new filesystem object, returning the inode in *ipp.  The
 * returned inode will be referenced.  The inode is created in-memory.
 *
 * If pfsm is non-NULL the caller wishes to create the root inode for
 * a master PFS.
 */
int
hammer_create_inode(hammer_transaction_t trans, struct vattr *vap,
		    struct ucred *cred, hammer_inode_t dip,
		    hammer_pseudofs_inmem_t pfsm, struct hammer_inode **ipp)
{
	hammer_mount_t hmp;
	hammer_inode_t ip;
	uid_t xuid;
	int error;

	hmp = trans->hmp;

	ip = kmalloc(sizeof(*ip), M_HAMMER, M_WAITOK|M_ZERO);
	++hammer_count_inodes;
	++hmp->count_inodes;

	if (pfsm) {
		KKASSERT(pfsm->localization != 0);
		ip->obj_id = HAMMER_OBJID_ROOT;
		ip->obj_localization = pfsm->localization;
	} else {
		KKASSERT(dip != NULL);
		ip->obj_id = hammer_alloc_objid(hmp, dip);
		ip->obj_localization = dip->obj_localization;
	}

	KKASSERT(ip->obj_id != 0);
	ip->obj_asof = hmp->asof;
	ip->hmp = hmp;
	ip->flush_state = HAMMER_FST_IDLE;
	ip->flags = HAMMER_INODE_DDIRTY |
		    HAMMER_INODE_ATIME | HAMMER_INODE_MTIME;
	ip->cache[0].ip = ip;
	ip->cache[1].ip = ip;

	ip->trunc_off = 0x7FFFFFFFFFFFFFFFLL;
	/* ip->save_trunc_off = 0; (already zero) */
	RB_INIT(&ip->rec_tree);
	TAILQ_INIT(&ip->target_list);

	ip->ino_data.atime = trans->time;
	ip->ino_data.mtime = trans->time;
	ip->ino_data.size = 0;
	ip->ino_data.nlinks = 0;

	/*
	 * A nohistory designator on the parent directory is inherited by
	 * the child.  We will do this even for pseudo-fs creation... the
	 * sysad can turn it off.
	 */
	if (dip) {
		ip->ino_data.uflags = dip->ino_data.uflags &
				      (SF_NOHISTORY|UF_NOHISTORY|UF_NODUMP);
	}

	ip->ino_leaf.base.btype = HAMMER_BTREE_TYPE_RECORD;
	ip->ino_leaf.base.localization = ip->obj_localization +
					 HAMMER_LOCALIZE_INODE;
	ip->ino_leaf.base.obj_id = ip->obj_id;
	ip->ino_leaf.base.key = 0;
	ip->ino_leaf.base.create_tid = 0;
	ip->ino_leaf.base.delete_tid = 0;
	ip->ino_leaf.base.rec_type = HAMMER_RECTYPE_INODE;
	ip->ino_leaf.base.obj_type = hammer_get_obj_type(vap->va_type);

	ip->ino_data.obj_type = ip->ino_leaf.base.obj_type;
	ip->ino_data.version = HAMMER_INODE_DATA_VERSION;
	ip->ino_data.mode = vap->va_mode;
	ip->ino_data.ctime = trans->time;

	/*
	 * Setup the ".." pointer.  This only needs to be done for directories
	 * but we do it for all objects as a recovery aid.
	 */
	if (dip)
		ip->ino_data.parent_obj_id = dip->ino_leaf.base.obj_id;
#if 0
	/*
	 * The parent_obj_localization field only applies to pseudo-fs roots.
	 * XXX this is no longer applicable, PFSs are no longer directly
	 * tied into the parent's directory structure.
	 */
	if (ip->ino_data.obj_type == HAMMER_OBJTYPE_DIRECTORY &&
	    ip->obj_id == HAMMER_OBJID_ROOT) {
		ip->ino_data.ext.obj.parent_obj_localization = 
						dip->obj_localization;
	}
#endif

	switch(ip->ino_leaf.base.obj_type) {
	case HAMMER_OBJTYPE_CDEV:
	case HAMMER_OBJTYPE_BDEV:
		ip->ino_data.rmajor = vap->va_rmajor;
		ip->ino_data.rminor = vap->va_rminor;
		break;
	default:
		break;
	}

	/*
	 * Calculate default uid/gid and overwrite with information from
	 * the vap.
	 */
	if (dip) {
		xuid = hammer_to_unix_xid(&dip->ino_data.uid);
		xuid = vop_helper_create_uid(hmp->mp, dip->ino_data.mode,
					     xuid, cred, &vap->va_mode);
	} else {
		xuid = 0;
	}
	ip->ino_data.mode = vap->va_mode;

	if (vap->va_vaflags & VA_UID_UUID_VALID)
		ip->ino_data.uid = vap->va_uid_uuid;
	else if (vap->va_uid != (uid_t)VNOVAL)
		hammer_guid_to_uuid(&ip->ino_data.uid, vap->va_uid);
	else
		hammer_guid_to_uuid(&ip->ino_data.uid, xuid);

	if (vap->va_vaflags & VA_GID_UUID_VALID)
		ip->ino_data.gid = vap->va_gid_uuid;
	else if (vap->va_gid != (gid_t)VNOVAL)
		hammer_guid_to_uuid(&ip->ino_data.gid, vap->va_gid);
	else if (dip)
		ip->ino_data.gid = dip->ino_data.gid;

	hammer_ref(&ip->lock);

	if (pfsm) {
		ip->pfsm = pfsm;
		hammer_ref(&pfsm->lock);
		error = 0;
	} else if (dip->obj_localization == ip->obj_localization) {
		ip->pfsm = dip->pfsm;
		hammer_ref(&ip->pfsm->lock);
		error = 0;
	} else {
		ip->pfsm = hammer_load_pseudofs(trans,
						ip->obj_localization,
						&error);
		error = 0;	/* ignore ENOENT */
	}

	if (error) {
		hammer_free_inode(ip);
		ip = NULL;
	} else if (RB_INSERT(hammer_ino_rb_tree, &hmp->rb_inos_root, ip)) {
		panic("hammer_create_inode: duplicate obj_id %llx", ip->obj_id);
		/* not reached */
		hammer_free_inode(ip);
	}
	*ipp = ip;
	return(error);
}

/*
 * Final cleanup / freeing of an inode structure
 */
static void
hammer_free_inode(hammer_inode_t ip)
{
	KKASSERT(ip->lock.refs == 1);
	hammer_uncache_node(&ip->cache[0]);
	hammer_uncache_node(&ip->cache[1]);
	hammer_inode_wakereclaims(ip);
	if (ip->objid_cache)
		hammer_clear_objid(ip);
	--hammer_count_inodes;
	--ip->hmp->count_inodes;
	if (ip->pfsm) {
		hammer_rel_pseudofs(ip->hmp, ip->pfsm);
		ip->pfsm = NULL;
	}
	kfree(ip, M_HAMMER);
	ip = NULL;
}

/*
 * Retrieve pseudo-fs data.  NULL will never be returned.
 *
 * If an error occurs *errorp will be set and a default template is returned,
 * otherwise *errorp is set to 0.  Typically when an error occurs it will
 * be ENOENT.
 */
hammer_pseudofs_inmem_t
hammer_load_pseudofs(hammer_transaction_t trans,
		     u_int32_t localization, int *errorp)
{
	hammer_mount_t hmp = trans->hmp;
	hammer_inode_t ip;
	hammer_pseudofs_inmem_t pfsm;
	struct hammer_cursor cursor;
	int bytes;

retry:
	pfsm = RB_LOOKUP(hammer_pfs_rb_tree, &hmp->rb_pfsm_root, localization);
	if (pfsm) {
		hammer_ref(&pfsm->lock);
		*errorp = 0;
		return(pfsm);
	}

	/*
	 * PFS records are stored in the root inode (not the PFS root inode,
	 * but the real root).  Avoid an infinite recursion if loading
	 * the PFS for the real root.
	 */
	if (localization) {
		ip = hammer_get_inode(trans, NULL, HAMMER_OBJID_ROOT,
				      HAMMER_MAX_TID,
				      HAMMER_DEF_LOCALIZATION, 0, errorp);
	} else {
		ip = NULL;
	}

	pfsm = kmalloc(sizeof(*pfsm), M_HAMMER, M_WAITOK | M_ZERO);
	pfsm->localization = localization;
	pfsm->pfsd.unique_uuid = trans->rootvol->ondisk->vol_fsid;
	pfsm->pfsd.shared_uuid = pfsm->pfsd.unique_uuid;

	hammer_init_cursor(trans, &cursor, (ip ? &ip->cache[1] : NULL), ip);
	cursor.key_beg.localization = HAMMER_DEF_LOCALIZATION +
				      HAMMER_LOCALIZE_MISC;
	cursor.key_beg.obj_id = HAMMER_OBJID_ROOT;
	cursor.key_beg.create_tid = 0;
	cursor.key_beg.delete_tid = 0;
	cursor.key_beg.rec_type = HAMMER_RECTYPE_PFS;
	cursor.key_beg.obj_type = 0;
	cursor.key_beg.key = localization;
	cursor.asof = HAMMER_MAX_TID;
	cursor.flags |= HAMMER_CURSOR_ASOF;

	if (ip)
		*errorp = hammer_ip_lookup(&cursor);
	else
		*errorp = hammer_btree_lookup(&cursor);
	if (*errorp == 0) {
		*errorp = hammer_ip_resolve_data(&cursor);
		if (*errorp == 0) {
			bytes = cursor.leaf->data_len;
			if (bytes > sizeof(pfsm->pfsd))
				bytes = sizeof(pfsm->pfsd);
			bcopy(cursor.data, &pfsm->pfsd, bytes);
		}
	}
	hammer_done_cursor(&cursor);

	pfsm->fsid_udev = hammer_fsid_to_udev(&pfsm->pfsd.shared_uuid);
	hammer_ref(&pfsm->lock);
	if (ip)
		hammer_rel_inode(ip, 0);
	if (RB_INSERT(hammer_pfs_rb_tree, &hmp->rb_pfsm_root, pfsm)) {
		kfree(pfsm, M_HAMMER);
		goto retry;
	}
	return(pfsm);
}

/*
 * Store pseudo-fs data.  The backend will automatically delete any prior
 * on-disk pseudo-fs data but we have to delete in-memory versions.
 */
int
hammer_save_pseudofs(hammer_transaction_t trans, hammer_pseudofs_inmem_t pfsm)
{
	struct hammer_cursor cursor;
	hammer_record_t record;
	hammer_inode_t ip;
	int error;

	ip = hammer_get_inode(trans, NULL, HAMMER_OBJID_ROOT, HAMMER_MAX_TID,
			      HAMMER_DEF_LOCALIZATION, 0, &error);
retry:
	pfsm->fsid_udev = hammer_fsid_to_udev(&pfsm->pfsd.shared_uuid);
	hammer_init_cursor(trans, &cursor, &ip->cache[1], ip);
	cursor.key_beg.localization = ip->obj_localization +
				      HAMMER_LOCALIZE_MISC;
	cursor.key_beg.obj_id = HAMMER_OBJID_ROOT;
	cursor.key_beg.create_tid = 0;
	cursor.key_beg.delete_tid = 0;
	cursor.key_beg.rec_type = HAMMER_RECTYPE_PFS;
	cursor.key_beg.obj_type = 0;
	cursor.key_beg.key = pfsm->localization;
	cursor.asof = HAMMER_MAX_TID;
	cursor.flags |= HAMMER_CURSOR_ASOF;

	error = hammer_ip_lookup(&cursor);
	if (error == 0 && hammer_cursor_inmem(&cursor)) {
		record = cursor.iprec;
		if (record->flags & HAMMER_RECF_INTERLOCK_BE) {
			KKASSERT(cursor.deadlk_rec == NULL);
			hammer_ref(&record->lock);
			cursor.deadlk_rec = record;
			error = EDEADLK;
		} else {
			record->flags |= HAMMER_RECF_DELETED_FE;
			error = 0;
		}
	}
	if (error == 0 || error == ENOENT) {
		record = hammer_alloc_mem_record(ip, sizeof(pfsm->pfsd));
		record->type = HAMMER_MEM_RECORD_GENERAL;

		record->leaf.base.localization = ip->obj_localization +
						 HAMMER_LOCALIZE_MISC;
		record->leaf.base.rec_type = HAMMER_RECTYPE_PFS;
		record->leaf.base.key = pfsm->localization;
		record->leaf.data_len = sizeof(pfsm->pfsd);
		bcopy(&pfsm->pfsd, record->data, sizeof(pfsm->pfsd));
		error = hammer_ip_add_record(trans, record);
	}
	hammer_done_cursor(&cursor);
	if (error == EDEADLK)
		goto retry;
	hammer_rel_inode(ip, 0);
	return(error);
}

/*
 * Create a root directory for a PFS if one does not alredy exist.
 *
 * The PFS root stands alone so we must also bump the nlinks count
 * to prevent it from being destroyed on release.
 */
int
hammer_mkroot_pseudofs(hammer_transaction_t trans, struct ucred *cred,
		       hammer_pseudofs_inmem_t pfsm)
{
	hammer_inode_t ip;
	struct vattr vap;
	int error;

	ip = hammer_get_inode(trans, NULL, HAMMER_OBJID_ROOT, HAMMER_MAX_TID,
			      pfsm->localization, 0, &error);
	if (ip == NULL) {
		vattr_null(&vap);
		vap.va_mode = 0755;
		vap.va_type = VDIR;
		error = hammer_create_inode(trans, &vap, cred, NULL, pfsm, &ip);
		if (error == 0) {
			++ip->ino_data.nlinks;
			hammer_modify_inode(ip, HAMMER_INODE_DDIRTY);
		}
	}
	if (ip)
		hammer_rel_inode(ip, 0);
	return(error);
}

/*
 * Release a reference on a PFS
 */
void
hammer_rel_pseudofs(hammer_mount_t hmp, hammer_pseudofs_inmem_t pfsm)
{
	hammer_unref(&pfsm->lock);
	if (pfsm->lock.refs == 0) {
		RB_REMOVE(hammer_pfs_rb_tree, &hmp->rb_pfsm_root, pfsm);
		kfree(pfsm, M_HAMMER);
	}
}

/*
 * Called by hammer_sync_inode().
 */
static int
hammer_update_inode(hammer_cursor_t cursor, hammer_inode_t ip)
{
	hammer_transaction_t trans = cursor->trans;
	hammer_record_t record;
	int error;
	int redirty;

retry:
	error = 0;

	/*
	 * If the inode has a presence on-disk then locate it and mark
	 * it deleted, setting DELONDISK.
	 *
	 * The record may or may not be physically deleted, depending on
	 * the retention policy.
	 */
	if ((ip->flags & (HAMMER_INODE_ONDISK|HAMMER_INODE_DELONDISK)) ==
	    HAMMER_INODE_ONDISK) {
		hammer_normalize_cursor(cursor);
		cursor->key_beg.localization = ip->obj_localization + 
					       HAMMER_LOCALIZE_INODE;
		cursor->key_beg.obj_id = ip->obj_id;
		cursor->key_beg.key = 0;
		cursor->key_beg.create_tid = 0;
		cursor->key_beg.delete_tid = 0;
		cursor->key_beg.rec_type = HAMMER_RECTYPE_INODE;
		cursor->key_beg.obj_type = 0;
		cursor->asof = ip->obj_asof;
		cursor->flags &= ~HAMMER_CURSOR_INITMASK;
		cursor->flags |= HAMMER_CURSOR_GET_LEAF | HAMMER_CURSOR_ASOF;
		cursor->flags |= HAMMER_CURSOR_BACKEND;

		error = hammer_btree_lookup(cursor);
		if (hammer_debug_inode)
			kprintf("IPDEL %p %08x %d", ip, ip->flags, error);
		if (error) {
			kprintf("error %d\n", error);
			Debugger("hammer_update_inode");
		}

		if (error == 0) {
			error = hammer_ip_delete_record(cursor, ip, trans->tid);
			if (hammer_debug_inode)
				kprintf(" error %d\n", error);
			if (error && error != EDEADLK) {
				kprintf("error %d\n", error);
				Debugger("hammer_update_inode2");
			}
			if (error == 0) {
				ip->flags |= HAMMER_INODE_DELONDISK;
			}
			if (cursor->node)
				hammer_cache_node(&ip->cache[0], cursor->node);
		}
		if (error == EDEADLK) {
			hammer_done_cursor(cursor);
			error = hammer_init_cursor(trans, cursor,
						   &ip->cache[0], ip);
			if (hammer_debug_inode)
				kprintf("IPDED %p %d\n", ip, error);
			if (error == 0)
				goto retry;
		}
	}

	/*
	 * Ok, write out the initial record or a new record (after deleting
	 * the old one), unless the DELETED flag is set.  This routine will
	 * clear DELONDISK if it writes out a record.
	 *
	 * Update our inode statistics if this is the first application of
	 * the inode on-disk.
	 */
	if (error == 0 && (ip->flags & HAMMER_INODE_DELETED) == 0) {
		/*
		 * Generate a record and write it to the media
		 */
		record = hammer_alloc_mem_record(ip, 0);
		record->type = HAMMER_MEM_RECORD_INODE;
		record->flush_state = HAMMER_FST_FLUSH;
		record->leaf = ip->sync_ino_leaf;
		record->leaf.base.create_tid = trans->tid;
		record->leaf.data_len = sizeof(ip->sync_ino_data);
		record->leaf.create_ts = trans->time32;
		record->data = (void *)&ip->sync_ino_data;
		record->flags |= HAMMER_RECF_INTERLOCK_BE;

		/*
		 * If this flag is set we cannot sync the new file size
		 * because we haven't finished related truncations.  The
		 * inode will be flushed in another flush group to finish
		 * the job.
		 */
		if ((ip->flags & HAMMER_INODE_WOULDBLOCK) &&
		    ip->sync_ino_data.size != ip->ino_data.size) {
			redirty = 1;
			ip->sync_ino_data.size = ip->ino_data.size;
		} else {
			redirty = 0;
		}

		for (;;) {
			error = hammer_ip_sync_record_cursor(cursor, record);
			if (hammer_debug_inode)
				kprintf("GENREC %p rec %08x %d\n",	
					ip, record->flags, error);
			if (error != EDEADLK)
				break;
			hammer_done_cursor(cursor);
			error = hammer_init_cursor(trans, cursor,
						   &ip->cache[0], ip);
			if (hammer_debug_inode)
				kprintf("GENREC reinit %d\n", error);
			if (error)
				break;
		}
		if (error) {
			kprintf("error %d\n", error);
			Debugger("hammer_update_inode3");
		}

		/*
		 * The record isn't managed by the inode's record tree,
		 * destroy it whether we succeed or fail.
		 */
		record->flags &= ~HAMMER_RECF_INTERLOCK_BE;
		record->flags |= HAMMER_RECF_DELETED_FE;
		record->flush_state = HAMMER_FST_IDLE;
		hammer_rel_mem_record(record);

		/*
		 * Finish up.
		 */
		if (error == 0) {
			if (hammer_debug_inode)
				kprintf("CLEANDELOND %p %08x\n", ip, ip->flags);
			ip->sync_flags &= ~(HAMMER_INODE_DDIRTY |
					    HAMMER_INODE_ATIME |
					    HAMMER_INODE_MTIME);
			ip->flags &= ~HAMMER_INODE_DELONDISK;
			if (redirty)
				ip->sync_flags |= HAMMER_INODE_DDIRTY;

			/*
			 * Root volume count of inodes
			 */
			hammer_sync_lock_sh(trans);
			if ((ip->flags & HAMMER_INODE_ONDISK) == 0) {
				hammer_modify_volume_field(trans,
							   trans->rootvol,
							   vol0_stat_inodes);
				++ip->hmp->rootvol->ondisk->vol0_stat_inodes;
				hammer_modify_volume_done(trans->rootvol);
				ip->flags |= HAMMER_INODE_ONDISK;
				if (hammer_debug_inode)
					kprintf("NOWONDISK %p\n", ip);
			}
			hammer_sync_unlock(trans);
		}
	}

	/*
	 * If the inode has been destroyed, clean out any left-over flags
	 * that may have been set by the frontend.
	 */
	if (error == 0 && (ip->flags & HAMMER_INODE_DELETED)) { 
		ip->sync_flags &= ~(HAMMER_INODE_DDIRTY |
				    HAMMER_INODE_ATIME |
				    HAMMER_INODE_MTIME);
	}
	return(error);
}

/*
 * Update only the itimes fields.
 *
 * ATIME can be updated without generating any UNDO.  MTIME is updated
 * with UNDO so it is guaranteed to be synchronized properly in case of
 * a crash.
 *
 * Neither field is included in the B-Tree leaf element's CRC, which is how
 * we can get away with updating ATIME the way we do.
 */
static int
hammer_update_itimes(hammer_cursor_t cursor, hammer_inode_t ip)
{
	hammer_transaction_t trans = cursor->trans;
	int error;

retry:
	if ((ip->flags & (HAMMER_INODE_ONDISK|HAMMER_INODE_DELONDISK)) !=
	    HAMMER_INODE_ONDISK) {
		return(0);
	}

	hammer_normalize_cursor(cursor);
	cursor->key_beg.localization = ip->obj_localization + 
				       HAMMER_LOCALIZE_INODE;
	cursor->key_beg.obj_id = ip->obj_id;
	cursor->key_beg.key = 0;
	cursor->key_beg.create_tid = 0;
	cursor->key_beg.delete_tid = 0;
	cursor->key_beg.rec_type = HAMMER_RECTYPE_INODE;
	cursor->key_beg.obj_type = 0;
	cursor->asof = ip->obj_asof;
	cursor->flags &= ~HAMMER_CURSOR_INITMASK;
	cursor->flags |= HAMMER_CURSOR_ASOF;
	cursor->flags |= HAMMER_CURSOR_GET_LEAF;
	cursor->flags |= HAMMER_CURSOR_GET_DATA;
	cursor->flags |= HAMMER_CURSOR_BACKEND;

	error = hammer_btree_lookup(cursor);
	if (error) {
		kprintf("error %d\n", error);
		Debugger("hammer_update_itimes1");
	}
	if (error == 0) {
		hammer_cache_node(&ip->cache[0], cursor->node);
		if (ip->sync_flags & HAMMER_INODE_MTIME) {
			/*
			 * Updating MTIME requires an UNDO.  Just cover
			 * both atime and mtime.
			 */
			hammer_sync_lock_sh(trans);
			hammer_modify_buffer(trans, cursor->data_buffer,
				     HAMMER_ITIMES_BASE(&cursor->data->inode),
				     HAMMER_ITIMES_BYTES);
			cursor->data->inode.atime = ip->sync_ino_data.atime;
			cursor->data->inode.mtime = ip->sync_ino_data.mtime;
			hammer_modify_buffer_done(cursor->data_buffer);
			hammer_sync_unlock(trans);
		} else if (ip->sync_flags & HAMMER_INODE_ATIME) {
			/*
			 * Updating atime only can be done in-place with
			 * no UNDO.
			 */
			hammer_sync_lock_sh(trans);
			hammer_modify_buffer(trans, cursor->data_buffer,
					     NULL, 0);
			cursor->data->inode.atime = ip->sync_ino_data.atime;
			hammer_modify_buffer_done(cursor->data_buffer);
			hammer_sync_unlock(trans);
		}
		ip->sync_flags &= ~(HAMMER_INODE_ATIME | HAMMER_INODE_MTIME);
	}
	if (error == EDEADLK) {
		hammer_done_cursor(cursor);
		error = hammer_init_cursor(trans, cursor,
					   &ip->cache[0], ip);
		if (error == 0)
			goto retry;
	}
	return(error);
}

/*
 * Release a reference on an inode, flush as requested.
 *
 * On the last reference we queue the inode to the flusher for its final
 * disposition.
 */
void
hammer_rel_inode(struct hammer_inode *ip, int flush)
{
	hammer_mount_t hmp = ip->hmp;

	/*
	 * Handle disposition when dropping the last ref.
	 */
	for (;;) {
		if (ip->lock.refs == 1) {
			/*
			 * Determine whether on-disk action is needed for
			 * the inode's final disposition.
			 */
			KKASSERT(ip->vp == NULL);
			hammer_inode_unloadable_check(ip, 0);
			if (ip->flags & HAMMER_INODE_MODMASK) {
				if (hmp->rsv_inodes > desiredvnodes) {
					hammer_flush_inode(ip,
							   HAMMER_FLUSH_SIGNAL);
				} else {
					hammer_flush_inode(ip, 0);
				}
			} else if (ip->lock.refs == 1) {
				hammer_unload_inode(ip);
				break;
			}
		} else {
			if (flush)
				hammer_flush_inode(ip, 0);

			/*
			 * The inode still has multiple refs, try to drop
			 * one ref.
			 */
			KKASSERT(ip->lock.refs >= 1);
			if (ip->lock.refs > 1) {
				hammer_unref(&ip->lock);
				break;
			}
		}
	}
}

/*
 * Unload and destroy the specified inode.  Must be called with one remaining
 * reference.  The reference is disposed of.
 *
 * This can only be called in the context of the flusher.
 */
static int
hammer_unload_inode(struct hammer_inode *ip)
{
	hammer_mount_t hmp = ip->hmp;

	KASSERT(ip->lock.refs == 1,
		("hammer_unload_inode: %d refs\n", ip->lock.refs));
	KKASSERT(ip->vp == NULL);
	KKASSERT(ip->flush_state == HAMMER_FST_IDLE);
	KKASSERT(ip->cursor_ip_refs == 0);
	KKASSERT(ip->lock.lockcount == 0);
	KKASSERT((ip->flags & HAMMER_INODE_MODMASK) == 0);

	KKASSERT(RB_EMPTY(&ip->rec_tree));
	KKASSERT(TAILQ_EMPTY(&ip->target_list));

	RB_REMOVE(hammer_ino_rb_tree, &hmp->rb_inos_root, ip);

	hammer_free_inode(ip);
	return(0);
}

/*
 * Called on mount -u when switching from RW to RO or vise-versa.  Adjust
 * the read-only flag for cached inodes.
 *
 * This routine is called from a RB_SCAN().
 */
int
hammer_reload_inode(hammer_inode_t ip, void *arg __unused)
{
	hammer_mount_t hmp = ip->hmp;

	if (hmp->ronly || hmp->asof != HAMMER_MAX_TID)
		ip->flags |= HAMMER_INODE_RO;
	else
		ip->flags &= ~HAMMER_INODE_RO;
	return(0);
}

/*
 * A transaction has modified an inode, requiring updates as specified by
 * the passed flags.
 *
 * HAMMER_INODE_DDIRTY: Inode data has been updated
 * HAMMER_INODE_XDIRTY: Dirty in-memory records
 * HAMMER_INODE_BUFS:   Dirty buffer cache buffers
 * HAMMER_INODE_DELETED: Inode record/data must be deleted
 * HAMMER_INODE_ATIME/MTIME: mtime/atime has been updated
 */
void
hammer_modify_inode(hammer_inode_t ip, int flags)
{
	KKASSERT(ip->hmp->ronly == 0 ||
		  (flags & (HAMMER_INODE_DDIRTY | HAMMER_INODE_XDIRTY | 
			    HAMMER_INODE_BUFS | HAMMER_INODE_DELETED |
			    HAMMER_INODE_ATIME | HAMMER_INODE_MTIME)) == 0);
	if ((ip->flags & HAMMER_INODE_RSV_INODES) == 0) {
		ip->flags |= HAMMER_INODE_RSV_INODES;
		++ip->hmp->rsv_inodes;
	}

	ip->flags |= flags;
}

/*
 * Request that an inode be flushed.  This whole mess cannot block and may
 * recurse (if not synchronous).  Once requested HAMMER will attempt to
 * actively flush the inode until the flush can be done.
 *
 * The inode may already be flushing, or may be in a setup state.  We can
 * place the inode in a flushing state if it is currently idle and flag it
 * to reflush if it is currently flushing.
 *
 * If the HAMMER_FLUSH_SYNCHRONOUS flag is specified we will attempt to
 * flush the indoe synchronously using the caller's context.
 */
void
hammer_flush_inode(hammer_inode_t ip, int flags)
{
	int good;

	/*
	 * Trivial 'nothing to flush' case.  If the inode is ina SETUP
	 * state we have to put it back into an IDLE state so we can
	 * drop the extra ref.
	 */
	if ((ip->flags & HAMMER_INODE_MODMASK) == 0) {
		if (ip->flush_state == HAMMER_FST_SETUP) {
			ip->flush_state = HAMMER_FST_IDLE;
			hammer_rel_inode(ip, 0);
		}
		return;
	}

	/*
	 * Our flush action will depend on the current state.
	 */
	switch(ip->flush_state) {
	case HAMMER_FST_IDLE:
		/*
		 * We have no dependancies and can flush immediately.  Some
		 * our children may not be flushable so we have to re-test
		 * with that additional knowledge.
		 */
		hammer_flush_inode_core(ip, flags);
		break;
	case HAMMER_FST_SETUP:
		/*
		 * Recurse upwards through dependancies via target_list
		 * and start their flusher actions going if possible.
		 *
		 * 'good' is our connectivity.  -1 means we have none and
		 * can't flush, 0 means there weren't any dependancies, and
		 * 1 means we have good connectivity.
		 */
		good = hammer_setup_parent_inodes(ip);

		/*
		 * We can continue if good >= 0.  Determine how many records
		 * under our inode can be flushed (and mark them).
		 */
		if (good >= 0) {
			hammer_flush_inode_core(ip, flags);
		} else {
			ip->flags |= HAMMER_INODE_REFLUSH;
			if (flags & HAMMER_FLUSH_SIGNAL) {
				ip->flags |= HAMMER_INODE_RESIGNAL;
				hammer_flusher_async(ip->hmp);
			}
		}
		break;
	default:
		/*
		 * We are already flushing, flag the inode to reflush
		 * if needed after it completes its current flush.
		 */
		if ((ip->flags & HAMMER_INODE_REFLUSH) == 0)
			ip->flags |= HAMMER_INODE_REFLUSH;
		if (flags & HAMMER_FLUSH_SIGNAL) {
			ip->flags |= HAMMER_INODE_RESIGNAL;
			hammer_flusher_async(ip->hmp);
		}
		break;
	}
}

/*
 * Scan ip->target_list, which is a list of records owned by PARENTS to our
 * ip which reference our ip.
 *
 * XXX This is a huge mess of recursive code, but not one bit of it blocks
 *     so for now do not ref/deref the structures.  Note that if we use the
 *     ref/rel code later, the rel CAN block.
 */
static int
hammer_setup_parent_inodes(hammer_inode_t ip)
{
	hammer_record_t depend;
#if 0
	hammer_record_t next;
	hammer_inode_t  pip;
#endif
	int good;
	int r;

	good = 0;
	TAILQ_FOREACH(depend, &ip->target_list, target_entry) {
		r = hammer_setup_parent_inodes_helper(depend);
		KKASSERT(depend->target_ip == ip);
		if (r < 0 && good == 0)
			good = -1;
		if (r > 0)
			good = 1;
	}
	return(good);

#if 0
retry:
	good = 0;
	next = TAILQ_FIRST(&ip->target_list);
	if (next) {
		hammer_ref(&next->lock);
		hammer_ref(&next->ip->lock);
	}
	while ((depend = next) != NULL) {
		if (depend->target_ip == NULL) {
			pip = depend->ip;
			hammer_rel_mem_record(depend);
			hammer_rel_inode(pip, 0);
			goto retry;
		}
		KKASSERT(depend->target_ip == ip);
		next = TAILQ_NEXT(depend, target_entry);
		if (next) {
			hammer_ref(&next->lock);
			hammer_ref(&next->ip->lock);
		}
		r = hammer_setup_parent_inodes_helper(depend);
		if (r < 0 && good == 0)
			good = -1;
		if (r > 0)
			good = 1;
		pip = depend->ip;
		hammer_rel_mem_record(depend);
		hammer_rel_inode(pip, 0);
	}
	return(good);
#endif
}

/*
 * This helper function takes a record representing the dependancy between
 * the parent inode and child inode.
 *
 * record->ip		= parent inode
 * record->target_ip	= child inode
 * 
 * We are asked to recurse upwards and convert the record from SETUP
 * to FLUSH if possible.
 *
 * Return 1 if the record gives us connectivity
 *
 * Return 0 if the record is not relevant 
 *
 * Return -1 if we can't resolve the dependancy and there is no connectivity.
 */
static int
hammer_setup_parent_inodes_helper(hammer_record_t record)
{
	hammer_mount_t hmp;
	hammer_inode_t pip;
	int good;

	KKASSERT(record->flush_state != HAMMER_FST_IDLE);
	pip = record->ip;
	hmp = pip->hmp;

	/*
	 * If the record is already flushing, is it in our flush group?
	 *
	 * If it is in our flush group but it is a general record or a 
	 * delete-on-disk, it does not improve our connectivity (return 0),
	 * and if the target inode is not trying to destroy itself we can't
	 * allow the operation yet anyway (the second return -1).
	 */
	if (record->flush_state == HAMMER_FST_FLUSH) {
		if (record->flush_group != hmp->flusher.next) {
			pip->flags |= HAMMER_INODE_REFLUSH;
			return(-1);
		}
		if (record->type == HAMMER_MEM_RECORD_ADD)
			return(1);
		/* GENERAL or DEL */
		return(0);
	}

	/*
	 * It must be a setup record.  Try to resolve the setup dependancies
	 * by recursing upwards so we can place ip on the flush list.
	 */
	KKASSERT(record->flush_state == HAMMER_FST_SETUP);

	good = hammer_setup_parent_inodes(pip);

	/*
	 * We can't flush ip because it has no connectivity (XXX also check
	 * nlinks for pre-existing connectivity!).  Flag it so any resolution
	 * recurses back down.
	 */
	if (good < 0) {
		pip->flags |= HAMMER_INODE_REFLUSH;
		return(good);
	}

	/*
	 * We are go, place the parent inode in a flushing state so we can
	 * place its record in a flushing state.  Note that the parent
	 * may already be flushing.  The record must be in the same flush
	 * group as the parent.
	 */
	if (pip->flush_state != HAMMER_FST_FLUSH)
		hammer_flush_inode_core(pip, HAMMER_FLUSH_RECURSION);
	KKASSERT(pip->flush_state == HAMMER_FST_FLUSH);
	KKASSERT(record->flush_state == HAMMER_FST_SETUP);

#if 0
	if (record->type == HAMMER_MEM_RECORD_DEL &&
	    (record->target_ip->flags & (HAMMER_INODE_DELETED|HAMMER_INODE_DELONDISK)) == 0) {
		/*
		 * Regardless of flushing state we cannot sync this path if the
		 * record represents a delete-on-disk but the target inode
		 * is not ready to sync its own deletion.
		 *
		 * XXX need to count effective nlinks to determine whether
		 * the flush is ok, otherwise removing a hardlink will
		 * just leave the DEL record to rot.
		 */
		record->target_ip->flags |= HAMMER_INODE_REFLUSH;
		return(-1);
	} else
#endif
	if (pip->flush_group == pip->hmp->flusher.next) {
		/*
		 * This is the record we wanted to synchronize.  If the
		 * record went into a flush state while we blocked it 
		 * had better be in the correct flush group.
		 */
		if (record->flush_state != HAMMER_FST_FLUSH) {
			record->flush_state = HAMMER_FST_FLUSH;
			record->flush_group = pip->flush_group;
			hammer_ref(&record->lock);
		} else {
			KKASSERT(record->flush_group == pip->flush_group);
		}
		if (record->type == HAMMER_MEM_RECORD_ADD)
			return(1);

		/*
		 * A general or delete-on-disk record does not contribute
		 * to our visibility.  We can still flush it, however.
		 */
		return(0);
	} else {
		/*
		 * We couldn't resolve the dependancies, request that the
		 * inode be flushed when the dependancies can be resolved.
		 */
		pip->flags |= HAMMER_INODE_REFLUSH;
		return(-1);
	}
}

/*
 * This is the core routine placing an inode into the FST_FLUSH state.
 */
static void
hammer_flush_inode_core(hammer_inode_t ip, int flags)
{
	int go_count;

	/*
	 * Set flush state and prevent the flusher from cycling into
	 * the next flush group.  Do not place the ip on the list yet.
	 * Inodes not in the idle state get an extra reference.
	 */
	KKASSERT(ip->flush_state != HAMMER_FST_FLUSH);
	if (ip->flush_state == HAMMER_FST_IDLE)
		hammer_ref(&ip->lock);
	ip->flush_state = HAMMER_FST_FLUSH;
	ip->flush_group = ip->hmp->flusher.next;
	++ip->hmp->flusher.group_lock;
	++ip->hmp->count_iqueued;
	++hammer_count_iqueued;

	/*
	 * We need to be able to vfsync/truncate from the backend.
	 */
	KKASSERT((ip->flags & HAMMER_INODE_VHELD) == 0);
	if (ip->vp && (ip->vp->v_flag & VINACTIVE) == 0) {
		ip->flags |= HAMMER_INODE_VHELD;
		vref(ip->vp);
	}

	/*
	 * Figure out how many in-memory records we can actually flush
	 * (not including inode meta-data, buffers, etc).
	 *
	 * Do not add new records to the flush if this is a recursion or
	 * if we must still complete a flush from the previous flush cycle.
	 */
	if (flags & HAMMER_FLUSH_RECURSION) {
		go_count = 1;
	} else if (ip->flags & HAMMER_INODE_WOULDBLOCK) {
		go_count = RB_SCAN(hammer_rec_rb_tree, &ip->rec_tree, NULL,
				   hammer_syncgrp_child_callback, NULL);
		go_count = 1;
	} else {
		go_count = RB_SCAN(hammer_rec_rb_tree, &ip->rec_tree, NULL,
				   hammer_setup_child_callback, NULL);
	}

	/*
	 * This is a more involved test that includes go_count.  If we
	 * can't flush, flag the inode and return.  If go_count is 0 we
	 * were are unable to flush any records in our rec_tree and
	 * must ignore the XDIRTY flag.
	 */
	if (go_count == 0) {
		if ((ip->flags & HAMMER_INODE_MODMASK_NOXDIRTY) == 0) {
			ip->flags |= HAMMER_INODE_REFLUSH;

			--ip->hmp->count_iqueued;
			--hammer_count_iqueued;

			ip->flush_state = HAMMER_FST_SETUP;
			if (ip->flags & HAMMER_INODE_VHELD) {
				ip->flags &= ~HAMMER_INODE_VHELD;
				vrele(ip->vp);
			}
			if (flags & HAMMER_FLUSH_SIGNAL) {
				ip->flags |= HAMMER_INODE_RESIGNAL;
				hammer_flusher_async(ip->hmp);
			}
			if (--ip->hmp->flusher.group_lock == 0)
				wakeup(&ip->hmp->flusher.group_lock);
			return;
		}
	}

	/*
	 * Snapshot the state of the inode for the backend flusher.
	 *
	 * We continue to retain save_trunc_off even when all truncations
	 * have been resolved as an optimization to determine if we can
	 * skip the B-Tree lookup for overwrite deletions.
	 *
	 * NOTE: The DELETING flag is a mod flag, but it is also sticky,
	 * and stays in ip->flags.  Once set, it stays set until the
	 * inode is destroyed.
	 *
	 * NOTE: If a truncation from a previous flush cycle had to be
	 * continued into this one, the TRUNCATED flag will still be
	 * set in sync_flags as will WOULDBLOCK.  When this occurs
	 * we CANNOT safely integrate a new truncation from the front-end
	 * because there may be data records in-memory assigned a flush
	 * state from the previous cycle that are supposed to be flushed
	 * before the next frontend truncation.
	 */
	if ((ip->flags & (HAMMER_INODE_TRUNCATED | HAMMER_INODE_WOULDBLOCK)) ==
	    HAMMER_INODE_TRUNCATED) {
		KKASSERT((ip->sync_flags & HAMMER_INODE_TRUNCATED) == 0);
		ip->sync_trunc_off = ip->trunc_off;
		ip->trunc_off = 0x7FFFFFFFFFFFFFFFLL;
		ip->flags &= ~HAMMER_INODE_TRUNCATED;
		ip->sync_flags |= HAMMER_INODE_TRUNCATED;

		/*
		 * The save_trunc_off used to cache whether the B-Tree
		 * holds any records past that point is not used until
		 * after the truncation has succeeded, so we can safely
		 * set it now.
		 */
		if (ip->save_trunc_off > ip->sync_trunc_off)
			ip->save_trunc_off = ip->sync_trunc_off;
	}
	ip->sync_flags |= (ip->flags & HAMMER_INODE_MODMASK &
			   ~HAMMER_INODE_TRUNCATED);
	ip->sync_ino_leaf = ip->ino_leaf;
	ip->sync_ino_data = ip->ino_data;
	ip->flags &= ~HAMMER_INODE_MODMASK | HAMMER_INODE_TRUNCATED;
#ifdef DEBUG_TRUNCATE
	if ((ip->sync_flags & HAMMER_INODE_TRUNCATED) && ip == HammerTruncIp)
		kprintf("truncateS %016llx\n", ip->sync_trunc_off);
#endif

	/*
	 * The flusher list inherits our inode and reference.
	 */
	TAILQ_INSERT_TAIL(&ip->hmp->flush_list, ip, flush_entry);
	if (--ip->hmp->flusher.group_lock == 0)
		wakeup(&ip->hmp->flusher.group_lock);

	if (flags & HAMMER_FLUSH_SIGNAL) {
		hammer_flusher_async(ip->hmp);
	}
}

/*
 * Callback for scan of ip->rec_tree.  Try to include each record in our
 * flush.  ip->flush_group has been set but the inode has not yet been
 * moved into a flushing state.
 *
 * If we get stuck on a record we have to set HAMMER_INODE_REFLUSH on
 * both inodes.
 *
 * We return 1 for any record placed or found in FST_FLUSH, which prevents
 * the caller from shortcutting the flush.
 */
static int
hammer_setup_child_callback(hammer_record_t rec, void *data)
{
	hammer_inode_t target_ip;
	hammer_inode_t ip;
	int r;

	/*
	 * Deleted records are ignored.  Note that the flush detects deleted
	 * front-end records at multiple points to deal with races.  This is
	 * just the first line of defense.  The only time DELETED_FE cannot
	 * be set is when HAMMER_RECF_INTERLOCK_BE is set. 
	 *
	 * Don't get confused between record deletion and, say, directory
	 * entry deletion.  The deletion of a directory entry that is on
	 * the media has nothing to do with the record deletion flags.
	 *
	 * The flush_group for a record already in a flush state must
	 * be updated.  This case can only occur if the inode deleting
	 * too many records had to be moved to the next flush group.
	 */
	if (rec->flags & (HAMMER_RECF_DELETED_FE|HAMMER_RECF_DELETED_BE)) {
		if (rec->flush_state == HAMMER_FST_FLUSH) {
			KKASSERT(rec->ip->flags & HAMMER_INODE_WOULDBLOCK);
			rec->flush_group = rec->ip->flush_group;
			r = 1;
		} else {
			r = 0;
		}
		return(r);
	}

	/*
	 * If the record is in an idle state it has no dependancies and
	 * can be flushed.
	 */
	ip = rec->ip;
	r = 0;

	switch(rec->flush_state) {
	case HAMMER_FST_IDLE:
		/*
		 * Record has no setup dependancy, we can flush it.
		 */
		KKASSERT(rec->target_ip == NULL);
		rec->flush_state = HAMMER_FST_FLUSH;
		rec->flush_group = ip->flush_group;
		hammer_ref(&rec->lock);
		r = 1;
		break;
	case HAMMER_FST_SETUP:
		/*
		 * Record has a setup dependancy.  Try to include the
		 * target ip in the flush. 
		 *
		 * We have to be careful here, if we do not do the right
		 * thing we can lose track of dirty inodes and the system
		 * will lockup trying to allocate buffers.
		 */
		target_ip = rec->target_ip;
		KKASSERT(target_ip != NULL);
		KKASSERT(target_ip->flush_state != HAMMER_FST_IDLE);
		if (target_ip->flush_state == HAMMER_FST_FLUSH) {
			/*
			 * If the target IP is already flushing in our group
			 * we are golden, otherwise make sure the target
			 * reflushes.
			 */
			if (target_ip->flush_group == ip->flush_group) {
				rec->flush_state = HAMMER_FST_FLUSH;
				rec->flush_group = ip->flush_group;
				hammer_ref(&rec->lock);
				r = 1;
			} else {
				target_ip->flags |= HAMMER_INODE_REFLUSH;
			}
		} else if (rec->type == HAMMER_MEM_RECORD_ADD) {
			/*
			 * If the target IP is not flushing we can force
			 * it to flush, even if it is unable to write out
			 * any of its own records we have at least one in
			 * hand that we CAN deal with.
			 */
			rec->flush_state = HAMMER_FST_FLUSH;
			rec->flush_group = ip->flush_group;
			hammer_ref(&rec->lock);
			hammer_flush_inode_core(target_ip,
						HAMMER_FLUSH_RECURSION);
			r = 1;
		} else {
			/*
			 * General or delete-on-disk record.
			 *
			 * XXX this needs help.  If a delete-on-disk we could
			 * disconnect the target.  If the target has its own
			 * dependancies they really need to be flushed.
			 *
			 * XXX
			 */
			rec->flush_state = HAMMER_FST_FLUSH;
			rec->flush_group = ip->flush_group;
			hammer_ref(&rec->lock);
			hammer_flush_inode_core(target_ip,
						HAMMER_FLUSH_RECURSION);
			r = 1;
		}
		break;
	case HAMMER_FST_FLUSH:
		/* 
		 * If the WOULDBLOCK flag is set records may have been left
		 * over from a previous flush attempt and should be moved
		 * to the current flush group.  If it is not set then all
		 * such records had better have been flushed already or
		 * already associated with the current flush group.
		 */
		if (ip->flags & HAMMER_INODE_WOULDBLOCK) {
			rec->flush_group = ip->flush_group;
		} else {
			KKASSERT(rec->flush_group == ip->flush_group);
		}
		r = 1;
		break;
	}
	return(r);
}

/*
 * This version just moves records already in a flush state to the new
 * flush group and that is it.
 */
static int
hammer_syncgrp_child_callback(hammer_record_t rec, void *data)
{
	hammer_inode_t ip = rec->ip;

	switch(rec->flush_state) {
	case HAMMER_FST_FLUSH:
		if (ip->flags & HAMMER_INODE_WOULDBLOCK) {
			rec->flush_group = ip->flush_group;
		} else {
			KKASSERT(rec->flush_group == ip->flush_group);
		}
		break;
	default:
		break;
	}
	return(0);
}

/*
 * Wait for a previously queued flush to complete.  Not only do we need to
 * wait for the inode to sync out, we also may have to run the flusher again
 * to get it past the UNDO position pertaining to the flush so a crash does
 * not 'undo' our flush.
 */
void
hammer_wait_inode(hammer_inode_t ip)
{
	hammer_mount_t hmp = ip->hmp;
	int sync_group;
	int waitcount;

	sync_group = ip->flush_group;
	waitcount = (ip->flags & HAMMER_INODE_REFLUSH) ? 2 : 1;

	if (ip->flush_state == HAMMER_FST_SETUP) {
		hammer_flush_inode(ip, HAMMER_FLUSH_SIGNAL);
	}
	/* XXX can we make this != FST_IDLE ? check SETUP depends */
	while (ip->flush_state == HAMMER_FST_FLUSH &&
	       (ip->flush_group - sync_group) < waitcount) {
		ip->flags |= HAMMER_INODE_FLUSHW;
		tsleep(&ip->flags, 0, "hmrwin", 0);
	}
	while (hmp->flusher.done - sync_group < waitcount) {
		kprintf("Y");
		hammer_flusher_sync(hmp);
	}
}

/*
 * Called by the backend code when a flush has been completed.
 * The inode has already been removed from the flush list.
 *
 * A pipelined flush can occur, in which case we must re-enter the
 * inode on the list and re-copy its fields.
 */
void
hammer_flush_inode_done(hammer_inode_t ip)
{
	hammer_mount_t hmp;
	int dorel;

	KKASSERT(ip->flush_state == HAMMER_FST_FLUSH);

	hmp = ip->hmp;

	/*
	 * Merge left-over flags back into the frontend and fix the state.
	 * Incomplete truncations are retained by the backend.
	 */
	ip->flags |= ip->sync_flags & ~HAMMER_INODE_TRUNCATED;
	ip->sync_flags &= HAMMER_INODE_TRUNCATED;

	/*
	 * The backend may have adjusted nlinks, so if the adjusted nlinks
	 * does not match the fronttend set the frontend's RDIRTY flag again.
	 */
	if (ip->ino_data.nlinks != ip->sync_ino_data.nlinks)
		ip->flags |= HAMMER_INODE_DDIRTY;

	/*
	 * Fix up the dirty buffer status.
	 */
	if (ip->vp && RB_ROOT(&ip->vp->v_rbdirty_tree)) {
		ip->flags |= HAMMER_INODE_BUFS;
	}

	/*
	 * Re-set the XDIRTY flag if some of the inode's in-memory records
	 * could not be flushed.
	 */
	KKASSERT((RB_EMPTY(&ip->rec_tree) &&
		  (ip->flags & HAMMER_INODE_XDIRTY) == 0) ||
		 (!RB_EMPTY(&ip->rec_tree) &&
		  (ip->flags & HAMMER_INODE_XDIRTY) != 0));

	/*
	 * Do not lose track of inodes which no longer have vnode
	 * assocations, otherwise they may never get flushed again.
	 */
	if ((ip->flags & HAMMER_INODE_MODMASK) && ip->vp == NULL)
		ip->flags |= HAMMER_INODE_REFLUSH;

	/*
	 * Clean up the vnode ref
	 */
	if (ip->flags & HAMMER_INODE_VHELD) {
		ip->flags &= ~HAMMER_INODE_VHELD;
		vrele(ip->vp);
	}

	/*
	 * Adjust flush_state.  The target state (idle or setup) shouldn't
	 * be terribly important since we will reflush if we really need
	 * to do anything.
	 *
	 * If the WOULDBLOCK flag is set we must re-flush immediately
	 * to continue a potentially large deletion.  The flag also causes
	 * the hammer_setup_child_callback() to move records in the old
	 * flush group to the new one.
	 */
	if (ip->flags & HAMMER_INODE_WOULDBLOCK) {
		ip->flush_state = HAMMER_FST_IDLE;
		hammer_flush_inode_core(ip, HAMMER_FLUSH_SIGNAL);
		ip->flags &= ~HAMMER_INODE_WOULDBLOCK;
		dorel = 1;
	} else if (TAILQ_EMPTY(&ip->target_list) && RB_EMPTY(&ip->rec_tree)) {
		ip->flush_state = HAMMER_FST_IDLE;
		dorel = 1;
	} else {
		ip->flush_state = HAMMER_FST_SETUP;
		dorel = 0;
	}

	--hmp->count_iqueued;
	--hammer_count_iqueued;

	/*
	 * If the frontend made more changes and requested another flush,
	 * then try to get it running.
	 */
	if (ip->flags & HAMMER_INODE_REFLUSH) {
		ip->flags &= ~HAMMER_INODE_REFLUSH;
		if (ip->flags & HAMMER_INODE_RESIGNAL) {
			ip->flags &= ~HAMMER_INODE_RESIGNAL;
			hammer_flush_inode(ip, HAMMER_FLUSH_SIGNAL);
		} else {
			hammer_flush_inode(ip, 0);
		}
	}

	/*
	 * If the inode is now clean drop the space reservation.
	 */
	if ((ip->flags & HAMMER_INODE_MODMASK) == 0 &&
	    (ip->flags & HAMMER_INODE_RSV_INODES)) {
		ip->flags &= ~HAMMER_INODE_RSV_INODES;
		--hmp->rsv_inodes;
	}

	/*
	 * Finally, if the frontend is waiting for a flush to complete,
	 * wake it up.
	 */
	if (ip->flush_state != HAMMER_FST_FLUSH) {
		if (ip->flags & HAMMER_INODE_FLUSHW) {
			ip->flags &= ~HAMMER_INODE_FLUSHW;
			wakeup(&ip->flags);
		}
	}
	if (dorel)
		hammer_rel_inode(ip, 0);
}

/*
 * Called from hammer_sync_inode() to synchronize in-memory records
 * to the media.
 */
static int
hammer_sync_record_callback(hammer_record_t record, void *data)
{
	hammer_cursor_t cursor = data;
	hammer_transaction_t trans = cursor->trans;
	hammer_mount_t hmp = trans->hmp;
	int error;

	/*
	 * Skip records that do not belong to the current flush.
	 */
	++hammer_stats_record_iterations;
	if (record->flush_state != HAMMER_FST_FLUSH)
		return(0);

#if 1
	if (record->flush_group != record->ip->flush_group) {
		kprintf("sync_record %p ip %p bad flush group %d %d\n", record, record->ip, record->flush_group ,record->ip->flush_group);
		Debugger("blah2");
		return(0);
	}
#endif
	KKASSERT(record->flush_group == record->ip->flush_group);

	/*
	 * Interlock the record using the BE flag.  Once BE is set the
	 * frontend cannot change the state of FE.
	 *
	 * NOTE: If FE is set prior to us setting BE we still sync the
	 * record out, but the flush completion code converts it to 
	 * a delete-on-disk record instead of destroying it.
	 */
	KKASSERT((record->flags & HAMMER_RECF_INTERLOCK_BE) == 0);
	record->flags |= HAMMER_RECF_INTERLOCK_BE;

	/*
	 * The backend may have already disposed of the record.
	 */
	if (record->flags & HAMMER_RECF_DELETED_BE) {
		error = 0;
		goto done;
	}

	/*
	 * If the whole inode is being deleting all on-disk records will
	 * be deleted very soon, we can't sync any new records to disk
	 * because they will be deleted in the same transaction they were
	 * created in (delete_tid == create_tid), which will assert.
	 *
	 * XXX There may be a case with RECORD_ADD with DELETED_FE set
	 * that we currently panic on.
	 */
	if (record->ip->sync_flags & HAMMER_INODE_DELETING) {
		switch(record->type) {
		case HAMMER_MEM_RECORD_DATA:
			/*
			 * We don't have to do anything, if the record was
			 * committed the space will have been accounted for
			 * in the blockmap.
			 */
			/* fall through */
		case HAMMER_MEM_RECORD_GENERAL:
			record->flags |= HAMMER_RECF_DELETED_FE;
			record->flags |= HAMMER_RECF_DELETED_BE;
			error = 0;
			goto done;
		case HAMMER_MEM_RECORD_ADD:
			panic("hammer_sync_record_callback: illegal add "
			      "during inode deletion record %p", record);
			break; /* NOT REACHED */
		case HAMMER_MEM_RECORD_INODE:
			panic("hammer_sync_record_callback: attempt to "
			      "sync inode record %p?", record);
			break; /* NOT REACHED */
		case HAMMER_MEM_RECORD_DEL:
			/* 
			 * Follow through and issue the on-disk deletion
			 */
			break;
		}
	}

	/*
	 * If DELETED_FE is set special handling is needed for directory
	 * entries.  Dependant pieces related to the directory entry may
	 * have already been synced to disk.  If this occurs we have to
	 * sync the directory entry and then change the in-memory record
	 * from an ADD to a DELETE to cover the fact that it's been
	 * deleted by the frontend.
	 *
	 * A directory delete covering record (MEM_RECORD_DEL) can never
	 * be deleted by the frontend.
	 *
	 * Any other record type (aka DATA) can be deleted by the frontend.
	 * XXX At the moment the flusher must skip it because there may
	 * be another data record in the flush group for the same block,
	 * meaning that some frontend data changes can leak into the backend's
	 * synchronization point.
	 */
	if (record->flags & HAMMER_RECF_DELETED_FE) {
		if (record->type == HAMMER_MEM_RECORD_ADD) {
			record->flags |= HAMMER_RECF_CONVERT_DELETE;
		} else {
			KKASSERT(record->type != HAMMER_MEM_RECORD_DEL);
			record->flags |= HAMMER_RECF_DELETED_BE;
			error = 0;
			goto done;
		}
	}

	/*
	 * Assign the create_tid for new records.  Deletions already
	 * have the record's entire key properly set up.
	 */
	if (record->type != HAMMER_MEM_RECORD_DEL)
		record->leaf.base.create_tid = trans->tid;
		record->leaf.create_ts = trans->time32;
	for (;;) {
		error = hammer_ip_sync_record_cursor(cursor, record);
		if (error != EDEADLK)
			break;
		hammer_done_cursor(cursor);
		error = hammer_init_cursor(trans, cursor, &record->ip->cache[0],
					   record->ip);
		if (error)
			break;
	}
	record->flags &= ~HAMMER_RECF_CONVERT_DELETE;

	if (error) {
		error = -error;
		if (error != -ENOSPC) {
			kprintf("hammer_sync_record_callback: sync failed rec "
				"%p, error %d\n", record, error);
			Debugger("sync failed rec");
		}
	}
done:
	hammer_flush_record_done(record, error);

	/*
	 * Do partial finalization if we have built up too many dirty
	 * buffers.  Otherwise a buffer cache deadlock can occur when
	 * doing things like creating tens of thousands of tiny files.
	 *
	 * The finalization lock is already being held by virtue of the
	 * flusher calling us.
	 */
        if (hammer_flusher_meta_limit(hmp))
                hammer_flusher_finalize(trans, 0);

	return(error);
}

/*
 * XXX error handling
 */
int
hammer_sync_inode(hammer_inode_t ip)
{
	struct hammer_transaction trans;
	struct hammer_cursor cursor;
	hammer_node_t tmp_node;
	hammer_record_t depend;
	hammer_record_t next;
	int error, tmp_error;
	u_int64_t nlinks;

	if ((ip->sync_flags & HAMMER_INODE_MODMASK) == 0)
		return(0);

	hammer_start_transaction_fls(&trans, ip->hmp);
	error = hammer_init_cursor(&trans, &cursor, &ip->cache[1], ip);
	if (error)
		goto done;

	/*
	 * Any directory records referencing this inode which are not in
	 * our current flush group must adjust our nlink count for the
	 * purposes of synchronization to disk.
	 *
	 * Records which are in our flush group can be unlinked from our
	 * inode now, potentially allowing the inode to be physically
	 * deleted.
	 *
	 * This cannot block.
	 */
	nlinks = ip->ino_data.nlinks;
	next = TAILQ_FIRST(&ip->target_list);
	while ((depend = next) != NULL) {
		next = TAILQ_NEXT(depend, target_entry);
		if (depend->flush_state == HAMMER_FST_FLUSH &&
		    depend->flush_group == ip->hmp->flusher.act) {
			/*
			 * If this is an ADD that was deleted by the frontend
			 * the frontend nlinks count will have already been
			 * decremented, but the backend is going to sync its
			 * directory entry and must account for it.  The
			 * record will be converted to a delete-on-disk when
			 * it gets synced.
			 *
			 * If the ADD was not deleted by the frontend we
			 * can remove the dependancy from our target_list.
			 */
			if (depend->flags & HAMMER_RECF_DELETED_FE) {
				++nlinks;
			} else {
				TAILQ_REMOVE(&ip->target_list, depend,
					     target_entry);
				depend->target_ip = NULL;
			}
		} else if ((depend->flags & HAMMER_RECF_DELETED_FE) == 0) {
			/*
			 * Not part of our flush group
			 */
			KKASSERT((depend->flags & HAMMER_RECF_DELETED_BE) == 0);
			switch(depend->type) {
			case HAMMER_MEM_RECORD_ADD:
				--nlinks;
				break;
			case HAMMER_MEM_RECORD_DEL:
				++nlinks;
				break;
			default:
				break;
			}
		}
	}

	/*
	 * Set dirty if we had to modify the link count.
	 */
	if (ip->sync_ino_data.nlinks != nlinks) {
		KKASSERT((int64_t)nlinks >= 0);
		ip->sync_ino_data.nlinks = nlinks;
		ip->sync_flags |= HAMMER_INODE_DDIRTY;
	}

	/*
	 * If there is a trunction queued destroy any data past the (aligned)
	 * truncation point.  Userland will have dealt with the buffer
	 * containing the truncation point for us.
	 *
	 * We don't flush pending frontend data buffers until after we've
	 * dealt with the truncation.
	 */
	if (ip->sync_flags & HAMMER_INODE_TRUNCATED) {
		/*
		 * Interlock trunc_off.  The VOP front-end may continue to
		 * make adjustments to it while we are blocked.
		 */
		off_t trunc_off;
		off_t aligned_trunc_off;
		int blkmask;

		trunc_off = ip->sync_trunc_off;
		blkmask = hammer_blocksize(trunc_off) - 1;
		aligned_trunc_off = (trunc_off + blkmask) & ~(int64_t)blkmask;

		/*
		 * Delete any whole blocks on-media.  The front-end has
		 * already cleaned out any partial block and made it
		 * pending.  The front-end may have updated trunc_off
		 * while we were blocked so we only use sync_trunc_off.
		 *
		 * This operation can blow out the buffer cache, EWOULDBLOCK
		 * means we were unable to complete the deletion.  The
		 * deletion will update sync_trunc_off in that case.
		 */
		error = hammer_ip_delete_range(&cursor, ip,
						aligned_trunc_off,
						0x7FFFFFFFFFFFFFFFLL, 2);
		if (error == EWOULDBLOCK) {
			ip->flags |= HAMMER_INODE_WOULDBLOCK;
			error = 0;
			goto defer_buffer_flush;
		}

		if (error)
			Debugger("hammer_ip_delete_range errored");

		/*
		 * Clear the truncation flag on the backend after we have
		 * complete the deletions.  Backend data is now good again
		 * (including new records we are about to sync, below).
		 *
		 * Leave sync_trunc_off intact.  As we write additional
		 * records the backend will update sync_trunc_off.  This
		 * tells the backend whether it can skip the overwrite
		 * test.  This should work properly even when the backend
		 * writes full blocks where the truncation point straddles
		 * the block because the comparison is against the base
		 * offset of the record.
		 */
		ip->sync_flags &= ~HAMMER_INODE_TRUNCATED;
		/* ip->sync_trunc_off = 0x7FFFFFFFFFFFFFFFLL; */
	} else {
		error = 0;
	}

	/*
	 * Now sync related records.  These will typically be directory
	 * entries, records tracking direct-writes, or delete-on-disk records.
	 */
	if (error == 0) {
		tmp_error = RB_SCAN(hammer_rec_rb_tree, &ip->rec_tree, NULL,
				    hammer_sync_record_callback, &cursor);
		if (tmp_error < 0)
			tmp_error = -error;
		if (tmp_error)
			error = tmp_error;
	}
	hammer_cache_node(&ip->cache[1], cursor.node);

	/*
	 * Re-seek for inode update, assuming our cache hasn't been ripped
	 * out from under us.
	 */
	if (error == 0) {
		tmp_node = hammer_ref_node_safe(ip->hmp, &ip->cache[0], &error);
		if (tmp_node) {
			hammer_cursor_downgrade(&cursor);
			hammer_lock_sh(&tmp_node->lock);
			if ((tmp_node->flags & HAMMER_NODE_DELETED) == 0)
				hammer_cursor_seek(&cursor, tmp_node, 0);
			hammer_unlock(&tmp_node->lock);
			hammer_rel_node(tmp_node);
		}
		error = 0;
	}

	/*
	 * If we are deleting the inode the frontend had better not have
	 * any active references on elements making up the inode.
	 *
	 * The call to hammer_ip_delete_clean() cleans up auxillary records
	 * but not DB or DATA records.  Those must have already been deleted
	 * by the normal truncation mechanic.
	 */
	if (error == 0 && ip->sync_ino_data.nlinks == 0 &&
		RB_EMPTY(&ip->rec_tree)  &&
	    (ip->sync_flags & HAMMER_INODE_DELETING) &&
	    (ip->flags & HAMMER_INODE_DELETED) == 0) {
		int count1 = 0;

		error = hammer_ip_delete_clean(&cursor, ip, &count1);
		if (error == 0) {
			ip->flags |= HAMMER_INODE_DELETED;
			ip->sync_flags &= ~HAMMER_INODE_DELETING;
			ip->sync_flags &= ~HAMMER_INODE_TRUNCATED;
			KKASSERT(RB_EMPTY(&ip->rec_tree));

			/*
			 * Set delete_tid in both the frontend and backend
			 * copy of the inode record.  The DELETED flag handles
			 * this, do not set RDIRTY.
			 */
			ip->ino_leaf.base.delete_tid = trans.tid;
			ip->sync_ino_leaf.base.delete_tid = trans.tid;
			ip->ino_leaf.delete_ts = trans.time32;
			ip->sync_ino_leaf.delete_ts = trans.time32;


			/*
			 * Adjust the inode count in the volume header
			 */
			hammer_sync_lock_sh(&trans);
			if (ip->flags & HAMMER_INODE_ONDISK) {
				hammer_modify_volume_field(&trans,
							   trans.rootvol,
							   vol0_stat_inodes);
				--ip->hmp->rootvol->ondisk->vol0_stat_inodes;
				hammer_modify_volume_done(trans.rootvol);
			}
			hammer_sync_unlock(&trans);
		} else {
			Debugger("hammer_ip_delete_clean errored");
		}
	}

	ip->sync_flags &= ~HAMMER_INODE_BUFS;

	if (error)
		Debugger("RB_SCAN errored");

defer_buffer_flush:
	/*
	 * Now update the inode's on-disk inode-data and/or on-disk record.
	 * DELETED and ONDISK are managed only in ip->flags.
	 *
	 * In the case of a defered buffer flush we still update the on-disk
	 * inode to satisfy visibility requirements if there happen to be
	 * directory dependancies.
	 */
	switch(ip->flags & (HAMMER_INODE_DELETED | HAMMER_INODE_ONDISK)) {
	case HAMMER_INODE_DELETED|HAMMER_INODE_ONDISK:
		/*
		 * If deleted and on-disk, don't set any additional flags.
		 * the delete flag takes care of things.
		 *
		 * Clear flags which may have been set by the frontend.
		 */
		ip->sync_flags &= ~(HAMMER_INODE_DDIRTY | HAMMER_INODE_XDIRTY |
				    HAMMER_INODE_ATIME | HAMMER_INODE_MTIME |
				    HAMMER_INODE_DELETING);
		break;
	case HAMMER_INODE_DELETED:
		/*
		 * Take care of the case where a deleted inode was never
		 * flushed to the disk in the first place.
		 *
		 * Clear flags which may have been set by the frontend.
		 */
		ip->sync_flags &= ~(HAMMER_INODE_DDIRTY | HAMMER_INODE_XDIRTY |
				    HAMMER_INODE_ATIME | HAMMER_INODE_MTIME |
				    HAMMER_INODE_DELETING);
		while (RB_ROOT(&ip->rec_tree)) {
			hammer_record_t record = RB_ROOT(&ip->rec_tree);
			hammer_ref(&record->lock);
			KKASSERT(record->lock.refs == 1);
			record->flags |= HAMMER_RECF_DELETED_FE;
			record->flags |= HAMMER_RECF_DELETED_BE;
			hammer_rel_mem_record(record);
		}
		break;
	case HAMMER_INODE_ONDISK:
		/*
		 * If already on-disk, do not set any additional flags.
		 */
		break;
	default:
		/*
		 * If not on-disk and not deleted, set DDIRTY to force
		 * an initial record to be written.
		 *
		 * Also set the create_tid in both the frontend and backend
		 * copy of the inode record.
		 */
		ip->ino_leaf.base.create_tid = trans.tid;
		ip->ino_leaf.create_ts = trans.time32;
		ip->sync_ino_leaf.base.create_tid = trans.tid;
		ip->sync_ino_leaf.create_ts = trans.time32;
		ip->sync_flags |= HAMMER_INODE_DDIRTY;
		break;
	}

	/*
	 * If RDIRTY or DDIRTY is set, write out a new record.  If the inode
	 * is already on-disk the old record is marked as deleted.
	 *
	 * If DELETED is set hammer_update_inode() will delete the existing
	 * record without writing out a new one.
	 *
	 * If *ONLY* the ITIMES flag is set we can update the record in-place.
	 */
	if (ip->flags & HAMMER_INODE_DELETED) {
		error = hammer_update_inode(&cursor, ip);
	} else 
	if ((ip->sync_flags & HAMMER_INODE_DDIRTY) == 0 &&
	    (ip->sync_flags & (HAMMER_INODE_ATIME | HAMMER_INODE_MTIME))) {
		error = hammer_update_itimes(&cursor, ip);
	} else
	if (ip->sync_flags & (HAMMER_INODE_DDIRTY | HAMMER_INODE_ATIME | HAMMER_INODE_MTIME)) {
		error = hammer_update_inode(&cursor, ip);
	}
	if (error)
		Debugger("hammer_update_itimes/inode errored");
done:
	/*
	 * Save the TID we used to sync the inode with to make sure we
	 * do not improperly reuse it.
	 */
	hammer_done_cursor(&cursor);
	hammer_done_transaction(&trans);
	return(error);
}

/*
 * This routine is called when the OS is no longer actively referencing
 * the inode (but might still be keeping it cached), or when releasing
 * the last reference to an inode.
 *
 * At this point if the inode's nlinks count is zero we want to destroy
 * it, which may mean destroying it on-media too.
 */
void
hammer_inode_unloadable_check(hammer_inode_t ip, int getvp)
{
	struct vnode *vp;

	/*
	 * Set the DELETING flag when the link count drops to 0 and the
	 * OS no longer has any opens on the inode.
	 *
	 * The backend will clear DELETING (a mod flag) and set DELETED
	 * (a state flag) when it is actually able to perform the
	 * operation.
	 */
	if (ip->ino_data.nlinks == 0 &&
	    (ip->flags & (HAMMER_INODE_DELETING|HAMMER_INODE_DELETED)) == 0) {
		ip->flags |= HAMMER_INODE_DELETING;
		ip->flags |= HAMMER_INODE_TRUNCATED;
		ip->trunc_off = 0;
		vp = NULL;
		if (getvp) {
			if (hammer_get_vnode(ip, &vp) != 0)
				return;
		}

		/*
		 * Final cleanup
		 */
		if (ip->vp) {
			vtruncbuf(ip->vp, 0, HAMMER_BUFSIZE);
			vnode_pager_setsize(ip->vp, 0);
		}
		if (getvp) {
			vput(vp);
		}
	}
}

/*
 * Re-test an inode when a dependancy had gone away to see if we
 * can chain flush it.
 */
void
hammer_test_inode(hammer_inode_t ip)
{
	if (ip->flags & HAMMER_INODE_REFLUSH) {
		ip->flags &= ~HAMMER_INODE_REFLUSH;
		hammer_ref(&ip->lock);
		if (ip->flags & HAMMER_INODE_RESIGNAL) {
			ip->flags &= ~HAMMER_INODE_RESIGNAL;
			hammer_flush_inode(ip, HAMMER_FLUSH_SIGNAL);
		} else {
			hammer_flush_inode(ip, 0);
		}
		hammer_rel_inode(ip, 0);
	}
}

/*
 * Clear the RECLAIM flag on an inode.  This occurs when the inode is
 * reassociated with a vp or just before it gets freed.
 *
 * Wakeup one thread blocked waiting on reclaims to complete.  Note that
 * the inode the thread is waiting on behalf of is a different inode then
 * the inode we are called with.  This is to create a pipeline.
 */
static void
hammer_inode_wakereclaims(hammer_inode_t ip)
{
	struct hammer_reclaim *reclaim;
	hammer_mount_t hmp = ip->hmp;

	if ((ip->flags & HAMMER_INODE_RECLAIM) == 0)
		return;

	--hammer_count_reclaiming;
	--hmp->inode_reclaims;
	ip->flags &= ~HAMMER_INODE_RECLAIM;

	if ((reclaim = TAILQ_FIRST(&hmp->reclaim_list)) != NULL) {
		TAILQ_REMOVE(&hmp->reclaim_list, reclaim, entry);
		reclaim->okydoky = 1;
		wakeup(reclaim);
	}
}

/*
 * Setup our reclaim pipeline.  We only let so many detached (and dirty)
 * inodes build up before we start blocking.
 *
 * When we block we don't care *which* inode has finished reclaiming,
 * as lone as one does.  This is somewhat heuristical... we also put a
 * cap on how long we are willing to wait.
 */
void
hammer_inode_waitreclaims(hammer_mount_t hmp)
{
	struct hammer_reclaim reclaim;
	int delay;

	if (hmp->inode_reclaims > HAMMER_RECLAIM_WAIT) {
		reclaim.okydoky = 0;
		TAILQ_INSERT_TAIL(&hmp->reclaim_list,
				  &reclaim, entry);
	} else {
		reclaim.okydoky = 1;
	}

	if (reclaim.okydoky == 0) {
		delay = (hmp->inode_reclaims - HAMMER_RECLAIM_WAIT) * hz /
			HAMMER_RECLAIM_WAIT;
		if (delay >= 0)
			tsleep(&reclaim, 0, "hmrrcm", delay + 1);
		if (reclaim.okydoky == 0)
			TAILQ_REMOVE(&hmp->reclaim_list, &reclaim, entry);
	}
}

