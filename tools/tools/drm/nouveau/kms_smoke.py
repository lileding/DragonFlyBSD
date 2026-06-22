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
    "status=0x19|RC_TRIGGERED|vblank wait timed out|flip_done timed out|"
    "commit.*failed"
)

ZERO_KEYS = (
    "fault_pending_count",
    "commit_error_count",
    "scanout_user",
    "atomic_tail_active",
    "atomic_tail_stage",
    "atomic_disable_vblank_keep_count",
    "atomic_disable_vblank_keep_error",
    "hotplug_enqueue_error_count",
    "dark_down_error_count",
    "dp_irq_error_count",
    "fb_create_error_count",
    "page_flip_error_count",
    "prepare_fb_error_count",
    "cursor_error_count",
    "console_flush_error_count",
    "bo_wait_error_count",
    "vm_bind_error_count",
    "vm_bind_wait_error_count",
    "prime_handle_to_fd_error_count",
    "prime_fd_to_handle_error_count",
)

WATCH_KEYS = (
    "display_head_capacity",
    "display_window_capacity",
    "display_cursor_capacity",
    "display_sor_capacity",
    "display_audit_scanout_pin_balance",
    "page_flip_count",
    "page_flip_event_count",
    "atomic_commit_tail_count",
    "atomic_flip_done_wait_count",
    "atomic_vblank_wait_count",
    "atomic_tail_seq",
    "atomic_tail_complete_count",
    "atomic_tail_last_stage",
    "atomic_tail_modeset_disables_count",
    "atomic_tail_commit_planes_count",
    "atomic_tail_modeset_enables_count",
    "atomic_tail_modeset_events_count",
    "atomic_tail_fake_vblank_count",
    "atomic_tail_hw_done_count",
    "atomic_tail_wait_flip_done_count",
    "atomic_tail_cleanup_planes_count",
    "atomic_tail_finish_prepared_count",
    "atomic_disable_vblank_off_count",
    "atomic_disable_vblank_keep_count",
    "fb_create_reject_count",
    "plane_update_count",
    "plane_disable_count",
    "cursor_update_count",
    "cursor_async_update_count",
    "cursor_disable_count",
    "prepare_fb_count",
    "cleanup_fb_count",
    "scanout_pin_count",
    "scanout_unpin_count",
    "cursor_pin_count",
    "cursor_unpin_count",
    "hotplug_count",
    "hotplug_changed_count",
    "hotplug_nochange_count",
    "hotplug_notify_only_count",
    "hotplug_auto_kms_count",
)

CAPACITY_KEYS = (
    "display_head_capacity",
    "display_window_capacity",
    "display_cursor_capacity",
    "display_sor_capacity",
)

COUNT_ORDER_PAIRS = (
    ("prepare_fb_count", "cleanup_fb_count"),
    ("scanout_pin_count", "scanout_unpin_count"),
    ("cursor_pin_count", "cursor_unpin_count"),
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


def capture_kms_property_probe(out_dir: pathlib.Path, phase: str) -> None:
    source = pathlib.Path(__file__).with_name("drmtest.c")
    binary = out_dir / "drmtest"
    build_cmd = (
        "set -eu; "
        "cc -Wall -Wextra -Werror $(pkg-config --cflags libdrm) "
        f"{shlex.quote(str(source))} -o {shlex.quote(str(binary))} "
        "$(pkg-config --libs libdrm)"
    )

    if run(["/bin/sh", "-c", build_cmd],
           out_dir / f"drmtest-build.{phase}") != 0:
        return
    run([str(binary)], out_dir / f"drmtest.{phase}")


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

    for label, log_path in (
        ("system", pathlib.Path("/var/log/Xorg.0.log")),
        ("user", pathlib.Path.home() / ".local/share/xorg/Xorg.0.log"),
    ):
        if log_path.exists():
            target = out_dir / f"Xorg-{label}.log.{phase}"
            target.write_bytes(log_path.read_bytes())

    if phase != "x11":
        if phase == "after":
            capture_kms_property_probe(out_dir, phase)
        return

    run(["sleep", "2"], out_dir / "x11-idle-wait.x11", env=env)
    run(["sysctl", "-n", "dev.drm.0.state"], out_dir / "drm_state.x11_idle")
    cursor_move_cmd = (
        "set -eu; "
        "for step in 1 2 3 4 5 6 7 8 9 10 11 12; do "
        "xdotool mousemove_relative -- 3 0; sleep 0.03; "
        "done"
    )
    run(["/bin/sh", "-c", cursor_move_cmd],
        out_dir / "xdotool-cursor-move.x11", env=env)
    run(["sysctl", "-n", "dev.drm.0.state"], out_dir / "drm_state.x11_cursor")
    run(["xrandr", "--verbose"], out_dir / "xrandr.x11", env=env)
    run(["glxinfo", "-B"], out_dir / "glxinfo-B.x11", env=env)
    if args.gears_seconds > 0:
        run(["timeout", str(args.gears_seconds), "glxgears", "-info"],
            out_dir / "glxgears.x11", env=env, timeout=args.gears_seconds + 5)
    if args.run_hpd_inject:
        run(["sysctl", "-n", "dev.drm.0.state"],
            out_dir / "drm_state.x11_hpd_before")
        run([
            "doas",
            "sysctl",
            f"dev.drm.0.kms_hpd_inject={args.hpd_inject_value}",
        ], out_dir / "kms-hpd-inject.x11")
        run(["sleep", "1"], out_dir / "hpd-inject-wait.x11")
        run(["sysctl", "-n", "dev.drm.0.state"],
            out_dir / "drm_state.x11_hpd_after")
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


def parse_state_value(text: str, key: str) -> int | None:
    match = re.search(
        rf"^{re.escape(key)}\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)\b",
        text,
        re.M,
    )
    if match is None:
        return None
    value = match.group(1)
    return int(value, 16 if value.startswith("0x") else 10)


def cursor_heads(text: str) -> list[int]:
    heads = {
        int(match.group(1))
        for match in re.finditer(r"^head\[(\d+)\]_cursor_", text, re.M)
    }
    return sorted(heads)


def active_hardware_cursor_heads(text: str) -> list[int]:
    active: list[int] = []
    for head in cursor_heads(text):
        enabled = parse_state_value(text, f"head[{head}]_cursor_enabled")
        fb = parse_state_value(text, f"head[{head}]_cursor_fb")
        bo = parse_state_value(text, f"head[{head}]_cursor_bo")
        if enabled == 1 and fb not in (None, 0) and bo not in (None, 0):
            active.append(head)
    return active


def command_return_code(path: pathlib.Path) -> int | None:
    if not path.exists():
        return None
    match = re.search(
        r"^### rc=([0-9]+)$", path.read_text(errors="replace"), re.M)
    if match is None:
        return None
    return int(match.group(1))


def captured_text(path: pathlib.Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(errors="replace")


def has_error_text(text: str) -> bool:
    return bool(re.search(
        r"(^|\n)(Error:|.*\bfailed\b|Segmentation fault|core dumped|DeviceLost|"
        r"couldn['’]?t open display|No protocol specified)",
        text,
        re.I,
    ))


LEFTOVER_BASENAMES = {
    "Xorg",
    "glxgears",
    "firefox",
    "firefox-bin",
}


def ps_command_lines(text: str) -> list[str]:
    commands: list[str] = []
    pattern = re.compile(r"^\s*-?\d+\s+\S+\s+\S+\s+\S+\s+(.*)$")
    for line in text.splitlines():
        if not line or line.startswith("###"):
            continue
        if line.lstrip().startswith("PID "):
            continue
        match = pattern.match(line)
        if match:
            commands.append(match.group(1))
    return commands


def command_basename(command: str) -> str:
    first = command.split(maxsplit=1)[0] if command.split() else ""
    return pathlib.PurePosixPath(first.strip("()")).name


def leftover_graphics_commands(ps_text: str) -> list[str]:
    leftovers: list[str] = []
    for command in ps_command_lines(ps_text):
        if command_basename(command) in LEFTOVER_BASENAMES:
            leftovers.append(command)
    return leftovers


def report(out_dir: pathlib.Path, allow_missing_x11: bool) -> int:
    failed = False

    def emit(ok: bool, text: str) -> None:
        nonlocal failed
        print(("PASS " if ok else "FAIL ") + text)
        failed = failed or not ok

    for name in ("drm_state.before", "drm_state.after", "ps.after"):
        emit((out_dir / name).exists(), f"found {name}")

    after = parse_state(out_dir / "drm_state.after")
    before = parse_state(out_dir / "drm_state.before")
    zero_failed = False
    for key in ZERO_KEYS:
        if key not in after:
            emit(False, f"missing {key}")
            zero_failed = True
        else:
            ok = after[key] == 0
            emit(ok, f"{key}={after[key]}")
            zero_failed = zero_failed or not ok

    if zero_failed:
        for key in (
            "last_error",
            "last_head",
            "last_win",
            "dark_down_last_error",
            "dp_sst_last_error",
            "bo_wait_last_error",
            "vm_bind_last_error",
        ):
            if key in after:
                print(f"INFO {key}={after[key]}")

    for key in WATCH_KEYS:
        if key in after:
            delta = after[key] - before.get(key, after[key])
            print(f"INFO {key}={after[key]} delta={delta}")

    for key in CAPACITY_KEYS:
        if key not in after:
            emit(False, f"missing {key}")
        else:
            emit(after[key] > 0, f"{key}={after[key]}")

    if all(key in after for key in (
        "display_head_capacity",
        "display_window_capacity",
        "display_cursor_capacity",
    )):
        heads = after["display_head_capacity"]
        emit(after["display_window_capacity"] >= heads,
             f"display_window_capacity covers heads ({after['display_window_capacity']}>={heads})")
        emit(after["display_cursor_capacity"] >= heads,
             f"display_cursor_capacity covers heads ({after['display_cursor_capacity']}>={heads})")

    for produce_key, consume_key in COUNT_ORDER_PAIRS:
        if produce_key not in after or consume_key not in after:
            emit(False, f"missing {produce_key}/{consume_key}")
            continue
        balance = after[produce_key] - after[consume_key]
        emit(balance >= 0, f"{produce_key}-{consume_key}={balance}")

    if "display_audit_scanout_pin_balance" not in after:
        emit(False, "missing display_audit_scanout_pin_balance")
    elif "scanout_pin_count" in after and "scanout_unpin_count" in after:
        expected = after["scanout_pin_count"] - after["scanout_unpin_count"]
        actual = after["display_audit_scanout_pin_balance"]
        emit(actual == expected,
             f"display_audit_scanout_pin_balance={actual} expected={expected}")

    after_state_text = captured_text(out_dir / "drm_state.after")
    emit(bool(cursor_heads(after_state_text)), "after cursor audit present")
    emit(not active_hardware_cursor_heads(after_state_text),
         "after has no hardware cursor enabled")

    tail_keys = (
        "atomic_tail_seq",
        "atomic_tail_complete_count",
        "atomic_tail_finish_prepared_count",
    )
    if all(key in after for key in tail_keys):
        emit(after["atomic_tail_seq"] == after["atomic_tail_complete_count"],
             "atomic tail has no unfinished transaction")
        emit(after["atomic_tail_finish_prepared_count"] == after["atomic_tail_complete_count"],
             "atomic tail finished prepared cleanup for every transaction")
    else:
        for key in tail_keys:
            if key not in after:
                emit(False, f"missing {key}")

    ps_after = (out_dir / "ps.after").read_text(errors="replace") if (out_dir / "ps.after").exists() else ""
    leftovers = leftover_graphics_commands(ps_after)
    emit(not leftovers, "no Xorg/glxgears/firefox leftovers")
    for command in leftovers:
        print(f"INFO leftover: {command}")

    x11_ran = (out_dir / "drm_state.x11").exists()
    if not x11_ran:
        emit(allow_missing_x11, "x11 phase optional/missing")
    else:
        x11_state = parse_state(out_dir / "drm_state.x11_idle")
        if not x11_state:
            x11_state = parse_state(out_dir / "drm_state.x11")
        x11_cursor_state = parse_state(out_dir / "drm_state.x11_cursor")
        x11_cursor_text = captured_text(out_dir / "drm_state.x11_cursor")
        for name in ("xrandr.x11", "glxinfo-B.x11"):
            rc = command_return_code(out_dir / name)
            emit(rc == 0, f"{name} rc={rc}")
        cursor_move_rc = command_return_code(out_dir / "xdotool-cursor-move.x11")
        emit(cursor_move_rc == 0, f"xdotool-cursor-move.x11 rc={cursor_move_rc}")
        emit(bool(active_hardware_cursor_heads(x11_cursor_text)),
             "x11 hardware cursor is enabled with fb/bo")
        if all(key in x11_state and key in x11_cursor_state for key in (
            "cursor_async_update_count",
            "plane_update_count",
        )):
            cursor_delta = (
                x11_cursor_state["cursor_async_update_count"] -
                x11_state["cursor_async_update_count"]
            )
            plane_delta = (
                x11_cursor_state["plane_update_count"] -
                x11_state["plane_update_count"]
            )
            emit(cursor_delta > 0,
                 f"cursor move used async cursor update delta={cursor_delta}")
            emit(plane_delta <= 2,
                 "cursor move did not trigger repeated primary plane "
                 f"updates delta={plane_delta}")
        else:
            emit(False, "missing cursor/plane counters around xdotool move")
        if all(key in before and key in x11_cursor_state for key in (
            "color_gamma_lut_count",
            "color_degamma_lut_count",
            "color_ctm_count",
        )):
            gamma_delta = (
                x11_cursor_state["color_gamma_lut_count"] -
                before["color_gamma_lut_count"]
            )
            degamma_delta = (
                x11_cursor_state["color_degamma_lut_count"] -
                before["color_degamma_lut_count"]
            )
            ctm_delta = (
                x11_cursor_state["color_ctm_count"] -
                before["color_ctm_count"]
            )
            if gamma_delta > 0 and degamma_delta == 0 and ctm_delta == 0:
                emit(True, "x11 color path is gamma-only")
            elif degamma_delta > 0 or ctm_delta > 0:
                print("INFO x11 used window-side color "
                      f"degamma_delta={degamma_delta} ctm_delta={ctm_delta}")
            else:
                print("INFO x11 did not change CRTC color counters")
        else:
            emit(False, "missing x11 color counters")
        xrandr_text = captured_text(out_dir / "xrandr.x11")
        emit(bool(re.search(r"^[A-Za-z0-9_.-]+ connected\b", xrandr_text, re.M)),
             "xrandr has connected output")
        emit(bool(re.search(r"\b[0-9]+x[0-9]+\b.*\*", xrandr_text)),
             "xrandr has active mode")
        gears_rc = command_return_code(out_dir / "glxgears.x11")
        emit(gears_rc in (0, 124), f"glxgears.x11 rc={gears_rc}")
        gears_text = captured_text(out_dir / "glxgears.x11")
        emit(bool(re.search(r"GL_RENDERER|GL_VERSION|frames in", gears_text, re.I)),
             "glxgears produced renderer/frame output")
        emit(not has_error_text(gears_text),
             "glxgears output has no errors")
        hpd_before = parse_state(out_dir / "drm_state.x11_hpd_before")
        hpd_after = parse_state(out_dir / "drm_state.x11_hpd_after")
        if hpd_before or hpd_after or (out_dir / "kms-hpd-inject.x11").exists():
            inject_rc = command_return_code(out_dir / "kms-hpd-inject.x11")
            emit(inject_rc == 0, f"kms-hpd-inject.x11 rc={inject_rc}")
            for key in (
                "hotplug_count",
                "hotplug_notify_only_count",
                "hotplug_nochange_count",
            ):
                if key in hpd_before and key in hpd_after:
                    delta = hpd_after[key] - hpd_before[key]
                    emit(delta > 0, f"{key} HPD delta={delta}")
                else:
                    emit(False, f"missing {key} HPD state")
            if (
                "hotplug_auto_kms_count" in hpd_before and
                "hotplug_auto_kms_count" in hpd_after
            ):
                delta = (
                    hpd_after["hotplug_auto_kms_count"] -
                    hpd_before["hotplug_auto_kms_count"]
                )
                emit(delta == 0, f"hotplug_auto_kms_count HPD delta={delta}")
            else:
                emit(False, "missing hotplug_auto_kms_count HPD state")
            if "scanout_user" in hpd_after:
                emit(hpd_after["scanout_user"] == 1,
                     f"x11 HPD kept user scanout={hpd_after['scanout_user']}")
            else:
                emit(False, "missing scanout_user HPD state")
            if "hpd_last_plug_mask" in hpd_after:
                emit(hpd_after["hpd_last_plug_mask"] != 0,
                     f"hpd_last_plug_mask=0x{hpd_after['hpd_last_plug_mask']:08x}")
            else:
                emit(False, "missing hpd_last_plug_mask HPD state")
        if (out_dir / "xrandr-panning.x11").exists():
            panning_rc = command_return_code(out_dir / "xrandr-panning.x11")
            emit(panning_rc == 0, f"xrandr-panning.x11 rc={panning_rc}")
        glxinfo = out_dir / "glxinfo-B.x11"
        if glxinfo.exists():
            text = captured_text(glxinfo)
            emit(bool(re.search(r"zink|NVK|Vulkan", text, re.I)),
                 "glxinfo shows zink/NVK/Vulkan")
            emit(not re.search(r"llvmpipe|softpipe|software rasterizer", text, re.I),
                 "glxinfo is not software rasterizer")

    xlog = sorted(out_dir.glob("*Xorg*.x11"))
    after_xlog = sorted(out_dir.glob("*Xorg*.after"))
    if x11_ran and xlog:
        texts = [path.read_text(errors="replace") for path in xlog]
        emit(any(re.search(r"glamor X acceleration enabled.*(zink|NVK|MESA_NVK)", text, re.I)
                 for text in texts),
             "Xorg log has glamor acceleration on zink/NVK")
        emit(not any(re.search(r"SWcursor|software cursor", text, re.I)
                     for text in texts),
             "Xorg log does not use software cursor")
        if after_xlog:
            after_texts = [path.read_text(errors="replace") for path in after_xlog]
            emit(any("Server terminated successfully" in text
                     for text in after_texts),
                 "Xorg terminated successfully")
        else:
            emit(False, "missing after Xorg log")
    elif x11_ran:
        emit(False, "missing x11 Xorg log")
    elif allow_missing_x11:
        print("INFO x11 phase not captured; skip Xorg log checks")

    for name in ("drmtest-build.after", "drmtest.after"):
        rc = command_return_code(out_dir / name)
        emit(rc == 0, f"{name} rc={rc}")
    drmtest_after = captured_text(out_dir / "drmtest.after")
    for text in (
        "DEGAMMA_LUT defaults to 0",
        "CTM defaults to 0",
        "GAMMA_LUT defaults to 0",
    ):
        emit(text in drmtest_after, f"drmtest.after has {text}")

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
                        help="briefly set and clear xrandr panning on the "
                             "first connected output")
    parser.add_argument("--run-hpd-inject", action="store_true",
                        help="inject a debug HPD event while X11 owns DRM "
                             "master")
    parser.add_argument("--hpd-inject-value", type=lambda value: int(value, 0),
                        default=0x400,
                        help="packed HPD mask: low16 plug, high16 unplug")
    parser.add_argument("--allow-missing-x11", action="store_true",
                        help="allow report-only console/debug runs without an x11 phase")
    args = parser.parse_args()

    out_dir = choose_out_dir(args.phase, args.out_dir)
    if args.phase == "report":
        print(out_dir)
        return report(out_dir, args.allow_missing_x11)

    capture_phase(out_dir, args.phase, args)
    print(out_dir)
    if args.phase == "after":
        return report(out_dir, args.allow_missing_x11)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
