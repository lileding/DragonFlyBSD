#!/usr/bin/env python3
"""Startup preparation must not release guest workers before publication."""
import unittest
from test_regress import COMMON, function, run_c

class LaunchFuture(unittest.TestCase):
    def test_future_completion_order(self):
        body = function("vmmfs_launch.c", "vmmfs_launch_complete")
        names = ["fdrevoke(", "launch->result =", "launch->post_launch(",
                 "atomic_store_rel_int(&launch->ready", "wakeup(launch)",
                 "vmmfs_vcpu_run("]
        positions = [body.index(name) for name in names]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(body.count("wakeup(launch)"), 1)

    def test_submit_and_cancel_claim_once(self):
        for name in ("vmmfs_launch_submit", "vmmfs_launch_cancel"):
            body = function("vmmfs_launch.c", name)
            self.assertIn("atomic_cmpset_int(&launch->claimed, 0, 1)", body)
            self.assertLess(body.index("atomic_cmpset_int"),
                            body.index("vmmfs_launch_revoke"))

    def test_wait_observes_ready_after_interlocking(self):
        body = function("vmmfs_launch.c", "vmmfs_launch_wait")
        self.assertLess(body.index("tsleep_interlock"),
                        body.index("atomic_load_acq_int(&launch->ready)"))
        self.assertIn("vmmfs_launch_cancel(launch)", body)
        self.assertNotIn("for (", body)
        self.assertNotIn("while (", body)
        self.assertEqual(body.count("tsleep(launch,"), 2)

    def test_nremove_does_not_unlink_after_wait(self):
        body = function("vmmfs_machine.c", "vmmfs_machine_nremove")
        self.assertNotIn("cache_unlink", body)
        self.assertIn("vmmfs_machine_post_launch", body)
        self.assertIn("vmmfs_launch_put(launch)", body)

    def test_prepare_does_not_release_workers(self):
        body = function("vmmfs_vcpu.c", "vmmfs_vcpu_prepare")
        success, failure = body.split("failed:", 1)
        self.assertNotIn("start_ready = true", success)
        self.assertNotIn("wakeup(vcpu)", success)
        self.assertIn("start_failed = true", failure)
        self.assertIn("start_ready = true", failure)

    def test_run_publishes_before_wakeup(self):
        run_c(COMMON + r"""
struct vmmfs_vcpu { int token; bool start_ready; };
static int wakes;
static void lwkt_gettoken(int *token) { assert(!*token); *token = 1; }
static void lwkt_reltoken(int *token) { assert(*token); *token = 0; }
static void wakeup(struct vmmfs_vcpu *vcpu) {
    assert(vcpu->start_ready && !vcpu->token); ++wakes;
}
void
""" + function("vmmfs_vcpu.c", "vmmfs_vcpu_run") + r"""
int main(void) {
    struct vmmfs_vcpu vcpu = {0};
    vmmfs_vcpu_run(&vcpu);
    assert(wakes == 1);
    return 0;
}
""")

if __name__ == "__main__":
    unittest.main()
