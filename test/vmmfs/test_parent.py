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
struct vnode { unsigned refs; };
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
static int
FUNCTION
int main(void) {
    root.registry = &registry;
    root.node.dead = true;
    for (mode = 0; mode != 2; ++mode) {
        registry.MEMBER = malloc(sizeof(*registry.MEMBER));
        child.node.vnode = &vnode; registry.MEMBER->CHILD = &child;
        vnode.refs = 1; release_count = 0;
        int error = DEACTIVATE(&root.node);
        assert(root.token.held == 0);
        assert(error == 0 && registry.MEMBER == NULL && vnode.refs == 0);
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
struct vmmfs_stopped { struct vmmfs_node node; };
struct vmmfs_machine {
    struct vmmfs_node node; struct token token;
    void *machine;
    bool runtime_releasing, runtime_released;
    unsigned runtime_references;
    struct { struct vmmfs_node node; struct token token; unsigned active_count; void *threads; } vcpu;
    struct { struct vmmfs_node node; } id_node, memory, loader, boot, pciroot, serialroot, events;
    struct vmmfs_stopped *stopped;
};
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
static struct vmmfs_machine machine;
static struct vnode children[9], parent;
static struct vmmfs_stopped stopped;
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
static void vrele(struct vnode *v) {
    assert(v != NULL && v->refs != 0);
    assert(machine.token.held == 1);
    if (--v->refs == 0)
        ++dropped;
}
static int
""" + function("vmmfs_machine.c", "vmmfs_machine_deactivate") + r"""
int main(void) {
    struct vnode **fields[] = {
        &machine.id_node.node.vnode, &machine.vcpu.node.vnode, &machine.memory.node.vnode,
        &machine.loader.node.vnode, &machine.boot.node.vnode, &stopped.node.vnode,
        &machine.pciroot.node.vnode, &machine.serialroot.node.vnode, &machine.events.node.vnode
    };
    const int errors[] = { 0, EBUSY };
    unsigned trial, index;
    for (trial = 0; trial < NELEM(errors); ++trial) {
        for (veto_index = 0; veto_index < NELEM(children); ++veto_index) {
            memset(&machine, 0, sizeof(machine));
            machine.stopped = &stopped; machine.token.held = 1;
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
            assert(vmmfs_machine_deactivate(&machine.node) == EBUSY);
            assert(called == 0 && dropped == 0 && machine.node.vnode == &parent);
            assert(machine.token.held == 1);
            assert(machine.node.dead && machine.token.acquired == 0);
            machine.machine = NULL;
            assert(vmmfs_machine_deactivate(&machine.node) == 0);
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

    def test_stop_admission_rejects_retired_siblings(self):
        run_c(COMMON + r"""
#include <stdlib.h>
struct token { unsigned held; };
struct vmmfs_node { struct vnode *vnode; struct token token; bool dead; unsigned references;  struct lock lock;};
struct vnode { void *v_data; unsigned refs; };
struct vmmfs_stopped { struct vmmfs_node node; };
struct vmmfs_vcpu { struct vmmfs_node node; };
struct vmmfs_events { struct vmmfs_node node; };
struct vmmfs_launch { struct vmmfs_node node; };
struct vmmfs_machine {
    struct vmmfs_node node; struct token token;
    void *machine;
    struct vmmfs_launch *launch; struct vmmfs_stopped *stopped;
    bool runtime_releasing, runtime_released;
    unsigned runtime_references;
    struct vmmfs_vcpu vcpu;
    struct vmmfs_events events;
};
static struct vnode candidate;
static unsigned allocated, discarded, logs;
#define VMMFS_MACHINE_EVENT_STOP_REQUESTED 1
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static void vref(struct vnode *v) { assert(v->refs); ++v->refs; }
static void vrele(struct vnode *v) { assert(v->refs); --v->refs; }
static void vmmfs_node_hold(struct vmmfs_node *n) {
    assert(n->references != 0); ++n->references;
}
static void vmmfs_node_put(struct vmmfs_node *n) {
    assert(n->references != 0);
    if (--n->references == 0) { free(n); --allocated; }
}
static int vmmfs_stopped_create(struct vmmfs_node *parent,
    struct vmmfs_stopped **out) {
    assert(parent->token.held == 0);
    struct vmmfs_stopped *s = calloc(1, sizeof(*s)); assert(s);
    s->node.references = 1; candidate.v_data = s; candidate.refs = 1;
    s->node.vnode = &candidate; *out = s; ++allocated; return 0;
}
static void vmmfs_vnode_discard(struct vnode *v) {
    assert(v == &candidate && v->refs == 1);
    v->v_data = NULL; v->refs = 0; ++discarded;
}
static int vmmfs_machine_abort(void *launch) { (void)launch; assert(0); return 0; }
static int vmmfs_machine_create_stopped(struct vmmfs_machine *m) {
    (void)m; assert(0); return 0;
}
static void vmmfs_machine_runtime_put(struct vmmfs_machine *m) {
    (void)m; assert(0);
}
static void vmmfs_vcpu_request_stop(struct vmmfs_vcpu *v) { (void)v; assert(0); }
static void vmmfs_events_log(struct vmmfs_events *e, unsigned verb,
    const char *format, const char *reason) {
    assert(e->node.references == 2 && verb == VMMFS_MACHINE_EVENT_STOP_REQUESTED);
    (void)format; (void)reason; ++logs;
}
static int
""" + function("vmmfs_machine.c", "vmmfs_machine_prepare_stopped") + "\nstatic int\n" + function(
            "vmmfs_machine.c", "vmmfs_machine_request_stop") + r"""
int main(void) {
    struct vmmfs_machine machine = {0};
    struct vnode cpu = { .v_data=&machine.vcpu, .refs=1 };
    struct vnode events = { .v_data=&machine.events, .refs=1 };
    machine.vcpu.node.vnode = &cpu; machine.events.node.vnode = &events;
    machine.vcpu.node.references = machine.events.node.references = 1;
    assert(vmmfs_machine_request_stop(&machine, "external") == 0 && logs == 1);
    assert(machine.vcpu.node.references == 1 && machine.events.node.references == 1);
    /* A child veto may reopen machine admission after the vCPU was reclaimed. */
    machine.vcpu.node.vnode = NULL; machine.vcpu.node.references = 0;
    assert(vmmfs_machine_request_stop(&machine, "external") == ENOENT);
    assert(vmmfs_machine_prepare_stopped(&machine) == EBUSY);
    assert(!machine.stopped && allocated == 0 && discarded == 1);
    /* Initial construction creates stopped before the events node exists. */
    machine.vcpu.node.vnode = &cpu; machine.vcpu.node.references = 1;
    machine.events.node.vnode = NULL; machine.events.node.references = 0;
    assert(vmmfs_machine_request_stop(&machine, "external") == ENOENT);
    assert(vmmfs_machine_prepare_stopped(&machine) == 0);
    assert(machine.stopped->node.vnode == &candidate && allocated == 1);
    struct vmmfs_node *node = candidate.v_data;
    machine.stopped = NULL;
    vmmfs_vnode_discard(&candidate); vmmfs_node_put(node);
    assert(!allocated && logs == 1 && !machine.token.held);
    return 0;
}
""")

if __name__ == "__main__":
    unittest.main(verbosity=2)
