#!/usr/bin/env python3
"""Exercise the production collection VOPs with explicit vnode ownership."""
import unittest
from test_regress import COMMON, function, run_c


class Collection(unittest.TestCase):
    def test_vops_transfer_references_and_preserve_veto(self):
        names = ("vmmfs_node_vop_branch", "vmmfs_node_nresolve",
                 "vmmfs_node_nmkdir", "vmmfs_node_nrmdir")
        bodies = "\n".join("static int\n" + function("vmmfs_node_vops.c", name)
                           for name in names)
        run_c(COMMON + r"""
#include <sys/types.h>
#define kprintf printf
enum { VDIR = 1, VREG };
struct vnode;
struct mount { int unused; };
struct vmmfs_node { struct vnode *vnode;
    int (*get_item)(struct vmmfs_node *, const char *, size_t, struct vnode **);
    int (*create_item)(struct vmmfs_node *, struct mount *, const char *,
        size_t, struct vnode **);
    int (*remove_item)(struct vmmfs_node *, const char *, size_t);
 struct lock lock; bool dead;};
struct vnode {
    int v_type; void *v_data; struct mount *v_mount;
    unsigned refs, holds, locked;
};
struct namecache { const char *nc_name; size_t nc_nlen; struct vnode *vnode; };
struct nchandle { struct namecache *ncp; };
struct vattr { int va_type; };
struct vop_nresolve_args { struct vnode *a_dvp; struct nchandle *a_nch; };
struct vop_nmkdir_args {
    struct vnode *a_dvp; struct nchandle *a_nch; struct vattr *a_vap;
    struct vnode **a_vpp;
};
struct vop_nrmdir_args {
    struct vnode *a_dvp; struct nchandle *a_nch; void *a_cred;
};
static struct vnode child, *registry;
static struct vmmfs_node child_node;
static int lookup_error, create_error, get_error, veto;
static unsigned deactivations, removals, unlinks;
static bool dead;
static void vhold(struct vnode *v) { assert(v->refs || v->holds); ++v->holds; }
static void vdrop(struct vnode *v) { assert(v->holds); --v->holds; }
static void vrele(struct vnode *v) { assert(v->refs); --v->refs; }
static int vget(struct vnode *v, int flags) {
    assert(flags == LK_EXCLUSIVE || flags == LK_SHARED);
    assert(v->refs || v->holds);
    if (get_error) return get_error;
    assert(!v->locked); ++v->refs; v->locked = 1; return 0;
}
static int vn_lock(struct vnode *v, int flags) {
    assert(flags == LK_EXCLUSIVE && v->refs);
    if (get_error) return get_error;
    assert(!v->locked); v->locked = 1; return 0;
}
static void vn_unlock(struct vnode *v) { assert(v->locked); v->locked = 0; }
static void cache_setunresolved(struct nchandle *nch) {
    if (nch->ncp->vnode) vdrop(nch->ncp->vnode);
    nch->ncp->vnode = NULL;
}
static void cache_setvp(struct nchandle *nch, struct vnode *v) {
    assert(nch->ncp->vnode == NULL);
    nch->ncp->vnode = v;
    if (v) vhold(v);
}
static int cache_vget(struct nchandle *nch, void *cred, int flags,
    struct vnode **out) {
    (void)cred; *out = NULL;
    if (!nch->ncp->vnode) return ENOENT;
    int error = vget(nch->ncp->vnode, flags);
    if (!error) *out = nch->ncp->vnode;
    return error;
}
static void cache_unlink(struct nchandle *nch) {
    /* Unlink marks the name destroyed; its vnode association can remain. */
    assert(nch->ncp->vnode == &child && dead && registry == NULL);
    ++unlinks;
}
static int vmmfs_vnode_deactivate(struct vnode *v) {
    assert(v == &child && v->refs && !v->locked);
    ++deactivations;
    if (veto) return veto;
    dead = true; return 0;
}
static void vref(struct vnode *v) { assert(v->refs); ++v->refs; }
static void vrele(struct vnode *);
static bool vmmfs_node_deactivate(struct vmmfs_node *n) {
    if (!n) return true;
    struct vnode *v = n->vnode;
    if (vmmfs_vnode_deactivate(v) != 0) return false;
    vrele(v); return true;
}
static int get_item(struct vmmfs_node *node, const char *name, size_t length,
    struct vnode **out) {
    (void)node; assert(length == 5 && !memcmp(name, "child", 5));
    *out = NULL;
    if (lookup_error) return lookup_error;
    assert(registry == &child); vref(registry); *out = registry; return 0;
}
static int create_item(struct vmmfs_node *node, struct mount *mount,
    const char *name, size_t length, struct vnode **out) {
    (void)node; (void)mount; assert(length == 5 && !memcmp(name, "child", 5));
    *out = NULL;
    if (create_error) return create_error;
    assert(registry == NULL && child.refs == 0);
    child_node.vnode = &child; child.v_data = &child_node;
    child.refs = 2; /* Registry and the independent create_item result. */
    registry = &child; *out = &child; return 0;
}
static int remove_item(struct vmmfs_node *node, const char *name,
    size_t length) {
    (void)node; assert(length == 5 && !memcmp(name, "child", 5));
    assert(dead && registry == &child && child.refs >= 1);
    registry = NULL; ++removals; vrele(&child); return 0;
}
""" + bodies + r"""
int main(void) {
    struct vmmfs_node node = { .get_item=get_item, .create_item=create_item, .remove_item=remove_item };
    struct mount mount = { 0 };
    struct vnode parent = { .v_type=VDIR, .v_data=&node, .v_mount=&mount };
    struct namecache name = { .nc_name="child", .nc_nlen=5 };
    struct nchandle handle = { &name };
    struct vattr attr = { VDIR };
    struct vnode *out = NULL;
    struct vop_nmkdir_args mk = { &parent, &handle, &attr, &out };
    struct vop_nresolve_args resolve = { &parent, &handle };
    struct vop_nrmdir_args rm = { &parent, &handle, NULL };
    child.v_type = VDIR;
    create_error = ENOMEM;
    assert(vmmfs_node_nmkdir(&mk) == ENOMEM && out == NULL);
    assert(!registry && !child.refs && !deactivations);
    create_error = 0; get_error = ENOENT;
    assert(vmmfs_node_nmkdir(&mk) == ENOENT);
    assert(!registry && !child.refs && !child.holds);
    assert(deactivations == 1 && removals == 1 && !unlinks);
    dead = false; veto = EBUSY;
    assert(vmmfs_node_nmkdir(&mk) == ENOENT);
    assert(registry == &child && child.refs == 1 && !dead);
    assert(deactivations == 2 && removals == 1);
    /* Lookup transfers a reference without acquiring a vnode lock. */
    assert(vmmfs_node_nresolve(&resolve) == 0 && name.vnode == &child);
    get_error = 0;
    assert(child.refs == 1 && child.holds == 1 && !child.locked);
    assert(vmmfs_node_nrmdir(&rm) == EBUSY);
    assert(registry == &child && child.refs == 1 && child.holds == 1);
    assert(removals == 1 && !unlinks && !dead);
    veto = 0;
    assert(vmmfs_node_nrmdir(&rm) == 0);
    assert(!registry && !child.refs && child.holds == 1 && !child.locked);
    assert(removals == 2 && unlinks == 1 && dead);
    cache_setunresolved(&handle); assert(!child.holds);
    dead = false;
    assert(vmmfs_node_nmkdir(&mk) == 0 && out == &child);
    assert(child.refs == 2 && child.holds == 1 && child.locked);
    vn_unlock(out); vrele(out); /* kern_mkdir's vput of the returned vnode. */
    assert(child.refs == 1);
    get_error = EIO;
    assert(vmmfs_node_nrmdir(&rm) == EIO && registry == &child);
    get_error = 0; child.v_type = VREG;
    unsigned calls = deactivations;
    assert(vmmfs_node_nrmdir(&rm) == ENOTDIR && deactivations == calls);
    assert(child.refs == 1 && !child.locked);
    child.v_type = VDIR;
    assert(vmmfs_node_nrmdir(&rm) == 0);
    cache_setunresolved(&handle);
    assert(!child.refs && !child.holds && !child.locked);
    lookup_error = ENOENT;
    assert(vmmfs_node_nresolve(&resolve) == ENOENT && name.vnode == NULL);
    return 0;
}
""")

    def test_readdir_copies_metadata_before_releasing_reference(self):
        run_c(COMMON + r"""
#include <sys/types.h>
enum { VDIR = 1, DT_DIR = 4 };
struct vnode;
struct vmmfs_node_item { struct vnode *vnode; ino_t inode; char name[16]; };
struct vmmfs_node { struct vnode *vnode;
    struct vmmfs_node *parent; ino_t inode;
    int (*read_item)(struct vmmfs_node *, uint64_t, struct vmmfs_node_item *);
 struct lock lock; bool dead;};
struct vnode { int v_type; void *v_data; unsigned refs; };
struct uio { off_t uio_offset; };
struct vop_readdir_args {
    struct vnode *a_vp; struct uio *a_uio;
    int *a_ncookies, *a_eofflag; void **a_cookies;
};
static struct vnode child;
static unsigned writes;
static void vrele(struct vnode *v) {
    assert(v == &child && v->refs == 1); --v->refs; v->v_data = NULL;
}
static int read_item(struct vmmfs_node *node, uint64_t index,
    struct vmmfs_node_item *item) {
    (void)node;
    if (index) return ENOENT;
    assert(child.refs == 0); child.refs = 1;
    item->vnode = &child; item->inode = 42; strcpy(item->name, "child");
    return 0;
}
static int vop_write_dirent(int *error, struct uio *uio, ino_t inode,
    unsigned type, uint16_t length, const char *name) {
    (void)uio; assert(type == DT_DIR && !child.refs);
    assert(inode == 42 && length == 5 && !strcmp(name, "child"));
    assert(child.v_data == NULL); ++writes; *error = 0; return 0;
}
static int
""" + function("vmmfs_node_vops.c", "vmmfs_node_vop_branch") + "\nstatic int\n" + function(
            "vmmfs_node_vops.c", "vmmfs_node_readdir") + r"""
int main(void) {
    struct vmmfs_node node = { .inode=1, .read_item=read_item };
    struct vnode parent = { .v_type=VDIR, .v_data=&node };
    struct uio uio = { 2 };
    int eof = 0;
    struct vop_readdir_args ap = { .a_vp=&parent, .a_uio=&uio, .a_eofflag=&eof };
    assert(vmmfs_node_readdir(&ap) == 0);
    assert(writes == 1 && eof == 1 && uio.uio_offset == 3 && !child.refs);
    return 0;
}
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
