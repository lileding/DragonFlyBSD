#!/usr/local/bin/python3
"""Collect a staged nvkm KMS runtime smoke-test report.

The runner is intentionally split into phases so physical testing can pause
between steps:

    kms_smoke.py before
    startx
    DISPLAY=:0 kms_smoke.py x11
    logout
    kms_smoke.py after

All output is written under /var/tmp so it survives reboot.
"""

import argparse
import datetime as dt
import os
import pathlib
import re
import shlex
import subprocess
import sys


LATEST = pathlib.Path("/var/tmp/nvkm-kms-smoke.latest")
DEFAULT_PREFIX = "/var/tmp/nvkm-kms-smoke"
FAULT_PATTERN = (
    "panic|BADFREE|double fault|DeviceLost|EXEC timeout|fault|CMDre|"
    "status=0x19|RC_TRIGGERED|vblank wait timed out|commit.*failed"
)

ZERO_KEYS = (
    "fault_pending_count",
    "commit_error_count",
    "fb_create_error_count",
    "page_flip_error_count",
    "prepare_fb_error_count",
    "console_flush_error_count",
    "bo_wait_error_count",
    "vm_bind_error_count",
    "vm_bind_wait_error_count",
    "prime_handle_to_fd_error_count",
    "prime_fd_to_handle_error_count",
)

WATCH_KEYS = (
    "page_flip_count",
    "page_flip_event_count",
    "atomic_commit_tail_count",
    "atomic_flip_done_wait_count",
    "atomic_vblank_wait_count",
    "plane_update_count",
    "plane_disable_count",
    "cursor_update_count",
    "cursor_async_update_count",
    "cursor_disable_count",
    "prepare_fb_count",
    "cleanup_fb_count",
    "scanout_pin_count",
    "scanout_unpin_count",
    "hotplug_count",
    "hotplug_changed_count",
    "hotplug_notify_only_count",
)


def choose_out_dir(phase: str, explicit: str | None) -> pathlib.Path:
    if explicit:
        out_dir = pathlib.Path(explicit)
    elif os.environ.get("NVKM_KMS_SMOKE_DIR"):
        out_dir = pathlib.Path(os.environ["NVKM_KMS_SMOKE_DIR"])
    elif phase == "before" or not LATEST.is_symlink():
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        out_dir = pathlib.Path(f"{DEFAULT_PREFIX}-{stamp}")
    else:
        out_dir = pathlib.Path(os.readlink(LATEST))

    out_dir.mkdir(parents=True, exist_ok=True)
    if phase == "before" or not LATEST.exists():
        tmp_link = LATEST.with_suffix(".tmp")
        try:
            tmp_link.unlink()
        except FileNotFoundError:
            pass
        tmp_link.symlink_to(out_dir)
        tmp_link.replace(LATEST)
    return out_dir


def command_text(argv: list[str]) -> str:
    return " ".join(shlex.quote(str(part)) for part in argv)


def run(argv: list[str], path: pathlib.Path, env: dict[str, str] | None = None,
        timeout: int | None = None) -> int:
    with path.open("w") as out:
        out.write(f"### {command_text(argv)}\n")
        out.flush()
        try:
            result = subprocess.run(
                argv,
                stdout=out,
                stderr=subprocess.STDOUT,
                env=env,
                timeout=timeout,
            )
            out.write(f"\n### rc={result.returncode}\n")
            return result.returncode
        except subprocess.TimeoutExpired:
            out.write(f"\n### timeout={timeout}\n")
            return 124
        except FileNotFoundError as err:
            out.write(f"\n### missing={err.filename}\n")
            return 127


def capture_phase(out_dir: pathlib.Path, phase: str, args: argparse.Namespace) -> None:
    env = os.environ.copy()
    if args.display:
        env["DISPLAY"] = args.display
    if args.xauthority:
        env["XAUTHORITY"] = args.xauthority

    run(["date"], out_dir / f"date.{phase}")
    run(["uname", "-a"], out_dir / f"uname.{phase}")
    run(["kldstat"], out_dir / f"kldstat.{phase}")
    run(["sysctl", "-n", "dev.drm.0.state"], out_dir / f"drm_state.{phase}")
    run(["sysctl", "-n", "dev.drm.0.vram_state"], out_dir / f"vram_state.{phase}")
    run(["ps", "axww"], out_dir / f"ps.{phase}")
    run(["fstat", "/dev/dri/card0", "/dev/dri/renderD128"],
        out_dir / f"dri_fstat.{phase}")
    run(["/bin/sh", "-c", f"dmesg | egrep -i {shlex.quote(FAULT_PATTERN)} | tail -120 || true"],
        out_dir / f"dmesg_faults.{phase}")
    run(["/bin/sh", "-c", "dmesg | tail -260"],
        out_dir / f"dmesg_tail.{phase}")

    for log_path in (
        pathlib.Path("/var/log/Xorg.0.log"),
        pathlib.Path.home() / ".local/share/xorg/Xorg.0.log",
    ):
        if log_path.exists():
            target = out_dir / f"{log_path.name}.{phase}"
            target.write_bytes(log_path.read_bytes())

    if phase != "x11":
        return

    run(["xrandr", "--verbose"], out_dir / "xrandr.x11", env=env)
    run(["glxinfo", "-B"], out_dir / "glxinfo-B.x11", env=env)
    if args.gears_seconds > 0:
        run(["timeout", str(args.gears_seconds), "glxgears", "-info"],
            out_dir / "glxgears.x11", env=env, timeout=args.gears_seconds + 5)
    if args.run_panning:
        panning_cmd = (
            "set -eu; out=$(xrandr --query | awk '/ connected/{print $1; exit}'); "
            "mode=$(xrandr --query | awk '/\\*/{print $1; exit}'); "
            "xrandr --output \"$out\" --panning \"$mode+0+0\"; "
            "xrandr --output \"$out\" --panning 0x0"
        )
        run(["/bin/sh", "-c", panning_cmd], out_dir / "xrandr-panning.x11",
            env=env)


def parse_state(path: pathlib.Path) -> dict[str, int]:
    values: dict[str, int] = {}
    pattern = re.compile(r"^([A-Za-z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)")
    if not path.exists():
        return values
    for line in path.read_text(errors="replace").splitlines():
        match = pattern.match(line.strip())
        if not match:
            continue
        key, text = match.groups()
        values[key] = int(text, 16 if text.startswith("0x") else 10)
    return values


def report(out_dir: pathlib.Path) -> int:
    failed = False

    def emit(ok: bool, text: str) -> None:
        nonlocal failed
        print(("PASS " if ok else "FAIL ") + text)
        failed = failed or not ok

    for name in ("drm_state.before", "drm_state.after", "ps.after"):
        emit((out_dir / name).exists(), f"found {name}")

    after = parse_state(out_dir / "drm_state.after")
    before = parse_state(out_dir / "drm_state.before")
    for key in ZERO_KEYS:
        if key not in after:
            emit(False, f"missing {key}")
        else:
            emit(after[key] == 0, f"{key}={after[key]}")

    for key in WATCH_KEYS:
        if key in after:
            delta = after[key] - before.get(key, after[key])
            print(f"INFO {key}={after[key]} delta={delta}")

    ps_after = (out_dir / "ps.after").read_text(errors="replace") if (out_dir / "ps.after").exists() else ""
    emit(not re.search(r"(^|\s)(Xorg|glxgears|firefox)(\s|$)", ps_after),
         "no Xorg/glxgears/firefox leftovers")

    x11_ran = any((out_dir / name).exists() for name in (
        "xrandr.x11", "glxinfo-B.x11", "glxgears.x11"))
    xlog = list(out_dir.glob("*Xorg*.after"))
    if x11_ran and xlog:
        text = xlog[0].read_text(errors="replace")
        emit(bool(re.search(r"zink|NVK|glamor X acceleration enabled", text, re.I)),
             "Xorg log has zink/NVK/glamor")
        emit("Server terminated successfully" in text,
             "Xorg terminated successfully")
    elif x11_ran:
        emit(False, "missing after Xorg log")
    else:
        print("INFO x11 phase not captured; skip Xorg log checks")

    faults = out_dir / "dmesg_faults.after"
    if faults.exists():
        before_faults = out_dir / "dmesg_faults.before"
        before_lines = set()
        if before_faults.exists():
            before_lines = {
                line for line in before_faults.read_text(errors="replace").splitlines()
                if line and not line.startswith("###")
            }
        after_lines = [
            line for line in faults.read_text(errors="replace").splitlines()
            if line and not line.startswith("###")
        ]
        new_lines = [line for line in after_lines if line not in before_lines]
        emit(len(new_lines) == 0, "no new dmesg fault lines")
        for line in new_lines[:20]:
            print(f"INFO new_fault {line}")

    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="nvkm KMS staged smoke collector")
    parser.add_argument("phase", choices=("before", "x11", "after", "report"))
    parser.add_argument("--out-dir", default=None)
    parser.add_argument("--display", default=os.environ.get("DISPLAY", ":0"))
    parser.add_argument("--xauthority", default=os.environ.get("XAUTHORITY"))
    parser.add_argument("--gears-seconds", type=int, default=5)
    parser.add_argument("--run-panning", action="store_true",
                        help="briefly set and clear xrandr panning on the first connected output")
    args = parser.parse_args()

    out_dir = choose_out_dir(args.phase, args.out_dir)
    if args.phase == "report":
        print(out_dir)
        return report(out_dir)

    capture_phase(out_dir, args.phase, args)
    print(out_dir)
    if args.phase == "after":
        return report(out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
