"""Weak vnode backlinks: construction, reclamation, and parent lookup races."""
import unittest
from test_regress import COMMON, SOURCE, function, run_c


class Backlink(unittest.TestCase):
    def test_creation_and_reclaim_do_not_add_a_vnode_reference(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vnode;
struct vmmfs_node { struct token token; struct vnode *vnode; unsigned refs;  struct lock lock; bool dead;};
struct mount { int unused; };
struct vop_ops { int unused; };
enum vtype { VDIR, VCHR, VBAD };
struct cdev { int si_umajor, si_uminor; };
struct vnode { struct vmmfs_node *v_data; struct vop_ops **v_ops;
    enum vtype v_type; int v_umajor, v_uminor; unsigned refs; bool locked; };
struct vop_reclaim_args { struct vnode *a_vp; };
static struct vnode allocated;
static struct vmmfs_node node;
static int allocation_error, association_error;
static unsigned node_puts;
#define VT_SYNTH 1
void lwkt_gettoken(struct token *t) { ++t->held; }
void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static int getnewvnode(int tag, struct mount *m, struct vnode **vp, int a, int b) {
    assert(tag == VT_SYNTH && m && !a && !b);
    if (allocation_error) return allocation_error;
    memset(&allocated, 0, sizeof(allocated));
    allocated.refs = 1; allocated.locked = true; *vp = &allocated; return 0;
}
static int getspecialvnode(int tag, struct mount *m, struct vop_ops **ops,
    struct vnode **vp, int a, int b) {
    assert(ops); return getnewvnode(tag, m, vp, a, b);
}
static int v_associate_rdev(struct vnode *v, struct cdev *d) {
    assert(v == &allocated && d); return association_error;
}
static void vx_downgrade(struct vnode *v) { assert(v->locked); }
static void vn_unlock(struct vnode *v) { assert(v->locked); v->locked = false; }
static void vrele(struct vnode *v) {
    assert(v->refs && !v->locked && !v->v_data && !node.vnode); --v->refs;
}
static void vmmfs_node_put(struct vmmfs_node *n) {
    assert(n == &node && n->refs && !n->token.held);
    assert(!n->vnode && !allocated.v_data);
    --n->refs; ++node_puts;
}
int
""" + function("vmmfs_node.c", "vmmfs_vnode_create_regular") + "\nint\n" +
            function("vmmfs_node.c", "vmmfs_vnode_create_cdev") + "\nint\n" +
            function("vmmfs_node.c", "vmmfs_node_reclaim") + r"""
int main(void) {
    struct mount mount = {0}; struct vop_ops *ops = NULL;
    struct cdev dev = {2, 3}; struct vnode *vp = NULL;
    struct vop_reclaim_args args = { &allocated };
    node.refs = 2; /* A child may retain the node after vnode reclaim. */
    allocation_error = ENFILE;
    assert(vmmfs_vnode_create_regular(&mount, &ops, VDIR, &node) == ENFILE);
    assert(!node.vnode && !vp);
    allocation_error = 0;
    assert(vmmfs_vnode_create_regular(&mount, &ops, VDIR, &node) == 0);
    vp = node.vnode;
    assert(node.vnode == vp && vp->v_data == &node && vp->refs == 1);
    assert(vmmfs_node_reclaim(&args) == 0);
    assert(node.refs == 1 && node_puts == 1 && !node.vnode && !allocated.v_data);
    assert(vmmfs_node_reclaim(&args) == 0 && node_puts == 1);
    association_error = EIO;
    assert(vmmfs_vnode_create_cdev(&mount, &ops, &dev, &node) == EIO);
    assert(!node.vnode && !allocated.v_data && !allocated.refs && node_puts == 1);
    association_error = 0;
    assert(vmmfs_vnode_create_cdev(&mount, &ops, &dev, &node) == 0);
    vp = node.vnode;
    assert(node.vnode == vp && vp->refs == 1 && vp->v_data == &node);
    assert(vmmfs_node_reclaim(&args) == 0);
    vrele(vp);
    assert(!node.vnode && !allocated.v_data && !allocated.refs);
    assert(node.refs == 0 && node_puts == 2);
}
""")

    def test_parent_lookup_returns_reference_under_admission(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vnode;
struct vmmfs_node { struct token token; bool dead; struct vmmfs_node *parent;
    struct vnode *vnode;  struct lock lock;};
struct vnode { struct vmmfs_node *v_data; unsigned holds, refs; bool locked; };
struct vop_nlookupdotdot_args { struct vnode *a_dvp; struct vnode **a_vpp; };
static struct vmmfs_node parent, child;
static struct vnode parent_vnode, child_vnode;
static unsigned mode, get_calls;
void lwkt_gettoken(struct token *t) {
    if (t == &parent.token && mode == 4) {
        /* Contended parent acquisition releases and reacquires the child token. */
        assert(child.token.held); child.dead = true;
    }
    ++t->held;
}
void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static void vref(struct vnode *v) {
    assert(v == &parent_vnode && parent.lock.held && !child.lock.held);
    assert(!parent.dead && v->refs == 1); ++v->refs;
}

int
""" + function("vmmfs_node.c", "vmmfs_node_get_vnode") + "\nint\n" + function("vmmfs_node.c", "vmmfs_node_nlookupdotdot") + r"""
int main(void) {
    struct vnode *result;
    struct vop_nlookupdotdot_args args = { &child_vnode, &result };
    for (mode = 0; mode != 5; ++mode) {
        memset(&child, 0, sizeof(child)); memset(&parent, 0, sizeof(parent));
        memset(&parent_vnode, 0, sizeof(parent_vnode));
        child.parent = &parent; child_vnode.v_data = &child;
        parent.vnode = mode == 3 ? NULL : &parent_vnode;
        parent_vnode.v_data = &parent; parent_vnode.refs = 1;
        child.dead = mode == 1; parent.dead = mode == 2;
        result = NULL; get_calls = 0;
        int error = vmmfs_node_nlookupdotdot(&args);
        assert(!child.token.held && !parent.token.held && !parent_vnode.holds);
        if (mode == 0 || mode == 1 || mode == 4) {
            assert(!error && result == &parent_vnode && result->refs == 2);
            assert(!result->locked && get_calls == 0);
        } else {
            assert(error == ENOENT && result == NULL && parent_vnode.refs == 1);
            assert(get_calls == (mode == 5));
        }
    }
}
""")

    def test_all_directory_users_bind_the_common_vop(self):
        for name in ("machine", "pciroot", "serialroot", "pcislot"):
            text = (SOURCE / ("vmmfs_" + name + ".c")).read_text()
            self.assertIn("->node_vops", text)
            self.assertNotIn("vmmfs_" + name + "_nlookupdotdot(", text)
        self.assertIn(".vop_nlookupdotdot = vmmfs_node_nlookupdotdot,",
                      (SOURCE / "vmmfs_node_vops.c").read_text())
