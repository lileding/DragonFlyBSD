#!/usr/local/bin/python3
# SPDX-License-Identifier: BSD-2-Clause
"""nvgpu kldunload gate smoke test (phases 1+2: negative tests only).

Verifies that unload is refused with busy semantics while DRM users exist,
and that a refused unload does not damage the running driver:

    1. nvgpu must be loaded.
    2. With a render-node fd held open, `kldunload nvgpu` must fail,
       nvgpu must stay loaded, and dev.drm.X.unload_state counters must
       not report a committed unload (unloading stays 0).
    3. mmap outliving its fd must also block unload: create a DUMB
       buffer on card0, mmap it, close the fd, and verify kldunload
       still fails while mmap_active_count reports the live mapping;
       munmap must return the count to its baseline.
    4. After all references are gone, the nodes must still open.

With --try-unload the tool finishes by attempting a REAL unload with no
DRM users left (phase-3 acceptance): kldunload must succeed, nvgpu must
disappear from kldstat, and the /dev/dri nodes must be gone.

With --reload-loop N (phase-4 acceptance) it then runs N unload/reload
cycles: each cycle unloads, kldloads the project-tree nvgpu.ko again
(drm.ko and nvgsp_570.ko stay loaded), and verifies the nodes come
back and DUMB mmap works.  nvgpu is left loaded after the last cycle.

Run from an SSH shell, not from inside an X11/Wayland session (a
compositor holds its own DRM fds and would make the "close" step
meaningless).

Output goes to stdout; exit code 0 = all PASS.
"""

import fcntl
import mmap as mmap_mod
import os
import re
import struct
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


def nvgpu_loaded():
    _, out = run(["kldstat"])
    return re.search(r"\bnvgpu\.ko\b", out) is not None


def read_unload_state():
    # unload_state is a multi-line string sysctl ("key = value" lines);
    # probe the first few drm units for the one that answers.
    for unit in range(4):
        rc, out = run(["sysctl", "-n", "dev.drm.%d.unload_state" % unit])
        if rc != 0 or "unloading" not in out:
            continue
        state = {}
        for line in out.splitlines():
            match = re.match(
                r"(unload\w+|unloading|mmap_active_count)\s*=\s*(-?\d+)",
                line)
            if match:
                state[match.group(1)] = int(match.group(2))
        return state
    return {}


# BSD ioctl encodings for the DRM DUMB buffer calls (group 'd' = 0x64).
DRM_IOCTL_MODE_CREATE_DUMB = 0xC02064B2   # _IOWR('d', 0xB2, 32-byte struct)
DRM_IOCTL_MODE_MAP_DUMB = 0xC01064B3      # _IOWR('d', 0xB3, 16-byte struct)


def dumb_create_and_mmap(fd):
    """Create a 64x64x32 DUMB buffer, map it, return the mmap object."""
    arg = bytearray(struct.pack("IIIIIIQ", 64, 64, 32, 0, 0, 0, 0))
    fcntl.ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, arg)
    _, _, _, _, handle, _, size = struct.unpack("IIIIIIQ", arg)
    arg = bytearray(struct.pack("IIQ", handle, 0, 0))
    fcntl.ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, arg)
    _, _, offset = struct.unpack("IIQ", arg)
    mem = mmap_mod.mmap(fd, size, mmap_mod.MAP_SHARED,
                        mmap_mod.PROT_READ | mmap_mod.PROT_WRITE,
                        offset=offset)
    mem[0:8] = b"nvkmtest"                  # fault a page in
    return mem


def phase1_fd_blocks_unload():
    fd = os.open(RENDER_NODE, os.O_RDWR)
    try:
        rc, out = run(["doas", "kldunload", "nvgpu"])
        check("kldunload refused while fd open", rc != 0, out)
        check("nvgpu still loaded after refusal", nvgpu_loaded())

        state_busy = read_unload_state()
        check("gate did not commit unload",
              state_busy.get("unloading", 1) == 0, str(state_busy))
        # The bus DS_BUSY check may reject before the nvkm gate runs, so
        # a zero delta on the gate counters is acceptable; a committed
        # unload (unloading=1) never is.
    finally:
        os.close(fd)


def phase2_mmap_outlives_fd():
    baseline = read_unload_state().get("mmap_active_count", 0)

    fd = os.open(CARD_NODE, os.O_RDWR)
    mem = dumb_create_and_mmap(fd)
    os.close(fd)

    state_mapped = read_unload_state()
    check("mmap counted while fd closed",
          state_mapped.get("mmap_active_count", -1) == baseline + 1,
          str(state_mapped))

    rc, out = run(["doas", "kldunload", "nvgpu"])
    check("kldunload refused while mmap alive", rc != 0, out)
    check("nvgpu still loaded after mmap refusal", nvgpu_loaded())
    check("gate did not commit unload with mmap",
          read_unload_state().get("unloading", 1) == 0)

    mem[8:12] = b"post"                     # mapping still works
    check("mmap still writable after refused unload", True)

    mem.close()
    state_unmapped = read_unload_state()
    check("mmap count returns to baseline after munmap",
          state_unmapped.get("mmap_active_count", -1) == baseline,
          str(state_unmapped))


def phase3_real_unload(tag=""):
    """Attempt a real unload with no DRM users left (phase-3 gate)."""
    rc, out = run(["doas", "kldunload", "nvgpu"])
    check("kldunload succeeds with no users%s" % tag, rc == 0, out)
    check("nvgpu gone from kldstat%s" % tag, not nvgpu_loaded())
    check("render node removed after unload%s" % tag,
          not os.path.exists(RENDER_NODE))
    check("card node removed after unload%s" % tag,
          not os.path.exists(CARD_NODE))
    rc, out = run(["dmesg"])
    tail = "\n".join(out.splitlines()[-40:])
    check("dmesg reports clean unload%s" % tag,
          "unloaded cleanly" in tail, tail[-200:])


def phase4_reload_loop(cycles):
    """Unload/reload N times (phase-4 gate); leaves nvgpu loaded."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    src_root = os.path.abspath(os.path.join(script_dir, "..", "..", "..",
                                            ".."))
    nvgpu_ko = os.path.join(src_root, "sys", "dev", "drm", "nouveau",
                            "nvgpu.ko")
    if not check("project nvgpu.ko found for reload", os.path.exists(nvgpu_ko),
                 nvgpu_ko):
        return

    for i in range(1, cycles + 1):
        tag = " [cycle %d]" % i
        phase3_real_unload(tag)

        rc, out = run(["dmesg"])
        tail = "\n".join(out.splitlines()[-40:])
        check("WPR2 torn down%s" % tag, "torn down" in tail, tail[-300:])

        rc, out = run(["doas", "kldload", nvgpu_ko])
        check("kldload succeeds%s" % tag, rc == 0, out)
        check("nvgpu loaded%s" % tag, nvgpu_loaded())
        check("nodes back%s" % tag,
              os.path.exists(RENDER_NODE) and os.path.exists(CARD_NODE))

        fd = os.open(RENDER_NODE, os.O_RDWR)
        os.close(fd)
        fd = os.open(CARD_NODE, os.O_RDWR)
        mem = dumb_create_and_mmap(fd)
        os.close(fd)
        mem[16:20] = b"loop"
        mem.close()
        check("dumb mmap works after reload%s" % tag, True)

        state = read_unload_state()
        check("fresh unload_state%s" % tag,
              state.get("unloading", 1) == 0 and
              state.get("mmap_active_count", -1) == 0, str(state))


def main():
    try_unload = "--try-unload" in sys.argv[1:]
    reload_cycles = 0
    if "--reload-loop" in sys.argv[1:]:
        idx = sys.argv.index("--reload-loop")
        reload_cycles = int(sys.argv[idx + 1]) if idx + 1 < len(
            sys.argv) else 3

    if not check("nvgpu loaded before test", nvgpu_loaded()):
        return 1
    if not check("render node exists", os.path.exists(RENDER_NODE)):
        return 1

    state_before = read_unload_state()
    check("unload_state sysctl present", "unloading" in state_before,
          str(state_before))

    phase1_fd_blocks_unload()
    phase2_mmap_outlives_fd()

    fd2 = os.open(RENDER_NODE, os.O_RDWR)
    os.close(fd2)
    check("render node reopens after refused unload", True)

    card_fd = os.open(CARD_NODE, os.O_RDWR)
    os.close(card_fd)
    check("card node opens after refused unload", True)

    if reload_cycles > 0:
        phase4_reload_loop(reload_cycles)
    elif try_unload:
        phase3_real_unload()

    failed = [name for name, ok, _ in results if not ok]
    print("%d checks, %d failed" % (len(results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
