"""Exercise committed close paths without reopening admission after a wait."""
import unittest
from test_regress import COMMON, function, run_c


class Closing(unittest.TestCase):
    def test_serial_veto_does_not_block(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_node { struct vmmfs_node *parent; struct token token; bool dead;  struct lock lock;};
struct tty { struct token t_token; unsigned t_state, t_line; };
struct vmmfs_serialroot { struct vmmfs_node node; struct token token; };
struct vmmfs_machine { struct vmmfs_node node; struct token token; void *machine;
    unsigned runtime_references; };
struct vmmfs_serialport { struct vmmfs_node node; struct token token;
    void *entry; bool topology_reference, destroying; unsigned control_count;
    struct tty tty; };
static struct vmmfs_machine machine;
static struct vmmfs_serialroot root;
static struct vmmfs_serialport port;
static struct token *contended;
static unsigned blocks, revoked, retired;
#define PINTERLOCKED 1
#define TS_ISOPEN 1
#define kprintf printf
static bool lwkt_trytoken(struct token *t) {
    if (t == contended) return false;
    ++t->held; return true;
}
static void lwkt_gettoken(struct token *t) {
    /* Blocking acquisitions are permitted only after admission commits. */
    assert(t == &port.token || t == &port.tty.t_token);
    assert(port.node.dead && port.node.token.held == 1);
    assert(root.node.dead || port.topology_reference || port.entry == NULL);
    ++blocks; ++t->held;
}
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static struct vmmfs_machine *vmmfs_serialroot_machine(struct vmmfs_serialroot *r) {
    assert(r == &root); return &machine;
}
static void vmmfs_serialport_revoke(struct vmmfs_serialport *p) {
    assert(p == &port && port.destroying); ++revoked;
}
static void tsleep_interlock(void *p, int flags) { (void)p; (void)flags; }
static int tsleep(void *p, int flags, const char *name, int timeout) {
    assert(p == &port && flags == PINTERLOCKED && timeout == 0);
    (void)name; port.control_count = 0; return 0;
}
static int line_close(struct tty *t, int flags) {
    assert(t == &port.tty && flags == 0); return 0;
}
static struct { int (*l_close)(struct tty *, int); } linesw[] = {{ line_close }};
static void ttyclose(struct tty *t) { t->t_state = 0; }
static void vmmfs_serialport_tty_retire(struct vmmfs_serialport *p) {
    assert(p == &port); ++retired;
}
static int
""" + function("vmmfs_serialport.c", "vmmfs_serialport_deactivate") + r"""
int main(void) {
    port.node.parent = &root.node;
    port.node.dead = true; port.node.token.held = 1;
    port.entry = &port;
    machine.machine = &machine;
    assert(vmmfs_serialport_deactivate(&port.node) == EBUSY);
    machine.machine = NULL;
    contended = &root.token;
    assert(vmmfs_serialport_deactivate(&port.node) == EBUSY);
    contended = &machine.token;
    assert(vmmfs_serialport_deactivate(&port.node) == EBUSY);
    assert(!blocks && !revoked && !retired && !machine.runtime_references);
    assert(root.token.held == 0 && machine.token.held == 0);
    contended = NULL;
    port.control_count = 1; port.tty.t_state = TS_ISOPEN;
    assert(vmmfs_serialport_deactivate(&port.node) == 0);
    assert(machine.runtime_references == 1 && port.topology_reference);
    assert(revoked == 1 && retired == 1 && port.control_count == 0);
    /* Parent closure obeys the parent's admission decision, with no veto. */
    machine.runtime_references = 0; port.topology_reference = false;
    root.node.dead = true; machine.node.dead = true;
    port.entry = NULL; /* Parent detached the registry entry first. */
    assert(vmmfs_serialport_deactivate(&port.node) == 0);
    assert(!machine.runtime_references && revoked == 2 && retired == 2);
    assert(port.node.token.held == 1 && !port.token.held && !port.tty.t_token.held);
}
""")

    def test_vcpu_close_drains_tail_without_veto(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_node { struct token token; bool dead;  struct lock lock;};
struct vmmfs_vcpu { struct vmmfs_node node; struct token token;
    unsigned active_count; void *threads; };
static struct vmmfs_vcpu cpu;
static unsigned waited;
#define lwkt_gettoken(t) (++(t)->held)
#define lwkt_reltoken(t) (--(t)->held)
#define kprintf printf
int tsleep(void *channel, int flags, const char *name, int timeout) {
    assert(channel == &cpu && flags == 0 && timeout == 0);
    assert(cpu.node.dead && cpu.node.token.held == 1 && cpu.token.held == 1);
    (void)name; ++waited; cpu.active_count = 0; cpu.threads = NULL;
    return 0;
}
static int
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_deactivate") + r"""
int main(void) {
    cpu.node.dead = true; cpu.node.token.held = 1;
    cpu.active_count = 1; cpu.threads = &cpu;
    assert(vmmfs_vcpu_deactivate(&cpu.node) == 0);
    assert(waited == 1 && cpu.node.dead && cpu.node.token.held == 1);
    assert(cpu.token.held == 0);
    assert(vmmfs_vcpu_deactivate(&cpu.node) == 0 && waited == 1);
}
""")

    def test_descriptor_close_drains_commit(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_node { struct token token; bool dead;  struct lock lock;};
struct vmmfs_pcislot { struct vmmfs_node node; struct token token; };
struct vmmfs_pcislot_auth { int unused; };
struct vmmfs_pcislot_descriptor { struct vmmfs_node node;
    bool updating, committed; struct vmmfs_pcislot_auth *auth; };
static struct vmmfs_pcislot slot;
static struct vmmfs_pcislot_descriptor descriptor;
static struct vmmfs_pcislot_auth auth;
static unsigned waited, revoked;
#define lwkt_gettoken(t) (++(t)->held)
#define lwkt_reltoken(t) (--(t)->held)
#define kprintf printf
static struct vmmfs_pcislot *vmmfs_pcislot_descriptor_slot(
    struct vmmfs_pcislot_descriptor *d) { assert(d == &descriptor); return &slot; }
int tsleep(void *channel, int flags, const char *name, int timeout) {
    assert(channel == &descriptor && flags == 0 && timeout == 0);
    assert(descriptor.node.dead && descriptor.node.token.held == 1);
    assert(slot.token.held == 1);
    (void)name; ++waited; descriptor.updating = false;
    return 0;
}
static void vmmfs_pcislot_auth_revoke(struct vmmfs_pcislot_auth *a) {
    assert(a == &auth && descriptor.auth == NULL && !descriptor.committed);
    ++revoked;
}
static int
""" + function("vmmfs_pcislot_descriptor.c", "vmmfs_pcislot_descriptor_deactivate") + r"""
int main(void) {
    descriptor.node.dead = true; descriptor.node.token.held = 1;
    descriptor.auth = &auth; descriptor.committed = descriptor.updating = true;
    assert(vmmfs_pcislot_descriptor_deactivate(&descriptor.node) == 0);
    assert(waited == 1 && revoked == 1 && descriptor.node.dead);
    assert(descriptor.node.token.held == 1 && slot.token.held == 0);
}
""")

    def test_launch_cleanup_error_does_not_veto(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_node { bool dead;  struct lock lock;};
struct vmmfs_launch { struct vmmfs_node node; struct token token; };
static int result;
static unsigned revoked;
#define kprintf(...) ((void)0)
static int vmmfs_machine_abort(struct vmmfs_launch *l) {
    assert(l->node.dead && l->token.held == 0); return result;
}
static void vmmfs_launch_revoke(struct vmmfs_launch *l) {
    assert(l->node.dead && l->token.held == 0); ++revoked;
}
static int
""" + function("vmmfs_launch.c", "vmmfs_launch_deactivate") + r"""
int main(void) {
    struct vmmfs_launch launch = { .node = { .dead = true } };
    for (unsigned i = 0; i != 2; ++i) {
        result = i == 0 ? 0 : EIO;
        assert(vmmfs_launch_deactivate(&launch.node) == 0);
        assert(launch.node.dead && launch.token.held == 0);
    }
    assert(revoked == 2);
}
""")
