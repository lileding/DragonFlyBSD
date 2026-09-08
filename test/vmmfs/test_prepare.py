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
    bool reset_requested, stop_requested, start_ready;
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
static int vmmfs_machine_reset(struct vmmfs_machine *p) {
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
    group.threads = threads; group.count = 4; group.start_ready = true;
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
    bool reset_requested, stop_requested, start_ready;
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

    def test_stopped_is_a_persistent_child(self):
        from pathlib import Path
        base = Path(__file__).resolve().parents[2] / "sys/vfs/vmmfs"
        source = (base / "vmmfs_stopped.c").read_text()
        self.assertNotIn("vmmfs_stopped_create", source)
        self.assertNotIn("vmmfs_stopped_inactive", source)
        self.assertIn(".vop_inactive = vmmfs_node_inactive", source)
        init = function("vmmfs_stopped.c", "vmmfs_stopped_init")
        self.assertNotIn("kmalloc", init)
        self.assertIn("vmmfs_root_allocate_inode", init)
        self.assertNotIn("kfree", function("vmmfs_stopped.c", "vmmfs_stopped_drop"))
        self.assertIn("vmmfs_stopped_init", function("vmmfs_machine.c", "vmmfs_machine_create"))
        self.assertIn("&machine->stopped.node", function("vmmfs_machine.c", "vmmfs_machine_deactivate"))
        for name in ("vmmfs_machine_get_item", "vmmfs_machine_touch_stopped"):
            body = function("vmmfs_machine.c", name)
            self.assertNotIn("vmmfs_stopped_init", body)
            self.assertNotIn("vmmfs_stopped_create", body)
        self.assertIn("cache_inval_vp", function("vmmfs_machine.c", "vmmfs_machine_post_launch"))
        self.assertIn("cache_setvp", function("vmmfs_machine.c", "vmmfs_machine_stopped"))

    def test_empty_vmspace_claim_precedes_memory_prepare(self):
        body = function("vmmfs_machine.c", "vmmfs_machine_boot")
        sequence = ["vmspace_alloc(", "vmm_machine_create(", "atomic_cmpset_ptr(",
                    "vmmfs_memory_prepare(", "vmmfs_platform_x64_start(", "vmmfs_launch_create("]
        offsets = [body.index(item) for item in sequence]
        self.assertEqual(offsets, sorted(offsets))
        self.assertNotIn("stop_requested", body)
        self.assertNotIn("machine_abort", body)
        loser = body.split("rejected:")[1]
        self.assertIn("vmm_machine_destroy(runtime)", loser)
        self.assertIn("vmspace_rel(vmspace)", loser)


    def test_nremove_runs_loader_as_launch_continuation(self):
        body = function("vmmfs_machine.c", "vmmfs_machine_nremove")
        self.assertIn("VMMFS_WORK(machine, vmmfs_machine_boot", body)
        self.assertNotIn("VMMFS_WORK(machine, vmmfs_loader_run", body)
        self.assertLess(body.index("cache_unlock"), body.index("vmmfs_launch_wait"))
        self.assertLess(body.index("vmmfs_launch_wait"), body.index("cache_lock"))
        self.assertNotIn("cache_unlink", body)

if __name__ == "__main__":
    unittest.main(verbosity=2)
