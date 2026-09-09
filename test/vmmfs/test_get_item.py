"""Persistent stopped node and fixed-node lock scope."""
import unittest
from test_regress import function, run_c
from pathlib import Path
import re

class GetItem(unittest.TestCase):
    def test_token_scope_and_handoff(self):
        source = (Path(__file__).resolve().parents[2]/"sys/vfs/vmmfs/vmmfs_machine.c").read_text()
        table = re.search(r"static const struct \{.*?\} vmmfs_machine_items\[\] = \{.*?\n\};", source, re.S).group(0)
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
    void *machine; struct vmmfs_stopped stopped;
};
struct vmmfs_node_item { char name[256]; unsigned inode; uint8_t type; };
#define bcopy(s,d,n) memcpy((d),(s),(n))
#define NELEM(a) (sizeof(a)/sizeof((a)[0]))
''' + table + r'''
static struct vmmfs_machine m;
static struct thread bsp;
static unsigned held, locks;
static void lwkt_gettoken(int *t) { assert(!*t); *t=1; ++held; ++locks; }
static void lwkt_reltoken(int *t) { assert(*t && held); *t=0; --held; }
static void vref(struct vnode *v) { assert(!held); ++v->holds; }
static bool vmmfs_vcpu_is_stopped(struct child *c) {
    lwkt_gettoken(&c->token);
    bool stopped = c->threads == NULL || c->threads[0].vcpu == NULL;
    lwkt_reltoken(&c->token);
    return stopped;
}
static int
''' + function('vmmfs_machine.c', 'vmmfs_machine_get_item') + r'''
static int
''' + function('vmmfs_machine.c', 'vmmfs_machine_read_item') + r'''
int main(void) {
    struct vnode fixed={0}, *v;
    struct vmmfs_node_item item;
    struct child *children[]={&m.id_node,&m.vcpu,&m.memory,&m.loader,&m.boot,&m.events,&m.pciroot,&m.serialroot};
    const char *names[]={"id","vcpu","mem","loader","boot","events","pci","serial"};
    m.stopped.node.inode=1234;
    m.stopped.node.vnode=&m.stopped.vnode;
    m.stopped.vnode.holds=1;
    for (unsigned i=0;i<8;++i) {
        children[i]->node.vnode=&fixed; children[i]->node.inode=i+10;
        assert(!vmmfs_machine_get_item(&m.node,names[i],strlen(names[i]),&v));
        assert(v==&fixed && !locks);
        assert(!vmmfs_machine_read_item(&m.node,i<6?i:i+1,&item));
        assert(item.inode==i+10 && !locks);
        assert(!strcmp(item.name,names[i]));
        assert(item.type==(i==4?DT_CHR:i>=6?DT_DIR:DT_REG));
    }
    assert(vmmfs_machine_get_item(&m.node,"vc",2,&v)==ENOENT);
    assert(vmmfs_machine_get_item(&m.node,"vcpuX",5,&v)==ENOENT);
    assert(vmmfs_machine_get_item(&m.node,"",0,&v)==ENOENT);
    assert(vmmfs_machine_read_item(&m.node,9,&item)==ENOENT);
    assert(vmmfs_machine_read_item(&m.node,UINT64_MAX,&item)==ENOENT);
    for (unsigned runtime=0;runtime<2;++runtime)
    for (unsigned ready=0;ready<2;++ready)
    for (unsigned workers=0;workers<2;++workers)
    for (unsigned cpu=0;cpu<2;++cpu) {
        m.machine=runtime?&m:NULL; m.vcpu.start_ready=ready;
        m.vcpu.threads=workers?&bsp:NULL; bsp.vcpu=cpu?&m:NULL;
        bool visible=!(workers&&cpu);
        assert(!vmmfs_machine_read_item(&m.node,6,&item));
        assert((item.name[0]!='\0')==visible);
        if (visible) assert(item.inode==1234);
        assert(vmmfs_machine_get_item(&m.node,"stopped",7,&v)==(visible?0:ENOENT));
        if (visible) { assert(v==&m.stopped.vnode && v->holds==2); --v->holds; }
        assert(!held && m.stopped.vnode.holds==1);
    }
    return 0;
}
''')
