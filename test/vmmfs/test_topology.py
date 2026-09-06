#!/usr/bin/env python3
"""Production slot removal must reserve boot admission through registry removal."""
import unittest
from test_regress import COMMON, function, run_c

class Topology(unittest.TestCase):

    def test_bdf_fields_do_not_alias(self):
        run_c(COMMON + r"""
#define bcmp memcmp
#define ksnprintf snprintf
static int
""" + function("vmmfs_pciroot.c", "vmmfs_pciroot_parse_hex") + r"""
static int
""" + function("vmmfs_pciroot.c", "vmmfs_pciroot_parse_bdf") + r"""
static void
""" + function("vmmfs_pciroot.c", "vmmfs_pciroot_format_bdf") + r"""
int main(void) {
    char name[32], formatted[32];
    uint16_t bdf;
    for (unsigned bus = 0; bus < 256; ++bus) {
        for (unsigned device = 0; device < 32; ++device) {
            for (unsigned function = 0; function < 16; ++function) {
                snprintf(name, sizeof(name), "0000:%02x:%02x.%x",
                         bus, device, function);
                int error = vmmfs_pciroot_parse_bdf(name, strlen(name), &bdf);
                if (function >= 8 || (bus == 0 && device == 0 && function == 0)) {
                    assert(error == EINVAL);
                } else {
                    assert(error == 0);
                    assert(bdf == ((bus << 8) | (device << 3) | function));
                    vmmfs_pciroot_format_bdf(bdf, formatted, sizeof(formatted));
                    assert(strcmp(name, formatted) == 0);
                }
            }
        }
    }
    const char *invalid[] = {
        "0001:00:01.0", "0000:00:20.0", "0000:00:ff.0",
        "0000:00:01.00", "0000:00:01.g", "0000:00.01"
    };
    for (unsigned index = 0; index < sizeof(invalid)/sizeof(invalid[0]); ++index)
        assert(vmmfs_pciroot_parse_bdf(invalid[index],
                                     strlen(invalid[index]), &bdf) == EINVAL);
    return 0;
}
""")

    def test_slot_removal_reservation(self):
        run_c(COMMON + r"""
#include <stdlib.h>
struct token { unsigned held; };
struct vnode { void *v_data; };
struct vmmfs_node { struct vmmfs_node *parent; struct token token; bool dead;
    int (*get_item)(struct vmmfs_node *, const char *, size_t, struct vnode **); };
struct vmmfs_machine { struct vmmfs_node node; void *machine; unsigned runtime_references; };
struct vmmfs_pcislot { struct vmmfs_node node; unsigned bdf; bool topology_reference; void *entry;
    struct vnode *descriptor_vnode, *config_vnode, *events_vnode; };
struct vmmfs_pciroot_slot { struct vnode *vnode; struct vmmfs_pcislot *slot; };
struct registry { struct vmmfs_pciroot_slot *slots; };
struct vmmfs_pciroot { struct vmmfs_node node; struct registry *registry; };
static struct vmmfs_machine machine;
static struct vmmfs_pciroot root;
static struct vmmfs_pcislot slot;
static struct vnode vnode = { &slot }, child;
static struct registry registry;
static unsigned closed;
static struct token *contended;
static bool registered;
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
#define ksnprintf snprintf
#define M_VMMFS 0
#define RB_REMOVE(type, head, entry) do { assert(*(head) == (entry)); *(head) = NULL; } while (0)
static void lwkt_gettoken(struct token *t) { ++t->held; }
static bool lwkt_trytoken(struct token *t) {
    if (t == contended) return false;
    ++t->held; return true;
}
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static struct vmmfs_pciroot *vmmfs_pcislot_pciroot(struct vmmfs_pcislot *s) { (void)s; return &root; }
static struct vmmfs_machine *vmmfs_pciroot_machine(struct vmmfs_pciroot *r) { (void)r; return &machine; }
static void vrele(struct vnode *v) { (void)v; }
static void kfree(void *p, int tag) { (void)tag; free(p); }
static void vmmfs_pcislot_power_off(struct vmmfs_pcislot *s) {
    assert(s->node.dead && s->node.token.held == 1);
    assert(machine.runtime_references == (registered && !root.node.dead ? 1U : 0U));
}
static int vmmfs_vnode_deactivate(struct vnode *v) {
    assert(v == &child && slot.node.token.held == 1);
    ++closed; return 0;
}
static int vmmfs_pciroot_parse_bdf(const char *n, size_t l, uint16_t *b) {
    (void)n; (void)l; *b = 8; return 0;
}
static struct vmmfs_pciroot_slot *vmmfs_pciroot_entry_find_locked(struct vmmfs_pciroot *r, uint16_t b) {
    (void)b; return r->registry->slots;
}
static int
""" + function("vmmfs_pcislot.c", "vmmfs_pcislot_deactivate") + "\nstatic void\n" +
              function("vmmfs_pciroot.c", "vmmfs_pciroot_release_entry") + "\nstatic void\n" + function("vmmfs_pciroot.c", "vmmfs_pciroot_remove_item") + r"""
int main(void) {
    slot.node.parent = &root.node; slot.bdf = 8;
    slot.node.token.held = 1; slot.node.dead = true;
    slot.descriptor_vnode = slot.config_vnode = slot.events_vnode = &child;
    root.registry = &registry;
    registered = true; slot.entry = &slot; machine.machine = &machine;
    assert(vmmfs_pcislot_deactivate(&slot.node) == EBUSY);
    assert(machine.runtime_references == 0);
    machine.machine = NULL; machine.node.dead = true;
    assert(vmmfs_pcislot_deactivate(&slot.node) == EBUSY);
    assert(machine.runtime_references == 0);
    root.node.dead = true;
    assert(vmmfs_pcislot_deactivate(&slot.node) == 0 && closed == 3);
    assert(machine.runtime_references == 0 && !slot.topology_reference);
    root.node.dead = false; machine.node.dead = false;
    contended = &root.node.token;
    assert(vmmfs_pcislot_deactivate(&slot.node) == EBUSY);
    assert(machine.runtime_references == 0 && closed == 3);
    contended = &machine.node.token;
    assert(vmmfs_pcislot_deactivate(&slot.node) == EBUSY);
    assert(machine.runtime_references == 0 && closed == 3);
    assert(root.node.token.held == 0 && machine.node.token.held == 0);
    contended = NULL;
    assert(vmmfs_pcislot_deactivate(&slot.node) == 0);
    assert(machine.runtime_references == 1);
    registry.slots = malloc(sizeof(*registry.slots));
    registry.slots->vnode = &vnode; registry.slots->slot = &slot;
    vmmfs_pciroot_remove_item(&root.node, "0000:00:01.0", 12);
    assert(machine.runtime_references == 0 && registry.slots == NULL);
    registered = false; slot.entry = NULL; machine.machine = &machine;
    assert(vmmfs_pcislot_deactivate(&slot.node) == 0);
    assert(machine.runtime_references == 0);
    registry.slots = malloc(sizeof(*registry.slots));
    registry.slots->vnode = &vnode; registry.slots->slot = &slot;
    vmmfs_pciroot_remove_item(&root.node, "0000:00:01.0", 12);
    assert(machine.runtime_references == 0);
    return 0;
}
""")


    def test_serial_reservation_release(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_node { struct token token; };
struct vmmfs_machine { struct vmmfs_node node; unsigned runtime_references; };
struct vmmfs_serialport { bool topology_reference; void *entry; };
struct vmmfs_serialroot_port { struct vmmfs_serialport *port; };
struct vmmfs_serialroot { struct vmmfs_node node; };
static struct vmmfs_machine machine;
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static struct vmmfs_machine *vmmfs_serialroot_machine(struct vmmfs_serialroot *r) {
    (void)r; return &machine;
}
static void
""" + function("vmmfs_serialroot.c", "vmmfs_serialroot_release_entry") + r"""
int main(void) {
    struct vmmfs_serialroot root = {0};
    struct vmmfs_serialport port = {0};
    struct vmmfs_serialroot_port entry = { &port };
    vmmfs_serialroot_release_entry(&root, &entry);
    assert(machine.runtime_references == 0);
    machine.runtime_references = 2;
    port.topology_reference = true;
    vmmfs_serialroot_release_entry(&root, &entry);
    assert(machine.runtime_references == 1 && !port.topology_reference);
    vmmfs_serialroot_release_entry(&root, &entry);
    assert(machine.runtime_references == 1);
    return 0;
}
""")

if __name__ == "__main__":

    unittest.main(verbosity=2)
