#!/usr/bin/env python3
"""Run directory teardown functions against deterministic child-removal races."""
import unittest
from test_regress import COMMON, function, run_c

class ParentTeardown(unittest.TestCase):
    def check_parent(self, kind, member, entry_type):
        source = COMMON + r"""
#include <stdlib.h>
struct token { unsigned held; };
struct vmmfs_node { struct token token; };
struct vnode { unsigned refs; };
struct ENTRY { struct vnode *vnode; };
struct registry { struct ENTRY *MEMBER; };
struct ROOT_TYPE { struct vmmfs_node node; struct registry *registry; };
static struct ROOT_TYPE root;
static struct registry registry;
static struct vnode vnode;
static unsigned mode, release_count;
#define M_VMMFS 0
#define RB_ROOT(p) (*(p))
#define RB_REMOVE(type, p, entry) do { assert(*(p) == (entry)); *(p) = NULL; } while (0)
#define RB_INSERT(type, p, entry) do { assert(*(p) == NULL); *(p) = (entry); } while (0)
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
#define vref(v) do { assert((v)->refs); ++(v)->refs; } while (0)
static void vrele(struct vnode *v) { assert(v->refs); --v->refs; }
static void kfree(void *p, int tag) { (void)tag; assert(p); free(p); }
static void RELEASE(struct ROOT_TYPE *r, struct ENTRY *e) {
    assert(r->node.token.held); assert(e->vnode == &vnode); ++release_count;
}
static int vmmfs_vnode_deactivate(struct vnode *v) {
    struct ENTRY *entry = registry.MEMBER;
    assert(v == &vnode && v->refs == 2);
    assert(entry != NULL); /* Veto cannot resurrect an entry already removed. */
    if (mode == 2) {
        registry.MEMBER = NULL;
        vrele(entry->vnode); free(entry);
        return EBUSY;
    }
    return mode == 1 ? EBUSY : 0;
}
static int
FUNCTION
int main(void) {
    root.registry = &registry;
    for (mode = 0; mode != 3; ++mode) {
        registry.MEMBER = malloc(sizeof(*registry.MEMBER));
        registry.MEMBER->vnode = &vnode;
        vnode.refs = 1; release_count = 0;
        int error = DEACTIVATE(&root.node);
        assert(root.node.token.held == 0);
        if (mode == 1) {
            assert(error == EBUSY && registry.MEMBER != NULL);
            assert(vnode.refs == 1 && release_count == 0);
            free(registry.MEMBER); registry.MEMBER = NULL;
            vrele(&vnode);
        } else {
            assert(error == (mode == 2 ? EBUSY : 0));
            assert(registry.MEMBER == NULL && vnode.refs == 0);
            assert(release_count == (mode == 0 ? 1 : 0));
        }
    }
    return 0;
}
"""
        root = "vmmfs_" + kind
        source = source.replace("FUNCTION", function(root + ".c", root + "_deactivate"))
        for token, value in (("ENTRY", entry_type), ("MEMBER", member),
                             ("ROOT_TYPE", root), ("RELEASE", root + "_release_entry"),
                             ("DEACTIVATE", root + "_deactivate")):
            source = source.replace(token, value)
        run_c(source)

    def test_pci_parent_preserves_registry_until_deactivation(self):
        self.check_parent("pciroot", "slots", "vmmfs_pciroot_slot")

    def test_serial_parent_preserves_registry_until_deactivation(self):
        self.check_parent("serialroot", "ports", "vmmfs_serialroot_port")

if __name__ == "__main__":
    unittest.main(verbosity=2)
