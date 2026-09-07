"""Machine lookup locks only the mutable stopped node."""
import unittest
from test_regress import function, run_c


class GetItem(unittest.TestCase):
    def test_token_scope_and_handoff(self):
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
struct vnode { int holds; int dynamic; };
struct node { struct vnode *vnode; };
struct child { struct node node; };
struct vmmfs_machine {
    int token;
    struct child id_node, vcpu, memory, loader, boot, events, pciroot, serialroot;
    struct child *stopped;
    void *machine, *launch;
};
static int held, locks;
static void lwkt_gettoken(int *token) {
    (void)token; assert(!held); held = 1; ++locks;
}
static void lwkt_reltoken(int *token) {
    (void)token; assert(held); held = 0;
}
static void vhold(struct vnode *vnode) {
    assert(held == vnode->dynamic); ++vnode->holds;
}
static int
''' + function('vmmfs_machine.c', 'vmmfs_machine_get_item') + r'''
int main(void) {
    struct vnode fixed = {0}, stopped = { .dynamic = 1 }, *result;
    struct child child = { .node.vnode = &stopped };
    struct vmmfs_machine m = {0};
    struct child *children[] = { &m.id_node, &m.vcpu, &m.memory, &m.loader,
        &m.boot, &m.events, &m.pciroot, &m.serialroot };
    const char *names[] = { "id", "vcpu", "mem", "loader", "boot", "events", "pci", "serial" };
    for (unsigned i = 0; i < 8; ++i) {
        children[i]->node.vnode = &fixed;
        assert(vmmfs_machine_get_item(&m, names[i], strlen(names[i]), &result) == 0);
        assert(result == &fixed && locks == 0 && fixed.holds == (int)i + 1);
    }
    assert(vmmfs_machine_get_item(&m, "unknown", 7, &result) == ENOENT);
    assert(locks == 0);
    for (int present = 0; present < 2; ++present)
    for (int running = 0; running < 2; ++running)
    for (int loading = 0; loading < 2; ++loading) {
        m.stopped = present ? &child : NULL;
        m.machine = running ? &m : NULL; m.launch = loading ? &m : NULL;
        int visible = present && (!running || loading), before = stopped.holds;
        assert(vmmfs_machine_get_item(&m, "stopped", 7, &result) == (visible ? 0 : ENOENT));
        assert(!held && stopped.holds == before + visible);
        if (visible) assert(result == &stopped);
    }
    assert(locks == 8); return 0;
}
''')
