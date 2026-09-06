#!/usr/bin/env python3
"""Exercise private PREPARE against concurrent machine deactivation."""
import unittest
from test_regress import COMMON, function, run_c

class PrepareLifetime(unittest.TestCase):
    def test_private_prepare_retains_vcpu(self):
        run_c(COMMON + r"""
struct token { int valid, held; };
struct vmmfs_node { struct token token; struct vmmfs_node *parent; bool dead; };
struct vnode { void *v_data; unsigned refs; };
struct vmmfs_memory { struct vmmfs_node node; uint64_t size; void *object, *run_vmspace; bool mapped; };
struct cpu { struct token token; void *threads; unsigned active_count, count; };
typedef void *vmm_machine_t;
struct vmmfs_machine {
    struct vmmfs_node node; struct vmmfs_memory memory; struct cpu vcpu;
    void *mount; vmm_machine_t machine; struct vnode *vcpu_vnode, *launch_vnode;
    bool runtime_releasing, runtime_released; unsigned runtime_references;
    int platform, pciroot, serialroot, rtc;
};
struct vmmfs_launch { struct vmmfs_node node; };
static struct vmmfs_machine machine;
static struct vmmfs_launch launch;
static struct vnode cpu_vnode, launch_vnode;
static unsigned mode, stage, fail_stage;
static void lwkt_gettoken(struct token *t) { assert(t->valid); ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->valid && t->held); --t->held; }
static void vref(struct vnode *v) { assert(v->refs); ++v->refs; }
static void vrele(struct vnode *v) {
    assert(v->refs);
    if (--v->refs == 0 && v == &cpu_vnode) machine.vcpu.token.valid = 0;
}
#define bzero(p,n) memset(p,0,n)
static void kprintf(const char *fmt, int e) { (void)fmt; (void)e; }
static int next(void) { return ++stage == fail_stage ? ENOMEM : 0; }
static int vmmfs_launch_create(void *m, struct vmmfs_node *parent, uint64_t size,
    struct vnode **v) {
    (void)m; (void)size;
    int error = next(); if (error) return error;
    launch.node.parent = parent; launch_vnode.v_data = &launch;
    launch_vnode.refs = 1; *v = &launch_vnode;
    return 0;
}
static int vmmfs_memory_prepare(struct vmmfs_memory *m, uint64_t size) {
    (void)size;
    if (mode == 1 || mode == 2) {
        /* Namespace owner drops its child during a sleeping allocation. */
        machine.node.dead = mode == 1;
        machine.vcpu_vnode = NULL;
        vrele(&cpu_vnode);
    }
    m->object = m; m->run_vmspace = m;
    return next();
}
static int vmm_machine_create(void *memory, vmm_machine_t *v) { *v = memory; return next(); }
static int vmmfs_memory_map(struct vmmfs_memory *m) { m->mapped = true; return next(); }
static int vmmfs_launch_map(struct vmmfs_launch *l, void *o) { (void)l; (void)o; return next(); }
static int vmm_machine_create_irqchip(void *m) { (void)m; return next(); }
static int vmm_machine_create_pit(void *m) { (void)m; return next(); }
static int vmmfs_platform_x64_prepare(int *p, struct vmmfs_memory *m,
    unsigned count, int *pci, int *serial) {
    (void)p; (void)m; (void)count; (void)pci; (void)serial; return next();
}
static int vmmfs_rtc_start(int *r, void *m) { (void)r; (void)m; return next(); }
static int vmmfs_pciroot_start(int *r, void *m) { (void)r; (void)m; return next(); }
static int vmmfs_serialroot_start(int *r, void *m) { (void)r; (void)m; return next(); }
static int vmmfs_platform_x64_start(int *r, void *m) { (void)r; (void)m; return next(); }
static void vmmfs_machine_runtime_put(struct vmmfs_machine *m) { assert(m->runtime_references); --m->runtime_references; }
static int vmmfs_machine_abort(struct vmmfs_launch *l) {
    (void)l;
    if (machine.launch_vnode) vrele(machine.launch_vnode);
    machine.launch_vnode = NULL; machine.machine = NULL; return 0;
}
static void vmmfs_launch_revoke(struct vmmfs_launch *l) { (void)l; }
static int vmm_machine_destroy(void *m) { (void)m; return 0; }
static void vmmfs_memory_release(struct vmmfs_memory *m) { (void)m; }
static int vmmfs_vnode_deactivate(struct vnode *v) { (void)v; return 0; }
int
""" + function("vmmfs_machine.c", "vmmfs_machine_boot") + r"""
int main(void) {
    for (mode = 0; mode != 3; ++mode) {
        for (fail_stage = 0; fail_stage <= 12; ++fail_stage) {
            memset(&machine, 0, sizeof(machine)); memset(&launch_vnode, 0, sizeof(launch_vnode));
            machine.node.token.valid = machine.vcpu.token.valid = 1;
            machine.memory.size = 4096; machine.vcpu.count = 1;
            cpu_vnode.refs = 1; machine.vcpu_vnode = &cpu_vnode;
            stage = 0; struct vnode *result = NULL;
            int error = vmmfs_machine_boot(&machine, &result);
            assert(machine.node.token.held == 0 && machine.vcpu.token.held == 0);
            assert(machine.runtime_references == 0);
            if (error == 0) {
                assert(mode == 0 && fail_stage == 0 && result == &launch_vnode);
                vmmfs_machine_abort(&launch); vrele(result);
            } else {
                assert(result == NULL);
            }
            assert(launch_vnode.refs == 0);
            if (machine.vcpu_vnode != NULL) {
                assert(cpu_vnode.refs == 1); vrele(&cpu_vnode);
            }
            assert(cpu_vnode.refs == 0 && machine.vcpu.token.valid == 0);
        }
    }
    return 0;
}
""")


    def test_loader_handoff_retains_its_node(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_node { struct token token; bool dead; unsigned references; };
struct vmmfs_loader { struct vmmfs_node node; };
struct vmmfs_launch { int unused; };
struct vnode { unsigned references; void *v_data; };
struct vmmfs_machine {
    struct vmmfs_node node;
    struct vmmfs_loader loader;
    struct vnode *loader_vnode;
};
struct ucred { int unused; };
static struct vmmfs_machine machine;
static struct vnode loader_vnode, launch_vnode;
static struct vmmfs_launch launch;
static unsigned mode, aborts, runs, waits;
void lwkt_gettoken(struct token *token) { ++token->held; }
void lwkt_reltoken(struct token *token) { assert(token->held); --token->held; }
void vref(struct vnode *vnode) { assert(vnode->references); ++vnode->references; }
static void vrele(struct vnode *vnode) {
    assert(vnode->references);
    if (--vnode->references == 0 && vnode == &loader_vnode) {
        assert(machine.loader.node.references == 1);
        machine.loader.node.references = 0;
    }
}
static void remove_loader(void) {
    assert(machine.loader_vnode == &loader_vnode);
    machine.loader.node.dead = true;
    machine.loader_vnode = NULL;
    vrele(&loader_vnode);
}
static int vmmfs_machine_boot(struct vmmfs_machine *m, struct vnode **result) {
    assert(m == &machine && !m->node.token.held);
    *result = NULL;
    if (mode == 1) {
        /* Cancellation and rmdir after boot's admission, before handoff. */
        remove_loader();
    }
    if (mode == 2) return EBUSY;
    launch_vnode.references = 1; launch_vnode.v_data = &launch;
    *result = &launch_vnode; return 0;
}
static int vmmfs_loader_run(struct vmmfs_loader *loader, struct vnode *vnode,
    struct ucred *cred) {
    (void)cred;
    assert(loader == &machine.loader && vnode == &launch_vnode);
    if (mode == 3) remove_loader(); /* A sleeping allocation in loader_run. */
    assert(loader->node.references != 0); /* Token must still be initialized. */
    ++runs;
    return loader->node.dead ? EINVAL : mode == 4 ? ENOMEM : 0;
}
static int vmmfs_launch_wait(struct vmmfs_launch *l) {
    assert(l == &launch); ++waits; return mode == 5 ? EINTR : 0;
}
static int vmmfs_machine_abort(struct vmmfs_launch *l) {
    assert(l == &launch); ++aborts; return 0;
}
static int
""" + function("vmmfs_machine.c", "vmmfs_machine_start") + r"""
int main(void) {
    for (mode=0; mode<=6; ++mode) {
        memset(&machine,0,sizeof(machine));
        loader_vnode = (struct vnode){1,&machine.loader};
        launch_vnode = (struct vnode){0,NULL};
        machine.loader.node.references = 1;
        machine.loader_vnode = &loader_vnode;
        aborts=runs=waits=0;
        if (mode == 6) remove_loader();
        int error = vmmfs_machine_start(&machine, NULL);
        int expected = mode==1 || mode==3 ? EINVAL : mode==2 ? EBUSY :
            mode==4 ? ENOMEM : mode==5 ? EINTR : mode==6 ? ENOENT : 0;
        assert(error==expected);
        assert(aborts==(mode==1 || mode==3 || mode==4));
        assert(waits==(mode==0 || mode==5));
        assert(runs==(mode!=2 && mode!=6));
        assert(!machine.node.token.held && !launch_vnode.references);
        if (machine.loader_vnode != NULL) {
            assert(loader_vnode.references==1); remove_loader();
        }
        assert(loader_vnode.references==0 && machine.loader.node.references==0);
    }
}
""")

if __name__ == "__main__":
    unittest.main(verbosity=2)
