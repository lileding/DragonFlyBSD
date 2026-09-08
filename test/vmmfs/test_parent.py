#!/usr/bin/env python3
"""Run directory teardown functions against deterministic child-removal races."""
import unittest
from test_regress import COMMON, function, run_c

class ParentTeardown(unittest.TestCase):
    def check_parent(self, kind, member, entry_type):
        source = COMMON + r"""
#include <stdlib.h>
struct token { unsigned held; };
struct vmmfs_node { struct vnode *vnode; struct token token;  struct lock lock; bool dead;};
struct vnode { unsigned refs; void *v_data; };
struct CHILD_TYPE { struct vmmfs_node node; };
struct ENTRY { struct CHILD_TYPE *CHILD; };
struct registry { struct ENTRY *MEMBER; };
struct ROOT_TYPE { struct vmmfs_node node; struct token token; struct registry *registry; };
static struct ROOT_TYPE root;
static struct registry registry;
static struct vnode vnode;
static struct CHILD_TYPE child;
static unsigned mode, release_count;
#define M_VMMFS 0
#define RB_ROOT(p) (*(p))
#define RB_REMOVE(type, p, entry) do { assert(*(p) == (entry)); *(p) = NULL; } while (0)
#define RB_INSERT(type, p, entry) do { assert(*(p) == NULL); *(p) = (entry); } while (0)
#define kprintf printf
#define vref(v) do { assert((v)->refs); ++(v)->refs; } while (0)
static void vrele(struct vnode *v) { assert(v->refs); --v->refs; }
static void kfree(void *p, int tag) { (void)tag; assert(p); free(p); }
static void RELEASE(struct ROOT_TYPE *r, struct ENTRY *e) {
    assert(r->node.dead); assert(e->CHILD->node.vnode == &vnode); ++release_count;
}
static int vmmfs_vnode_deactivate(struct vnode *v) {
    assert(root.node.dead && root.token.held == 0);
    assert(v == &vnode && v->refs == 1 && registry.MEMBER == NULL);
    /* A previously admitted remover may already be closing this child. */
    return mode == 1 ? EBUSY : 0;
}
static void vrele(struct vnode *);
static bool vmmfs_node_deactivate(struct vmmfs_node *n) {
    if (!n) return true;
    struct vnode *v = n->vnode;
    if (vmmfs_vnode_deactivate(v) != 0) return false;
    vrele(v); return true;
}
static bool
FUNCTION
int main(void) {
    root.registry = &registry;
    root.node.dead = true;
    for (mode = 0; mode != 2; ++mode) {
        registry.MEMBER = malloc(sizeof(*registry.MEMBER));
        child.node.vnode = &vnode; registry.MEMBER->CHILD = &child;
        vnode.refs = 1; vnode.v_data = &child; release_count = 0;
        bool closed = DEACTIVATE(&root.node);
        assert(root.token.held == 0);
        assert(closed && registry.MEMBER == NULL && vnode.refs == 0);
        assert(release_count == 1);
    }
    return 0;
}
"""
        root = "vmmfs_" + kind
        source = source.replace("FUNCTION", function(root + ".c", root + "_deactivate"))
        for token, value in (("CHILD_TYPE", "vmmfs_pcislot" if kind == "pciroot" else "vmmfs_serialport"),
                             ("CHILD", "slot" if kind == "pciroot" else "port"), ("ENTRY", entry_type), ("MEMBER", member),
                             ("ROOT_TYPE", root), ("RELEASE", root + "_release_entry"),
                             ("DEACTIVATE", root + "_deactivate")):
            source = source.replace(token, value)
        run_c(source)

    def test_pci_parent_detaches_before_blocking_cleanup(self):
        self.check_parent("pciroot", "slots", "vmmfs_pciroot_slot")

    def test_serial_parent_detaches_before_blocking_cleanup(self):
        self.check_parent("serialroot", "ports", "vmmfs_serialroot_port")


class MachineTeardown(unittest.TestCase):
    def test_machine_veto_is_final_for_all_children(self):
        run_c(COMMON + r"""
struct token { unsigned held, acquired; };
struct vmmfs_node { struct token token; bool dead; struct vnode *vnode;  struct lock lock;};
struct vnode { unsigned refs, index; };
struct vmmfs_machine {
    struct vmmfs_node node; struct token token;
    void *machine;
    bool runtime_releasing, runtime_released;

    struct { struct vmmfs_node node; struct token token; unsigned active_count; void *threads; } vcpu;
    struct { struct vmmfs_node node; } id_node, memory, loader, boot, stopped, pciroot, serialroot, events;
};
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
static struct vmmfs_machine machine;
static struct vnode children[9], parent;
static unsigned called, dropped, veto_index;
static int child_error;
#define vref(v) do { assert((v)->refs); ++(v)->refs; } while (0)
static int vmmfs_vnode_deactivate(struct vnode *v) {
    assert(machine.node.dead);
    assert(machine.token.acquired == 0);
    assert(v != NULL && v->refs != 0);
    assert(machine.token.held == 1);
    assert(v->index == called++);
    return v->index == veto_index ? child_error : 0;
}
static void vrele(struct vnode *);
static bool vmmfs_node_deactivate(struct vmmfs_node *n) {
    if (!n) return true;
    struct vnode *v = n->vnode;
    if (vmmfs_vnode_deactivate(v) != 0) return false;
    vrele(v); return true;
}
static void vrele(struct vnode *v) {
    assert(v != NULL && v->refs != 0);
    assert(machine.token.held == 1);
    if (--v->refs == 0)
        ++dropped;
}
static bool
""" + function("vmmfs_machine.c", "vmmfs_machine_deactivate") + r"""
int main(void) {
    struct vnode **fields[] = {
        &machine.id_node.node.vnode, &machine.vcpu.node.vnode, &machine.memory.node.vnode,
        &machine.loader.node.vnode, &machine.boot.node.vnode, &machine.stopped.node.vnode,
        &machine.pciroot.node.vnode, &machine.serialroot.node.vnode, &machine.events.node.vnode
    };
    const int errors[] = { 0 }; /* Children cannot veto machine teardown. */
    unsigned trial, index;
    for (trial = 0; trial < NELEM(errors); ++trial) {
        for (veto_index = 0; veto_index < NELEM(children); ++veto_index) {
            memset(&machine, 0, sizeof(machine));
            machine.token.held = 1;
            machine.node.dead = true; /* Set by the generic caller. */
            machine.node.vnode = &parent;
            called = dropped = 0;
            child_error = errors[trial];
            for (index = 0; index < NELEM(children); ++index) {
                children[index].index = index;
                children[index].refs = 1;
                *fields[index] = &children[index];
            }
            machine.machine = &machine;
            assert(vmmfs_machine_deactivate(&machine.node) == false);
            assert(called == 0 && dropped == 0 && machine.node.vnode == &parent);
            assert(machine.token.held == 1);
            assert(machine.node.dead && machine.token.acquired == 0);
            machine.machine = NULL;
            assert(vmmfs_machine_deactivate(&machine.node) == true);
            assert(machine.node.dead && machine.token.acquired == 0);
            /* Deactivation does not detach the vnode backlink. */
            assert(called == 9 && dropped == 9 && machine.node.vnode == &parent);
            for (index = 0; index < NELEM(children); ++index)
                assert(children[index].refs == 0);
            assert(machine.token.held == 1);
        }
    }
    return 0;
}
""")

    def test_stop_is_local_and_idle_is_noop(self):
        run_c(COMMON + r"""
struct vmmfs_vcpu_thread { void *vcpu; };
struct vmmfs_vcpu { int token; struct vmmfs_vcpu_thread *threads;
    unsigned count; bool stop_requested, reset_requested; };
static unsigned kicks, wakes;
static struct vmmfs_vcpu group;
static void lwkt_gettoken(int *t) { assert(!*t); ++*t; }
static void lwkt_reltoken(int *t) { assert(*t); --*t; }
static void vmmfs_vcpu_thread_kick(struct vmmfs_vcpu_thread *t) {
    assert(group.token && t->vcpu); ++kicks;
}
static void wakeup(void *p) { assert(p == &group); ++wakes; }
void
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_request_stop") + r"""
int main(void) {
    struct vmmfs_vcpu_thread threads[2] = {{&group}, {NULL}};
    group.count = 2; group.reset_requested = true;
    vmmfs_vcpu_request_stop(&group);
    assert(!group.stop_requested && group.reset_requested && !kicks && !wakes);
    group.threads = threads;
    vmmfs_vcpu_request_stop(&group);
    assert(group.stop_requested && !group.reset_requested && kicks == 1);
    vmmfs_vcpu_request_stop(&group);
    assert(group.stop_requested && kicks == 2);
    /* Reset temporarily removes all VMM instances, not the live workers. */
    threads[0].vcpu = NULL; group.stop_requested = false; group.reset_requested = true;
    vmmfs_vcpu_request_stop(&group);
    assert(group.stop_requested && !group.reset_requested && kicks == 2);
    group.threads = NULL; group.stop_requested = false;
    unsigned before = wakes;
    vmmfs_vcpu_request_stop(&group);
    assert(!group.stop_requested && wakes == before && !group.token);
    return 0;
}
""")

if __name__ == "__main__":
    unittest.main(verbosity=2)
