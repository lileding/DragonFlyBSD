"""BSP publishes stopped from its known mount namespace before releasing runtime."""
import unittest
from test_regress import function, run_c

class StopNotify(unittest.TestCase):
    def test_publication_order(self):
        run_c(r"""
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef void *vmm_machine_t;
struct vnode { int unused; };
struct nchandle { int value; };
struct mount { struct nchandle mnt_ncmountpt; };
struct vmmfs_mount { struct mount *mount; };
struct node { struct vnode *vnode; struct vmmfs_mount *mount; };
struct vmmfs_machine { vmm_machine_t machine; struct node node;
    struct { struct node node; } stopped;
    char name[256]; int platform, serialroot, pciroot, rtc, memory, events; };
struct nlcomponent { char *nlc_nameptr; int nlc_namelen; };
static struct vmmfs_machine m;
static struct vnode machine_vnode, stopped_vnode;
static int steps, parent_locked, child_locked, parent_refs, child_refs;
#define VMMFS_MACHINE_EVENT_STOPPED 1
static int stop(int *p) { (void)p; assert(m.machine == &m); ++steps; return 0; }
#define vmmfs_platform_x64_stop stop
#define vmmfs_serialroot_stop stop
#define vmmfs_pciroot_stop stop
#define vmmfs_rtc_stop stop
static int vmm_machine_destroy(void *p) { assert(p == &m && steps == 4); ++steps; return 0; }
static void vmmfs_memory_release(int *p) { (void)p; assert(steps == 5); ++steps; }
static struct nchandle cache_nlookup(struct nchandle *p, struct nlcomponent *n) {
    assert(m.machine == &m);
    if (p->value == 0) {
        assert(!strcmp(n->nlc_nameptr,m.name) && n->nlc_namelen == (int)strlen(m.name));
        parent_locked=parent_refs=1; return (struct nchandle){1};
    }
    assert(p->value == 1 && parent_refs == 1 && !parent_locked);
    assert(!strcmp(n->nlc_nameptr,"stopped") && n->nlc_namelen == 7);
    child_locked=child_refs=1; return (struct nchandle){2};
}
static void cache_setunresolved(struct nchandle *p) {
    assert(p->value == 1 ? parent_locked : child_locked);
}
static void cache_setvp(struct nchandle *p, struct vnode *v) {
    if (p->value == 1) { assert(parent_locked && v == &machine_vnode && steps == 6); return; }
    assert(child_locked && v == &stopped_vnode && steps == 6); ++steps;
}
static void cache_unlock(struct nchandle *p) { assert(p->value == 1 && parent_locked); parent_locked=0; }
static void cache_put(struct nchandle *p) { assert(p->value == 2 && child_locked && child_refs); child_locked=child_refs=0; }
static void cache_drop(struct nchandle *p) { assert(p->value == 1 && !parent_locked && parent_refs); parent_refs=0; }
static void vmmfs_events_log(int *p, int e, const char *s) {
    (void)p; (void)s; assert(e == 1 && steps == 7 && m.machine == &m); ++steps;
}
static int atomic_cmpset_ptr(void **p, void *old, void *new) {
    assert(p == &m.machine && *p == old && new == NULL && steps == 8); *p = new; ++steps; return 1;
}
void
""" + function("vmmfs_machine.c", "vmmfs_machine_stopped") + r"""
int main(void) {
    struct mount mount = {{0}};
    struct vmmfs_mount state = {&mount};
    m.machine=&m; m.node.vnode=&machine_vnode; m.node.mount=&state;
    m.stopped.node.vnode=&stopped_vnode; strcpy(m.name,"test");
    vmmfs_machine_stopped(&m);
    assert(m.machine == NULL && steps == 9);
    assert(!parent_locked && !child_locked && !parent_refs && !child_refs);
    return 0;
}
""")
