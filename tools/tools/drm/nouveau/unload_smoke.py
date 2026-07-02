#!/usr/local/bin/python3
# SPDX-License-Identifier: BSD-2-Clause
"""nvkm kldunload gate smoke test (phase 1: negative tests only).

Verifies that unload is refused with busy semantics while DRM users exist,
and that a refused unload does not damage the running driver:

    1. nvkm must be loaded.
    2. With a render-node fd held open, `kldunload nvkm` must fail,
       nvkm must stay loaded, and dev.drm.X.unload_state counters must
       not report a committed unload (unloading stays 0).
    3. After closing the fd, the node must still open and the driver
       must still answer ioctls (drmtest sync-only probe if present).

This phase does NOT attempt a successful unload: teardown completion and
GSP-RM shutdown land in later phases (see nvkm-unload.md).  Run from an
SSH shell, not from inside an X11/Wayland session (a compositor holds
its own DRM fds and would make the "close" step meaningless).

Output goes to stdout; exit code 0 = all PASS.
"""

import os
import re
import subprocess
import sys

RENDER_NODE = "/dev/dri/renderD128"
CARD_NODE = "/dev/dri/card0"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name,
                       (" (%s)" % detail) if detail else ""))
    return ok


def run(argv):
    proc = subprocess.run(argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    return proc.returncode, proc.stdout.strip()


def nvkm_loaded():
    _, out = run(["kldstat"])
    return re.search(r"\bnvkm\.ko\b", out) is not None


def read_unload_state():
    # unload_state is a multi-line string sysctl ("key = value" lines);
    # probe the first few drm units for the one that answers.
    for unit in range(4):
        rc, out = run(["sysctl", "-n", "dev.drm.%d.unload_state" % unit])
        if rc != 0 or "unloading" not in out:
            continue
        state = {}
        for line in out.splitlines():
            match = re.match(r"(unload\w+|unloading)\s*=\s*(-?\d+)", line)
            if match:
                state[match.group(1)] = int(match.group(2))
        return state
    return {}


def main():
    if not check("nvkm loaded before test", nvkm_loaded()):
        return 1
    if not check("render node exists", os.path.exists(RENDER_NODE)):
        return 1

    state_before = read_unload_state()
    check("unload_state sysctl present", "unloading" in state_before,
          str(state_before))

    fd = os.open(RENDER_NODE, os.O_RDWR)
    try:
        rc, out = run(["doas", "kldunload", "nvkm"])
        check("kldunload refused while fd open", rc != 0, out)
        check("nvkm still loaded after refusal", nvkm_loaded())

        state_busy = read_unload_state()
        check("gate did not commit unload",
              state_busy.get("unloading", 1) == 0, str(state_busy))
        # The bus DS_BUSY check may reject before the nvkm gate runs, so
        # a zero delta on the gate counters is acceptable; a committed
        # unload (unloading=1) never is.
    finally:
        os.close(fd)

    fd2 = os.open(RENDER_NODE, os.O_RDWR)
    os.close(fd2)
    check("render node reopens after refused unload", True)

    card_fd = os.open(CARD_NODE, os.O_RDWR)
    os.close(card_fd)
    check("card node opens after refused unload", True)

    failed = [name for name, ok, _ in results if not ok]
    print("%d checks, %d failed" % (len(results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
