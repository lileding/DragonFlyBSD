#!/usr/bin/env python3
"""Slot owns its powered resources; descriptor deactivation owns only auth."""
import unittest
from test_regress import COMMON, SOURCE, function, run_c


class SlotOwnership(unittest.TestCase):
    def test_resources_are_a_direct_slot_child(self):
        slot = (SOURCE / "vmmfs_pcislot.h").read_text()
        descriptor = (SOURCE / "vmmfs_pcislot_descriptor.h").read_text()
        self.assertIn("struct vmmfs_pcislot_resources *resources;", slot)
        self.assertNotIn("vmmfs_pcislot_resources", descriptor)
        self.assertNotIn("vmmfs_pcislot_resources",
                         (SOURCE / "vmmfs_pcislot_descriptor.c").read_text())
        for name in ("vmmfs_pcislot.c", "vmmfs_pciroot.c"):
            self.assertNotIn("descriptor.resources", (SOURCE / name).read_text())
        create = function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resources_create")
        self.assertIn("resources->node.parent = &slot->node;", create)
        self.assertIn("vmmfs_node_hold(&slot->node);", create)
        self.assertIn("slot->resources == NULL",
                      function("vmmfs_pcislot.c", "vmmfs_pcislot_drop"))

    def test_slot_detaches_resources_before_revoke(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_node { struct token token; void *parent;  struct lock lock; bool dead;};
struct vmmfs_pcislot_auth { unsigned references; };
struct vmmfs_pcislot_descriptor {
    struct vmmfs_node node;
    bool updating, committed;
    struct vmmfs_pcislot_auth *auth;
};
struct vmmfs_pcislot_resources { unsigned references; bool dead; };
struct vmmfs_pcislot {
    struct vmmfs_node node;
    struct vmmfs_pcislot_descriptor descriptor;
    struct vmmfs_pcislot_resources *resources;
    struct { unsigned state; } type0;
    bool config_powered;
    int config;
};
static struct vmmfs_pcislot slot;
static struct vmmfs_pcislot_resources resources;
static unsigned revoked, notifications, auth_revoked;
static void lwkt_gettoken(struct token *t) { assert(!t->held); ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
#define kprintf printf
int tsleep(void *p, int flags, const char *name, int timeout) {
    (void)p; (void)flags; (void)name; (void)timeout;
    assert(0); return 0;
}
#define vmmfs_pcislot_pciroot(s) (s)
#define vmmfs_pcislot_descriptor_slot(d) ((struct vmmfs_pcislot *)(d)->node.parent)
#define bzero(p,n) memset((p),0,(n))
static void vmmfs_pciroot_invalidate_slot(struct vmmfs_pcislot *root,
    struct vmmfs_pcislot *s) {
    assert(root == &slot && s == &slot && !s->node.token.held);
    ++notifications;
}
static void vmmfs_pcislot_config_power_off(int *c) {
    assert(c == &slot.config && !slot.node.token.held);
    slot.config_powered = false;
}
static void vmmfs_pcislot_resources_deactivate(struct vmmfs_pcislot_resources *r) {
    if (r == NULL) return;
    assert(r == &resources && slot.resources == NULL && !slot.node.token.held);
    assert(!slot.config_powered && slot.type0.state == 0);
    r->dead = true; assert(r->references == 2); --r->references; ++revoked;
}
static void vmmfs_pcislot_auth_revoke(struct vmmfs_pcislot_auth *auth) {
    assert(!slot.node.token.held && slot.descriptor.node.token.held == 1);
    if (auth != NULL) { assert(auth->references == 1); --auth->references; ++auth_revoked; }
}
void
""" + function("vmmfs_pcislot.c", "vmmfs_pcislot_power_off") + "\nstatic int\n" +
              function("vmmfs_pcislot_descriptor.c", "vmmfs_pcislot_descriptor_deactivate") +
              r"""
int main(void) {
    struct vmmfs_pcislot_auth auth = { 1 };
    slot.descriptor.node.parent = &slot;
    slot.descriptor.auth = &auth; slot.descriptor.committed = true;
    slot.descriptor.node.token.held = 1;
    /* Descriptor owns auth, not its sibling powered collection. */
    slot.resources = &resources; resources.references = 2;
    assert(vmmfs_pcislot_descriptor_deactivate(&slot.descriptor.node) == 0);
    assert(slot.resources == &resources && resources.references == 2);
    assert(!slot.descriptor.committed && slot.descriptor.auth == NULL);
    assert(auth_revoked == 1 && revoked == 0);
    slot.config_powered = true; slot.type0.state = 1;
    vmmfs_pcislot_power_off(&slot);
    assert(slot.resources == NULL && resources.dead && resources.references == 1);
    assert(revoked == 1 && notifications == 2);
    vmmfs_pcislot_power_off(&slot);
    assert(resources.references == 1 && revoked == 1 && notifications == 4);
    /* Old fd/pager reference is independent of the slot's current generation. */
    --resources.references; assert(resources.references == 0);
    vmmfs_pcislot_power_off(NULL);
}
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
