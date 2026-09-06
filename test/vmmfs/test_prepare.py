#!/usr/bin/env python3
"""Exercise private PREPARE against concurrent machine deactivation."""
import unittest
from test_regress import COMMON, function, run_c

class PrepareLifetime(unittest.TestCase):

    def test_reset_stop_cannot_destroy_a_candidate_under_configuration(self):
        run_c(COMMON + r"""
#define bzero(p, n) memset(p, 0, n)
#define VMM_MEMORY_EXIT_EMULATE 1
struct token { unsigned held; };
struct vmm_cpustate { unsigned marker; };
struct instance { bool live; unsigned index; };
typedef struct instance *vmm_vcpu_t;
typedef void *vmm_machine_t;
struct vmmfs_vcpu;
struct vmmfs_vcpu_thread {
    struct vmmfs_vcpu *group;
    struct vmm_cpustate state;
    vmm_vcpu_t vcpu;
};
struct vmmfs_vcpu {
    struct token token;
    struct vmmfs_vcpu_thread *threads;
    unsigned count, reset_waiting;
    bool reset_requested, stop_requested;
    vmm_machine_t runtime_machine;
};
static struct vmmfs_vcpu group;
static struct vmmfs_vcpu_thread threads[4];
static struct instance instances[4];
static unsigned mode, target, created, destroyed, configured;
static bool injected;
static void vmmfs_vcpu_request_stop(struct vmmfs_vcpu *);
static void stop_and_run_aps(void);
static void lwkt_gettoken(struct token *token) { ++token->held; }
static void lwkt_reltoken(struct token *token) {
    assert(token->held); --token->held;
    if (!token->held && mode == 5 && !injected &&
        threads[target].vcpu != NULL)
        stop_and_run_aps();
}
static void wakeup(void *channel) { assert(channel == &group); }
static void vmmfs_vcpu_thread_kick(struct vmmfs_vcpu_thread *thread) {
    assert(group.token.held && thread->vcpu != NULL && thread->vcpu->live);
}
static int vmm_vcpu_destroy(vmm_vcpu_t cpu) {
    assert(cpu != NULL && cpu->live);
    cpu->live = false; ++destroyed;
    return 0;
}
static void vmmfs_vcpu_thread_destroy(struct vmmfs_vcpu_thread *thread) {
    vmm_vcpu_t cpu = thread->vcpu;
    thread->vcpu = NULL;
    if (cpu != NULL) assert(vmm_vcpu_destroy(cpu) == 0);
}
static void stop_and_run_aps(void) {
    assert(!group.token.held);
    injected = true;
    vmmfs_vcpu_request_stop(&group);
    assert(group.stop_requested && !group.reset_requested);
    /* AP reset waiters may now leave their barriers and stop. */
    for (unsigned index = 1; index < group.count; ++index)
        vmmfs_vcpu_thread_destroy(&threads[index]);
}
static int vmm_vcpu_create(vmm_machine_t machine, struct vmm_cpustate *state,
                           vmm_vcpu_t *result) {
    assert(machine == &group);
    unsigned index;
    for (index = 0; index < group.count; ++index)
        if (state == &threads[index].state) break;
    assert(index < group.count);
    assert(state->marker == (index == 0 ? 42U : 0U));
    if (mode == 1 && index == target) return ENOMEM;
    assert(!instances[index].live);
    instances[index].index = index;
    instances[index].live = true;
    *result = &instances[index]; ++created;
    if (mode == 3 && index == target) stop_and_run_aps();
    return 0;
}
static int vmm_vcpu_set_memory_exit_mode(vmm_vcpu_t cpu, unsigned value) {
    assert(value == VMM_MEMORY_EXIT_EMULATE);
    if (mode == 4 && cpu->index == target) stop_and_run_aps();
    /* If reset published too soon, the AP has already freed this CPU. */
    assert(cpu->live);
    ++configured;
    return mode == 2 && cpu->index == target ? EIO : 0;
}
void
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_request_stop") + r"""
int
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_reset") + r"""
int main(void) {
    struct vmm_cpustate state = { 42 };
    for (mode = 0; mode <= 5; ++mode) {
        for (target = 0; target < 4; ++target) {
            memset(&group, 0, sizeof(group));
            memset(threads, 0, sizeof(threads));
            memset(instances, 0, sizeof(instances));
            group.count = 4; group.threads = threads;
            group.reset_requested = true; group.reset_waiting = 3;
            for (unsigned index = 0; index < 4; ++index) {
                threads[index].group = &group;
                threads[index].state.marker = 99;
            }
            created = destroyed = configured = 0; injected = false;
            int error = vmmfs_vcpu_reset(&group, &group, &state);
            assert(group.token.held == 0);
            if (mode == 0) {
                assert(error == 0 && created == 4 && configured == 4);
                assert(!group.reset_requested && group.reset_waiting == 0);
                assert(group.runtime_machine == &group);
                for (unsigned index = 0; index < 4; ++index)
                    vmmfs_vcpu_thread_destroy(&threads[index]);
            } else {
                assert(error == (mode == 1 ? ENOMEM : mode == 2 ? EIO : EINTR));
                for (unsigned index = 0; index < 4; ++index)
                    assert(threads[index].vcpu == NULL);
            }
            assert(created == destroyed);
            for (unsigned index = 0; index < 4; ++index)
                assert(!instances[index].live);
        }
    }
    return 0;
}
""")

    def test_stopped_publication_token_order(self):
        run_c(COMMON + r"""
struct token { bool held; };
struct vmmfs_node { struct token token; bool dead; };
struct vmmfs_machine {
    struct vmmfs_node node;
    struct {
        struct token token;
        void *threads;
        bool stop_requested, reset_requested;
    } vcpu;
    void *machine;
    bool runtime_releasing, runtime_released;
};
static struct vmmfs_machine machine;
static unsigned mode, invalidated;
static void lwkt_gettoken(struct token *token) {
    assert(!token->held);
    if (token == &machine.node.token)
        assert(machine.vcpu.token.held);
    else {
        assert(token == &machine.vcpu.token);
        assert(!machine.node.token.held);
    }
    token->held = true;
}
static void lwkt_reltoken(struct token *token) {
    assert(token->held);
    if (token == &machine.vcpu.token)
        assert(!machine.node.token.held);
    token->held = false;
}
static int vmmfs_machine_prepare_stopped(struct vmmfs_machine *m) {
    assert(m == &machine);
    assert(!m->node.token.held && !m->vcpu.token.held);
    return mode == 1 ? ENOMEM : 0;
}
static void vmmfs_machine_invalidate_children(struct vmmfs_machine *m) {
    assert(m == &machine && m->machine == NULL);
    assert(!m->node.token.held && !m->vcpu.token.held);
    ++invalidated;
}
static int
""" + function("vmmfs_machine.c", "vmmfs_machine_create_stopped") + r"""
int main(void) {
    for (mode = 0; mode < 7; ++mode) {
        memset(&machine, 0, sizeof(machine));
        invalidated = 0;
        machine.vcpu.stop_requested = machine.vcpu.reset_requested = true;
        machine.node.dead = mode == 2;
        machine.runtime_releasing = mode == 3;
        machine.machine = mode == 4 || mode == 6 ? &machine : NULL;
        machine.runtime_released = mode == 6;
        machine.vcpu.threads = mode == 5 ? &machine : NULL;
        int error = vmmfs_machine_create_stopped(&machine);
        assert(!machine.node.token.held && !machine.vcpu.token.held);
        if (mode >= 1 && mode <= 4) {
            assert(error == (mode == 1 ? ENOMEM : EBUSY));
            assert(invalidated == 0);
            assert(machine.vcpu.stop_requested && machine.vcpu.reset_requested);
        } else {
            assert(error == 0 && invalidated == 1);
            assert(machine.machine == NULL);
            assert(!machine.runtime_releasing && !machine.runtime_released);
            assert(machine.vcpu.stop_requested == (mode == 5));
            assert(machine.vcpu.reset_requested == (mode == 5));
        }
    }
}
""")

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
