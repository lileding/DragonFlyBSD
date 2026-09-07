#!/usr/bin/env python3
"""Focused VMMFS regressions using production C bodies and mocked dependencies."""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "sys/vfs/vmmfs"


def balanced(text, start, left="{", right="}"):
    depth = 0
    for pos in range(start, len(text)):
        if text[pos] == left:
            depth += 1
        elif text[pos] == right:
            depth -= 1
            if depth == 0:
                return text[start:pos + 1]
    raise ValueError("unbalanced source")


def function(filename, name):
    text = (SOURCE / filename).read_text()
    start = text.index("\n" + name + "(") + 1
    brace = text.index("{", start)
    return text[start:brace] + balanced(text, brace)


def run_c(source):
    with tempfile.TemporaryDirectory(prefix="vmmfs-regress-") as directory:
        path = pathlib.Path(directory)
        (path / "test.c").write_text(source)
        compiled = subprocess.run(
            ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
             str(path / "test.c"), "-o", str(path / "test")],
            capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        result = subprocess.run([str(path / "test")], cwd=path,
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f"test process exited {result.returncode}\n" + result.stdout + result.stderr)


COMMON = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define KKASSERT(x) assert(x)
#define panic(...) do { fprintf(stderr, __VA_ARGS__); assert(0); } while (0)
#ifndef VMMFS_TEST_CUSTOM_LOCK
#define LK_SHARED 1
#define LK_EXCLUSIVE 2
#define LK_RELEASE 6
#define LK_RETRY 0x20000
struct lock { unsigned held; };
#define lockstatus(lock, thread) vmmfs_test_lockstatus(lock)
static inline int vmmfs_test_lockstatus(struct lock *lock) { (void)lock; return 0; }
static inline void lockinit(struct lock *lock, const char *name, int timeout, int flags) {
    (void)name; (void)timeout; (void)flags; lock->held = 0;
}
static inline void lockuninit(struct lock *lock) { assert(!lock->held); }
static inline int lockmgr(struct lock *lock, int flags) {
    if (flags == LK_RELEASE) { assert(lock->held); --lock->held; }
    else ++lock->held;
    return 0;
}
#endif
"""


def work_macros():
    header = (SOURCE / "vmmfs_node.h").read_text()
    begin = header.index("#define VMMFS_WORK(")
    end = header.index("/* Object references", begin)
    return header[begin:end]


COMMON += work_macros()


class Regressions(unittest.TestCase):

    def test_control_properties_preserve_commit_and_admission(self):
        from test_work import call_macro
        run_c(COMMON + call_macro() + r"""
#include <sys/types.h>
#define PAGE_SIZE 4096
#define ksnprintf snprintf
#define bcopy(s, d, n) memcpy(d, s, n)
struct token { unsigned held; };
struct vmmfs_node { struct vnode *vnode; bool dead; off_t size;  struct lock lock;};
struct vmmfs_vcpu { struct vmmfs_node node; uint32_t count; };
struct vmmfs_memory { struct vmmfs_node node; uint64_t size; };
struct vmmfs_loader { struct vmmfs_node node; char script[PAGE_SIZE]; };
struct vmmfs_machine {
    struct vmmfs_node node; struct token token;
    void *machine;
    struct { struct vmmfs_node node; } boot;
};
static struct vmmfs_machine machine;
static void lwkt_gettoken(struct token *token) {
    assert(token == &machine.token);
    ++token->held;
}
static void lwkt_reltoken(struct token *token) {
    assert(token->held); --token->held;
}
static struct vmmfs_machine *vmmfs_vcpu_machine(struct vmmfs_vcpu *vcpu) {
    assert(vcpu->node.lock.held == 1); return &machine;
}
static struct vmmfs_machine *vmmfs_memory_machine(struct vmmfs_memory *memory) {
    assert(memory->node.lock.held == 1); return &machine;
}
static struct vmmfs_machine *vmmfs_loader_machine(struct vmmfs_loader *loader) {
    assert(loader->node.lock.held == 1); return &machine;
}
off_t
""" + function("vmmfs_node.c", "vmmfs_node_decimal_size") + """
static int
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_load") + """
static int
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_store") + """
static int
""" + function("vmmfs_memory.c", "vmmfs_memory_load") + """
static int
""" + function("vmmfs_memory.c", "vmmfs_memory_store") + """
static int
""" + function("vmmfs_loader.c", "vmmfs_loader_load") + """
static int
""" + function("vmmfs_loader.c", "vmmfs_loader_store") + r"""
int main(void) {
    struct vmmfs_vcpu vcpu = { .count = 4 };
    struct vmmfs_memory memory = { 0 };
    struct vmmfs_loader loader = { 0 };
    char buffer[PAGE_SIZE + 1];
    size_t length;
    assert(VMMFS_WORK(&memory, vmmfs_memory_store((void *)&memory, "4096\n", 5)) == 0);
    assert(memory.size == 4096 && memory.node.size == 5);
    assert(machine.boot.node.size == 4096 && !machine.token.held);
    assert(VMMFS_WORK(&memory, vmmfs_memory_load((void *)&memory, buffer, sizeof(buffer), &length)) == 0);
    assert(length == 5 && memcmp(buffer, "4096\n", 5) == 0);
    const char *invalid[] = { "", "\n", "-1", " 1", "1x", "1\n\n",
        "18446744073709551616" };
    for (unsigned index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        assert(VMMFS_WORK(&memory, vmmfs_memory_store((void *)&memory, invalid[index],
            strlen(invalid[index]))) == (index == 6 ? ERANGE : EINVAL));
        assert(memory.size == 4096 && memory.node.size == 5 &&
            machine.boot.node.size == 4096 && !machine.token.held);
    }
    assert(VMMFS_WORK(&memory, vmmfs_memory_load((void *)&memory, buffer, 5, &length)) == EOVERFLOW);
    machine.machine = &machine;
    assert(VMMFS_WORK(&memory, vmmfs_memory_store((void *)&memory, "8192", 4)) == EBUSY);
    assert(memory.size == 4096 && machine.boot.node.size == 4096);
    machine.machine = NULL;
    memory.node.dead = true;
    assert(VMMFS_WORK(&memory, vmmfs_memory_store((void *)&memory, "8192", 4)) == ENOENT);
    assert(VMMFS_WORK(&memory, vmmfs_memory_load((void *)&memory, buffer, sizeof(buffer), &length)) == ENOENT);

    assert(VMMFS_WORK(&loader, vmmfs_loader_store((void *)&loader, "exec loader\n", 12)) == 0);
    assert(strcmp(loader.script, "exec loader") == 0 && loader.node.size == 12);
    assert(VMMFS_WORK(&loader, vmmfs_loader_load((void *)&loader, buffer, sizeof(buffer), &length)) == 0);
    assert(length == 12 && memcmp(buffer, "exec loader\n", 12) == 0);
    assert(VMMFS_WORK(&loader, vmmfs_loader_store((void *)&loader, "", 0)) == EINVAL);
    assert(VMMFS_WORK(&loader, vmmfs_loader_store((void *)&loader, "\n", 1)) == ENAMETOOLONG);
    memset(buffer, 'x', PAGE_SIZE);
    assert(VMMFS_WORK(&loader, vmmfs_loader_store((void *)&loader, buffer, PAGE_SIZE)) == ENAMETOOLONG);
    assert(strcmp(loader.script, "exec loader") == 0 && loader.node.size == 12);
    machine.machine = &machine;
    assert(VMMFS_WORK(&loader, vmmfs_loader_store((void *)&loader, "other", 5)) == EBUSY);
    assert(strcmp(loader.script, "exec loader") == 0 && loader.node.size == 12);
    machine.machine = NULL;
    assert(VMMFS_WORK(&loader, vmmfs_loader_store((void *)&loader, buffer, PAGE_SIZE - 1)) == 0);
    assert(loader.script[PAGE_SIZE - 1] == 0 && loader.node.size == PAGE_SIZE);
    assert(VMMFS_WORK(&loader, vmmfs_loader_load((void *)&loader, buffer, PAGE_SIZE, &length)) == EOVERFLOW);
    assert(VMMFS_WORK(&loader, vmmfs_loader_load((void *)&loader, buffer, sizeof(buffer), &length)) == 0);
    assert(length == PAGE_SIZE && buffer[PAGE_SIZE - 1] == '\n');
    loader.node.dead = true;
    assert(VMMFS_WORK(&loader, vmmfs_loader_store((void *)&loader, "other", 5)) == ENOENT);
    assert(VMMFS_WORK(&loader, vmmfs_loader_load((void *)&loader, buffer, sizeof(buffer), &length)) == ENOENT);
    assert(VMMFS_WORK(&vcpu, vmmfs_vcpu_store((void *)&vcpu, "8", 1)) == 0);
    assert(vcpu.count == 8 && vcpu.node.size == 2);
    assert(VMMFS_WORK(&vcpu, vmmfs_vcpu_load((void *)&vcpu, buffer, sizeof(buffer), &length)) == 0);
    assert(length == 2 && memcmp(buffer, "8\n", 2) == 0);
    assert(!machine.token.held);
    return 0;
}
""")

    def test_config_copy_failure_retries_only_the_same_live_request(self):
        run_c(COMMON + r"""
#include <sys/queue.h>
typedef char *caddr_t;
struct token { unsigned held; };
struct vmmfs_pci_config_request { uint64_t generation, sequence, value; };
struct vmmfs_pcislot_config_request {
    TAILQ_ENTRY(vmmfs_pcislot_config_request) entry;
    struct vmmfs_pci_config_request request;
    bool delivered, completed;
};
TAILQ_HEAD(requests, vmmfs_pcislot_config_request);
struct file;
struct vmmfs_node { struct vnode *vnode; struct token token; bool dead; struct lock lock; };
struct vmmfs_pcislot_config {
    struct vmmfs_node node;
    struct token token;
    struct { int ki_note; } kq;
    bool closed;
    void *responder;
    struct requests requests;
};
struct vnode { void *v_data; };
struct uio { size_t uio_resid; };
struct vop_read_args {
    struct vnode *a_vp; struct uio *a_uio; void *a_fp; int a_ioflag;
};
#define IO_NDELAY 1
#define PINTERLOCKED 2
#define PCATCH 4
static struct vmmfs_pcislot_config config;
static struct vmmfs_pcislot_config_request request, next;
static unsigned mode, copies, notifications, sleeps;
static int copy_error;
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
#define KNOTE(list, hint) ((void)(list), (void)(hint))
static void tsleep_interlock(void *p, int flags) {
    assert(p == &config && flags == PCATCH);
    assert(config.node.lock.held && config.token.held);
}
static int tsleep(void *p, int flags, const char *name, int timeout) {
    (void)name;
    assert(p == &config && flags == (PINTERLOCKED | PCATCH) && !timeout);
    assert(!config.node.lock.held && !config.token.held);
    ++sleeps;
    if (mode == 10) { config.node.dead = config.closed = true; return 0; }
    if (mode == 11 && sleeps == 1) return 0;
    return EINTR;
}
static void
vmmfs_pcislot_config_wake_next(struct vmmfs_pcislot_config *c) {
    assert(c == &config && !c->token.held && !c->node.token.held);
    ++notifications;
}
static int uiomove(caddr_t data, size_t size, struct uio *uio) {
    struct vmmfs_pci_config_request *record = (void *)data;
    assert(!config.token.held && !config.node.lock.held);
    assert(size == sizeof(*record) && size == uio->uio_resid);
    assert(record->generation == 11 && record->sequence == 22);
    ++copies;
    if (mode == 2 || mode == 3 || mode == 7) {
        TAILQ_REMOVE(&config.requests, &request, entry);
        memset(&request, 0xa5, sizeof(request));
        if (mode != 7) {
            next.request.generation = mode == 3 ? 12 : 11;
            next.request.sequence = mode == 3 ? 22 : 23;
            next.delivered = true;
            TAILQ_INSERT_TAIL(&config.requests, &next, entry);
        }
    } else if (mode == 4) request.completed = true;
    else if (mode == 5) config.node.dead = config.closed = true;
    else if (mode == 6) config.responder = &next;
    return copy_error;
}

#define curthread NULL
#define crit_enter() ((void)0)
#define crit_exit() ((void)0)
static void tsleep_remove(void *thread) { (void)thread; }
static int
""" + (function('vmmfs_pcislot_config.c', 'vmmfs_pcislot_config_receive') + '\nstatic void\n' + function('vmmfs_pcislot_config.c', 'vmmfs_pcislot_config_redeliver') + '\nstatic int\n' + function('vmmfs_pcislot_config.c', 'vmmfs_pcislot_config_read')) + r"""
int main(void) {
    struct vnode vnode = { &config };
    struct uio uio = { sizeof(struct vmmfs_pci_config_request) };
    struct vop_read_args args = { &vnode, &uio, &config, IO_NDELAY };
    for (mode = 0; mode < 12; ++mode) {
        memset(&config, 0, sizeof(config));
        memset(&request, 0, sizeof(request)); memset(&next, 0, sizeof(next));
        TAILQ_INIT(&config.requests);
        request.request.generation = 11; request.request.sequence = 22;
        if (mode < 8) TAILQ_INSERT_TAIL(&config.requests, &request, entry);
        config.responder = &config;
        copies = notifications = sleeps = 0;
        args.a_ioflag = mode < 8 || mode == 9 ? IO_NDELAY : 0;
        if (mode >= 8) {
            int expected = mode == 9 ? EAGAIN : mode == 10 ? ENOENT : EINTR;
            assert(vmmfs_pcislot_config_read(&args) == expected);
            assert(!config.token.held && !config.node.token.held);
            assert(!copies && !notifications && TAILQ_EMPTY(&config.requests));
            assert(config.responder == &config);
            assert(sleeps == (mode == 9 ? 0 : mode == 11 ? 2 : 1));
            continue;
        }
        copy_error = mode == 0 ? 0 : EFAULT;
        assert(vmmfs_pcislot_config_read(&args) == copy_error);
        assert(copies == 1 && !config.token.held && !config.node.token.held);
        if (mode == 1) {
            assert(!request.delivered && notifications == 1);
            copy_error = 0;
            assert(vmmfs_pcislot_config_read(&args) == 0);
            assert(request.delivered && copies == 2);
        } else {
            assert(notifications == 0);
            if (mode == 2 || mode == 3) assert(next.delivered);
            else if (mode == 7) assert(TAILQ_EMPTY(&config.requests));
            else assert(request.delivered);
        }
    }
    return 0;
}
""")


    def test_parent_views_have_consumers(self):
        header = (SOURCE / "vmmfs_parent.h").read_text()
        source = "\n".join(path.read_text() for path in SOURCE.glob("*.c"))
        for name in re.findall(r"^#define (vmmfs_\w+)\(", header, re.M):
            with self.subTest(name=name):
                self.assertTrue(re.search(r"\b" + name + r"\b", source),
                                f"{name} has no C consumer")

    def test_empty_node_write(self):
        run_c(COMMON + r"""
#include <stdlib.h>
typedef long off_t;
struct token { bool held; };
struct vmmfs_node { struct vnode *vnode;
    struct token token; bool dead; size_t store_limit;
    int (*store)(struct vmmfs_node *, const char *, size_t);
 struct lock lock;};
struct vnode { void *v_data; };
struct uio { off_t uio_offset; size_t uio_resid; };
struct vop_write_args { struct vnode *a_vp; struct uio *a_uio; };
#define M_VMMFS 0
#define M_WAITOK 0
static unsigned stored, freed;
static int store_error;
static bool close_on_copy;
static struct vmmfs_node node;
static void *kmalloc(size_t n, int tag, int flags) {
    (void)tag; (void)flags; assert(n != 0); return malloc(n);
}
static void kfree(void *p, int tag) { (void)tag; assert(p != NULL); ++freed; free(p); }
static int uiomove(void *p, size_t n, struct uio *u) {
    assert(n == u->uio_resid && !node.token.held); memset(p, 'x', n);
    if (close_on_copy) node.dead = true;
    return 0;
}
static int store(struct vmmfs_node *n, const char *p, size_t length) {
    assert(n == &node && n->lock.held);
    assert((length == 0) == (p == NULL)); ++stored; return store_error;
}
int
""" + function("vmmfs_node.c", "vmmfs_node_write") + r"""
int main(void) {
    struct vnode vnode = { &node };
    struct uio uio = { 0, 0 };
    struct vop_write_args args = { &vnode, &uio };
    node.store = store; node.store_limit = 4096;
    assert(vmmfs_node_write(&args) == 0 && stored == 1 && freed == 0);
    store_error = EINVAL;
    assert(vmmfs_node_write(&args) == EINVAL && stored == 2 && freed == 0);
    uio.uio_resid = 1; store_error = 0;
    assert(vmmfs_node_write(&args) == 0 && stored == 3 && freed == 1);
    close_on_copy = true;
    assert(vmmfs_node_write(&args) == ENOENT && stored == 3 && freed == 2);
    assert(!node.token.held);
}
""")

    def test_resource_interrupt_drain(self):
        run_c(COMMON + r"""
typedef char *caddr_t;
typedef void *vmm_machine_t;
struct token { unsigned held; };
struct vmmfs_node { struct vnode *vnode; struct token token; bool dead;  struct lock lock;};
struct slot { struct { unsigned intx_gsi; } type0; };
struct vmmfs_vcpu { struct token token; bool stop_requested, reset_requested; };
struct owner { struct vmmfs_vcpu vcpu; };
static struct owner owner;
struct vmmfs_pcislot_resource;
struct vmmfs_pcislot_resources {
    struct vmmfs_node node; struct token token;
    vmm_machine_t machine;
    unsigned interrupt_users;
    bool powered, destroying;
    size_t count;
    struct vmmfs_pcislot_resource *items;
};
struct vmmfs_pcislot_resource {
    struct vmmfs_node node;
    struct token token;
    struct vmmfs_pcislot_resources *resources;
    bool revoked, mapped, intx_asserted;
    unsigned kind;
};
struct vmmfs_pci_intx { uint8_t asserted, reserved[7]; };
struct vmmfs_pci_interrupt { uint64_t reserved; };
struct vnode { void *v_data; };
struct uio { size_t uio_resid; };
struct vop_write_args { struct vnode *a_vp; struct uio *a_uio; };
#define VMMFS_PCISLOT_RESOURCE_INTX 1
#define VMMFS_PCISLOT_RESOURCE_MSI 2
#define VMMFS_PCISLOT_RESOURCE_MSIX 3
#define PINTERLOCKED 1
#define bcmp memcmp
static struct slot slot;
static struct vmmfs_pcislot_resources resources;
static struct vmmfs_pcislot_resource resource;
static unsigned delivered, removed, slept, woken;
static bool close_during_copy, reset_during_delivery;
static int copy_error, delivery_error;
#define vmmfs_pcislot_resource_resources(r) ((r)->resources)
#define vmmfs_pcislot_resources_slot(r) ((void)(r), &slot)
#define vmmfs_pcislot_pciroot(s) (s)
#define vmmfs_pciroot_machine(s) ((void)(s), &owner)
void lwkt_gettoken(struct token *t) { assert(!t->held); ++t->held; }
void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static bool vmmfs_pcislot_resource_enabled(struct vmmfs_pcislot_resource *r) {
    return r != NULL && !r->node.dead && !r->revoked &&
        resources.powered && !resources.destroying;
}
static int uiomove(caddr_t p, size_t n, struct uio *u) {
    assert(!resource.node.token.held && !resources.token.held);
    assert(resources.interrupt_users == 0 && n == u->uio_resid);
    memset(p, 0, n);
    if (resource.kind == VMMFS_PCISLOT_RESOURCE_INTX) p[0] = 1;
    if (close_during_copy) { resource.node.dead = true; resources.machine = NULL; }
    return copy_error;
}
static int dispatch(vmm_machine_t machine) {
    assert(machine == &slot && resources.interrupt_users == 1);
    assert(!resource.node.token.held && !resources.token.held);
    ++delivered;
    if (reset_during_delivery) owner.vcpu.reset_requested = true;
    return delivery_error;
}
static int vmm_machine_set_irq(vmm_machine_t m, unsigned gsi, bool level) {
    assert(gsi == slot.type0.intx_gsi && level); return dispatch(m);
}
#define vmmfs_pcislot_resource_raise_msi(r, m) dispatch(m)
#define vmmfs_pcislot_resource_raise_msix(r, m) dispatch(m)
void wakeup(void *p) { assert(p == &resources); ++woken; }
void tsleep_interlock(void *p, int flags) {
    assert(p == &resources && flags == 0 && resources.token.held);
}
int tsleep(void *p, int flags, const char *name, int timeout) {
    assert(p == &resources && flags == PINTERLOCKED && timeout == 0);
    assert(!resources.token.held && resources.machine == NULL);
    assert(resources.interrupt_users == 1); (void)name;
    ++slept; resources.interrupt_users = 0; return 0;
}
static void remove_traps(struct vmmfs_pcislot_resource *r, vmm_machine_t machine) {
    assert(machine == &slot);
    assert(r == &resource && resources.machine == NULL);
    assert(resources.interrupt_users == 0); ++removed;
}
#define vmmfs_pcislot_resource_remove_traps(r, m) remove_traps(r, m)
static int
""" + (function('vmmfs_pcislot_resource.c', 'vmmfs_pcislot_resource_raise') + '\nstatic int\n' + function('vmmfs_pcislot_resource.c', 'vmmfs_pcislot_resource_write')) + r"""
void
""" + function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resources_unbind") + r"""
int main(void) {
    struct vnode vnode = { &resource };
    struct uio uio = { sizeof(struct vmmfs_pci_interrupt) };
    struct vop_write_args args = { &vnode, &uio };
    resources.machine = &slot; resources.powered = true;
    resources.items = &resource; resources.count = 1;
    resource.resources = &resources; resource.mapped = true;
    slot.type0.intx_gsi = 17;
    for (unsigned kind = 1; kind <= 3; ++kind) {
        resource.kind = kind;
        assert(vmmfs_pcislot_resource_write(&args) == 0);
        assert(resources.interrupt_users == 0);
    }
    assert(delivered == 3 && woken == 3);
    owner.vcpu.reset_requested = true;
    assert(vmmfs_pcislot_resource_write(&args) == 0 && delivered == 3);
    owner.vcpu.reset_requested = false;
    owner.vcpu.stop_requested = true;
    assert(vmmfs_pcislot_resource_write(&args) == 0 && delivered == 3);
    owner.vcpu.stop_requested = false;
    delivery_error = EIO;
    assert(vmmfs_pcislot_resource_write(&args) == EIO);
    assert(resources.interrupt_users == 0 && woken == 4);
    copy_error = EFAULT;
    assert(vmmfs_pcislot_resource_write(&args) == EFAULT && delivered == 4);
    copy_error = 0; close_during_copy = true;
    assert(vmmfs_pcislot_resource_write(&args) == ENOENT && delivered == 4);
    assert(resources.interrupt_users == 0);
    close_during_copy = false; resource.node.dead = false;
    /* A reset has no destination; old completions must not reach its successor. */
    assert(vmmfs_pcislot_resource_write(&args) == 0 && delivered == 4);
    resources.machine = &slot;
    delivery_error = ENOENT; reset_during_delivery = true;
    assert(vmmfs_pcislot_resource_write(&args) == 0 && delivered == 5);
    owner.vcpu.reset_requested = false; reset_during_delivery = false;
    assert(vmmfs_pcislot_resource_write(&args) == ENOENT && delivered == 6);
    resources.interrupt_users = 1;
    vmmfs_pcislot_resources_unbind(&resources);
    assert(slept == 1 && removed == 1 && resources.machine == NULL);
    assert(!resource.node.token.held && !resources.token.held);
}
""")

    def test_resource_read_and_kqueue_admission(self):
        run_c(COMMON + r"""
typedef char *caddr_t;
struct token { unsigned held; };
struct vmmfs_node { struct vnode *vnode; struct token token; bool dead;  struct lock lock;};
struct vmmfs_pci_kick { uint64_t value; };
struct vmmfs_pcislot_resource {
    struct vmmfs_node node;
    struct token token;
    unsigned kind;
    bool revoked, kick_pending;
    struct vmmfs_pci_kick kick;
    struct { unsigned ki_note; } read_kq;
};
struct vnode { void *v_data; };
struct uio { size_t uio_resid; };
struct vop_read_args { struct vnode *a_vp; struct uio *a_uio; int a_ioflag; };
struct knote { int kn_filter; void *kn_fop; caddr_t kn_hook; };
struct vop_kqfilter_args { struct vnode *a_vp; struct knote *a_kn; };
#define VMMFS_PCISLOT_RESOURCE_KICK 1
#define IO_NDELAY 1
#define PCATCH 2
#define PINTERLOCKED 4
#define EVFILT_READ 1
static unsigned vmmfs_pcislot_resource_read_filterops;
static struct vmmfs_pcislot_resource resource;
static unsigned copied, inserted, sleep_mode;
static bool interlocked;
#define curthread NULL
#define crit_enter() ((void)0)
#define crit_exit() ((void)0)
static void tsleep_remove(void *thread) { (void)thread; interlocked = false; }

static void lwkt_gettoken(struct token *t) { assert(!t->held); ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static bool vmmfs_pcislot_resource_enabled(struct vmmfs_pcislot_resource *r) {
    if (r == NULL) return false;
    assert(r->node.lock.held && r->token.held);
    return !r->node.dead && !r->revoked;
}
static void knote_insert(unsigned *notes, struct knote *kn) {
    assert(resource.node.lock.held && resource.token.held);
    (void)notes; (void)kn; ++inserted;
}
static void tsleep_interlock(void *p, int flags) {
    assert(p == &resource && flags == PCATCH);
    assert(resource.node.lock.held && resource.token.held);
    interlocked = true;
}
static int tsleep(void *p, int flags, const char *name, int timeout) {
    assert(p == &resource && interlocked && flags == (PINTERLOCKED | PCATCH));
    assert(!resource.node.lock.held && !resource.token.held);
    assert(timeout == 0); (void)name;
    interlocked = false;
    resource.kick_pending = true;
    if (sleep_mode == 1) resource.revoked = true;
    return sleep_mode == 2 ? EINTR : 0;
}
static int uiomove(caddr_t p, size_t n, struct uio *u) {
    assert(!resource.node.lock.held && !resource.token.held);
    assert(n == sizeof(struct vmmfs_pci_kick) && u->uio_resid == n);
    assert(((struct vmmfs_pci_kick *)p)->value == 42); ++copied; return 0;
}
static int
""" + (function('vmmfs_pcislot_resource.c', 'vmmfs_pcislot_resource_receive') + '\nstatic int\n' + function('vmmfs_pcislot_resource.c', 'vmmfs_pcislot_resource_read')) + r"""
static int
""" + (function('vmmfs_pcislot_resource.c', 'vmmfs_pcislot_resource_subscribe') + '\nstatic int\n' + function('vmmfs_pcislot_resource.c', 'vmmfs_pcislot_resource_kqfilter')) + r"""
int main(void) {
    struct vnode vnode = { &resource };
    struct uio uio = { sizeof(struct vmmfs_pci_kick) };
    struct vop_read_args read = { &vnode, &uio, IO_NDELAY };
    struct knote kn = { EVFILT_READ, NULL, NULL };
    struct vop_kqfilter_args filter = { &vnode, &kn };
    resource.kind = VMMFS_PCISLOT_RESOURCE_KICK; resource.kick.value = 42;
    assert(vmmfs_pcislot_resource_read(&read) == EAGAIN);
    resource.kick_pending = true;
    assert(vmmfs_pcislot_resource_read(&read) == 0 && copied == 1);
    assert(!resource.kick_pending);
    assert(vmmfs_pcislot_resource_kqfilter(&filter) == 0 && inserted == 1);
    kn.kn_filter = 99;
    assert(vmmfs_pcislot_resource_kqfilter(&filter) == EOPNOTSUPP);
    resource.node.dead = true; resource.kick_pending = true;
    assert(vmmfs_pcislot_resource_read(&read) == ENOENT);
    assert(vmmfs_pcislot_resource_kqfilter(&filter) == ENOENT && inserted == 1);
    resource.node.dead = false; resource.kick_pending = false;
    read.a_ioflag = 0; sleep_mode = 1;
    assert(vmmfs_pcislot_resource_read(&read) == ENXIO && copied == 1);
    resource.revoked = false; resource.kick_pending = false; sleep_mode = 2;
    assert(vmmfs_pcislot_resource_read(&read) == EINTR);
    resource.kick_pending = false; sleep_mode = 0;
    assert(vmmfs_pcislot_resource_read(&read) == 0 && copied == 2);
    assert(!resource.node.token.held && !resource.token.held && !interlocked);
    /* VFS pins a valid object for every dispatched VOP. */
}
""")

    def test_guest_logs_are_durable_before_machine_creation(self):
        source = (ROOT / "test/vmmfs/test_guest.py").read_text()
        anchor = source.index("def store(")
        setup = source[:anchor]
        self.assertIn("os.fsync(log.fileno())", setup)
        self.assertIn("os.fsync(backend_log.fileno())", setup)
        self.assertIn("for directory in (log_directory, log_directory.parent):", setup)
        self.assertIn("os.fsync(directory_fd)", setup)
        self.assertIn("os.close(directory_fd)", setup)

    def test_resource_namespace_handoff(self):
        run_c(COMMON + r"""
typedef unsigned long ino_t;
struct token { unsigned held; };
struct vmmfs_node { struct vnode *vnode; struct token token; unsigned references; bool dead; ino_t inode;  struct lock lock;};
struct vnode { void *v_data; unsigned holds; };
struct vmmfs_pcislot_resource { struct vmmfs_node node; };
struct vmmfs_pcislot_resources {
    struct vmmfs_node node; struct token token;
    bool destroying;
    struct vnode **vnodes;
    size_t count;
    struct vmmfs_pcislot_resource items[1];
};
struct child { struct vmmfs_node node; };
struct descriptor {
    struct vmmfs_node node;
    bool committed, updating;
};
struct vmmfs_pcislot {
    struct vmmfs_node node; struct token token;
    struct descriptor descriptor;
    struct vmmfs_pcislot_resources *resources;
    struct child config, events;
    struct vnode *descriptor_vnode, *events_vnode, *config_vnode;
};
struct vmmfs_pcislot_item { ino_t inode; int type; char name[32]; };
struct namecache { const char *nc_name; size_t nc_nlen; };
struct nchandle { struct namecache *ncp; };
struct vop_nresolve_args { struct vnode *a_dvp; struct nchandle *a_nch; };
static struct vmmfs_pcislot slot;
static struct vmmfs_pcislot_resources resources;
static struct vnode bar;
static bool retire_on_unlock;
static unsigned cached;
static void lwkt_gettoken(struct token *t) {
    if (t == &resources.token) assert(slot.token.held == 0);
    assert(t->held == 0); ++t->held;
}
static void lwkt_reltoken(struct token *t) {
    assert(t->held == 1); --t->held;
    if (t == &slot.token && retire_on_unlock) {
        assert(resources.node.references == 2);
        resources.destroying = true;
        resources.items[0].node.vnode = NULL;
        slot.resources = NULL;
        --resources.node.references;
        retire_on_unlock = false;
    }
}
void vmmfs_node_hold(struct vmmfs_node *n) {
    assert(slot.token.held == 1); ++n->references;
}
void vmmfs_node_put(struct vmmfs_node *n) {
    assert(slot.token.held == 0);
    assert(n->references != 0); --n->references;
}
static void vhold(struct vnode *v) {
    assert(slot.token.held || resources.token.held); ++v->holds;
}
static void vdrop(struct vnode *v) { assert(v->holds != 0); --v->holds; }
static int vget(struct vnode *v, int flags) {
    (void)flags; assert(v->holds == 1);
    assert(!slot.token.held && !resources.token.held); return 0;
}
#define DT_REG 8
#define bcmp(a,b,n) memcmp(a,b,n)
#define bcopy(a,b,n) memcpy(b,a,n)
#define vmmfs_pcislot_pciroot(s) (s)
#define vn_unlock(v) ((void)(v))
#define vrele(v) ((void)(v))
#define cache_setvp(h,v) ((void)(h), cached = ((v) != NULL))
static int vmmfs_pcislot_resource_name(struct vmmfs_pcislot_resource *r,
    char *name, size_t size, size_t *length) {
    (void)r; assert(resources.token.held == 1);
    assert(size >= 5); memcpy(name, "bar0", 5); *length = 4; return 0;
}
int
""" + function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resources_lookup") + r"""
int
""" + function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resources_read_item") + r"""
static int
""" + (function('vmmfs_pcislot.c', 'vmmfs_pcislot_get_item') + '\nstatic int\n' + function('vmmfs_pcislot.c', 'vmmfs_pcislot_nresolve')) + r"""
static int
""" + function("vmmfs_pcislot.c", "vmmfs_pcislot_read_item") + r"""
int main(void) {
    struct vnode *vnodes[] = { &bar };
    struct vnode parent = { .v_data = &slot };
    struct namecache name = { "bar0", 4 };
    struct nchandle handle = { &name };
    struct vop_nresolve_args args = { &parent, &handle };
    struct vmmfs_pcislot_item item;
    struct vnode *found = NULL;
    resources.node.references = 1;
    resources.items[0].node.vnode = vnodes[0]; resources.count = 1;
    resources.items[0].node.inode = 42;
    slot.resources = &resources;

    /* The child must be held before lookup releases its registry token. */
    assert(vmmfs_pcislot_resources_lookup(&resources, "bar0", 4, &found) == 0);
    assert(found == &bar && bar.holds == 1); vdrop(found);
    assert(vmmfs_pcislot_nresolve(&args) == 0 && cached == 1);
    assert(bar.holds == 0 && resources.node.references == 1);
    assert(vmmfs_pcislot_read_item(&slot, 0, &item) == 0);
    assert(item.inode == 42 && strcmp(item.name, "bar0") == 0);

    /* Detachment between parent and child locks cannot free the collection. */
    retire_on_unlock = true;
    assert(vmmfs_pcislot_nresolve(&args) == ENOENT && cached == 0);
    assert(resources.node.references == 0 && bar.holds == 0);
    assert(vmmfs_pcislot_nresolve(&args) == ENOENT);
    assert(vmmfs_pcislot_read_item(&slot, 0, &item) == ENOENT);

    resources.node.references = 1; resources.destroying = false;
    resources.items[0].node.vnode = &bar; slot.resources = &resources;
    retire_on_unlock = true;
    assert(vmmfs_pcislot_read_item(&slot, 0, &item) == ENOENT);
    assert(resources.node.references == 0);
    slot.descriptor.node.vnode = &bar; name.nc_name = "descriptor"; name.nc_nlen = 10;
    assert(vmmfs_pcislot_nresolve(&args) == 0 && bar.holds == 0);
    slot.node.dead = true;
    assert(vmmfs_pcislot_nresolve(&args) == ENOENT);
}
""")

    def test_resource_pager_retains_its_own_node(self):
        run_c(COMMON + r"""
#include <sys/queue.h>
typedef long long vm_ooffset_t;
typedef int vm_prot_t;
typedef unsigned short u_short;
struct ucred;
#define VM_PROT_EXECUTE 4
#define M_VMMFS 0
struct vmmfs_node { struct vnode *vnode;
    struct vmmfs_node *parent;
    unsigned references;
    void (*drop)(struct vmmfs_node *);
 struct lock lock; bool dead;};
struct device { void *si_drv1; };
struct vmmfs_pcislot_resource {
    struct vmmfs_node node;
    unsigned token;
    bool revoked, mapped;
    vm_ooffset_t mapping_size;
    void *pager_object, *backing_object, *vmspace, *traps;
    SLIST_HEAD(, vmmfs_pcislot_object) objects;
    struct device *dev;
};
static unsigned freed, objects, devices, tokens, arrays, spaces;
#define lwkt_gettoken(t) (++*(t))
#define lwkt_reltoken(t) (--*(t))
#define lwkt_token_uninit(t) (assert(*(t) == 0), ++tokens)
#define vm_object_deallocate(p) ((void)(p), ++objects)
#define vmspace_rel(p) ((void)(p), ++spaces)
#define destroy_only_dev(p) ((void)(p), ++devices)
#define kfree(p, t) (assert((p) != NULL), ++arrays)
static void vmmfs_node_hold(struct vmmfs_node *n) { ++n->references; }
static void vmmfs_node_put(struct vmmfs_node *n) {
    struct vmmfs_node *parent = n->parent;
    assert(n->references != 0);
    if (--n->references != 0) return;
    n->drop(n);
    if (parent != NULL) vmmfs_node_put(parent);
}
#define vmmfs_pcislot_resource_resources(r) ((r)->node.parent)
#define vmmfs_pcislot_resources_hold(n) vmmfs_node_hold(n)
#define vmmfs_pcislot_resources_put(n) vmmfs_node_put(n)
static void root_drop(struct vmmfs_node *n) { (void)n; ++freed; }
static int
""" + function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resource_pager_ctor") + r"""
static void
""" + function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resource_pager_dtor") + r"""
static void
""" + function("vmmfs_pcislot_resource.c", "vmmfs_pcislot_resource_drop") + r"""
int main(void) {
    struct vmmfs_node root = { .references=2, .drop=root_drop };
    struct device dev = {0};
    struct vmmfs_pcislot_resource resource = {0};
    resource.node.parent = &root;
    resource.node.references = 1;
    resource.node.drop = vmmfs_pcislot_resource_drop;
    resource.mapping_size = 4096;
    resource.backing_object = &root;
    resource.vmspace = &root;
    resource.traps = &root;
    resource.dev = &dev;
    u_short color = 9;
    assert(vmmfs_pcislot_resource_pager_ctor(&resource, 4096, 3, 0,
        NULL, &color) == 0);
    assert(color == 0 && resource.node.references == 2 && root.references == 2);
    vmmfs_node_put(&root);
    resource.revoked = true;
    vmmfs_node_put(&resource.node);
    assert(resource.node.references == 1 && root.references == 1);
    assert(objects == 0 && devices == 0 && tokens == 0);
    assert(vmmfs_pcislot_resource_pager_ctor(&resource, 4096, 3, 0,
        NULL, &color) == EINVAL);
    vmmfs_pcislot_resource_pager_dtor(&resource);
    assert(objects == 1 && devices == 1 && tokens == 1 && arrays == 1 && spaces == 1);
    assert(freed == 1);
    /* Partial construction and stopped resources may have no trap array. */
    for (unsigned mask = 0; mask != 16; ++mask) {
        memset(&resource, 0, sizeof(resource));
        resource.backing_object = (mask & 1) ? &root : NULL;
        resource.vmspace = (mask & 2) ? &root : NULL;
        resource.dev = (mask & 4) ? &dev : NULL;
        resource.traps = (mask & 8) ? &root : NULL;
        unsigned old_arrays = arrays;
        vmmfs_pcislot_resource_drop(&resource.node);
        assert(arrays == old_arrays + ((mask & 8) != 0));
    }
    return 0;
}
""")


    def test_serial_stream_gate(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_node { struct vnode *vnode; struct token token; bool dead;  struct lock lock;};
struct tty { struct token t_token; };
struct vmmfs_serialport { struct vmmfs_node node; struct tty tty; struct token token; bool closed, destroying; unsigned control_count; };
struct knote;
struct device { int unused; };
typedef struct device *cdev_t;
struct vnode { void *v_data; cdev_t v_rdev; bool locked; };
struct uio { int uio_resid; };
struct vop_read_args { struct vnode *a_vp; struct uio *a_uio; int a_ioflag; void *a_fp; };
struct vop_write_args { struct vnode *a_vp; struct uio *a_uio; int a_ioflag; void *a_fp; };
struct vop_kqfilter_args { struct vnode *a_vp; void *a_kn; };
static struct vmmfs_serialport *current;
static bool cancel;
static unsigned dispatched;
void lwkt_gettoken(struct token *t) { ++t->held; }
void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static void vn_unlock(struct vnode *v) {
    assert(v->locked); v->locked = false;
    if (cancel) current->node.dead = true;
}
static void vn_lock(struct vnode *v, int flags) {
    (void)flags;
    assert(!current->node.token.held && !current->tty.t_token.held);
    assert(!v->locked); v->locked = true;
}
static int dispatch(void) {
    assert(current->tty.t_token.held);
    assert(current->control_count || current->node.lock.held);
    assert(!current->node.dead); ++dispatched; return EWOULDBLOCK;
}
static int dev_dread(cdev_t d, struct uio *u, int f, void *p) {
    (void)d; (void)u; (void)f; (void)p; return dispatch();
}
static int dev_dwrite(cdev_t d, struct uio *u, int f, void *p) {
    (void)d; (void)u; (void)f; (void)p; return dispatch();
}
static int dev_dkqfilter(cdev_t d, void *k, void *p) {
    (void)d; (void)k; (void)p; return dispatch();
}
static void wakeup(void *p) { assert(p == current); }
static int
""" + (function('vmmfs_serialport.c', 'vmmfs_serialport_begin_io') + '\nstatic void\n' + function('vmmfs_serialport.c', 'vmmfs_serialport_end_io') + '\nstatic int\n' + function('vmmfs_serialport.c', 'vmmfs_serialport_read')) + r"""
static int
""" + function("vmmfs_serialport.c", "vmmfs_serialport_write") + r"""
static int
""" + (function('vmmfs_serialport.c', 'vmmfs_serialport_subscribe') + '\nstatic int\n' + function('vmmfs_serialport.c', 'vmmfs_serialport_kqfilter')) + r"""
int main(void) {
    struct device dev = {0};
    for (int test = 0; test < 4; ++test) {
        struct vmmfs_serialport port = {0};
        struct vnode vnode = {&port, &dev, true};
        struct uio uio = {1};
        struct vop_read_args r = {&vnode, &uio, 0, NULL};
        struct vop_write_args w = {&vnode, &uio, 0, NULL};
        struct vop_kqfilter_args k = {&vnode, NULL};
        current = &port; cancel = test == 2; dispatched = 0;
        port.node.dead = test == 1;
        if (test == 3) uio.uio_resid = 0;
        int expected = test == 3 ? 0 : (test == 0 ? EWOULDBLOCK : ENOENT);
        assert(vmmfs_serialport_read(&r) == expected);
        assert(vmmfs_serialport_write(&w) == expected);
        assert(vmmfs_serialport_kqfilter(&k) ==
            (port.node.dead ? ENOENT : EWOULDBLOCK));
        assert(vnode.locked && !port.node.token.held && !port.tty.t_token.held);
        assert(dispatched == (test == 0 ? 3U : test == 3 ? 1U : 0U));
    }
    return 0;
}
""")


    def test_serial_tty_reference_handoff(self):
        run_c(COMMON + r"""
#define kprintf(...) fprintf(stderr, __VA_ARGS__)
struct token { unsigned held; };
struct vmmfs_node { struct vnode *vnode; unsigned references;  struct lock lock; bool dead;};
struct tty {
    struct token t_token;
    unsigned t_refs;
    void (*t_unhold)(struct tty *);
    void *t_sc;
};
struct task { unsigned queued; };
struct vmmfs_serialport {
    struct vmmfs_node node;
    struct token token;
    struct tty tty;
    struct task tty_release_task;
    unsigned control_count;
    bool destroying;
};
static void *taskqueue_thread[] = { NULL };
#define mycpuid 0
static int enqueue_error, releases;
void lwkt_gettoken(struct token *t) { ++t->held; }
void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static void vmmfs_node_put(struct vmmfs_node *n) {
    assert(n->references); --n->references; ++releases;
}
static int taskqueue_enqueue(void *q, struct task *t) {
    (void)q;
    if (enqueue_error) return enqueue_error;
    assert(t->queued == 0); ++t->queued; return 0;
}
static void vmmfs_serialport_tty_retire(struct vmmfs_serialport *);
static void
""" + function("vmmfs_serialport.c", "vmmfs_serialport_tty_unhold") + r"""
static void
""" + function("vmmfs_serialport.c", "vmmfs_serialport_tty_retire") + r"""
static void
""" + function("vmmfs_serialport.c", "vmmfs_serialport_tty_release") + r"""
int main(void) {
    struct vmmfs_serialport port = {0};
    port.node.references = 2;
    port.tty.t_sc = &port;
    port.tty.t_unhold = vmmfs_serialport_tty_unhold;
    port.tty.t_refs = 2;
    lwkt_gettoken(&port.tty.t_token);
    vmmfs_serialport_tty_unhold(&port.tty);
    assert(port.tty.t_refs == 1 && port.tty_release_task.queued == 0);
    port.destroying = true;
    vmmfs_serialport_tty_retire(&port);
    assert(port.tty_release_task.queued == 0);
    port.control_count = 1;
    vmmfs_serialport_tty_unhold(&port.tty);
    assert(port.tty_release_task.queued == 0);
    port.control_count = 0;
    vmmfs_serialport_tty_retire(&port);
    assert(port.tty.t_refs == 0 && port.tty_release_task.queued == 1);
    assert(port.node.references == 2 && releases == 0);
    assert(port.tty.t_unhold == NULL && port.tty.t_token.held == 1);
    vmmfs_serialport_tty_retire(&port);
    assert(port.tty_release_task.queued == 1);
    lwkt_reltoken(&port.tty.t_token);
    vmmfs_serialport_tty_release(&port, 1);
    assert(port.node.references == 1 && releases == 1);
    assert(port.tty.t_token.held == 0 && port.token.held == 0);
    port.tty.t_unhold = vmmfs_serialport_tty_unhold;
    enqueue_error = EPIPE;
    lwkt_gettoken(&port.tty.t_token);
    vmmfs_serialport_tty_retire(&port);
    assert(port.tty.t_unhold != NULL && port.node.references == 1);
    lwkt_reltoken(&port.tty.t_token);
    return 0;
}
""")


    def test_serial_open_token_balance(self):
        run_c(COMMON + r"""
#define kprintf(...) fprintf(stderr, __VA_ARGS__)
#define S_IFCHR 0020000
#define MAXPHYS 65536
#define VNOTSEEKABLE 1
#define min(a, b) ((a) < (b) ? (a) : (b))
struct token { int held; };
struct vmmfs_node { struct vnode *vnode; struct token token; bool dead;  struct lock lock;};
struct vmmfs_serialport {
    struct vmmfs_node node;
    struct token token;
    bool destroying, closed;
    unsigned control_count;
};
struct device { int si_iosize_max; };
typedef struct device *cdev_t;
struct vnode { void *v_data; cdev_t v_rdev; int locked; };
struct vop_open_args {
    struct vnode *a_vp; int a_mode; void *a_cred; void **a_fpp;
};
static int open_error, cancel_open, closes, opens;
static void lwkt_gettoken(struct token *t) { assert(!t->held); t->held = 1; }
static void lwkt_reltoken(struct token *t) { assert(t->held); t->held = 0; }
static void vn_unlock(struct vnode *v) { assert(v->locked); v->locked = 0; }
static void vn_lock(struct vnode *v, int flags) {
    (void)flags; assert(!v->locked); v->locked = 1;
}
static void vsetflags(struct vnode *v, int f) { (void)v; (void)f; }
static void wakeup(void *p) { (void)p; }
static int dev_dopen(cdev_t d, int m, int type, void *c, void **f,
                    struct vnode *v) {
    struct vmmfs_serialport *p = v->v_data;
    (void)d; (void)m; (void)type; (void)c; (void)f;
    assert(!p->node.token.held && !p->token.held && !v->locked);
    if (cancel_open) { p->node.dead = true; p->closed = true; }
    return open_error;
}
static int dev_dclose(cdev_t d, int m, int type, void *f) {
    (void)d; (void)m; (void)type; (void)f; ++closes; return 0;
}
static int vop_stdopen(struct vop_open_args *a) {
    struct vmmfs_serialport *p = a->a_vp->v_data;
    assert(!p->token.held && !p->closed && p->control_count == 1); ++opens; return 0;
}
static int
""" + (function('vmmfs_serialport.c', 'vmmfs_serialport_begin_io') + '\nstatic void\n' + function('vmmfs_serialport.c', 'vmmfs_serialport_end_io') + '\nstatic int\n' + function("vmmfs_serialport.c", "vmmfs_serialport_open")) + r"""
int main(void) {
    for (int test = 0; test < 6; ++test) {
        struct vmmfs_serialport port = {0};
        struct device dev = {0};
        struct vnode vnode = {&port, &dev, 1};
        void *file = NULL;
        struct vop_open_args args = {&vnode, 0, NULL, &file};
        int expected = 0;
        open_error = cancel_open = closes = opens = 0;
        switch (test) {
        case 1: port.node.dead = true; expected = ENOENT; break;
        case 2: vnode.v_rdev = NULL; expected = ENXIO; break;
        case 3: open_error = EINTR; expected = EINTR; break;
        case 4: cancel_open = 1; expected = ENXIO; break;
        case 5: vnode.v_data = NULL; expected = ENOENT; break;
        }
        assert(vmmfs_serialport_open(&args) == expected);
        assert(port.control_count == 0);
        assert(!port.node.token.held && !port.token.held && vnode.locked);
        assert(closes == (test == 4));
        assert(opens == (test == 0));
    }
    return 0;
}
""")


    def test_serial_deactivation_owns_quiescence(self):
        drop = function("vmmfs_serialport.c", "vmmfs_serialport_drop")
        for operation in ("tsleep(", "vmmfs_serialport_revoke(", "l_close", "ttyclose("):
            self.assertNotIn(operation, drop)
        deactivate = function("vmmfs_serialport.c", "vmmfs_serialport_deactivate")
        self.assertLess(deactivate.index("vmmfs_serialport_revoke(port)"),
                        deactivate.index("while (port->control_count"))
        self.assertIn("ttyclose(", deactivate)
        opened = function("vmmfs_serialport.c", "vmmfs_serialport_open")
        self.assertIn("VMMFS_WORK(port, vmmfs_serialport_begin_io(port))", opened)
        self.assertNotIn("port->node.dead", opened)


    def test_auth_retains_slot_until_final_file_close(self):
        run_c(COMMON + r"""
#include <stdlib.h>
typedef unsigned int u_int;
typedef unsigned long ino_t;
struct vmmfs_node { struct vnode *vnode; unsigned references; ino_t inode;  struct lock lock; bool dead;};
struct token { int unused; };
struct vmmfs_pcislot { struct vmmfs_node node; struct token token; };
struct vmmfs_pcislot_auth {
    struct vmmfs_pcislot *slot;
    ino_t slot_inode;
    uint64_t generation;
    volatile u_int valid, references;
};
struct file { int references, f_type, f_flag; void *f_ops, *f_data; };
struct process { void *p_fd; };
static struct process process;
#define curproc (&process)
#define M_VMMFS 0
#define M_WAITOK 0
#define M_ZERO 0
#define FREAD 1
#define DTYPE_DMABUF 1
static int vmmfs_pcislot_auth_fileops;
static int allocations, failure;
static struct file *descriptor_file;
static void *allocate(size_t size) { ++allocations; return calloc(1, size); }
#define kmalloc(n, t, f) allocate(n)
#define kfree(p, t) (free(p), --allocations)
#define atomic_add_int(p, n) (*(p) += (n))
#define atomic_fetchadd_int(p, n) ((*(p) += (n)) - (n))
#define atomic_store_rel_int(p, n) (*(p) = (n))
#define vmmfs_node_hold(n) (++(n)->references)
#define vmmfs_node_put(n) (--(n)->references)
static void vmmfs_pcislot_auth_put(struct vmmfs_pcislot_auth *);
static void vmmfs_pcislot_auth_hold(struct vmmfs_pcislot_auth *a) { ++a->references; }
static int falloc(void *p, struct file **fp, void *f) {
    (void)p; (void)f;
    if (failure) return ENOMEM;
    *fp = calloc(1, sizeof(**fp)); (*fp)->references = 1;
    return 0;
}
static void fdrop(struct file *f) {
    if (--f->references == 0) {
        vmmfs_pcislot_auth_put(f->f_data);
        free(f);
    }
}
static int fdalloc(struct process *p, int low, int *fd) {
    (void)p; (void)low; *fd = 3; return 0;
}
static void fsetfd(void *p, struct file *f, int fd) {
    (void)p; (void)fd; ++f->references; descriptor_file = f;
}
static void
""" + function("vmmfs_pcislot_auth.c", "vmmfs_pcislot_auth_put") + """
int
""" + function("vmmfs_pcislot_auth.c", "vmmfs_pcislot_auth_create") + """
void
""" + function("vmmfs_pcislot_auth.c", "vmmfs_pcislot_auth_revoke") + """
int main(void) {
    struct vmmfs_pcislot slot = { .node.references=1, .node.inode=7 };
    struct vmmfs_pcislot_auth *auth;
    failure = 1;
    assert(vmmfs_pcislot_auth_create(&slot, 1, &auth) == ENOMEM);
    assert(allocations == 0 && slot.node.references == 1);
    failure = 0;
    assert(vmmfs_pcislot_auth_create(&slot, 1, &auth) == 0);
    assert(slot.node.references == 2 && allocations == 1);
    vmmfs_pcislot_auth_revoke(auth);
    assert(auth->valid == 0 && slot.node.references == 2);
    fdrop(descriptor_file);
    assert(allocations == 0 && slot.node.references == 1);
    return 0;
}
""")

    def test_module_gate_covers_root_and_loader_tail(self):
        uninit = function("vmmfs.c", "vmmfs_vfs_uninit")
        self.assertIn("vmmfs_root_module_fini()", uninit)
        child = function("vmmfs_loader.c", "vmmfs_loader_child")
        self.assertLess(child.index("acquire_curproc"),
                        child.index("vmmfs_node_put(&launch->node)"))


    def test_identity_failed_vnode_releases_parent(self):
        run_c(COMMON + r"""
typedef unsigned int u_int;
struct vnode { void *v_data; };
struct vmmfs_node { struct vnode *vnode;
    struct vmmfs_mount *mount;
    struct vmmfs_node *parent;
    bool dead;
    unsigned references, inode, mode, size, load_limit, store_limit;
    int token;
    int (*deactivate)(struct vmmfs_node *);
    void (*drop)(struct vmmfs_node *);
    int (*load)(struct vmmfs_node *, char *, size_t, size_t *);
    void *store;
 struct lock lock;};
struct token { int unused; };
struct vmmfs_machine { struct vmmfs_node node; struct token token; unsigned id; };
struct vmmfs_root { int unused; };
struct vmmfs_machine_id { struct vmmfs_node node; };
struct vmmfs_mount { void *root; void *machine_id_vops, *mount; };
static unsigned vmmfs_machine_next_id;
static int tokens, drops;
#define VMMFS_MACHINE_ID_MAX 999999
#define VMMFS_MACHINE_ID_MODE 0444
#define VREG 1
#define bzero(p, n) memset(p, 0, n)
#define atomic_fetchadd_int(p, n) ((*(p) += (n)) - (n))
#define lwkt_token_init(p, n) ((void)(p), (void)(n), ++tokens)
#define vmmfs_node_hold(p) (++(p)->references)
#define vmmfs_root_allocate_inode(p) ((void)(p), 1)
#define vmmfs_node_decimal_size(v) ((void)(v), 2)
static int vmmfs_machine_id_deactivate(struct vmmfs_node *n) { (void)n; return 0; }
static int vmmfs_machine_id_load(struct vmmfs_node *n, char *b, size_t c, size_t *l) {
    (void)n; (void)b; (void)c; (void)l; return 0;
}
static void vmmfs_machine_id_drop(struct vmmfs_node *n) { (void)n; ++drops; }
static inline void vmmfs_node_put(struct vmmfs_node *n) {
    assert(n->references == 1);
    --n->references;
    --n->parent->references;
    n->drop(n);
}
static int vmmfs_vnode_create_regular(void *m, void **o, int t,
    struct vmmfs_node *n) {
    struct vnode **v = &n->vnode;
    (void)m; (void)o; (void)t; (void)n; (void)v;
    return ENOMEM;
}
int
""" + function("vmmfs_machine_id.c", "vmmfs_machine_id_init") + """
int main(void) {
    struct vmmfs_root root = {0};
    struct vnode root_vnode = { &root };
    struct vmmfs_mount mount = { &root_vnode, &root, &root };
    struct vmmfs_machine machine = {0};
    struct vmmfs_machine_id identity;
    machine.node.references = 1;
    machine.node.mount = &mount;
    assert(vmmfs_machine_id_init(&machine.node, &identity) == ENOMEM);
    assert(identity.node.vnode == NULL && machine.id == 0);
    assert(tokens == 0 && drops == 1);
    assert(machine.node.references == 1);
    return 0;
}
""")


    def test_assertions_have_no_effects(self):
        allowed = {"sizeof", "RB_EMPTY", "TAILQ_EMPTY", "SLIST_EMPTY", "atomic_load_acq_int"}
        failures = []
        for path in sorted(SOURCE.glob("*")):
            if path.suffix not in {".c", ".h"}:
                continue
            text = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(), flags=re.S)
            for match in re.finditer(r"\bKKASSERT\s*\(", text):
                expr = balanced(text, text.index("(", match.start()), "(", ")")
                calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", expr))
                if calls - allowed or re.search(r"\+\+|--|(?<![=!<>])=(?!=)", expr):
                    failures.append(path.name + ": " + expr)
        self.assertEqual([], failures)

    def test_vcpu_detached_before_destroy(self):
        run_c(COMMON + """
struct token { int unused; };
struct runtime { int unused; };
typedef struct runtime *vmm_vcpu_t;
struct vmmfs_vcpu { struct token token; };
struct vmmfs_vcpu_thread {
    struct vmmfs_vcpu *group;
    vmm_vcpu_t vcpu;
    unsigned int kick_count;
};
static struct vmmfs_vcpu_thread *current;
#define lwkt_gettoken(x) ((void)(x))
#define lwkt_reltoken(x) ((void)(x))
#define tsleep_interlock(x, y) ((void)(x), (void)(y))
#define PINTERLOCKED 0
#define crit_enter() ((void)0)
#define crit_exit() ((void)0)
#define curthread NULL
#define tsleep_remove(x) ((void)(x))
#define tsleep(x, y, z, t) (current->kick_count = 0, 0)
static int vmm_vcpu_destroy(vmm_vcpu_t value) {
    assert(value != NULL);
    assert(current->vcpu == NULL);
    assert(current->kick_count == 0);
    return 0;
}
static void
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_thread_destroy") + """
int main(void) {
    struct runtime runtime = {0};
    struct vmmfs_vcpu group = {{0}};
    struct vmmfs_vcpu_thread thread = { &group, &runtime, 0 };
    current = &thread;
    vmmfs_vcpu_thread_destroy(&thread);
    assert(thread.vcpu == NULL);
    thread.vcpu = &runtime;
    thread.kick_count = 1;
    vmmfs_vcpu_thread_destroy(&thread);
    assert(thread.vcpu == NULL);
    return 0;
}
""")


    def test_kick_pins_only_constructed_vcpus(self):
        run_c(COMMON + """
struct token { int unused; };
struct runtime { int unused; };
typedef struct runtime *vmm_vcpu_t;
struct vmmfs_vcpu_thread;
struct vmmfs_vcpu {
    struct token token;
    struct vmmfs_vcpu_thread *threads;
    unsigned int count;
    bool stop_requested, reset_requested;
};
struct vmmfs_vcpu_thread {
    struct vmmfs_vcpu *group;
    vmm_vcpu_t vcpu;
    unsigned int kick_count;
};
static struct vmmfs_vcpu_thread *current;
static int kicks;
#define lwkt_gettoken(x) ((void)(x))
#define lwkt_reltoken(x) ((void)(x))
#define wakeup(x) ((void)(x))
#define kprintf(...) fprintf(stderr, __VA_ARGS__)
static int vmm_vcpu_kick(vmm_vcpu_t value) {
    assert(value == current->vcpu);
    assert(current->kick_count == 1);
    ++kicks;
    return 0;
}
static void
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_thread_kick") + """
void
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_request_stop") + """
void
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_request_reset") + """
int main(void) {
    struct runtime runtime = {0};
    struct vmmfs_vcpu group = {0};
    struct vmmfs_vcpu_thread threads[2] = {{ &group, &runtime, 0 }, {0}};
    group.threads = threads;
    group.count = 2;
    current = &threads[0];
    vmmfs_vcpu_request_stop(&group);
    assert(kicks == 1 && current->kick_count == 0);
    group.stop_requested = false;
    vmmfs_vcpu_request_reset(&group);
    assert(kicks == 2 && current->kick_count == 0);
    return 0;
}
""")

    def test_reset_synchronizes_resource_addresses(self):
        run_c(COMMON + """
#define VMMFS_PCISLOT_MAX_BARS 6
#define VMMFS_PCI_EVENT_RESET 0
struct vmmfs_pcislot_resources { uint64_t bars[6], rom; };
struct vmmfs_pcislot {
    struct { bool dead; } node;
    struct {
        bool committed;
        uint64_t generation;
        struct { struct { bool present; } bars[6]; bool rom_present; } value;
    } descriptor;
    struct { bool powered; uint64_t bar_address[6], rom_address; } type0;
    struct vmmfs_pcislot_resources *resources;
    int config, events;
};
#define vmmfs_pcislot_pciroot(s) (s)
#define vmmfs_pcislot_config_power_on(c, g) ((void)(c), (void)(g))
#define vmmfs_pcislot_events_log(...) ((void)0)
static int vmmfs_pcislot_resources_set_decode(
    struct vmmfs_pcislot_resources *r, bool a, bool b, bool c) {
    (void)r; (void)a; (void)b; (void)c; return 0;
}
static int vmmfs_pcislot_type0_build(struct vmmfs_pcislot *s) {
    s->type0.bar_address[0] = 0x80000000;
    s->type0.rom_address = 0x90000000;
    return 0;
}
#define vmmfs_pcislot_resources_bar_relocate(r, b, a) ((r)->bars[b] = (a), 0)
#define vmmfs_pcislot_resources_rom_relocate(r, a) ((r)->rom = (a), 0)
int
""" + function("vmmfs_pcislot.c", "vmmfs_pcislot_reset") + """
int main(void) {
    struct vmmfs_pcislot_resources resources = {{0xa0000000}, 0xb0000000};
    struct vmmfs_pcislot slot = {0};
    slot.descriptor.committed = true;
    slot.resources = &resources;
    slot.descriptor.value.bars[0].present = true;
    slot.descriptor.value.rom_present = true;
    assert(vmmfs_pcislot_reset(&slot) == 0);
    assert(resources.bars[0] == slot.type0.bar_address[0]);
    assert(resources.rom == slot.type0.rom_address);
    return 0;
}
""")

    def test_rom_enable_readback(self):
        text = (SOURCE / "vmmfs_pcislot.c").read_text()
        start = text.index("if (slot->descriptor.value.rom_present && offset")
        brace = text.index("{", start)
        branch = text[start:brace] + balanced(text, brace)
        run_c(COMMON + """
struct vmmfs_pcislot_resources { bool enabled; };
struct vmmfs_pcislot {
    struct { struct { bool rom_present; uint64_t rom_size; } value; } descriptor;
    struct vmmfs_pcislot_resources *resources;
    struct { uint8_t bytes[256]; bool rom_probe; uint64_t rom_address; } type0;
};
static uint32_t vmmfs_pcislot_type0_read32(uint8_t *p, unsigned off) {
    uint32_t v; memcpy(&v, p + off, 4); return v;
}
static void vmmfs_pcislot_type0_write32(uint8_t *p, unsigned off, uint32_t v) {
    memcpy(p + off, &v, 4);
}
#define vmmfs_pcislot_type0_width_mask(w) ((uint32_t)(UINT32_MAX >> (32 - (w)*8)))
#define vmmfs_pcislot_resources_rom_relocate(r, a) ((void)(r), (void)(a), 0)
#define vmmfs_pcislot_resources_rom_enable(r, e) ((r)->enabled = (e), 0)
static int write_rom(struct vmmfs_pcislot *slot, unsigned offset,
                     unsigned width, uint32_t value) {
    uint32_t old, mask;
    struct vmmfs_pcislot_resources *resources;
    void *vcpu = NULL;
""" + branch + """
    return EINVAL;
}
int main(void) {
    struct vmmfs_pcislot_resources resources = {false};
    struct vmmfs_pcislot slot = {0};
    slot.descriptor.value.rom_present = true;
    slot.descriptor.value.rom_size = 0x10000;
    slot.resources = &resources;
    assert(write_rom(&slot, 0x30, 4, 0x90001235) == 0);
    assert(vmmfs_pcislot_type0_read32(slot.type0.bytes, 0x30) == 0x90000001);
    assert(resources.enabled);
    assert(write_rom(&slot, 0x30, 1, 0) == 0);
    assert(vmmfs_pcislot_type0_read32(slot.type0.bytes, 0x30) == 0x90000000);
    assert(!resources.enabled);
    return 0;
}
""")


class NodeLifetime(unittest.TestCase):
    def test_reference_handoff(self):
        run_c(COMMON + """
typedef unsigned int u_int;
struct token { int unused; };
struct vmmfs_node { struct vnode *vnode;
    struct vmmfs_node *parent;
    struct token token;
    u_int references;
    void (*drop)(struct vmmfs_node *);
 struct lock lock; bool dead;};
static unsigned int destroyed;
static unsigned int uninitialized;
static u_int fetchadd(u_int *value, int delta) {
    u_int old = *value;
    *value += delta;
    return old;
}
#define atomic_load_acq_int(p) (*(p))
#define atomic_add_int(p, n) (*(p) += (n))
#define atomic_fetchadd_int(p, n) fetchadd((p), (n))
#define lockuninit(p) (assert(!(p)->held), ++uninitialized)
void vmmfs_node_put(struct vmmfs_node *);
static void destroy(struct vmmfs_node *node) {
    assert(node->references == 0);
    if (node->parent != NULL)
        assert(node->parent->references != 0);
    ++destroyed;
}
void
""" + function("vmmfs_node.c", "vmmfs_node_hold") + """
void
""" + function("vmmfs_node.c", "vmmfs_node_put") + """
int main(void) {
    struct vmmfs_node parent = { .references=1, .drop=destroy };
    struct vmmfs_node child = { .parent=&parent, .references=1, .drop=destroy };
    vmmfs_node_hold(&parent);
    vmmfs_node_hold(&child);
    vmmfs_node_put(&parent);
    vmmfs_node_put(&child);
    assert(destroyed == 0);
    assert(parent.references == 1 && child.references == 1);
    vmmfs_node_put(&child);
    assert(destroyed == 2 && uninitialized == 2);
    return 0;
}
""")

    def test_flat_node_interface(self):
        header = (SOURCE / "vmmfs_node.h").read_text()
        start = header.index("struct vmmfs_node {")
        node = balanced(header, header.index("{", start))
        for field in ("parent", "lock", "references", "deactivate", "drop",
                      "load", "store", "get_item", "read_item", "create_item",
                      "remove_item"):
            self.assertRegex(node, r"\b" + field + r"\b")
        for path in SOURCE.glob("*.[ch]"):
            self.assertNotIn("struct vmmfs_branch", path.read_text(), path.name)
            self.assertNotIn("final_drop", path.read_text(), path.name)


class Deactivation(unittest.TestCase):
    def test_gate_veto_and_reentry(self):
        run_c(COMMON + r"""
struct token { int held; };
struct vmmfs_node { struct vnode *vnode; struct token token; bool dead;
    int (*deactivate)(struct vmmfs_node *);  struct lock lock;};
struct vnode { struct vmmfs_node *v_data; int refs; };
static struct { void *p_ucred; } proc0;
static struct vnode *current;
static int calls, revoked, veto;
#define lwkt_gettoken(p) (++(p)->held)
#define lwkt_reltoken(p) (--(p)->held)
#define vref(p) (++(p)->refs)
#define vrele(p) (--(p)->refs)
#define DTYPE_VNODE 1
#define CINV_CHILDREN 1
#define fdrevoke(v, t, c) (assert((v)->v_data->token.held == 0), (void)(t), (void)(c), ++revoked, 0)
#define cache_inval_vp(v, f) (assert((v)->v_data->token.held == 0), (void)(f))
int vmmfs_vnode_deactivate(struct vnode *);
static int deactivate(struct vmmfs_node *node) {
    assert(node->dead && node->token.held == 0);
    ++calls;
    assert(vmmfs_vnode_deactivate(current) == EBUSY);
    assert(node->token.held == 0);
    return veto;
}
int
""" + function("vmmfs_node.c", "vmmfs_vnode_deactivate") + """
int main(void) {
    struct vmmfs_node node = { .deactivate=deactivate };
    struct vnode vnode = {&node, 1};
    current = &vnode;
    veto = EBUSY;
    assert(vmmfs_vnode_deactivate(&vnode) == EBUSY);
    assert(!node.dead && calls == 1 && revoked == 0 && vnode.refs == 1);
    veto = 0;
    assert(vmmfs_vnode_deactivate(&vnode) == 0);
    assert(node.dead && calls == 2 && revoked == 1 && vnode.refs == 1);
    assert(vmmfs_vnode_deactivate(&vnode) == EBUSY);
    assert(calls == 2 && revoked == 1);
    assert(node.token.held == 0);
    struct vmmfs_node passive = {0};
    struct vnode passive_vnode = {&passive, 1};
    assert(vmmfs_vnode_deactivate(&passive_vnode) == 0);
    assert(passive.dead && passive.token.held == 0);
    assert(calls == 2 && revoked == 2 && passive_vnode.refs == 1);
    assert(vmmfs_vnode_deactivate(&passive_vnode) == EBUSY);
    assert(revoked == 2 && passive_vnode.refs == 1);
    return 0;
}
""")


class LaunchContract(unittest.TestCase):
    def test_private_resource_releases_pager_before_parent_reference(self):
        body = function("vmmfs_pcislot_resource.c",
                        "vmmfs_pcislot_resources_deactivate")
        private = body[body.index("} else {"):]
        self.assertLess(private.index("vmmfs_pcislot_resource_deactivate("),
                        private.index("vmmfs_node_put(&resource->node)"))

    def test_config_wait_owns_completion_and_cleans_queue(self):
        body = function("vmmfs_pcislot_config.c", "vmmfs_pcislot_config_submit")
        self.assertNotIn("config_done", body)
        cancel = function("vmmfs_pcislot_config.c",
                          "vmmfs_pcislot_config_cancel_locked")
        run_c(COMMON + r"""
#include <stdlib.h>
#include <sys/queue.h>
#define M_VMMFS 0
#define M_WAITOK 0
#define M_ZERO 0
#define PINTERLOCKED 1
#define VMMFS_PCI_CONFIG_SUCCESS 0
#define VMMFS_PCI_CONFIG_FAILURE 1
#define VMMFS_PCI_CONFIG_UNSUPPORTED 2
enum vmm_io_width { VMM_IO_WIDTH_32 = 4 };
typedef void *vmm_vcpu_t;
struct token { int held; };
struct vmmfs_node { struct vnode *vnode; struct token token; bool dead;  struct lock lock;};
struct group { struct token token; bool stop_requested, reset_requested; };
struct vmmfs_vcpu_thread { struct group *group; vmm_vcpu_t vcpu; };
struct vmmfs_pcislot_resource { int unused; };
struct vmmfs_pci_config_response {
    uint64_t generation, sequence, value;
    uint32_t status, reserved;
};
struct record {
    uint64_t generation, sequence, offset, value;
    uint16_t bar;
    uint8_t space, width, operation;
};
struct vmmfs_pcislot_config_request {
    TAILQ_ENTRY(vmmfs_pcislot_config_request) entry;
    vmm_vcpu_t vcpu;
    struct record request;
    uint64_t response_value;
    uint32_t response_status;
    bool delivered, completed;
};
TAILQ_HEAD(queue, vmmfs_pcislot_config_request);
struct vmmfs_pcislot_config {
    struct vmmfs_node node;
    struct token token;
    struct queue requests;
    bool closed, powered;
    void *responder;
    uint64_t generation, next_sequence;
};
static struct vmmfs_pcislot_config config;
static struct group group;
static int scenario, allocation, reservation;
static void *kmalloc(size_t n, int type, int flags) {
    (void)type; (void)flags; ++allocation; return calloc(1,n);
}
static void kfree(void *p, int type) { (void)type; --allocation; free(p); }
#define bzero(p,n) memset((p),0,(n))
void lwkt_gettoken(struct token *t) { ++t->held; }
void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static void wakeup(void *p) { (void)p; }
static void tsleep_interlock(void *p, int flags) {
    (void)p; (void)flags; assert(!reservation); reservation=1;
}
static void vmmfs_pcislot_config_wake_next(struct vmmfs_pcislot_config *c) {
    (void)c;
}
static int vmmfs_pcislot_resource_index(struct vmmfs_pcislot_resource *r,
    uint16_t *index) { (void)r; *index=0; return 0; }
""" + "static void\n" + cancel + r"""
static int tsleep(void *p, int flags, const char *name, int timeout) {
    struct vmmfs_pcislot_config_request *r=TAILQ_FIRST(&config.requests);
    (void)p; (void)name; assert(flags == PINTERLOCKED && timeout == 0);
    assert(reservation && r); reservation=0;
    assert(!config.token.held && !config.node.token.held && !group.token.held);
    if (scenario == 1) group.stop_requested=true;
    else if (scenario == 2) group.reset_requested=true;
    else if (scenario == 3) return EINTR;
    else if (scenario == 4)
        vmmfs_pcislot_config_cancel_locked(&config,VMMFS_PCI_CONFIG_FAILURE);
    else { r->response_value=42; r->completed=true; }
    return 0;
}
""" + "static int\n" + body + r"""
int main(void) {
    struct vmmfs_vcpu_thread thread={ &group, &group };
    struct vmmfs_pcislot_resource resource;
    struct vmmfs_pci_config_response response;
    for (scenario=0; scenario<5; ++scenario) {
        memset(&config,0,sizeof(config)); memset(&group,0,sizeof(group));
        TAILQ_INIT(&config.requests);
        config.powered=true; config.responder=&config; config.generation=1;
        int error=vmmfs_pcislot_config_submit(&config,&thread,&resource,0,0,
            VMM_IO_WIDTH_32,0,0,&response);
        assert(error == ((scenario==1 || scenario==3) ? EINTR : 0));
        assert(TAILQ_EMPTY(&config.requests) && !allocation && !reservation);
        assert(!config.token.held && !config.node.token.held && !group.token.held);
        if (scenario==0) assert(response.value==42 && response.status==0);
        if (scenario==2 || scenario==4)
            assert(response.status==VMMFS_PCI_CONFIG_FAILURE);
    }
    return 0;
}
""")

    def test_machine_events_admission_and_final_drop(self):
        for verb in ("read", "kqfilter"):
            body = function("vmmfs_events.c", "vmmfs_events_" + verb)
            self.assertIn("VMMFS_WORK(events,", body)
            self.assertNotIn("events->node.dead", body)
        drop = function("vmmfs_events.c", "vmmfs_events_drop")
        self.assertNotIn("vmmfs_events_revoke", drop)
        self.assertNotIn("lwkt_gettoken", drop)


    def test_stream_shutdown_is_owned_by_deactivate(self):
        objects = ("vmmfs_events", "vmmfs_pcislot_events", "vmmfs_pcislot_config")
        for name in objects:
            header = (SOURCE / (name + ".h")).read_text()
            self.assertNotIn(name + "_revoke", header)
            self.assertNotIn(name + "_revoke",
                             (SOURCE / (name + ".c")).read_text())
        definitions = "\n".join(
            "struct " + name + " { struct vmmfs_node node; struct token token; "
            "bool closed, powered, opening; void *responder; struct kq kq; };"
            for name in objects)
        bodies = "\n".join(
            "static int\n" + function(name + ".c", name + "_deactivate")
            for name in objects)
        run_c(COMMON + r"""
struct vmmfs_node { struct vnode *vnode; bool dead;  struct lock lock;};
struct token { bool held; };
struct kq { int ki_note; };
#define VMMFS_PCI_CONFIG_FAILURE 1
static unsigned wakes, notes, cancellations;
static bool *closed;
static struct token *token;
static void *object;
static int *note;
void lwkt_gettoken(struct token *t) {
    assert(t == token && !t->held); t->held = true;
}
void lwkt_reltoken(struct token *t) {
    assert(t == token && t->held && *closed); t->held = false;
}
static void wakeup(void *p) {
    assert(p == object && *closed && !token->held); ++wakes;
}
#define KNOTE(p,h) do { assert((p) == note && (h) == 0 && !token->held); ++notes; } while (0)
""" + definitions + r"""
static void vmmfs_pcislot_config_cancel_locked(struct vmmfs_pcislot_config *c,
    unsigned status) {
    assert(c == object && c->token.held && c->closed);
    assert(!c->powered && !c->opening && c->responder == NULL);
    assert(status == VMMFS_PCI_CONFIG_FAILURE); ++cancellations;
}
""" + bodies + r"""
#define CHECK(value, callback) do { \
    object = &(value); closed = &(value).closed; token = &(value).token; \
    note = &(value).kq.ki_note; (value).node.dead = true; \
    assert(callback(&(value).node) == 0); \
    assert((value).closed && !(value).token.held); \
} while (0)
int main(void) {
    struct vmmfs_events events = {0};
    struct vmmfs_pcislot_events pci_events = {0};
    struct vmmfs_pcislot_config config = {0};
    config.powered = config.opening = true; config.responder = &config;
    CHECK(events, vmmfs_events_deactivate);
    CHECK(pci_events, vmmfs_pcislot_events_deactivate);
    CHECK(config, vmmfs_pcislot_config_deactivate);
    assert(wakes == 3 && notes == 3 && cancellations == 1);
}
""")

    def test_stream_vop_admission_uses_work_dispatch(self):
        for object_name in ("config", "events"):
            filename = "vmmfs_pcislot_" + object_name + ".c"
            for verb in ("open", "kqfilter", "read"):
                body = function(filename, "vmmfs_pcislot_" + object_name + "_" + verb)
                self.assertIn("VMMFS_WORK(", body)
                self.assertNotIn("->node.dead", body)
                self.assertNotIn("->node.token", body)

    def test_control_tail_retains_events(self):
        for name in ("vmmfs_machine_run", "vmmfs_machine_request_stop",
                     "vmmfs_machine_reset"):
            body = function("vmmfs_machine.c", name)
            self.assertIn("vmmfs_node_hold(&machine->events.node)", body)
            self.assertIn("vmmfs_node_put(&machine->events.node)", body)

    def test_mgtdevice_revoke_uses_mapping_owner(self):
        body = function("vmmfs_launch.c", "vmmfs_launch_revoke")
        self.assertIn("vm_object_pip_wait(object", body)
        self.assertIn("vm_object_page_remove(object, 0, 0, FALSE)", body)
        self.assertNotIn("vm_page_protect", body)

    def test_mount_vops_failure_cleanup(self):
        body = function("vmmfs.c", "vmmfs_mount")
        added = set(re.findall(r"vfs_add_vnodeops\([^;]*&(state->\w+)", body))
        removed = set(re.findall(r"vfs_rm_vnodeops\([^;]*&(state->\w+)", body))
        self.assertEqual(added, removed)

    def test_no_impossible_parent_hold(self):
        self.assertNotIn("vmmfs_node_hold(NULL)",
                         (SOURCE / "vmmfs_root.c").read_text())


    def test_wait_signal_aborts_without_timeout(self):
        run_c(COMMON + r"""
struct token { int held; };
struct vmmfs_node { struct vnode *vnode; struct token token;  struct lock lock; bool dead;};
struct vmmfs_launch { struct vmmfs_node node; struct token token; int result; };
#define PCATCH 1
#define PINTERLOCKED 2
#define curthread NULL
static unsigned mode, sleep_count, abort_count, reservation, removals;
static struct vmmfs_launch launch;
#define crit_enter() ((void)0)
#define crit_exit() ((void)0)
#define lwkt_gettoken(t) (++(t)->held)
#define lwkt_reltoken(t) (--(t)->held)
static void tsleep_interlock(void *channel, int flags) {
    assert(channel == &launch && !reservation);
    assert(flags == (abort_count ? 0 : PCATCH));
    reservation = 1;
    /* Completion after reservation but before the result check. */
    if (mode == 7) launch.result = 0;
}
static void tsleep_remove(void *channel) {
    (void)channel;
    assert(reservation);
    reservation = 0;
    ++removals;
}
static int tsleep(void *channel, int flags, const char *name, int ticks) {
    (void)name;
    assert(channel == &launch && ticks == 0 && !launch.token.held);
    assert(reservation && (flags & PINTERLOCKED));
    assert(flags == (PINTERLOCKED | (abort_count ? 0 : PCATCH)));
    reservation = 0;
    ++sleep_count;
    assert(sleep_count <= 3);
    if (sleep_count == 1 && mode <= 4)
        return EINTR;
    if (mode == 5 && sleep_count == 1)
        return 0; /* Spurious wakeup must not finish the wait. */
    launch.result = mode == 2 ? EINVAL : 0;
    return 0;
}
static int vmmfs_machine_abort(struct vmmfs_launch *argument) {
    assert(argument == &launch && !launch.token.held && !reservation);
    assert(++abort_count == 1);
    if (mode == 0)
        launch.result = ECANCELED; /* Cancellation won ownership. */
    else if (mode == 3)
        return EIO; /* Cleanup failure is not reported as a signal. */
    else if (mode == 4)
        launch.result = 0; /* Run completed while abort checked identity. */
    /* Modes 1/2: run owns identity but has not yet reported its result. */
    return 0;
}
int
""" + function("vmmfs_launch.c", "vmmfs_launch_wait") + r"""
int main(void) {
    for (mode = 0; mode < 9; ++mode) {
        launch.result = mode == 8 ? ENOMEM : EINPROGRESS;
        sleep_count = abort_count = reservation = removals = 0;
        int error = vmmfs_launch_wait(&launch);
        int expected = mode == 0 ? EINTR : mode == 2 ? EINVAL :
            mode == 3 ? EIO : mode == 8 ? ENOMEM : 0;
        assert(error == expected && !reservation && !launch.token.held);
        assert(abort_count == (mode <= 4));
        unsigned expected_sleeps = mode >= 7 ? 0 :
            mode == 1 || mode == 2 || mode == 5 ? 2 : 1;
        assert(sleep_count == expected_sleeps);
        assert(removals == (mode != 3));
    }
    return 0;
}
""")

    def test_run_abort_single_owner(self):
        run_c(COMMON + r"""
struct token { int held; };
struct vmmfs_node { struct vnode *vnode; struct token token; struct vmmfs_node *parent; bool dead;  struct lock lock;};
struct vnode { void *v_data; int refs; };
struct vmm_cpustate { int marker; };
typedef void *vmm_machine_t;
struct vmmfs_launch {
    struct vmmfs_node node; struct token token;
    struct vmm_cpustate cpustate;
    int result;
};
struct vmmfs_machine {
    struct vmmfs_node node; struct token token;
    struct vmmfs_launch *launch;
    vmm_machine_t machine;
    struct { unsigned count; } vcpu;
    int memory;
    struct { struct vmmfs_node node; } events;
    struct vmm_cpustate boot_state;
    unsigned runtime_references;
};
static int cleanup_count, start_count, snapshot_error, start_error, race_abort;
static int event_refs, event_count, last_event, last_error;
#define lwkt_gettoken(t) (++(t)->held)
#define lwkt_reltoken(t) (--(t)->held)
#define VMMFS_MACHINE_EVENT_BOOT_COMPLETED 1
#define VMMFS_MACHINE_EVENT_BOOT_FAILED 2
#define vmmfs_node_hold(n) ((void)(n), ++event_refs)
#define vmmfs_node_put(n) ((void)(n), --event_refs)
#define vmmfs_events_log(events, verb, format, error) do {     assert(event_refs == 1);     ++event_count; last_event = (verb); last_error = (error); } while (0)
static struct vmmfs_launch *active;
int vmmfs_machine_run(struct vmmfs_launch *);
int vmmfs_machine_abort(struct vmmfs_launch *);
static void vmmfs_launch_revoke(struct vmmfs_launch *l) { (void)l; }
static void vmmfs_machine_runtime_wait(struct vmmfs_machine *m) {
    assert(m->runtime_references == 0);
}
static void vmmfs_machine_runtime_put(struct vmmfs_machine *m) {
    assert(m->runtime_references != 0);
    --m->runtime_references;
}
static int vmmfs_memory_snapshot(int *memory) {
    (void)memory;
    if (race_abort) assert(vmmfs_machine_abort(active) == 0);
    return snapshot_error;
}
static int vmmfs_vcpu_start(void *c, unsigned n, vmm_machine_t m,
                           const struct vmm_cpustate *state) {
    (void)c;
    assert(n == 4 && m != NULL && state->marker == 42);
    ++start_count;
    return start_error;
}
static void vmmfs_machine_cleanup_stopped(struct vmmfs_machine *m) { (void)m; }
static int vmmfs_machine_release_to_stopped(struct vmmfs_machine *m) {
    ++cleanup_count;
    assert(m->runtime_references == 0);
    m->machine = NULL;
    return 0;
}
static int vmmfs_vnode_deactivate(struct vnode *vp) {
    return vmmfs_machine_abort(vp->v_data);
}
static void vmmfs_launch_complete(struct vmmfs_launch *l, int e) {
    assert(event_refs == 1);
    assert(last_event == (e == 0 ? VMMFS_MACHINE_EVENT_BOOT_COMPLETED :
                                  VMMFS_MACHINE_EVENT_BOOT_FAILED));
    assert(last_error == e);
    l->result = e;
}
static void vrele(struct vnode *vp) { assert(vp->refs > 0); --vp->refs; }
int
""" + function("vmmfs_machine.c", "vmmfs_machine_abort") + "\nint\n" +
              function("vmmfs_machine.c", "vmmfs_machine_run") + r"""
int main(void) {
    struct vmmfs_machine m = { .vcpu.count = 4 };
    struct vmmfs_launch l = { .node.parent = &m.node,
                              .cpustate.marker = 42, .result = EINPROGRESS };
    struct vnode vp = { .v_data = &l, .refs = 1 };
    active = &l;
    m.machine = &m;
    m.launch = &l; l.node.vnode = &vp;
    race_abort = 1;
    assert(vmmfs_machine_run(&l) == 0);
    assert(start_count == 1 && cleanup_count == 0);
    assert(event_count == 1 && event_refs == 0);
    assert(l.result == 0 && m.launch == NULL && vp.refs == 0);
    assert(vmmfs_machine_abort(&l) == 0);
    assert(cleanup_count == 0 && event_count == 1);
    assert(vmmfs_machine_run(&l) == EPIPE);
    m.launch = &l; l.node.vnode = &vp; vp.refs = 1; l.result = EINPROGRESS;
    assert(vmmfs_machine_abort(&l) == 0);
    assert(cleanup_count == 1 && m.machine == NULL && l.result == ECANCELED);
    assert(vmmfs_machine_run(&l) == EPIPE);
    assert(event_count == 2 && event_refs == 0);
    m.launch = &l; l.node.vnode = &vp; vp.refs = 1; m.machine = &m;
    snapshot_error = ENOMEM;
    assert(vmmfs_machine_run(&l) == ENOMEM);
    assert(cleanup_count == 2 && start_count == 1 && vp.refs == 0);
    assert(!l.token.held && !m.token.held);
    assert(event_count == 3 && event_refs == 0);
    /* A failed VCPU constructor must complete the same launch exactly once. */
    m.launch = &l; l.node.vnode = &vp; vp.refs = 1; m.machine = &m;
    l.result = EINPROGRESS;
    snapshot_error = 0; start_error = ENOMEM;
    assert(vmmfs_machine_run(&l) == ENOMEM);
    assert(cleanup_count == 3 && start_count == 2 && vp.refs == 0);
    assert(m.machine == NULL && m.launch == NULL);
    assert(m.runtime_references == 0 && l.result == ENOMEM);
    assert(event_count == 4 && event_refs == 0);
    assert(vmmfs_machine_abort(&l) == 0);
    assert(vmmfs_machine_run(&l) == EPIPE);
    assert(cleanup_count == 3 && event_count == 4 && vp.refs == 0);
    assert(!l.token.held && !m.token.held);

}
""")

    def test_no_redundant_node_clear(self):
        for path in SOURCE.glob("*.c"):
            self.assertNotRegex(path.read_text(),
                                r"bzero\(&\w+->node, sizeof\(\w+->node\)\)")

    def test_single_launch_protocol(self):
        source = (SOURCE / "vmmfs_launch.h").read_text()
        self.assertNotIn("phase", source)
        source = (SOURCE / "vmmfs_loader.c").read_text()
        self.assertNotIn("WAIT_TICKS", source)
        self.assertNotIn("vmmfs_loader_file_state", source)
        self.assertNotIn("vmmfs_loader_process_wait", source)
        machine = (SOURCE / "vmmfs_machine.c").read_text()
        for name in ("vmmfs_machine_run", "vmmfs_machine_abort"):
            body = function("vmmfs_machine.c", name)
            self.assertIn("machine->launch", body)
            self.assertNotIn("machine->node.dead", body)
        self.assertNotIn("vmmfs_boot_arm_locked", machine)


if __name__ == "__main__":
    unittest.main(verbosity=2)
