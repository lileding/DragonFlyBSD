#!/usr/bin/env python3
"""Verify create_item retains its vnode through the VOP handoff."""
import unittest
from test_regress import COMMON, function, run_c

class CreateHandoff(unittest.TestCase):
    def test_producers_acquire_reference_before_unlock(self):
        for filename, name, insertion in (
            ("vmmfs_root.c", "vmmfs_root_create_item", "RB_INSERT("),
            ("vmmfs_pciroot.c", "vmmfs_pciroot_create_item", "entry->slot->entry = entry;"),
        ):
            body = function(filename, name)
            start = body.index(insertion)
            unlock = body.index("lwkt_reltoken(", start)
            self.assertIn("vref(vnode);", body[start:unlock], name)

    def test_nmkdir_balances_handoff_on_all_paths(self):
        source = COMMON + r"""
#define kprintf printf
struct mount { int unused; };
struct vnode { unsigned refs; struct mount *v_mount; };
struct namecache { const char *nc_name; size_t nc_nlen; };
struct nchandle { struct namecache *ncp; };
struct vattr { int va_type; };
struct vmmfs_node {
    int (*create_item)(struct vmmfs_node *, struct mount *,
        const char *, size_t, struct vnode **);
    int (*remove_item)(struct vmmfs_node *, const char *, size_t);
 struct lock lock; bool dead;};
struct vop_nmkdir_args {
    struct vattr *a_vap; struct vnode *a_dvp;
    struct nchandle *a_nch; struct vnode **a_vpp;
};
#define VDIR 1
static unsigned mode, removed;
static struct vmmfs_node parent;
static struct vnode child;
static int vmmfs_node_vop_branch(struct vnode *v, struct vmmfs_node **n) {
    (void)v; *n = &parent; return 0;
}
static void vrele(struct vnode *v) { assert(v->refs); --v->refs; }
static int create(struct vmmfs_node *n, struct mount *m,
    const char *name, size_t len, struct vnode **v) {
    (void)n; (void)m; (void)name; (void)len;
    child.refs = 2; /* registry + create_item caller */
    *v = &child; return 0;
}
static int remove_item(struct vmmfs_node *n, const char *name, size_t len) {
    (void)n; (void)name; (void)len;
    ++removed; vrele(&child); return 0;
}
static int vget(struct vnode *v, int flags) {
    (void)flags;
    if (mode == 2) {
        /* Parent completed deactivation while vget blocked. */
        vrele(v); ++removed;
        return ENOENT;
    }
    if (mode == 1) return ENOENT;
    ++v->refs; return 0;
}
static int vmmfs_vnode_deactivate(struct vnode *v) {
    assert(v->refs);
    return mode == 2 ? EBUSY : 0;
}
static void cache_setunresolved(struct nchandle *n) { (void)n; }
static void cache_setvp(struct nchandle *n, struct vnode *v) { (void)n; (void)v; }
int
""" + function("vmmfs_node_vops.c", "vmmfs_node_nmkdir") + r"""
int main(void) {
    struct vnode directory = { 1, NULL }, *result = NULL;
    struct vattr attr = { VDIR };
    struct namecache name = { "child", 5 };
    struct nchandle handle = { &name };
    struct vop_nmkdir_args args = { &attr, &directory, &handle, &result };
    parent.create_item = create; parent.remove_item = remove_item;
    for (mode = 0; mode < 3; ++mode) {
        removed = 0;
        int error = vmmfs_node_nmkdir(&args);
        if (mode == 0) {
            assert(error == 0 && result == &child && child.refs == 2);
            vrele(&child); /* VOP result */
            vrele(&child); /* registry */
        } else {
            assert(error == ENOENT && child.refs == 0 && removed == 1);
        }
    }
    return 0;
}
"""
        run_c(source)

if __name__ == "__main__":
    unittest.main(verbosity=2)
