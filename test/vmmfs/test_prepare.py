#!/usr/bin/env python3
"""Exercise private runtime construction and worker handoffs."""
import unittest
from test_regress import COMMON, function, run_c

class PrepareLifetime(unittest.TestCase):

    def test_ap_finishes_previous_barrier_before_a_second_reset(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_vcpu;
struct vmmfs_vcpu_thread {
    struct vmmfs_vcpu *group;
    unsigned index;
    void *vcpu;
};
struct vmmfs_vcpu {
    struct token token;
    struct vmmfs_vcpu_thread *threads;
    unsigned count, reset_waiting;
    bool reset_requested, stop_requested;
};
static struct vmmfs_vcpu group;
static struct vmmfs_vcpu_thread threads[4];
static unsigned announced, finished;
static int identities[4];
static bool rebuilding;
static void *curthread;
#define PINTERLOCKED 0
#define VMMFS_MACHINE_EVENT_RESET_FAILED 0
#define VMMFS_MACHINE_EVENT_RESET_COMPLETED 1
struct vmmfs_machine { int events; };
static struct vmmfs_machine machine;
static void vmmfs_vcpu_request_reset(struct vmmfs_vcpu *);
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static void vmmfs_vcpu_thread_destroy(struct vmmfs_vcpu_thread *thread) {
    assert(thread->vcpu != NULL);
    thread->vcpu = NULL;
}
static void vmmfs_vcpu_thread_kick(struct vmmfs_vcpu_thread *thread) {
    assert(thread->vcpu != NULL);
}
static void wakeup(void *channel) {
    assert(channel == &group);
    if (rebuilding) return;
    ++announced;
    /* Other APs already reached this barrier; BSP commits the new set. */
    rebuilding = true;
    group.reset_waiting = 0;
    group.reset_requested = false;
    for (unsigned index = 0; index < group.count; ++index)
        threads[index].vcpu = &identities[index];
    /* A new request arrives before this AP reevaluates its old wait. */
    vmmfs_vcpu_request_reset(&group);
    /* Earlier APs may already have joined the next barrier. */
    for (unsigned index = 1; index < announced; ++index)
        threads[index].vcpu = NULL;
    group.reset_waiting = announced - 1;
    rebuilding = false;
}
static void tsleep_interlock(void *p, int flags) { (void)flags; assert(p == &group); }
static void crit_enter(void) {}
static void crit_exit(void) {}
static void tsleep_remove(void *p) { (void)p; ++finished; }
static int tsleep(void *p, int flags, const char *name, int time) {
    (void)p; (void)flags; (void)name; (void)time;
    assert(!"AP slept across an already committed reset");
    return 0;
}
static bool vmmfs_vcpu_is_stop_requested(struct vmmfs_vcpu *p) {
    return p->stop_requested;
}
static struct vmmfs_machine *vmmfs_vcpu_machine(struct vmmfs_vcpu *p) {
    (void)p; return &machine;
}
static int vmmfs_machine_vcpu_reset(struct vmmfs_machine *p) {
    (void)p; assert(!"test must use the AP path"); return 0;
}
static void vmmfs_vcpu_request_stop(struct vmmfs_vcpu *p) { p->stop_requested = true; }
static void vmmfs_events_log(int *events, int verb, const char *format, ...) {
    (void)events; (void)verb; (void)format;
}
void
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_request_reset") + r"""
static void
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_thread_reset") + r"""
int main(void) {
    group.threads = threads; group.count = 4;
    for (unsigned index = 0; index < 4; ++index) {
        threads[index].group = &group; threads[index].index = index;
        threads[index].vcpu = &identities[index];
    }
    /* Every AP must leave its old barrier even after another AP joins anew. */
    for (unsigned index = 1; index < 4; ++index) {
        group.reset_requested = true;
        vmmfs_vcpu_thread_reset(&threads[index]);
        assert(threads[index].vcpu == &identities[index]);
        assert(group.reset_requested && !group.stop_requested);
        assert(group.reset_waiting == index - 1);
    }
    assert(announced == 3 && finished == 3 && group.token.held == 0);
    return 0;
}
""")

    def test_reset_stop_cannot_destroy_a_candidate_under_configuration(self):
        run_c(COMMON + r"""
#include <stdlib.h>
#define M_VMMFS 0
#define M_WAITOK 0
#define M_ZERO 0
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
static unsigned allocations;
static void *kmalloc(size_t size, int type, int flags) {
    (void)type; (void)flags; ++allocations;
    return calloc(1, size);
}
static void kfree(void *pointer, int type) {
    (void)type; assert(pointer != NULL && allocations == 1);
    --allocations; free(pointer);
}
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
    for (unsigned index = 0; index < group.count; ++index)
        assert(threads[index].vcpu == NULL);
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
            } else if (mode == 5) {
                /* Publication already committed; stop is worker-owned. */
                assert(error == 0 && group.stop_requested);
                assert(created == 4 && configured == 4);
                assert(group.runtime_machine == &group);
                vmmfs_vcpu_thread_destroy(&threads[0]);
            } else {
                assert(error == (mode == 1 ? ENOMEM : mode == 2 ? EIO : EINTR));
                for (unsigned index = 0; index < 4; ++index)
                    assert(threads[index].vcpu == NULL);
            }
            assert(created == destroyed && allocations == 0);
            for (unsigned index = 0; index < 4; ++index)
                assert(!instances[index].live);
        }
    }
    return 0;
}
""")

    def test_stopped_publication_token_order(self):
        run_c(COMMON + r"""
struct token { bool held, live; };
struct vmmfs_node { struct token token; bool dead; struct vnode *vnode;  struct lock lock;};
struct vnode { void *v_data; unsigned refs; };
struct vmmfs_stopped { struct vmmfs_node node; };
struct vmmfs_machine {
    struct vmmfs_node node; struct token token;
    struct {
        struct vmmfs_node node; struct token token;
        void *threads;
        bool stop_requested, reset_requested;
    } vcpu;
    void *machine;
    struct vmmfs_stopped *stopped;
    bool runtime_releasing, runtime_released;
};
static struct vmmfs_machine machine;
static struct vmmfs_stopped candidate, existing;
static struct vnode candidate_vnode, existing_vnode, cpu_vnode, self_vnode;
static unsigned mode, invalidated, allocated;
static int replacement;
static void lwkt_gettoken(struct token *token) {
    assert(!token->held);
    if (token == &machine.token)
        assert(machine.vcpu.token.held || allocated == 0 ||
            (machine.machine == NULL && machine.stopped != NULL));
    else {
        assert(token == &machine.vcpu.token);
        assert(token->live);
        assert(!machine.token.held);
    }
    token->held = true;
}
static void lwkt_reltoken(struct token *token) {
    assert(token->held);
    if (token == &machine.vcpu.token)
        assert(!machine.token.held);
    token->held = false;
}
static int vmmfs_stopped_create(struct vmmfs_node *parent,
    struct vmmfs_stopped **out) {
    assert(parent == &machine.node && machine.node.lock.held);
    assert(cpu_vnode.refs == 1);
    assert(!machine.token.held && !machine.vcpu.token.held);
    if (mode == 1) return ENOMEM;
    assert(allocated == 0); ++allocated;
    candidate_vnode.v_data = &candidate; candidate.node.vnode = &candidate_vnode; *out = &candidate;
    if (mode == 10) {
        /* Another completion and boot won while allocation was blocked. */
        machine.machine = &replacement; machine.runtime_released = false;
    }
    if (mode == 11 || mode == 12) {
        /* A competing completion finishes, but shared protection excludes
         * rmdir while candidate allocation is in progress. */
        assert(machine.node.lock.held);
        machine.machine = NULL; machine.runtime_released = false;
    }
    return 0;
}
static void vmmfs_vnode_discard(struct vnode *vnode) {
    assert(vnode == &candidate_vnode && vnode->v_data == &candidate);
    assert(!machine.token.held && !machine.vcpu.token.held);
    vnode->v_data = NULL;
}
static void vmmfs_node_put(struct vmmfs_node *);
static bool vmmfs_node_deactivate(struct vmmfs_node *n) {
    if (!n) return true;
    vmmfs_vnode_discard(n->vnode); vmmfs_node_put(n); return true;
}
static void vmmfs_node_put(struct vmmfs_node *node) {
    assert(node == &candidate.node && allocated == 1); --allocated;
}
#define CINV_CHILDREN 1
static void vhold(struct vnode *vnode) {
    assert(vnode == &self_vnode && machine.token.held);
    assert(vnode->refs == 0);
    ++vnode->refs;
}
static void cache_inval_vp(struct vnode *vnode, int flags) {
    assert(vnode == &self_vnode && vnode->refs == 1);
    assert(flags == CINV_CHILDREN && machine.machine == NULL);
    assert(!machine.node.lock.held);
    assert(machine.stopped != NULL);
    assert(!machine.token.held && !machine.vcpu.token.held);
    ++invalidated;
}
static void vdrop(struct vnode *vnode) {
    assert(vnode == &self_vnode && vnode->refs == 1);
    assert(!machine.token.held);
    --vnode->refs;
}
static int
""" + function("vmmfs_machine.c", "vmmfs_machine_create_stopped") + r"""
int main(void) {
    for (mode = 0; mode < 13; ++mode) {
        memset(&machine, 0, sizeof(machine));
        machine.node.vnode = &self_vnode;
        invalidated = 0; existing.node.vnode = &existing_vnode;
        assert(allocated == 0);
        machine.vcpu.token.live = true;
        cpu_vnode.refs = 1;
        machine.vcpu.node.vnode = &cpu_vnode;
        machine.vcpu.stop_requested = machine.vcpu.reset_requested = true;
        machine.node.dead = mode == 2 || mode == 7 || mode == 8;
        machine.runtime_releasing = mode == 3;
        machine.machine = mode == 4 || mode >= 6 ? &machine : NULL;
        machine.runtime_released = mode >= 6;
        machine.vcpu.threads = mode == 5 || mode == 8 ? &machine : NULL;
        if (mode == 9) machine.stopped = &existing;
        int error = vmmfs_machine_create_stopped(&machine);
        assert(!machine.token.held && !machine.vcpu.token.held);
        assert(self_vnode.refs == 0 && !machine.node.lock.held);
        assert(machine.node.dead == (mode == 2 || mode == 7 || mode == 8));
        assert(cpu_vnode.refs == (1));
        assert(machine.vcpu.token.live == (true));
        if ((mode >= 1 && mode <= 4) || mode == 10) {
            assert(error == (mode == 1 ? ENOMEM : EBUSY));
            assert(invalidated == 0);
            assert(!machine.stopped && !allocated);
            assert(machine.vcpu.stop_requested && machine.vcpu.reset_requested);
        } else {
            assert(error == 0 && invalidated == 1);
            assert(machine.machine == NULL);
            assert(!machine.runtime_releasing && !machine.runtime_released);
            assert(machine.vcpu.stop_requested == (mode == 5 || mode == 8));
            assert(machine.vcpu.reset_requested == (mode == 5 || mode == 8));
            assert(machine.stopped == (mode == 9 ? &existing : &candidate));
            assert(allocated == (mode == 9 ? 0 : 1));
            if (allocated) {
                vmmfs_vnode_discard(&candidate_vnode);
                vmmfs_node_put(&candidate.node);
            }
        }
    }
}
""")

    def test_empty_vmspace_claim_precedes_memory_prepare(self):
        body = function("vmmfs_machine.c", "vmmfs_machine_boot")
        self.assertIn("if (!atomic_cmpset_ptr(&machine->machine, NULL, runtime))", body)
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vnode { unsigned refs; };
struct vmmfs_node { struct vnode *vnode; struct lock lock; bool dead; };
struct vmspace { int unused; };
struct vmmfs_memory { uint64_t size; void *object; struct vmspace *run_vmspace; };
struct cpu { struct token token; void *threads; unsigned active_count, count; bool stop_requested; };
typedef void *vmm_machine_t;
struct vmmfs_machine {
    struct vmmfs_node node; struct token token; struct vmmfs_memory memory; struct cpu vcpu;
    vmm_machine_t machine; struct vmmfs_launch *launch;
    bool runtime_releasing, runtime_released; unsigned runtime_references;
    int platform, pciroot, serialroot, rtc;
};
struct vmmfs_launch { struct vmmfs_node node; };
static struct vmmfs_machine machine;
static struct vmmfs_launch launch;
static struct vnode launch_vnode;
static struct vmspace space;
static int competitor;
static unsigned mode, stage, fail_stage, spaces, runtimes;
#define VM_MIN_USER_ADDRESS 0
#define VMMFS_GPA_MAX 127
static int next(void) { return ++stage == fail_stage ? ENOMEM : 0; }
static void lwkt_gettoken(struct token *t) { ++t->held; }
static void lwkt_reltoken(struct token *t) { assert(t->held); --t->held; }
static void vref(struct vnode *v) { assert(v->refs); ++v->refs; }
static void vrele(struct vnode *v) { assert(v->refs); --v->refs; }
static struct vmspace *vmspace_alloc(int low, int high) {
    assert(low==0 && high==127); if(next()) return NULL; ++spaces; return &space;
}
static void pmap_del_all_cpus(struct vmspace *v) { assert(v==&space); }
static void vmspace_rel(struct vmspace *v) { assert(v==&space && spaces==1); --spaces; }
static int vmm_machine_create(struct vmspace *v, vmm_machine_t *out) {
    assert(v==&space); int e=next(); if(e) return e;
    ++runtimes; *out=v; if(mode==1) machine.machine=&competitor; return 0;
}
static int vmm_machine_destroy(vmm_machine_t v) { assert(v==&space && runtimes==1); --runtimes; return 0; }
static bool atomic_cmpset_ptr(vmm_machine_t *p, void *old, void *value) {
    assert(machine.token.held && machine.vcpu.token.held);
    if(*p!=old) return false;
    *p=value; return true;
}
static int vmmfs_launch_create(struct vmmfs_node *parent, uint64_t size, struct vmmfs_launch **out) {
    assert(parent==&machine.node && size==4096 && machine.machine==&space);
    assert(machine.runtime_references==1 && machine.memory.run_vmspace==&space);
    if(mode==2) machine.vcpu.stop_requested=true;
    int e=next(); if(e) return e;
    launch_vnode.refs=1; launch.node.vnode=&launch_vnode; *out=&launch; return 0;
}
static int vmmfs_memory_prepare(struct vmmfs_memory *m, uint64_t size) {
    assert(m==&machine.memory && size==4096 && m->run_vmspace==&space);
    assert(machine.machine==&space && machine.launch==&launch);
    int e=next(); if(!e) m->object=m; return e;
}
static int vmmfs_memory_map(struct vmmfs_memory *m) { assert(m==&machine.memory); return next(); }
static int vmmfs_launch_map(struct vmmfs_launch *l, void *o) { assert(l==&launch && o==&machine.memory); return next(); }
static int vmm_machine_create_irqchip(void *m) { assert(m==&space); return next(); }
static int vmm_machine_create_pit(void *m) { assert(m==&space); return next(); }
static int vmmfs_platform_x64_prepare(int *p, struct vmmfs_memory *m, unsigned count, int *pci, int *serial) {
    (void)p; (void)pci; (void)serial; assert(m==&machine.memory && count==1); return next();
}
static int vmmfs_rtc_start(int *r, void *m) { (void)r; (void)m; return next(); }
static int vmmfs_pciroot_start(int *r, void *m) { (void)r; (void)m; return next(); }
static int vmmfs_serialroot_start(int *r, void *m) { (void)r; (void)m; return next(); }
static int vmmfs_platform_x64_start(int *r, void *m) {
    (void)r; (void)m; if(mode==3) machine.vcpu.stop_requested=true; return next();
}
static void vmmfs_machine_runtime_put(struct vmmfs_machine *m) { assert(m->runtime_references==1); --m->runtime_references; }
static int vmmfs_machine_release_to_stopped(struct vmmfs_machine *m) {
    assert(!m->runtime_references); vmm_machine_destroy(m->machine);
    vmspace_rel(m->memory.run_vmspace); m->memory.run_vmspace=NULL;
    m->memory.object=NULL; m->machine=NULL; m->vcpu.stop_requested=false; return 0;
}
static int vmmfs_machine_abort(struct vmmfs_launch *l) {
    assert(l==&launch); machine.launch=NULL;
    vmmfs_machine_release_to_stopped(&machine); vrele(l->node.vnode); return 0;
}
int
""" + function("vmmfs_machine.c", "vmmfs_machine_boot") + r"""
int main(void) {
    for(mode=0;mode<4;++mode) for(fail_stage=0;fail_stage<=13;++fail_stage) {
        memset(&machine,0,sizeof(machine)); memset(&launch_vnode,0,sizeof(launch_vnode));
        machine.memory.size=4096; machine.vcpu.count=1; stage=spaces=runtimes=0;
        struct vmmfs_launch *result=NULL;
        int e=VMMFS_WORK(&machine,vmmfs_machine_boot(&machine,&result));
        assert(!machine.token.held && !machine.vcpu.token.held && !machine.runtime_references);
        if(!e) {
            assert(mode==0 && fail_stage==0 && result==&launch);
            vmmfs_machine_abort(result); vrele(result->node.vnode);
        } else assert(!result);
        assert(!spaces && !runtimes && !launch_vnode.refs);
        if(mode==1 && stage>=2 && fail_stage!=2) assert(machine.machine==&competitor);
        else assert(!machine.machine);
    }
}
""")


    def test_nremove_runs_loader_as_launch_continuation(self):
        run_c(COMMON + r"""
struct vnode { void *v_data; unsigned refs; };
struct vmmfs_node { struct lock lock; bool dead; struct vnode *vnode; };
struct vmmfs_loader { struct vmmfs_node node; };
struct vmmfs_launch { struct vmmfs_node node; };
struct vmmfs_machine { struct vmmfs_node node; struct vmmfs_loader loader; };
struct namecache { const char *nc_name; size_t nc_nlen; struct vnode *nc_vp; };
struct nchandle { struct namecache *ncp; };
struct vop_nremove_args { struct vnode *a_dvp; struct nchandle *a_nch; void *a_cred; };
static struct vmmfs_machine machine;
static struct vmmfs_launch launch;
static struct vnode launch_vnode, stopped, replacement;
static unsigned mode, boots, runs, waits, aborts, unlinks;
static bool cache_locked;
static void vref(struct vnode *v) { assert(v->refs); ++v->refs; }
static void vrele(struct vnode *v) { assert(v->refs); --v->refs; }
static void cache_unlock(struct nchandle *n) { (void)n; assert(cache_locked); cache_locked=false; }
static void cache_lock(struct nchandle *n) { (void)n; assert(!cache_locked); cache_locked=true; }
static void cache_unlink(struct nchandle *n) { (void)n; assert(cache_locked); ++unlinks; }
static int vmmfs_machine_boot(struct vmmfs_machine *m, struct vmmfs_launch **out) {
    assert(m == &machine && m->node.lock.held && !cache_locked); ++boots;
    if (mode == 1) return EBUSY;
    launch_vnode.refs=1; launch.node.vnode=&launch_vnode; *out=&launch; return 0;
}
static int vmmfs_loader_run(struct vmmfs_loader *l, struct vmmfs_launch *s, void *cred) {
    (void)cred; assert(l == &machine.loader && s == &launch);
    assert(!machine.node.lock.held && !cache_locked); ++runs;
    return mode == 2 ? ENOMEM : 0;
}
static int vmmfs_launch_wait(struct vmmfs_launch *s) {
    assert(s == &launch && !machine.node.lock.held && !cache_locked); ++waits;
    return mode == 3 ? EINTR : 0;
}
static int vmmfs_machine_abort(struct vmmfs_launch *s) {
    assert(s == &launch && !machine.node.lock.held); ++aborts; return 0;
}
static int
""" + function("vmmfs_machine.c", "vmmfs_machine_nremove") + r"""
int main(void) {
    struct vnode parent = { .v_data=&machine };
    struct namecache name = { "stopped", 7, &stopped };
    struct nchandle handle = { &name };
    struct vop_nremove_args args = { &parent, &handle, NULL };
    for (mode=0; mode<5; ++mode) {
        machine.node.dead = mode == 4;
        stopped.refs=1; launch_vnode.refs=0; cache_locked=true;
        boots=runs=waits=aborts=unlinks=0;
        int error=vmmfs_machine_nremove(&args);
        assert(error == (mode==1 ? EBUSY : mode==2 ? ENOMEM : mode==3 ? EINTR : mode==4 ? ENOENT : 0));
        assert(boots==(mode!=4) && runs==(mode==0 || mode==2 || mode==3));
        assert(waits==(mode==0 || mode==3) && aborts==(mode==2));
        assert(unlinks==(mode==0) && stopped.refs==1 && !launch_vnode.refs);
        assert(cache_locked && !machine.node.lock.held);
    }
    (void)replacement;
}
""")

if __name__ == "__main__":
    unittest.main(verbosity=2)
