#!/usr/bin/env python3
"""Exercise resource pager handoff against revoke and reset interleavings."""
import unittest
from test_regress import COMMON, function, run_c

class Pager(unittest.TestCase):
    def test_resource_fault_rechecks_ownership(self):
        run_c(COMMON + r"""
typedef long vm_ooffset_t;
struct token { bool held; };
struct vm_object { void *handle; int references; };
typedef struct vm_object *vm_object_t;
struct vmspace { int vm_map; int references; };
struct page { unsigned valid; bool busy; };
typedef struct page *vm_page_t;
struct vmmfs_pcislot_resource {
    struct token token; bool revoked, bus_master_enabled;
    long mapping_size; int kind;
    struct vmspace *vmspace; struct vm_object *backing_object;
};
#define VM_PROT_EXECUTE 4
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_FAULT_DIRTY 1
#define VM_PAGER_ERROR 5
#define VM_PAGER_OK 0
#define VM_ALLOC_NORMAL 0
#define VM_ALLOC_SYSTEM 0
#define VM_ALLOC_ZERO 0
#define VM_ALLOC_RETRY 0
#define VMMFS_PCISLOT_RESOURCE_DMA 1
#define VM_PAGE_BITS_ALL 255
#define TRUE 1
#define OFF_TO_IDX(x) ((x) / 4096)
static struct vmmfs_pcislot_resource resource;
static struct vmspace original, replacement;
static struct vm_object backing;
static struct page page;
static int change, wakes;
static void lwkt_gettoken(struct token *t) { assert(!t->held); t->held = true; }
static void lwkt_reltoken(struct token *t) { assert(t->held); t->held = false; }
static void interleave(void) {
    if (change == 1) resource.revoked = true;
    if (change == 2) resource.bus_master_enabled = false;
    if (change == 3) resource.vmspace = &replacement;
}
static void vmspace_ref(struct vmspace *v) { ++v->references; }
static void vmspace_rel(struct vmspace *v) {
    assert(v->references > 0);
    if (resource.vmspace != v || resource.revoked || !resource.bus_master_enabled)
        assert(!page.busy);
    --v->references;
}
static vm_page_t vm_fault_page(int *map, long off, int prot, int flags,
                              int *error, int *busy) {
    (void)map; (void)off; (void)prot; (void)flags;
    interleave(); page.busy = true; *error = 0; *busy = 1; return &page;
}
static void vm_object_reference_quick(vm_object_t o) { ++o->references; }
static void vm_object_deallocate(vm_object_t o) { assert(o->references > 0); --o->references; }
static vm_page_t vm_page_grab(vm_object_t o, long index, int flags) {
    (void)o; (void)index; (void)flags;
    interleave(); page.busy = true; return &page;
}
void vm_page_wakeup(vm_page_t p) { assert(p->busy); p->busy = false; ++wakes; }
static void vm_page_unhold(vm_page_t p) { assert(!p->busy); }
static void vm_page_zero_invalid(vm_page_t p, int set) { (void)set; p->valid = VM_PAGE_BITS_ALL; }
static int vmmfs_pcislot_resource_track_page(
    struct vmmfs_pcislot_resource *r, vm_page_t p, struct vmspace *v) {
    assert(r->token.held && p->busy && !r->revoked);
    assert(r->kind != VMMFS_PCISLOT_RESOURCE_DMA ||
           (r->bus_master_enabled && r->vmspace == v));
    return 0;
}
static int
""" + function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resource_pager_fault") + r"""
int main(void) {
    struct vm_object object = { &resource, 1 };
    for (int kind = 0; kind <= 1; ++kind) {
        for (change = 0; change <= 3; ++change) {
            memset(&resource, 0, sizeof(resource));
            memset(&page, 0, sizeof(page));
            original.references = replacement.references = backing.references = 0;
            resource.kind = kind;
            resource.mapping_size = 4096;
            resource.bus_master_enabled = true;
            resource.vmspace = &original;
            resource.backing_object = &backing;
            vm_page_t result = NULL;
            wakes = 0;
            int error = vmmfs_pcislot_resource_pager_fault(&object, 0, VM_PROT_READ, &result);
            bool reject = change == 1 || (kind == 1 && change == 2);
            assert(error == (reject ? VM_PAGER_ERROR : VM_PAGER_OK));
            assert(result == (reject ? NULL : &page));
            assert(wakes == ((reject || (kind == 1 && change == 3)) ? 1 : 0));
            assert(!resource.token.held);
            assert(original.references == 0 && replacement.references == 0 && backing.references == 0);
        }
    }
    return 0;
}
""")

    def test_launch_revoke_drains_pmap_handoff(self):
        from test_regress import SOURCE
        text = (SOURCE / "vmmfs_launch.c").read_text()
        drain = ("static int\n" + function("vmmfs_launch.c", "vmmfs_launch_drain_page")
                 if "\nvmmfs_launch_drain_page(" in text else "")
        run_c(COMMON + r"""
#define TRUE 1
#define FALSE 0
typedef struct vm_page { bool busy; bool pte; int contents; } *vm_page_t;
struct vm_page_rb_tree { struct vm_page pages[2]; };
struct vm_object { struct vm_page_rb_tree rb_memq; bool locked; int references; };
struct token { bool held; };
struct vmmfs_node { struct token token;  struct lock lock; bool dead;};
struct vmmfs_launch {
    struct vmmfs_node node;
    struct vm_object *pager_object, *backing_object;
};
static struct vm_object pager, backing;
static struct vmmfs_launch launch;
static int waits, removals;
void lwkt_gettoken(struct token *t) { assert(!t->held); t->held = true; }
void lwkt_reltoken(struct token *t) { assert(t->held); t->held = false; }
void vm_object_hold(struct vm_object *o) { assert(!o->locked); o->locked = true; }
void vm_object_drop(struct vm_object *o) { assert(o->locked); o->locked = false; }
#define VM_OBJECT_LOCK(o) vm_object_hold(o)
#define VM_OBJECT_UNLOCK(o) vm_object_drop(o)
void vm_object_pip_wait(struct vm_object *o, const char *w) {
    (void)w; assert(o == &pager && o->locked);
    assert(launch.pager_object == NULL);
    /* PIP is zero, but an accepted fault has not installed its PTE. */
}
int vm_page_busy_try(vm_page_t p, int both) {
    assert(both == TRUE && backing.locked);
    if (p->busy) return 1;
    p->busy = true;
    return 0;
}
void vm_page_sleep_busy(vm_page_t p, int both, const char *w) {
    (void)w; assert(both == TRUE && p->busy);
    /* The old outer fault installs its PTE before releasing busy. */
    p->pte = true;
    p->busy = false;
    ++waits;
}
void vm_page_wakeup(vm_page_t p) { assert(p->busy); p->busy = false; }
int vm_page_rb_tree_RB_SCAN(struct vm_page_rb_tree *tree, void *cmp,
                         int (*callback)(vm_page_t, void *), void *data) {
    assert(cmp == NULL && backing.locked);
    for (int i = 0; i < 2; ++i)
        assert(callback(&tree->pages[i], data) == 0);
    return 0;
}
void vm_object_page_remove(struct vm_object *o, int start, int end, int clean) {
    assert(o == &pager && start == 0 && end == 0 && clean == FALSE);
    for (int i = 0; i < 2; ++i) {
        assert(!backing.rb_memq.pages[i].busy);
        backing.rb_memq.pages[i].pte = false;
        assert(backing.rb_memq.pages[i].contents == 42);
    }
    ++removals;
}
void vm_object_deallocate(struct vm_object *o) {
    assert(o == &pager && o->references == 1);
    --o->references;
}
""" + drain + "\nvoid\n" +
              function("vmmfs_launch.c", "vmmfs_launch_revoke") + r"""
int main(void) {
    for (int in_flight = 0; in_flight <= 1; ++in_flight) {
        memset(&pager, 0, sizeof(pager));
        memset(&backing, 0, sizeof(backing));
        pager.references = 1;
        launch.pager_object = &pager;
        launch.backing_object = &backing;
        waits = removals = 0;
        for (int i = 0; i < 2; ++i) {
            backing.rb_memq.pages[i].contents = 42;
            backing.rb_memq.pages[i].pte = true;
        }
        backing.rb_memq.pages[0].busy = in_flight;
        backing.rb_memq.pages[0].pte = !in_flight;
        vmmfs_launch_revoke(&launch);
        assert(waits == in_flight && removals == 1);
        assert(!pager.locked && !backing.locked);
        assert(!launch.node.token.held && pager.references == 0);
        /* Duplicate cleanup never touches the released pager. */
        vmmfs_launch_revoke(&launch);
        assert(removals == 1);
    }
    return 0;
}
""")

    def test_resource_revoke_releases_vmspace_after_mappings(self):
        from test_regress import SOURCE
        source = (SOURCE / "vmmfs_pcislot_resource.c").read_text()
        bodies = ""
        for result, name in (
            ("int", "vmmfs_pcislot_resource_track_page"),
            ("int", "vmmfs_pcislot_resource_drain_page"),
            ("void", "vmmfs_pcislot_resource_drain")):
            if "\n" + name + "(" in source:
                bodies += "static " + result + "\n" + function(
                    "vmmfs_pcislot_resource.c", name) + "\n"
        tracking = ("assert(vmmfs_pcislot_resource_track_page(&resource, &page, &space) == 0);"
                    "assert(vmmfs_pcislot_resource_track_page(&resource, &page, &space) == 0);"
                    "assert(allocated == 1 && ram.references == 2);"
                    if "vmmfs_pcislot_resource_track_page" in bodies else "")
        run_c(COMMON + r"""
#include <stdlib.h>
#include <sys/queue.h>
#define TRUE 1
#define FALSE 0
#define M_VMMFS 0
#define M_WAITOK 0
#define VMMFS_PCISLOT_RESOURCE_DMA 1
struct token { bool held; };
struct vm_object;
typedef struct vm_page {
    struct vm_object *object; bool busy;
} *vm_page_t;
struct vm_page_rb_tree { vm_page_t page; };
struct vm_object {
    int references; struct vm_page_rb_tree rb_memq;
};
typedef struct vm_object *vm_object_t;
struct vmspace { int references; };
struct vmmfs_pcislot_object {
    SLIST_ENTRY(vmmfs_pcislot_object) entry;
    struct vm_object *object;
};
SLIST_HEAD(vmmfs_pcislot_objects, vmmfs_pcislot_object);
struct vmmfs_pcislot_resource {
    struct token token; bool revoked, bus_master_enabled; int kind;
    vm_object_t pager_object;
    struct vmspace *vmspace;
    struct vmmfs_pcislot_objects objects;
    struct { int ki_note; } read_kq;
};
static struct vm_object pager, ram;
static struct vm_page page;
static struct vmspace space;
static bool removed;
static int mappings, allocated, freed, allocation_change;
static struct vmmfs_pcislot_resource *allocating;
static struct vmspace replacement;
void lwkt_gettoken(struct token *t) { assert(!t->held); t->held = true; }
void lwkt_reltoken(struct token *t) { assert(t->held); t->held = false; }
void *kmalloc(size_t size, int type, int flags) {
    (void)type; (void)flags;
    ++allocated;
    if (allocation_change == 1) allocating->revoked = true;
    if (allocation_change == 2) allocating->bus_master_enabled = false;
    if (allocation_change == 3) allocating->vmspace = &replacement;
    return calloc(1, size);
}
void kfree(void *p, int type) { (void)type; ++freed; free(p); }
void vm_object_reference_quick(vm_object_t o) { ++o->references; }
void vm_object_deallocate(vm_object_t o) { assert(o->references > 0); --o->references; }
void vmspace_rel(struct vmspace *v) {
    assert(v == &space && removed && mappings == 0);
    assert(v->references == 1); --v->references;
}
#define VM_OBJECT_LOCK(o) ((void)(o))
#define VM_OBJECT_UNLOCK(o) ((void)(o))
void vm_object_pip_wait(vm_object_t o, const char *w) { (void)o; (void)w; }
int vm_page_busy_try(vm_page_t p, int both) {
    assert(both); if (p->busy) return 1; p->busy = true; return 0;
}
void vm_page_sleep_busy(vm_page_t p, int both, const char *w) {
    (void)w; assert(both && p->busy);
    mappings = 1; p->busy = false;
}
void vm_page_wakeup(vm_page_t p) { assert(p->busy); p->busy = false; }
int vm_page_rb_tree_RB_SCAN(struct vm_page_rb_tree *tree, void *cmp,
                         int (*callback)(vm_page_t, void *), void *data) {
    assert(cmp == NULL); return callback(tree->page, data);
}
void vm_object_page_remove(vm_object_t o, int start, int end, int clean) {
    assert(o == &pager && start == 0 && end == 0 && !clean);
    assert(!page.busy);
    mappings = 0; removed = true;
}
void wakeup(void *p) { (void)p; }
#define KNOTE(p, h) ((void)(p), (void)(h))
""" + bodies + "static void\n" +
              function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resource_revoke") + r"""
int main(void) {
    struct vmmfs_pcislot_resource resource = {0};
    resource.kind = VMMFS_PCISLOT_RESOURCE_DMA;
    resource.bus_master_enabled = true;
    resource.pager_object = &pager;
    resource.vmspace = &space;
    pager.references = ram.references = space.references = 1;
    page.object = &ram; page.busy = true;
    ram.rb_memq.page = &page;
    lwkt_gettoken(&resource.token);
""" + tracking + r"""
    lwkt_reltoken(&resource.token);
    vmmfs_pcislot_resource_revoke(&resource);
    assert(resource.revoked && resource.vmspace == NULL);
    assert(removed && !page.busy && mappings == 0);
    assert(pager.references == 1 && ram.references == 1 && space.references == 0);
    assert(SLIST_EMPTY(&resource.objects));
    vmmfs_pcislot_resource_revoke(&resource);
    assert(allocated == freed);
    for (allocation_change = 1; allocation_change <= 3; ++allocation_change) {
        memset(&resource, 0, sizeof(resource));
        resource.kind = VMMFS_PCISLOT_RESOURCE_DMA;
        resource.bus_master_enabled = true;
        resource.vmspace = &space;
        allocating = &resource;
        page.busy = true;
        lwkt_gettoken(&resource.token);
        int error = vmmfs_pcislot_resource_track_page(&resource, &page, &space);
        assert(error == (allocation_change == 3 ? EAGAIN : EFAULT));
        assert(SLIST_EMPTY(&resource.objects) && ram.references == 1);
        assert(allocated == freed);
        lwkt_reltoken(&resource.token);
        vm_page_wakeup(&page);
    }
    return 0;
}
""")

    def test_rebind_and_bme_detach_objects_before_drain(self):
        for name in ("vmmfs_pcislot_resources_rebind",
                     "vmmfs_pcislot_resources_set_decode"):
            body = function("vmmfs_pcislot_resource.c", name)
            detached = body.index("objects = resource->objects;")
            closed = body.index("SLIST_INIT(&resource->objects);", detached)
            unlocked = body.index("lwkt_reltoken(&resource->token);", closed)
            drained = body.index("vmmfs_pcislot_resource_drain(pager, &objects);",
                                 unlocked)
            prefix = body[:detached]
            if name.endswith("set_decode"):
                from test_regress import balanced
                start = prefix.index("if (bus_master_enabled) {")
                branch = balanced(prefix, prefix.index("{", start))
                self.assertIn("continue;", branch)
                prefix = prefix[:start] + prefix[start:].replace(branch, "", 1)
            self.assertGreater(prefix.rfind("lwkt_gettoken(&resource->token);"),
                               prefix.rfind("lwkt_reltoken(&resource->token);"))
            self.assertIn("vm_object_reference_quick(pager);",
                          body[closed:unlocked])
            self.assertNotIn("vm_object_page_remove(", body)
            if name.endswith("rebind"):
                self.assertLess(drained, body.index("vmspace_rel(old_vmspace);"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
