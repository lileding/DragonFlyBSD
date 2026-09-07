"""On-demand stopped projection and fixed-node lock scope."""
import unittest
from test_regress import function, run_c

class GetItem(unittest.TestCase):
    def test_token_scope_and_handoff(self):
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#define DT_REG 1
#define DT_CHR 2
#define DT_DIR 3
struct vnode { int holds; };
struct vmmfs_node { struct vnode *vnode; unsigned inode; };
struct thread { void *vcpu; };
struct child { struct vmmfs_node node; int token, start_ready; struct thread *threads; };
struct vmmfs_stopped { struct vmmfs_node node; struct vnode vnode; };
struct vmmfs_machine {
    struct vmmfs_node node;
    struct child id_node, vcpu, memory, loader, boot, events, pciroot, serialroot;
    void *machine; unsigned stopped_inode;
};
struct vmmfs_machine_item { const char *name; unsigned inode; int type; };
static struct vmmfs_machine m;
static struct thread bsp;
static unsigned held, locks, allocations, mode;
static void lwkt_gettoken(int *t) { assert(!*t); *t=1; ++held; ++locks; }
static void lwkt_reltoken(int *t) { assert(*t && held); *t=0; --held; }
static void vref(struct vnode *v) { assert(!held); ++v->holds; }
static void vrele(struct vnode *v) {
    assert(!held && v->holds==1 && allocations);
    --allocations;
    free((char *)v - offsetof(struct vmmfs_stopped, vnode));
}
static int vmmfs_stopped_create(struct vmmfs_node *parent, struct vmmfs_stopped **out) {
    assert(parent == &m.node && !held);
    if (mode==1) return ENOMEM;
    struct vmmfs_stopped *s=calloc(1,sizeof(*s)); assert(s);
    s->vnode.holds=1; s->node.vnode=&s->vnode; s->node.inode=m.stopped_inode;
    *out=s; ++allocations;
    if (mode==2) { m.vcpu.threads=&bsp; bsp.vcpu=&m; }
    return 0;
}
static int
''' + function('vmmfs_machine.c', 'vmmfs_machine_get_item') + r'''
static int
''' + function('vmmfs_machine.c', 'vmmfs_machine_read_item') + r'''
int main(void) {
    struct vnode fixed={0}, *v;
    struct vmmfs_machine_item item;
    struct child *children[]={&m.id_node,&m.vcpu,&m.memory,&m.loader,&m.boot,&m.events,&m.pciroot,&m.serialroot};
    const char *names[]={"id","vcpu","mem","loader","boot","events","pci","serial"};
    m.stopped_inode=1234;
    for (unsigned i=0;i<8;++i) {
        children[i]->node.vnode=&fixed; children[i]->node.inode=i+10;
        assert(!vmmfs_machine_get_item(&m,names[i],strlen(names[i]),&v));
        assert(v==&fixed && !locks);
        assert(!vmmfs_machine_read_item(&m,i<6?i:i+1,&item));
        assert(item.inode==i+10 && !locks);
    }
    for (unsigned runtime=0;runtime<2;++runtime)
    for (unsigned ready=0;ready<2;++ready)
    for (unsigned workers=0;workers<2;++workers)
    for (unsigned cpu=0;cpu<2;++cpu) {
        m.machine=runtime?&m:NULL; m.vcpu.start_ready=ready;
        m.vcpu.threads=workers?&bsp:NULL; bsp.vcpu=cpu?&m:NULL;
        bool visible=!(workers&&cpu);
        assert(!vmmfs_machine_read_item(&m,6,&item));
        assert((item.name!=NULL)==visible);
        if (visible) assert(item.inode==1234);
        assert(vmmfs_machine_get_item(&m,"stopped",7,&v)==(visible?0:ENOENT));
        if (visible) { assert(v->holds==1); vrele(v); }
        assert(!held && !allocations);
    }
    for (mode=1;mode<=2;++mode) {
        m.vcpu.threads=NULL;
        assert(vmmfs_machine_get_item(&m,"stopped",7,&v)==(mode==1?ENOMEM:ENOENT));
        assert(!held && !allocations);
    }
    return 0;
}
''')
