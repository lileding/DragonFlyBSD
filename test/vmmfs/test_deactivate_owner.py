"""Consuming close contract, tested against the production C function."""
import unittest
from test_regress import COMMON, SOURCE, function, run_c

class DeactivateOwner(unittest.TestCase):
    def test_reference_transfer(self):
        run_c(COMMON + r"""
struct vnode { unsigned refs; };
struct vmmfs_node {
    struct vnode *vnode;
    struct lock lock;
    bool dead;
    bool (*deactivate)(struct vmmfs_node *);
};
static unsigned calls, revokes, invalidates, releases;
static bool accept;
#define DTYPE_VNODE 1
#define CINV_CHILDREN 1
static struct { void *p_ucred; } proc0;
static int fdrevoke(struct vnode *v, int type, void *cred) {
    (void)type; (void)cred; assert(v->refs); ++revokes; return 0;
}
static void cache_inval_vp(struct vnode *v, int flags) {
    (void)flags; assert(v->refs); ++invalidates;
}
static void vrele(struct vnode *v) {
    assert(v->refs); --v->refs; ++releases;
}
static bool callback(struct vmmfs_node *n) {
    assert(n->dead); ++calls; return accept;
}
bool
""" + function("vmmfs_node.c", "vmmfs_node_deactivate") + r"""
int main(void) {
    struct vnode v = { 1 };
    struct vmmfs_node n = { .vnode = &v };
    assert(vmmfs_node_deactivate(NULL));
    assert(vmmfs_node_deactivate(&n));
    assert(v.refs == 1 && releases == 0 && calls == 0);
    n.deactivate = callback;
    assert(!vmmfs_node_deactivate(&n));
    assert(!n.dead && v.refs == 1 && calls == 1 && revokes == 0);
    n.dead = true;
    assert(!vmmfs_node_deactivate(&n));
    assert(v.refs == 1 && calls == 1);
    n.dead = false; accept = true;
    assert(vmmfs_node_deactivate(&n));
    assert(n.dead && v.refs == 0 && releases == 1 && calls == 2);
    assert(revokes == 1 && invalidates == 1);
}
""")

    def test_constructors_install_callback_after_vnode_creation(self):
        targets = {
            "boot": "init", "events": "init",
            "loader": "init", "machine": "create", "machine_id": "init",
            "memory": "init", "pciroot": "init", "pcislot": "create",
            "pcislot_config": "init", "pcislot_descriptor": "init",
            "pcislot_powered": "init", "root": "create",
            "serialport": "create", "serialroot": "init",
            "stopped": "init", "vcpu": "init",
        }
        for object_name, verb in targets.items():
            with self.subTest(object=object_name):
                name = "vmmfs_" + object_name
                body = function(name + ".c", name + "_" + verb)
                self.assertGreater(body.index("node.deactivate ="),
                                   body.index("vmmfs_vnode_create_"))
        for path in SOURCE.glob("vmmfs*.[ch]"):
            self.assertNotIn("vmmfs_vnode_deactivate", path.read_text())
            self.assertNotIn("vmmfs_vnode_discard", path.read_text())

if __name__ == '__main__':
    unittest.main()
