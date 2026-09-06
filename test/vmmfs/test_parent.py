#!/usr/bin/env python3
"""Run directory teardown functions against deterministic child-removal races."""
import unittest
from test_regress import COMMON, function, run_c

class ParentTeardown(unittest.TestCase):
    def check_parent(self, kind, member, entry_type):
        source = COMMON + r"""
#include <stdlib.h>
struct token { unsigned held; };
struct vmmfs_node { struct token token; };
struct vnode { unsigned refs; };
struct ENTRY { struct vnode *vnode; };
struct registry { struct ENTRY *MEMBER; };
struct ROOT_TYPE { struct vmmfs_node node; struct registry *registry; };
static struct ROOT_TYPE root;
static struct registry registry;
static struct vnode vnode;
static unsigned mode, release_count;
#define M_VMMFS 0
#define RB_ROOT(p) (*(p))
#define RB_REMOVE(type, p, entry) do { assert(*(p) == (entry)); *(p) = NULL; } while (0)
#define RB_INSERT(type, p, entry) do { assert(*(p) == NULL); *(p) = (entry); } while (0)
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
#define vref(v) do { assert((v)->refs); ++(v)->refs; } while (0)
static void vrele(struct vnode *v) { assert(v->refs); --v->refs; }
static void kfree(void *p, int tag) { (void)tag; assert(p); free(p); }
static void RELEASE(struct ROOT_TYPE *r, struct ENTRY *e) {
    assert(r->node.token.held); assert(e->vnode == &vnode); ++release_count;
}
static int vmmfs_vnode_deactivate(struct vnode *v) {
    struct ENTRY *entry = registry.MEMBER;
    assert(v == &vnode && v->refs == 2);
    assert(entry != NULL); /* Veto cannot resurrect an entry already removed. */
    if (mode == 2) {
        registry.MEMBER = NULL;
        vrele(entry->vnode); free(entry);
        return EBUSY;
    }
    return mode == 1 ? EBUSY : 0;
}
static int
FUNCTION
int main(void) {
    root.registry = &registry;
    for (mode = 0; mode != 3; ++mode) {
        registry.MEMBER = malloc(sizeof(*registry.MEMBER));
        registry.MEMBER->vnode = &vnode;
        vnode.refs = 1; release_count = 0;
        int error = DEACTIVATE(&root.node);
        assert(root.node.token.held == 0);
        if (mode == 1) {
            assert(error == EBUSY && registry.MEMBER != NULL);
            assert(vnode.refs == 1 && release_count == 0);
            free(registry.MEMBER); registry.MEMBER = NULL;
            vrele(&vnode);
        } else {
            assert(error == (mode == 2 ? EBUSY : 0));
            assert(registry.MEMBER == NULL && vnode.refs == 0);
            assert(release_count == (mode == 0 ? 1 : 0));
        }
    }
    return 0;
}
"""
        root = "vmmfs_" + kind
        source = source.replace("FUNCTION", function(root + ".c", root + "_deactivate"))
        for token, value in (("ENTRY", entry_type), ("MEMBER", member),
                             ("ROOT_TYPE", root), ("RELEASE", root + "_release_entry"),
                             ("DEACTIVATE", root + "_deactivate")):
            source = source.replace(token, value)
        run_c(source)

    def test_pci_parent_preserves_registry_until_deactivation(self):
        self.check_parent("pciroot", "slots", "vmmfs_pciroot_slot")

    def test_serial_parent_preserves_registry_until_deactivation(self):
        self.check_parent("serialroot", "ports", "vmmfs_serialroot_port")


class MachineTeardown(unittest.TestCase):
    def test_partial_veto_does_not_leave_stopped_alias(self):
        source = COMMON + r"""
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
struct token { unsigned held; };
struct vmmfs_node { struct token token; bool dead; ino_t inode; };
struct mount { void *mnt_data; };
struct vnode { void *v_data; struct mount *v_mount; unsigned refs; };
struct child { struct vmmfs_node node; };
struct vmmfs_stopped { struct vmmfs_node node; };
struct vmmfs_mount { ino_t root_inode; };
struct vmmfs_machine {
    struct vmmfs_node node;
    void *machine;
    bool runtime_releasing, runtime_released;
    unsigned runtime_references;
    struct { struct vmmfs_node node; struct token token;
        unsigned active_count; void *threads; } vcpu;
    struct child id_node, memory, loader, boot, pciroot, serialroot, events;
    /* Retain the old alias in the harness so the pre-fix body can be tested. */
    struct vmmfs_stopped *stopped;
    struct vnode *vnode, *launch_vnode, *id_vnode, *vcpu_vnode, *memory_vnode;
    struct vnode *loader_vnode, *boot_vnode, *stopped_vnode, *pciroot_vnode;
    struct vnode *serialroot_vnode, *events_vnode;
};
struct uio { off_t uio_offset; };
struct vop_readdir_args {
    struct vnode *a_vp; struct uio *a_uio; int *a_ncookies, *a_eofflag;
    void **a_cookies;
};
#define NELEM(a) (sizeof(a)/sizeof((a)[0]))
#define DT_DIR 4
#define DT_REG 8
#define DT_CHR 2
static struct vmmfs_machine machine;
static struct vnode stopped_vnode, pci_vnode;
static void *stopped_page;
static size_t page_size;
static bool veto = true, saw_stopped;
static unsigned stopped_drops, pci_drops;
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static int vmmfs_vnode_deactivate(struct vnode *v) {
    assert(machine.node.token.held == 0 && machine.vcpu.token.held == 0);
    assert(v == &stopped_vnode || v == &pci_vnode);
    return v == &pci_vnode && veto ? EBUSY : 0;
}
static void vrele(struct vnode *v) {
    assert(v->refs == 1 && !machine.node.token.held);
    --v->refs;
    if (v == &stopped_vnode) {
        assert(machine.stopped_vnode == NULL);
        /* Model immediate reclaim: any later object dereference must fault. */
        assert(mprotect(stopped_page, page_size, PROT_NONE) == 0);
        ++stopped_drops;
    } else {
        assert(v == &pci_vnode && machine.pciroot_vnode == NULL);
        ++pci_drops;
    }
    v->v_data = NULL;
}
static int vop_write_dirent(int *error, struct uio *uio, ino_t inode,
    unsigned type, unsigned length, const char *name) {
    (void)uio; (void)type; (void)length;
    *error = 0;
    if (!strcmp(name, "stopped")) {
        assert(inode == 42); saw_stopped = true;
    }
    return 0;
}
static int
""" + function("vmmfs_machine.c", "vmmfs_machine_deactivate") + "\nstatic int\n" + function(
            "vmmfs_machine.c", "vmmfs_machine_readdir") + r"""
int main(void) {
    struct vmmfs_mount state = { .root_inode=1 };
    struct mount mount = { &state };
    struct vnode parent = { .v_data=&machine, .v_mount=&mount, .refs=1 };
    struct uio uio = { .uio_offset=8 };
    struct vop_readdir_args ap = { .a_vp=&parent, .a_uio=&uio };
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    stopped_page = mmap(NULL, page_size, PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANON, -1, 0);
    assert(stopped_page != MAP_FAILED);
    machine.stopped = stopped_page;
    machine.stopped->node.inode = 42;
    stopped_vnode.v_data = stopped_page; stopped_vnode.refs = 1;
    pci_vnode.refs = 1;
    machine.stopped_vnode = &stopped_vnode;
    machine.pciroot_vnode = &pci_vnode; machine.vnode = &parent;
    assert(vmmfs_machine_readdir(&ap) == 0 && saw_stopped);
    machine.node.dead = true; /* Generic deactivate closes admission first. */
    assert(vmmfs_machine_deactivate(&machine.node) == EBUSY);
    assert(stopped_drops == 1 && pci_drops == 0 && machine.vnode == &parent);
    assert(machine.stopped_vnode == NULL && machine.pciroot_vnode == &pci_vnode);
    machine.node.dead = false; /* Generic wrapper restores the gate on veto. */
    saw_stopped = false; uio.uio_offset = 8;
    assert(vmmfs_machine_readdir(&ap) == 0 && !saw_stopped);
    veto = false; machine.node.dead = true;
    assert(vmmfs_machine_deactivate(&machine.node) == 0);
    assert(stopped_drops == 1 && pci_drops == 1 && machine.vnode == NULL);
    assert(!machine.node.token.held && !machine.vcpu.token.held);
    assert(munmap(stopped_page, page_size) == 0);
    return 0;
}
"""
        run_c(source)

if __name__ == "__main__":
    unittest.main(verbosity=2)
