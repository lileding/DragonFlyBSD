#!/usr/bin/env python3
"""Loader child descriptor handoff using its production C entry function."""
import unittest
from test_regress import COMMON, function, run_c


class Loader(unittest.TestCase):
    def test_inherited_files_closed_before_launch_install(self):
        run_c(COMMON + r"""
struct vmmfs_node { unsigned references;  struct lock lock; bool dead;};
struct token { int unused; };
struct vmmfs_launch { struct vmmfs_node node; struct token token; };
struct file { unsigned references; };
struct vmmfs_loader_process {
    char script[4096];
    struct file *file;
    struct vmmfs_launch *launch;
};
struct trapframe { int unused; };
struct lwp;
struct usched { void (*acquire_curproc)(struct lwp *); };
struct proc { struct usched *p_usched; };
struct lwp { struct proc *lwp_proc; };
struct thread { struct lwp *td_lwp; };
static struct thread thread;
#define curthread (&thread)
#define M_VMMFS 0
static unsigned closed, installed, executed, aborted, released, acquired, exited;
static int close_error, install_error, exec_error;
static struct file file;
static struct vmmfs_launch launch;
static struct vmmfs_loader_process process;
int kern_closefrom(int first) {
    assert(first == 3 && installed == 0);
    assert(file.references == 1 && launch.node.references == 1);
    ++closed; return close_error;
}
static int vmmfs_loader_install_fd(struct file *f, int target) {
    assert(f == &file && target == 3 && closed == 1 && !close_error);
    ++installed; return install_error;
}
static int fp_close(struct file *f) {
    assert(f == &file && f->references == 1);
    --f->references; return 0;
}
static int vmmfs_loader_exec_shell(const char *script) {
    assert(script == process.script && installed == 1 && !install_error);
    ++executed; return exec_error;
}
static void vmmfs_launch_cancel(struct vmmfs_launch *l) {
    assert(l == &launch && l->node.references == 1);
    ++aborted;
}
#define kprintf(...) assert(0)
static void kfree(void *p, int type) {
    assert(p == &process && type == M_VMMFS); ++released;
}
static void acquire(struct lwp *l) {
    assert(l == thread.td_lwp && launch.node.references == 1); ++acquired;
}
static void vmmfs_launch_put(struct vmmfs_launch *l) {
    struct vmmfs_node *node = &l->node;
    assert(node == &launch.node && node->references == 1 && acquired == 1);
    --node->references;
}
static void exit1(int status) { assert(status == 1); ++exited; }
static void
""" + function("vmmfs_loader.c", "vmmfs_loader_child") + r"""
int main(void) {
    struct usched usched = { acquire };
    struct proc proc = { &usched };
    struct lwp lwp = { &proc };
    thread.td_lwp = &lwp;
    for (unsigned mode = 0; mode < 4; ++mode) {
        closed = installed = executed = aborted = released = acquired = exited = 0;
        close_error = mode == 1 ? EINTR : 0;
        install_error = mode == 2 ? EMFILE : 0;
        exec_error = mode == 3 ? ENOENT : 0;
        file.references = launch.node.references = 1;
        process.file = &file; process.launch = &launch;
        vmmfs_loader_child(&process, NULL);
        assert(closed == 1 && released == 1 && acquired == 1);
        assert(file.references == 0 && launch.node.references == 0);
        assert(installed == (mode != 1));
        assert(executed == (mode == 0 || mode == 3));
        assert(aborted == (mode != 0) && exited == (mode != 0));
    }
}
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
