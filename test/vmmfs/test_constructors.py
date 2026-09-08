#!/usr/bin/env python3
"""Inject every composite-constructor failure into production cleanup bodies."""
import unittest
from test_regress import COMMON, function, run_c

HARNESS = COMMON + r"""
#include <stdlib.h>
#define M_VMMFS 0
#define M_WAITOK 0
#define M_ZERO 0
#define VDIR 1
#define NAME_MAX 255
#define VMMFS_MACHINE_MODE 0755
#define VMMFS_PCISLOT_MODE 0755
#define VMMFS_MACHINE_EVENT_CREATED 0
#define VMMFS_MACHINE_EVENT_STOPPED 1
#define bcopy(s,d,n) memcpy((d),(s),(n))
struct vmmfs_node_item;
struct token { bool initialized; unsigned held; };
struct ucred;
struct vmmfs_node {
    struct vmmfs_mount *mount;
    struct vnode *vnode;
    struct vmmfs_node *parent; struct token token;
    unsigned references; bool dead; unsigned inode, mode, size;
    bool (*deactivate)(struct vmmfs_node *);
    void (*drop)(struct vmmfs_node *);
    int (*get_item)(struct vmmfs_node *, const char *, size_t, struct vnode **);
    int (*remove_item)(struct vmmfs_node *, const char *, size_t, struct ucred *);
    int (*create_item)(struct vmmfs_node *, const char *, size_t, struct vnode **);
    int (*read_item)(struct vmmfs_node *, uint64_t, struct vmmfs_node_item *);
 struct lock lock;};
struct vnode { void *v_data; };
struct vmmfs_root { int unused; };
struct vmmfs_mount { void *root; void *mount, *node_vops; };
struct child { struct vmmfs_node node; void *runtime_machine; };
struct component { void *machine; };
struct vmmfs_machine {
    struct vmmfs_node node; struct token token;
    char name[256];
    struct child id_node, vcpu, memory, loader, boot, pciroot, serialroot, events;
    struct child stopped; struct component rtc, platform;
    struct vnode *id_vnode, *vcpu_vnode, *memory_vnode, *loader_vnode;
    struct vnode *boot_vnode, *stopped_vnode, *pciroot_vnode, *serialroot_vnode, *events_vnode;
};
struct vmmfs_pciroot { struct vmmfs_node node; struct token token; };
struct vmmfs_pcislot {
    struct vmmfs_node node; struct token token; unsigned bdf; void *entry, *resources;

    struct child descriptor, config, powered;
    struct vnode *descriptor_vnode, *config_vnode, *events_vnode;
};
static unsigned stage, fail_at, objects, vnodes, child_live, token_live;
static unsigned component_live, object_drops;
static bool defer_reclaim;
static struct vmmfs_node *pending[16];
static unsigned pending_count;
static unsigned atomic_load_acq_int(unsigned *p) { return *p; }
static void atomic_add_int(unsigned *p, int n) { *p += n; }
static unsigned atomic_fetchadd_int(unsigned *p, int n) { unsigned old = *p; *p += n; return old; }
static void lwkt_token_init(struct token *t, const char *s) {
    (void)s; assert(!t->initialized); t->initialized = true; ++token_live;
}
static void lwkt_token_uninit(struct token *t) {
    assert(t->initialized && t->held == 0); t->initialized = false; --token_live;
}
static void *kmalloc(size_t n, int tag, int flags) {
    (void)tag; (void)flags; ++objects; return calloc(1,n);
}
static void kfree(void *p, int tag) {
    (void)tag; assert(p && objects); --objects; ++object_drops; free(p);
}
static unsigned vmmfs_root_allocate_inode(struct vmmfs_root *r) { assert(r); return 1; }
static void vmmfs_node_put(struct vmmfs_node *);
static void vmmfs_node_hold(struct vmmfs_node *);
static void child_drop(struct vmmfs_node *n) {
    assert(n->references == 0 && n->drop == NULL && child_live);
    --child_live;
}
static bool child_close(struct vmmfs_node *n) { (void)n; return true; }
static bool fail(void) { return ++stage == fail_at; }
static int child_init(struct vmmfs_node *p,
    struct child *c) {
    struct vnode **vp = &c->node.vnode;
    assert(p->mount); *vp = NULL;
    c->node.parent = p; c->node.mount = p->mount; c->node.references = 1; c->node.drop = child_drop;
    vmmfs_node_hold(p); ++child_live;
    /* A failed init must return its own parent reference before returning. */
    if (fail()) { vmmfs_node_put(&c->node); return ENFILE; }
    *vp = calloc(1,sizeof(**vp)); (*vp)->v_data = &c->node; ++vnodes;
    c->node.vnode = *vp;
    c->node.deactivate = child_close;
    return 0;
}
#define vmmfs_machine_id_init child_init
#define vmmfs_vcpu_init child_init
#define vmmfs_memory_init child_init
#define vmmfs_loader_init child_init
#define vmmfs_boot_init child_init
#define vmmfs_stopped_init child_init
#define vmmfs_pciroot_init child_init
#define vmmfs_serialroot_init child_init
#define vmmfs_events_init child_init
#define vmmfs_pcislot_powered_init child_init
#define vmmfs_pcislot_config_init child_init
#define vmmfs_pcislot_descriptor_init child_init
#define vmmfs_events_log(...) ((void)0)
static void vmmfs_vnode_discard(struct vnode *v) {
    if (!v) return;
    /* Detached unpublished vnode no longer owns v_data; caller puts the node. */
    ((struct vmmfs_node *)v->v_data)->vnode = NULL;
    assert(vnodes); --vnodes; v->v_data = NULL; free(v);
}
static bool vmmfs_node_deactivate(struct vmmfs_node *n) {
    if (!n || !n->deactivate) return true;
    assert(n->deactivate(n));
    if (defer_reclaim) {
        assert(pending_count < 16); pending[pending_count++] = n;
    } else {
        vmmfs_vnode_discard(n->vnode);
        vmmfs_node_put(n);
    }
    return true;
}
static int vmmfs_vnode_create_regular(void *m, void **ops, int type,
    struct vmmfs_node *n) {
    struct vnode **vp = &n->vnode;
    (void)m; assert(ops && type == VDIR && n);
    *vp = NULL;
    if (fail()) return ENFILE;
    *vp = calloc(1,sizeof(**vp)); (*vp)->v_data = n; n->vnode = *vp;
    ++vnodes; return 0;
}
static int component_init(struct vmmfs_machine *m, struct component *c) {
    if (fail()) return ENFILE;
    c->machine = m; ++component_live; return 0;
}
static void component_fini(struct component *c) {
    assert(c->machine && component_live); c->machine = NULL; --component_live;
}
#define vmmfs_platform_x64_init component_init
#define vmmfs_rtc_init component_init
#define vmmfs_platform_x64_fini component_fini
#define vmmfs_rtc_fini component_fini
#define vmmfs_machine_remove_item NULL
#define vmmfs_pcislot_remove_item NULL
#define vmmfs_pcislot_get_item vmmfs_machine_get_item
#define vmmfs_pcislot_read_item vmmfs_machine_read_item
#define vmmfs_machine_create_item vmmfs_machine_get_item
static int vmmfs_machine_get_item(struct vmmfs_node *n, const char *s, size_t l, struct vnode **v) { (void)n; (void)s; (void)l; (void)v; return 0; }
static int vmmfs_machine_read_item(struct vmmfs_node *n, uint64_t i, struct vmmfs_node_item *v) { (void)n; (void)i; (void)v; return 0; }
static bool vmmfs_machine_deactivate(struct vmmfs_node *n) { (void)n; assert(0); return true; }
static bool vmmfs_pcislot_deactivate(struct vmmfs_node *n) { (void)n; assert(0); return true; }
static void vmmfs_machine_drop(struct vmmfs_node *);
static void vmmfs_pcislot_drop(struct vmmfs_node *);
"""

class Constructors(unittest.TestCase):
    def test_every_composite_construction_failure_returns_ownership(self):
        source = HARNESS
        for filename, name, result in (
            ("vmmfs_node.c", "vmmfs_node_hold", "static void"),
            ("vmmfs_node.c", "vmmfs_node_put", "static void"),
            ("vmmfs_machine.c", "vmmfs_machine_drop", "static void"),
            ("vmmfs_machine.c", "vmmfs_machine_create", "int"),
            ("vmmfs_pcislot.c", "vmmfs_pcislot_drop", "static void"),
            ("vmmfs_pcislot.c", "vmmfs_pcislot_create", "int"),
        ):
            source += "\n" + result + "\n" + function(filename, name) + "\n"
        source += r"""
int main(void) {
    struct vmmfs_root root;
    struct vnode root_vnode = { &root };
    struct vmmfs_mount mount = { &root_vnode, &root, &root };
    struct vmmfs_node parent = { .references = 1, .mount = &mount };
    struct vmmfs_machine *machine;
    struct vmmfs_pcislot *slot;
    for (unsigned deferred = 0; deferred < 2; ++deferred) {
    defer_reclaim = deferred;
    for (unsigned kind = 0; kind < 2; ++kind) {
        unsigned limit = kind == 0 ? 12 : 4;
        for (fail_at = 1; fail_at <= limit; ++fail_at) {
            stage = object_drops = 0; machine = NULL; slot = NULL;
            int error = kind == 0 ?
                vmmfs_machine_create(&parent, "test", 4, &machine) :
                vmmfs_pcislot_create(&parent, 8, &slot);
            assert(error == ENFILE && stage == fail_at && machine == NULL && slot == NULL);
            if (pending_count) {
                /* The failed parent remains alive until its children reclaim. */
                assert(parent.references == 2 && objects == 1);
                while (pending_count) {
                    struct vmmfs_node *n = pending[--pending_count];
                    vmmfs_vnode_discard(n->vnode);
                    vmmfs_node_put(n);
                }
            }
            assert(parent.references == 1 && objects == 0 && vnodes == 0);
            assert(child_live == 0 && token_live == 0 && component_live == 0);
            assert(object_drops == 1);
        }
    }
    }
}
"""
        run_c(source)

    def test_launch_creation_releases_cdev_and_parent_on_failure(self):
        create = function("vmmfs_launch.c", "vmmfs_launch_create")
        put = function("vmmfs_launch.c", "vmmfs_launch_put")
        self.assertIn("vmmfs_node_hold(&machine->node)", create)
        self.assertIn("vmmfs_launch_put(launch)", create.split("fail:")[1])
        self.assertIn("vmmfs_node_put(&launch->machine->node)", put)
        self.assertIn("destroy_only_dev(launch->dev)", put)
        self.assertIn("vmmfs_launch_hold(launch)", create)
        self.assertIn("vmmfs_launch_put(launch)", function("vmmfs_launch.c", "vmmfs_launch_reclaim"))

if __name__ == "__main__":
    unittest.main(verbosity=2)
