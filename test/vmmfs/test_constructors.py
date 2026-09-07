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
#define VMMFS_PCI_EVENT_SLOT_CREATED 2
#define bcopy(s,d,n) memcpy((d),(s),(n))
struct token { bool initialized; unsigned held; };
struct vmmfs_node {
    struct vmmfs_mount *mount;
    struct vnode *vnode;
    struct vmmfs_node *parent; struct token token;
    unsigned references; bool dead; unsigned inode, mode, size;
    bool (*deactivate)(struct vmmfs_node *);
    void (*drop)(struct vmmfs_node *);
 struct lock lock;};
struct vnode { void *v_data; };
struct vmmfs_root { int unused; };
struct vmmfs_mount { void *root; void *mount, *machine_vops, *pcislot_vops; };
struct child { struct vmmfs_node node; void *runtime_machine; };
struct component { void *machine; };
struct vmmfs_machine {
    struct vmmfs_node node; struct token token;
    char name[256];
    struct child id_node, vcpu, memory, loader, boot, pciroot, serialroot, events;
    struct child *stopped; struct component rtc, platform;
    struct vnode *id_vnode, *vcpu_vnode, *memory_vnode, *loader_vnode;
    struct vnode *boot_vnode, *stopped_vnode, *pciroot_vnode, *serialroot_vnode, *events_vnode;
};
struct vmmfs_pciroot { struct vmmfs_node node; struct token token; };
struct vmmfs_pcislot {
    struct vmmfs_node node; struct token token; unsigned bdf; void *entry, *resources;
    bool topology_reference;
    struct child descriptor, config, events;
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
#define vmmfs_pciroot_init child_init
#define vmmfs_serialroot_init child_init
#define vmmfs_events_init child_init
#define vmmfs_pcislot_events_init child_init
#define vmmfs_pcislot_config_init child_init
#define vmmfs_pcislot_descriptor_init child_init
#define vmmfs_events_log(...) ((void)0)
#define vmmfs_pcislot_events_log(...) ((void)0)
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
static void stopped_drop(struct vmmfs_node *n) { child_drop(n); free(n); }
static int vmmfs_machine_create_stopped(struct vmmfs_machine *m) {
    struct child *s = calloc(1,sizeof(*s));
    int error = child_init(&m->node, s);
    if (error) free(s); else { s->node.drop = stopped_drop; m->stopped = s; }
    return error;
}
static void vmmfs_machine_cleanup_stopped(struct vmmfs_machine *m) {
    if (!m->stopped) return;
    vmmfs_node_deactivate(&m->stopped->node);
    m->stopped = NULL;
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
    struct vmmfs_mount mount = { &root_vnode, &root, &root, &root };
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
        source = COMMON + r"""
#include <stdlib.h>
typedef unsigned int u_int;
typedef long off_t;
#define M_VMMFS 0
#define M_WAITOK 0
#define M_ZERO 0
#define UID_ROOT 0
#define GID_WHEEL 0
struct token { bool initialized; };
struct vmmfs_node { struct vnode *vnode;
    struct vmmfs_mount *mount;
    struct vmmfs_node *parent; struct token token; unsigned references;
    unsigned mode, inode; uint64_t size;
    bool (*deactivate)(struct vmmfs_node *);
    void (*drop)(struct vmmfs_node *);
 struct lock lock; bool dead;};
struct cdev { void *si_drv1; };
struct vnode { void *v_data; };
struct vm_object { unsigned references; };
struct vmmfs_launch {
    struct vmmfs_node node; struct token token; struct cdev *dev;
    struct vm_object *pager_object, *backing_object; int result;
};
struct vmmfs_mount { void *mount, *launch_vops; void *root; };
static u_int vmmfs_launch_serial;
static int vmmfs_launch_dev_ops;
static unsigned fail_at, objects, devices, tokens, vnodes, dropped;
static unsigned atomic_load_acq_int(unsigned *p) { return *p; }
static void atomic_add_int(unsigned *p, int n) { *p += n; }
static unsigned atomic_fetchadd_int(unsigned *p, int n) { unsigned old = *p; *p += n; return old; }
static void lwkt_token_init(struct token *t, const char *s) {
    (void)s; assert(!t->initialized); t->initialized = true; ++tokens;
}
static void lwkt_token_uninit(struct token *t) {
    assert(t->initialized); t->initialized = false; --tokens;
}
static void *kmalloc(size_t n, int tag, int flags) {
    (void)tag; (void)flags; ++objects; return calloc(1,n);
}
static void kfree(void *p, int tag) {
    (void)tag; assert(objects && p); --objects; ++dropped; free(p);
}
static unsigned vmmfs_root_allocate_inode(void *root) { assert(root); return 1; }
static struct cdev *make_only_dev(int *ops, u_int serial, int uid, int gid,
    int mode, const char *name, unsigned number) {
    assert(ops == &vmmfs_launch_dev_ops && serial == number);
    assert(uid == 0 && gid == 0 && mode == 0600 && name);
    if (fail_at == 1) return NULL;
    ++devices; return calloc(1,sizeof(struct cdev));
}
static void destroy_only_dev(struct cdev *d) {
    assert(devices && d->si_drv1 == NULL); --devices; free(d);
}
static void vm_object_deallocate(struct vm_object *o) {
    assert(o->references); --o->references;
}
static int vmmfs_vnode_create_cdev(void *m, void **ops, struct cdev *dev,
    struct vmmfs_node *n) {
    struct vnode **vp = &n->vnode;
    (void)m; assert(ops && dev && dev->si_drv1 == n);
    if (fail_at == 2) return ENFILE;
    *vp = calloc(1,sizeof(**vp)); (*vp)->v_data = n; ++vnodes; return 0;
}
static bool vmmfs_launch_deactivate(struct vmmfs_node *n) { (void)n; assert(0); return true; }
static void vmmfs_launch_drop(struct vmmfs_node *);
static void vmmfs_node_put(struct vmmfs_node *);
"""
        for filename, name, result in (
            ("vmmfs_node.c", "vmmfs_node_hold", "static void"),
            ("vmmfs_node.c", "vmmfs_node_put", "static void"),
            ("vmmfs_launch.c", "vmmfs_launch_drop", "static void"),
            ("vmmfs_launch.c", "vmmfs_launch_create", "int"),
        ):
            source += "\n" + result + "\n" + function(filename, name) + "\n"
        source += r"""
int main(void) {
    struct vmmfs_node parent = { .references = 1 };
    struct vnode root = { &parent };
    struct vmmfs_mount mount = { &parent, &parent, &root };
    parent.mount = &mount;
    struct vmmfs_launch *result;
    for (fail_at = 0; fail_at <= 2; ++fail_at) {
        dropped = 0; result = (void *)1;
        int error = vmmfs_launch_create(&parent, 4096, &result);
        if (fail_at == 0) {
            assert(error == 0 && result != NULL && parent.references == 2);
            struct vmmfs_launch *launch = result;
            assert(launch->node.mount == parent.mount);
            assert(launch->result == EINPROGRESS && launch->node.size == 4096);
            /* A not-yet-published candidate has no pager; final put owns cdev. */
            struct vm_object backing = { 2 };
            launch->backing_object = &backing;
            free(result->node.vnode); result->node.vnode = NULL; --vnodes; vmmfs_node_put(&launch->node);
            assert(backing.references == 1);
        } else {
            assert(error == (fail_at == 1 ? ENOMEM : ENFILE) && result == NULL);
        }
        assert(parent.references == 1 && objects == 0 && devices == 0);
        assert(tokens == 0 && vnodes == 0 && dropped == 1);
    }
}
"""
        run_c(source)

if __name__ == "__main__":
    unittest.main(verbosity=2)
