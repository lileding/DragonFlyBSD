"""Work admission: production macro/deactivate, pthread lockmgr surrogate."""
import pathlib
import subprocess
import tempfile
import unittest
from test_regress import SOURCE, function, run_c, COMMON


def call_macro():
    text = (SOURCE / "vmmfs_node.h").read_text()
    begin = text.index("#define VMMFS_WORK(")
    end = text.index("/* Object references", begin)
    return text[begin:end] + "\n"


class Work(unittest.TestCase):
    def test_admitted_callbacks_do_not_repeat_dead_gate(self):
        for owner, verbs in {
            'root': ('get_item', 'read_item', 'create_item'),
            'pciroot': ('get_item', 'read_item', 'create_item'),
            'pcislot_descriptor': ('store',),
            'machine': ('get_item',),
            'pcislot': ('get_item', 'read_item'),
            'serialroot': ('get_item', 'create_port'),
            'vcpu': ('load', 'store'), 'memory': ('load', 'store'),
            'launch': ('pager_fault',),
            'pcislot_resource': ('enabled',),
            'loader': ('load', 'store'), 'machine_id': ('load',),
            'events': ('store',),
        }.items():
            for verb in verbs:
                body = function('vmmfs_' + owner + '.c', 'vmmfs_' + owner + '_' + verb)
                self.assertNotIn('dead', body, (owner, verb))
        self.assertNotIn('dead', function('vmmfs_machine.c', 'vmmfs_machine_cleanup_partial'))

    def test_data_tokens_belong_to_objects(self):
        node = (SOURCE / "vmmfs_node.h").read_text()
        self.assertNotIn("struct lwkt_token token;", node)
        for name in ("root", "machine", "pciroot", "pcislot", "serialroot", "launch", "pcislot_resources"):
            filename = {"root": "vmmfs_root.c", "pcislot_resources": "vmmfs_pcislot_resource.c"}.get(name, "vmmfs_" + name + ".h")
            text = (SOURCE / filename).read_text()
            fields = text.split("struct vmmfs_" + name + " {", 1)[1].split("};", 1)[0]
            self.assertIn("struct lwkt_token token;", fields, name)
        for path in SOURCE.glob("*.c"):
            self.assertNotIn("->node.token", path.read_text(), path.name)
        for name in ("vmmfs_vnode_deactivate", "vmmfs_node_reclaim", "vmmfs_node_put"):
            self.assertNotIn("lwkt_", function("vmmfs_node.c", name))

    def test_single_evaluation_and_errors(self):
        run_c("#define VMMFS_TEST_CUSTOM_LOCK\n" + COMMON + r'''
#define LK_SHARED 1
#define LK_RELEASE 2
struct lock { int held; };
struct vmmfs_node {
    struct lock lock; bool dead;
    int (*store)(struct vmmfs_node *, int);
};
static struct vmmfs_node node;
static int evaluations, arguments, calls, lock_error;
static int lockmgr(struct lock *lock, int operation) {
    if (operation == LK_SHARED) {
        if (lock_error) return lock_error;
        assert(!lock->held); lock->held = 1;
    } else { assert(lock->held); lock->held = 0; }
    return 0;
}
static struct vmmfs_node *object(void) { ++evaluations; return &node; }
static int argument(void) { ++arguments; return 7; }
static int store(struct vmmfs_node *n, int value) {
    assert(n == &node && n->lock.held && value == 7); ++calls; return EIO;
}
''' + call_macro() + r'''
int main(void) {
    node.store = store;
    assert(VMMFS_CALL(object(), store, argument()) == EIO);
    assert(evaluations == 1 && arguments == 1 && calls == 1 && !node.lock.held);
    node.dead = true;
    assert(VMMFS_CALL(object(), store, argument()) == ENOENT);
    assert(arguments == 1 && calls == 1 && !node.lock.held);
    node.dead = false; lock_error = EINTR;
    assert(VMMFS_CALL(object(), store, argument()) == EINTR);
    assert(arguments == 1 && calls == 1 && !node.lock.held);
    lock_error = 0; node.store = NULL;
    assert(VMMFS_CALL(object(), store, argument()) == EOPNOTSUPP);
    assert(arguments == 1 && calls == 1 && !node.lock.held);
}
''')

    def test_work_and_closure_are_mutually_exclusive(self):
        source = "#define VMMFS_TEST_CUSTOM_LOCK\n" + COMMON + r"""
#include <pthread.h>
#include <stdatomic.h>
#define LK_SHARED 1
#define LK_EXCLUSIVE 2
#define LK_RELEASE 3
#define DTYPE_VNODE 1
#define CINV_CHILDREN 1
struct lock { pthread_rwlock_t value; };
struct token { int unused; };
struct vmmfs_node {
    struct lock lock; struct token token; bool dead;
    struct vmmfs_node *parent;
    int (*deactivate)(struct vmmfs_node *);
    int (*load)(struct vmmfs_node *);
};
struct vnode { struct vmmfs_node *v_data; atomic_int refs; };
static struct { void *p_ucred; } proc0;
static struct vmmfs_node node;
static struct vnode vnode = { .v_data = &node, .refs = 1 };
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int entered, proceed, veto, closed, callback_count;
static _Thread_local bool exclusive;
#define curthread NULL
static int lockmgr(struct lock *lock, int operation) {
    if (operation == LK_SHARED) return pthread_rwlock_rdlock(&lock->value);
    if (operation == LK_EXCLUSIVE) {
        int error = pthread_rwlock_wrlock(&lock->value);
        if (!error) exclusive = true;
        return error;
    }
    exclusive = false;
    return pthread_rwlock_unlock(&lock->value);
}
static void vref(struct vnode *v) { atomic_fetch_add(&v->refs, 1); }
static void vrele(struct vnode *v) { assert(atomic_fetch_sub(&v->refs, 1) > 1); }
static int fdrevoke(struct vnode *v, int type, void *cred) {
    (void)v; (void)type; (void)cred; assert(!exclusive); ++closed; return 0;
}
static void cache_inval_vp(struct vnode *v, int flags) { (void)v; (void)flags; assert(!exclusive); }
static void rendezvous(void) {
    assert(pthread_mutex_lock(&mutex) == 0);
    entered = 1; pthread_cond_broadcast(&condition);
    while (!proceed) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);
}
static int load(struct vmmfs_node *n) {
    assert(!n->dead); rendezvous(); assert(!n->dead); return 0;
}
static int close_node(struct vmmfs_node *n) {
    assert(n->dead && !exclusive); ++callback_count;
    if (veto) { rendezvous(); return EBUSY; }
    return 0;
}
""" + call_macro() + "\nint\n" + function(
            "vmmfs_node.c", "vmmfs_vnode_deactivate") + r"""
static void *reader(void *arg) {
    (void)arg; assert(VMMFS_CALL(&node, load) == 0); return NULL;
}
static void *closer(void *arg) {
    (void)arg; assert(vmmfs_vnode_deactivate(&vnode) == EBUSY); return NULL;
}
static void await_entry(void) {
    pthread_mutex_lock(&mutex);
    while (!entered) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);
}
static void release_callback(void) {
    pthread_mutex_lock(&mutex); proceed = 1;
    pthread_cond_broadcast(&condition); pthread_mutex_unlock(&mutex);
}
int main(void) {
    pthread_t thread;
    pthread_rwlock_init(&node.lock.value, NULL);
    node.load = load; node.deactivate = close_node;
    pthread_create(&thread, NULL, reader, NULL); await_entry();
    assert(pthread_rwlock_trywrlock(&node.lock.value) == EBUSY);
    release_callback(); pthread_join(thread, NULL);
    entered = proceed = 0; veto = 1;
    pthread_create(&thread, NULL, closer, NULL); await_entry();
    assert(VMMFS_CALL(&node, load) == ENOENT);
    assert(vmmfs_vnode_deactivate(&vnode) == EBUSY);
    assert(callback_count == 1 && atomic_load(&vnode.refs) == 2);
    assert(pthread_rwlock_tryrdlock(&node.lock.value) == 0);
    assert(node.dead);
    pthread_rwlock_unlock(&node.lock.value);
    release_callback(); pthread_join(thread, NULL);
    assert(!node.dead && !closed && callback_count == 1);
    assert(VMMFS_CALL(&node, load) == 0);
    veto = 0;
    assert(vmmfs_vnode_deactivate(&vnode) == 0);
    assert(node.dead && closed == 1 && callback_count == 2);
    assert(VMMFS_CALL(&node, load) == ENOENT);
    assert(vmmfs_vnode_deactivate(&vnode) == EBUSY);
    assert(callback_count == 2 && atomic_load(&vnode.refs) == 1);
    pthread_rwlock_destroy(&node.lock.value);
}
"""
        with tempfile.TemporaryDirectory(prefix="vmmfs-work-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(source)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-pthread", str(path / "test.c"), "-o", str(path / "test")],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(path / "test")], check=True, timeout=15)
