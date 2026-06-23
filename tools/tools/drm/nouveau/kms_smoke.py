#!/usr/local/bin/python3
"""Collect a staged nvkm KMS runtime smoke-test report.

The runner is intentionally split into phases so physical testing can pause
between steps:

    kms_smoke.py before
    startx
    DISPLAY=:0 kms_smoke.py x11
    logout
    kms_smoke.py after
    kms_smoke.py syncobj_transfer

Wayland compositor tests may call `kms_smoke.py wayland_hpd` from a compositor
exec command while Wayland owns DRM master.

All output is written under /var/tmp so it survives reboot.
"""

import argparse
import datetime as dt
import os
import pathlib
import re
import shlex
import socket
import subprocess
import sys
import time


LATEST = pathlib.Path("/var/tmp/nvkm-kms-smoke.latest")
DEFAULT_PREFIX = "/var/tmp/nvkm-kms-smoke"
FULL_WAYLAND_PHASES = {
    "wayland",
    "wayland_egl",
    "wayland_glmark",
    "wayland_info",
    "wayland_hpd_smoke",
    "xwayland",
}
FAULT_PATTERN = (
    "panic|BADFREE|double fault|DeviceLost|EXEC timeout|fault|CMDre|"
    "status=0x19|RC_TRIGGERED|notifier timeout|vblank wait timed out|flip_done timed out|"
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
    "lastclose_restore_error_count",
    "dp_irq_error_count",
    "fb_create_error_count",
    "page_flip_error_count",
    "prepare_fb_error_count",
    "cursor_error_count",
    "console_flush_error_count",
    "bo_wait_error_count",
    "ttm_io_reserve_error_count",
    "ttm_io_reserve_last_error",
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
    "atomic_tail_plane_op_count",
    "atomic_tail_disable_op_count",
    "atomic_tail_color_op_count",
    "atomic_tail_enable_op_count",
    "atomic_tail_last_plane_op_count",
    "atomic_tail_last_disable_op_count",
    "atomic_tail_last_color_op_count",
    "atomic_tail_last_enable_op_count",
    "atomic_tail_last_legacy_cursor_update",
    "atomic_tail_last_async_update",
    "atomic_tail_last_lock_core",
    "atomic_tail_last_flush_disable",
    "atomic_tail_last_old_active_heads",
    "atomic_tail_last_new_active_heads",
    "atomic_tail_last_modeset_heads",
    "atomic_tail_last_disable_heads",
    "atomic_tail_last_enable_heads",
    "atomic_tail_last_primary_update_heads",
    "atomic_tail_last_primary_disable_heads",
    "atomic_tail_last_cursor_update_heads",
    "atomic_tail_last_cursor_disable_heads",
    "atomic_tail_last_plane_update_mask",
    "atomic_tail_last_plane_disable_mask",
    "atomic_tail_last_prepared_heads",
    "atomic_tail_last_prepared_displays",
    "atomic_disable_vblank_off_count",
    "atomic_disable_vblank_keep_count",
    "atomic_last_legacy_cursor_update",
    "atomic_last_async_update",
    "atomic_last_lock_core",
    "atomic_last_flush_disable",
    "atomic_last_old_active_heads",
    "atomic_last_new_active_heads",
    "atomic_last_modeset_heads",
    "atomic_last_disable_heads",
    "atomic_last_enable_heads",
    "atomic_last_primary_update_heads",
    "atomic_last_primary_disable_heads",
    "atomic_last_cursor_update_heads",
    "atomic_last_cursor_disable_heads",
    "atomic_last_plane_update_mask",
    "atomic_last_plane_disable_mask",
    "atomic_last_prepared_heads",
    "atomic_last_prepared_displays",
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
    "ttm_io_reserve_count",
    "ttm_io_reserve_bar1_retry_count",
    "ttm_io_free_count",
    "ttm_io_free_bar1_count",
    "ttm_io_reserve_last_size",
    "hotplug_count",
    "hotplug_changed_count",
    "hotplug_nochange_count",
    "hotplug_notify_only_count",
    "hotplug_auto_kms_count",
    "lastclose_restore_count",
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


def choose_new_out_dir() -> pathlib.Path:
    for attempt in range(100):
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        suffix = f"{stamp}-{os.getpid()}"
        if attempt:
            suffix = f"{suffix}-{attempt}"
        out_dir = pathlib.Path(f"{DEFAULT_PREFIX}-{suffix}")
        try:
            out_dir.mkdir(parents=True, exist_ok=False)
            return out_dir
        except FileExistsError:
            time.sleep(0.001)
    raise RuntimeError("failed to allocate a unique smoke output directory")


def choose_out_dir(phase: str, explicit: str | None) -> pathlib.Path:
    created = False
    if explicit:
        out_dir = pathlib.Path(explicit)
    elif os.environ.get("NVKM_KMS_SMOKE_DIR"):
        out_dir = pathlib.Path(os.environ["NVKM_KMS_SMOKE_DIR"])
    elif (phase == "before" or phase == "syncobj_transfer" or
          phase in FULL_WAYLAND_PHASES or not LATEST.is_symlink()):
        out_dir = choose_new_out_dir()
        created = True
    else:
        out_dir = pathlib.Path(os.readlink(LATEST))

    if not created:
        out_dir.mkdir(parents=True, exist_ok=True)
    if (phase == "before" or phase == "syncobj_transfer" or
        phase in FULL_WAYLAND_PHASES or not LATEST.exists()):
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


def x11_env(args: argparse.Namespace) -> dict[str, str]:
    env = os.environ.copy()
    if args.display:
        env["DISPLAY"] = args.display
    if args.xauthority:
        env["XAUTHORITY"] = args.xauthority
    return env


def probe_x11_display(out_dir: pathlib.Path, args: argparse.Namespace) -> int:
    env = x11_env(args)
    rc = run(["xrandr", "--query"], out_dir / "x11-display-probe.x11",
             env=env)
    if rc != 0:
        marker = out_dir / "x11-display-missing.x11"
        marker.write_text(
            "x11 phase requires an already running X server on DISPLAY. "
            "Start X from the physical console with startx, then run this "
            "phase with DISPLAY pointing at that server.\n"
        )
    return rc


def capture_hpd_inject(out_dir: pathlib.Path, inject_value: int,
                       label: str) -> int:
    path = out_dir / f"kms-hpd-inject.{label}"
    command = ["doas", "sysctl", f"dev.drm.0.kms_hpd_inject={inject_value:#x}"]
    devd_path = "/var/run/devd.seqpacket.pipe"

    with path.open("w") as out:
        out.write(f"### devd socket {devd_path}\n")
        out.write(f"### {command_text(command)}\n")
        out.flush()

        try:
            devd = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            devd.settimeout(8.0)
            devd.connect(devd_path)
        except OSError as err:
            out.write(f"\n### devd-connect-error={err}\n")
            out.write("\n### rc=1\n")
            return 1

        with devd:
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            out.write(result.stdout)
            out.flush()
            if result.returncode != 0:
                out.write(f"\n### rc={result.returncode}\n")
                return result.returncode

            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline:
                try:
                    data = devd.recv(4096)
                except socket.timeout:
                    break
                if not data:
                    break
                text = data.decode("utf-8", "replace").strip()
                out.write(f"### devd-event {text}\n")
                out.flush()
                if (
                    "system=DRM" in text and
                    "type=HOTPLUG" in text and
                    "HOTPLUG=1" in text
                ):
                    out.write("\n### rc=0\n")
                    return 0

            out.write("\n### devd-hotplug-missing\n")
            out.write("\n### rc=1\n")
            return 1


def capture_console_dark_down(out_dir: pathlib.Path, display_id: int) -> None:
    unplug_value = (display_id & 0x0000ffff) << 16

    run(["sysctl", "-n", "dev.drm.0.state"],
        out_dir / "drm_state.console_dark_before")
    run([
        "doas",
        "sysctl",
        f"dev.drm.0.kms_detect_force_disconnect_mask={display_id:#x}",
    ], out_dir / "kms-detect-force-disconnect.console")
    run([
        "doas",
        "sysctl",
        f"dev.drm.0.kms_hpd_inject={unplug_value:#x}",
    ], out_dir / "kms-hpd-unplug.console")
    run(["sleep", "2"], out_dir / "console-dark-wait.console")
    run(["sysctl", "-n", "dev.drm.0.state"],
        out_dir / "drm_state.console_dark_after")
    capture_kms_property_probe(out_dir, "console_dark", {
        "NVKM_DRMTEST_METADATA_ONLY": "1",
        "NVKM_DRMTEST_EXPECT_NO_CONNECTED": "1",
    })
    run([
        "doas",
        "sysctl",
        "dev.drm.0.kms_detect_force_disconnect_mask=0",
    ], out_dir / "kms-detect-force-disconnect-clear.console")
    run([
        "doas",
        "sysctl",
        f"dev.drm.0.kms_hpd_inject={display_id:#x}",
    ], out_dir / "kms-hpd-plug-restore.console")
    run(["sleep", "2"], out_dir / "console-restore-wait.console")
    run(["doas", "sysctl", "dev.drm.0.kms_lightup=1"],
        out_dir / "kms-lightup-restore.console")
    run(["sleep", "1"], out_dir / "console-lightup-wait.console")
    run(["sysctl", "-n", "dev.drm.0.state"],
        out_dir / "drm_state.console_dark_restore")


def capture_wayland_hpd(out_dir: pathlib.Path, inject_value: int) -> None:
    run(["sleep", "1"], out_dir / "wayland-hpd-owner-wait.wayland")
    run(["sysctl", "-n", "dev.drm.0.state"],
        out_dir / "drm_state.wayland_hpd_before")
    capture_hpd_inject(out_dir, inject_value, "wayland")
    run(["sleep", "1"], out_dir / "hpd-inject-wait.wayland")
    run(["sysctl", "-n", "dev.drm.0.state"],
        out_dir / "drm_state.wayland_hpd_after")


def wayland_smoke_env(out_dir: pathlib.Path) -> dict[str, str]:
    env = os.environ.copy()
    runtime_dir = pathlib.Path(f"/tmp/runtime-{os.getuid()}")
    runtime_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(runtime_dir, 0o700)
    env["XDG_RUNTIME_DIR"] = str(runtime_dir)
    env["NVKM_KMS_SMOKE_DIR"] = str(out_dir)
    env.setdefault("TERMINAL", "xfce4-terminal")
    return env


def write_sway_config(out_dir: pathlib.Path, mode: str,
                      args: argparse.Namespace) -> pathlib.Path:
    path = out_dir / f"sway-{mode}.conf"
    lines = ["output * bg #000000 solid_color"]
    if mode == "wayland":
        command = f"sleep {args.wayland_exit_delay}; swaymsg exit"
    elif mode == "wayland_info":
        command = (
            "sleep 1; "
            "{ echo WAYLAND_INFO_START; wayland-info; rc=$?; "
            "echo \"### rc=$rc\"; } "
            "> \"$NVKM_KMS_SMOKE_DIR/wayland-info.wayland\" 2>&1; "
            "swaymsg exit"
        )
    elif mode == "wayland_egl":
        egl_seconds = max(args.egl_seconds, 1)
        command = (
            "sleep 1; "
            f"{{ echo WAYLAND_EGL_START; timeout {egl_seconds} libdecor-egl; "
            "rc=$?; echo \"### rc=$rc\"; } "
            "> \"$NVKM_KMS_SMOKE_DIR/libdecor-egl.wayland\" 2>&1; "
            "swaymsg exit"
        )
    elif mode == "wayland_glmark":
        glmark_seconds = max(args.glmark_seconds, 1)
        command = (
            "sleep 1; "
            "echo GLMARK2_WAYLAND_START "
            "> \"$NVKM_KMS_SMOKE_DIR/glmark2-wayland.wayland\"; "
            "WAYLAND_DEBUG=client ZINK_KOPPER_TRACE=1 "
            "glmark2-wayland --run-forever "
            "--benchmark build --size 320x240 --debug "
            ">> \"$NVKM_KMS_SMOKE_DIR/glmark2-wayland.wayland\" 2>&1 & "
            "pid=$!; "
            f"sleep {glmark_seconds}; "
            "ps -p \"$pid\" -o pid,stat,command "
            "> \"$NVKM_KMS_SMOKE_DIR/ps.wayland_glmark\" 2>&1; "
            "sysctl -n dev.drm.0.state "
            "> \"$NVKM_KMS_SMOKE_DIR/drm_state.wayland_glmark_live\" 2>&1; "
            "timeout 10 gdb -batch -ex 'set pagination off' "
            "-ex 'info threads' -ex 'bt' -p \"$pid\" "
            "> \"$NVKM_KMS_SMOKE_DIR/gdb.wayland_glmark\" 2>&1; "
            "kill -TERM \"$pid\" 2>/dev/null; "
            "sleep 1; "
            "kill -KILL \"$pid\" 2>/dev/null; "
            "wait \"$pid\"; "
            "rc=$?; echo \"### rc=$rc\" "
            ">> \"$NVKM_KMS_SMOKE_DIR/glmark2-wayland.wayland\"; "
            "swaymsg exit"
        )
    elif mode == "wayland_hpd_smoke":
        phase = pathlib.Path(__file__)
        command = (
            f"sleep {args.wayland_hpd_delay}; "
            f"{shlex.quote(str(phase))} wayland_hpd "
            f"--out-dir \"$NVKM_KMS_SMOKE_DIR\" "
            f"--hpd-inject-value {args.hpd_inject_value:#x} "
            "> \"$NVKM_KMS_SMOKE_DIR/wayland-hpd-phase.stdout\" 2>&1; "
            "swaymsg exit"
        )
    elif mode == "xwayland":
        lines.append("xwayland enable")
        gears_seconds = max(args.gears_seconds, 1)
        command = (
            "sleep 1; "
            "{ echo GLXINFO_START; glxinfo -B; echo \"### rc=$?\"; } "
            "> \"$NVKM_KMS_SMOKE_DIR/glxinfo.xwayland\" 2>&1; "
            f"{{ echo GLXGEARS_START; timeout {gears_seconds} glxgears -info; "
            "echo \"### rc=$?\"; } "
            "> \"$NVKM_KMS_SMOKE_DIR/glxgears.xwayland\" 2>&1; "
            "swaymsg exit"
        )
    else:
        raise ValueError(f"unsupported sway smoke mode {mode}")
    lines.append(f"exec /bin/sh -c {shlex.quote(command)}")
    path.write_text("\n".join(lines) + "\n")
    return path


def run_wayland_smoke(out_dir: pathlib.Path, mode: str,
                      args: argparse.Namespace) -> int:
    capture_phase(out_dir, "before", args)
    env = wayland_smoke_env(out_dir)
    config = write_sway_config(out_dir, mode, args)
    rc = run(
        [args.sway_command, "-d", "-c", str(config)],
        out_dir / "sway.log",
        env=env,
        timeout=args.wayland_timeout,
    )
    (out_dir / "rc").write_text(f"{rc}\n")
    capture_phase(out_dir, "after", args)
    return report(out_dir, allow_missing_x11=True)


def capture_kms_property_probe(out_dir: pathlib.Path, phase: str,
                               env_extra: dict[str, str] | None = None) -> None:
    source = pathlib.Path(__file__).with_name("drmtest.c")
    binary = out_dir / "drmtest"
    env = os.environ.copy()
    build_cmd = (
        "set -eu; "
        "cc -Wall -Wextra -Werror $(pkg-config --cflags libdrm) "
        f"{shlex.quote(str(source))} -o {shlex.quote(str(binary))} "
        "$(pkg-config --libs libdrm)"
    )

    if run(["/bin/sh", "-c", build_cmd],
           out_dir / f"drmtest-build.{phase}") != 0:
        return
    if env_extra:
        env.update(env_extra)
    run([str(binary)], out_dir / f"drmtest.{phase}", env=env)


def capture_syncobj_transfer_probe(out_dir: pathlib.Path) -> int | None:
    capture_kms_property_probe(out_dir, "syncobj_transfer", {
        "NVKM_DRMTEST_SYNC_ONLY": "1",
    })
    return command_return_code(out_dir / "drmtest.syncobj_transfer")


def state_is_idle(state: dict[str, int]) -> bool:
    required = (
        "atomic_tail_active",
        "atomic_tail_stage",
        "atomic_tail_seq",
        "atomic_tail_complete_count",
        "atomic_tail_finish_prepared_count",
    )
    if any(key not in state for key in required):
        return False
    return (
        state["atomic_tail_active"] == 0 and
        state["atomic_tail_stage"] == 0 and
        state["atomic_tail_seq"] == state["atomic_tail_complete_count"] and
        state["atomic_tail_seq"] == state["atomic_tail_finish_prepared_count"]
    )


def wait_for_kms_idle(out_dir: pathlib.Path, timeout_sec: float) -> None:
    path = out_dir / "kms-idle-wait.after"
    deadline = time.monotonic() + timeout_sec

    with path.open("w") as out:
        out.write(f"### wait_for_kms_idle timeout={timeout_sec:.1f}s\n")
        while True:
            result = subprocess.run(
                ["sysctl", "-n", "dev.drm.0.state"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            state_path = out_dir / "drm_state.after_idle_last"
            state_path.write_text(result.stdout)
            state = parse_state(state_path)
            if result.returncode == 0 and state_is_idle(state):
                out.write("### idle=1\n")
                out.write(f"### rc={result.returncode}\n")
                return
            if time.monotonic() >= deadline:
                active = state.get("atomic_tail_active")
                stage = state.get("atomic_tail_stage")
                seq = state.get("atomic_tail_seq")
                complete = state.get("atomic_tail_complete_count")
                finish = state.get("atomic_tail_finish_prepared_count")
                out.write(
                    "### idle=0 "
                    f"active={active} stage={stage} seq={seq} "
                    f"complete={complete} finish={finish}\n"
                )
                out.write(f"### rc={result.returncode}\n")
                return
            time.sleep(0.1)


def capture_phase(out_dir: pathlib.Path, phase: str, args: argparse.Namespace) -> None:
    env = x11_env(args)

    if phase == "wayland_hpd":
        capture_wayland_hpd(out_dir, args.hpd_inject_value)
        return

    if phase == "after" and args.run_console_dark_down:
        capture_console_dark_down(out_dir, args.dark_down_display_id)
    if phase == "after":
        wait_for_kms_idle(out_dir, args.kms_idle_timeout)

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
        capture_hpd_inject(out_dir, args.hpd_inject_value, "x11")
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


def indexed_state_values(text: str, prefix: str, field: str) -> list[tuple[int, int]]:
    values: list[tuple[int, int]] = []
    pattern = re.compile(
        rf"^{re.escape(prefix)}\[(\d+)\]_{re.escape(field)}\s*=\s*"
        r"(0x[0-9a-fA-F]+|[0-9]+)\b",
        re.M,
    )
    for match in pattern.finditer(text):
        index_text, value_text = match.groups()
        values.append((
            int(index_text),
            int(value_text, 16 if value_text.startswith("0x") else 10),
        ))
    return values


def nonzero_indices(text: str, prefix: str, field: str) -> list[int]:
    return [
        index
        for index, value in indexed_state_values(text, prefix, field)
        if value != 0
    ]


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


def plain_return_code(path: pathlib.Path) -> int | None:
    if not path.exists():
        return None
    text = path.read_text(errors="replace").strip()
    if not re.fullmatch(r"[0-9]+", text):
        return None
    return int(text)


def captured_text(path: pathlib.Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(errors="replace")


def drmtest_object_suffixes(text: str, kind: str) -> dict[str, set[str]]:
    objects: dict[str, set[str]] = {}
    pattern = re.compile(rf"^PASS {re.escape(kind)} ([0-9]+) (.*)$", re.M)
    for match in pattern.finditer(text):
        object_id = match.group(1)
        suffix = match.group(2)
        objects.setdefault(object_id, set()).add(suffix)
    return objects


def report_all_object_suffixes(
    text: str,
    kind: str,
    suffixes: tuple[str, ...],
    emit,
) -> None:
    objects = drmtest_object_suffixes(text, kind)
    emit(bool(objects), f"{kind} dynamic object coverage exists")
    if not objects:
        return

    for suffix in suffixes:
        missing = sorted(
            object_id for object_id, seen in objects.items()
            if suffix not in seen
        )
        emit(not missing, f"every {kind} {object_suffix_report(suffix)}")


def report_any_object_suffixes(
    text: str,
    kind: str,
    suffixes: tuple[str, ...],
    label: str,
    emit,
) -> None:
    objects = drmtest_object_suffixes(text, kind)
    emit(bool(objects), f"{kind} dynamic object coverage exists for {label}")
    if not objects:
        return

    for suffix in suffixes:
        emit(
            any(suffix in seen for seen in objects.values()),
            f"{label} {object_suffix_report(suffix)}",
        )


def object_suffix_report(suffix: str) -> str:
    if suffix.startswith("has property "):
        return f"exposes property {suffix.removeprefix('has property ')}"
    if suffix.startswith("does not expose "):
        return suffix
    return f"passes {suffix}"


CONNECTOR_DYNAMIC_SUFFIXES = (
    "is not writeback",
    "has property EDID",
    "has property CRTC_ID",
    "has property link-status",
    "link-status is enum",
    "link-status has enum Good",
    "link-status defaults to Good",
    "has property scaling mode",
    "scaling mode is enum",
    "scaling mode has enum None",
    "scaling mode defaults to None",
    "has property dithering mode",
    "dithering mode is enum",
    "dithering mode has enum auto",
    "dithering mode defaults to auto",
    "has property dithering depth",
    "dithering depth is enum",
    "dithering depth has enum auto",
    "dithering depth defaults to auto",
    "has property max bpc",
    "max bpc is range",
    "max bpc min is 8",
    "max bpc max is 8",
    "max bpc value is 8",
    "does not expose unsupported connector property HDR_OUTPUT_METADATA",
    "does not expose unsupported connector property Colorspace",
    "does not expose unsupported connector property content type",
    "does not expose unsupported connector property vrr_capable",
    "has property underscan",
    "underscan is enum",
    "underscan has enum off",
    "underscan defaults to off",
    "has property underscan hborder",
    "underscan hborder is range",
    "underscan hborder min is 0",
    "underscan hborder max is 128",
    "underscan hborder value is 0",
    "has property underscan vborder",
    "underscan vborder is range",
    "underscan vborder min is 0",
    "underscan vborder max is 128",
    "underscan vborder value is 0",
)

CRTC_DYNAMIC_SUFFIXES = (
    "has property ACTIVE",
    "has property MODE_ID",
    "has property DEGAMMA_LUT",
    "DEGAMMA_LUT is blob",
    "DEGAMMA_LUT defaults to 0",
    "has property DEGAMMA_LUT_SIZE",
    "DEGAMMA_LUT_SIZE is range",
    "DEGAMMA_LUT_SIZE value is 1024",
    "has property CTM",
    "CTM is blob",
    "CTM defaults to 0",
    "has property GAMMA_LUT",
    "GAMMA_LUT is blob",
    "GAMMA_LUT defaults to 0",
    "has property GAMMA_LUT_SIZE",
    "GAMMA_LUT_SIZE is range",
    "GAMMA_LUT_SIZE value is 1024",
    "has property OUT_FENCE_PTR",
    "OUT_FENCE_PTR is range",
    "OUT_FENCE_PTR min is 0",
    "OUT_FENCE_PTR max is 18446744073709551615",
    "OUT_FENCE_PTR value is 0",
    "does not expose unsupported CRTC property VRR_ENABLED",
)

PLANE_DYNAMIC_SUFFIXES = (
    "has property type",
    "has property IN_FENCE_FD",
    "IN_FENCE_FD is signed range",
    "IN_FENCE_FD min is -1",
    "IN_FENCE_FD max is 2147483647",
    "IN_FENCE_FD value is -1",
    "has property CRTC_ID",
    "has property FB_ID",
)

ACTIVE_PRIMARY_PLANE_GEOMETRY_SUFFIXES = (
    "has property CRTC_X",
    "has property CRTC_Y",
    "has property CRTC_W",
    "has property CRTC_H",
    "has property SRC_X",
    "has property SRC_Y",
    "has property SRC_W",
    "has property SRC_H",
)


def has_error_text(text: str) -> bool:
    return bool(re.search(
        r"(^|\n)(Error:|.*\bfailed\b|Segmentation fault|core dumped|DeviceLost|"
        r"couldn['’]?t open display|No protocol specified)",
        text,
        re.I,
    ))


LEFTOVER_BASENAMES = {
    "Xorg",
    "Xwayland",
    "glxgears",
    "hikari",
    "firefox",
    "firefox-bin",
    "sway",
    "swayfx",
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


def is_software_renderer(text: str) -> bool:
    return bool(re.search(
        r"llvmpipe|softpipe|software rasterizer",
        text,
        re.I,
    ))


def report_wayland_log(out_dir: pathlib.Path, emit) -> None:
    logs = sorted(out_dir.glob("*sway*.log"))
    if not logs:
        return

    compositor_rc = plain_return_code(out_dir / "rc")
    emit(compositor_rc == 0, f"Wayland compositor rc={compositor_rc}")

    text = "\n".join(path.read_text(errors="replace") for path in logs)
    emit(bool(re.search(r"Initializing DRM backend for /dev/dri/card0 \(nouveau\)", text)),
         "Wayland log uses nouveau DRM backend")
    emit("Using atomic DRM interface" in text,
         "Wayland log uses atomic DRM interface")
    emit("ADDFB2 modifiers supported" in text,
         "Wayland log has ADDFB2 modifier support")
    emit(bool(re.search(r"EGL driver name:\s*zink\b", text, re.I)),
         "Wayland log uses zink EGL driver")
    emit(bool(re.search(r"GL renderer:\s*zink .*NVK|GL renderer:.*MESA_NVK", text, re.I)),
         "Wayland log uses zink/NVK renderer")
    emit(not is_software_renderer(text),
         "Wayland log is not software renderer")
    emit(bool(re.search(r"Allocated .* GBM buffer .*BLOCK_LINEAR_2D", text)),
         "Wayland log allocates NVIDIA block-linear GBM buffer")
    emit(bool(re.search(r"Commit of [0-9]+ outputs succeeded", text)),
         "Wayland log commits output successfully")

    emit("wl_display_terminate" not in text,
         "Wayland compositor exits without wl_display_terminate assert")
    if "Starting Xwayland" in text or "Xserver is ready" in text:
        emit(bool(re.search(r"Starting Xwayland on :[0-9]+", text)),
             "Xwayland server started")
        emit("Xserver is ready" in text,
             "Xwayland server became ready")
        emit(bool(re.search(r"New xwayland surface", text, re.I)),
             "Xwayland created an X11 surface")


def report_wayland_info(out_dir: pathlib.Path, emit) -> None:
    path = out_dir / "wayland-info.wayland"
    if not path.exists():
        return

    rc = command_return_code(path)
    emit(rc == 0, f"wayland-info rc={rc}")
    text = captured_text(path)
    emit("WAYLAND_INFO_START" in text, "wayland-info started")
    emit(bool(re.search(r"interface:\s+'wl_compositor'", text)),
         "wayland-info sees wl_compositor")
    emit(bool(re.search(r"interface:\s+'xdg_wm_base'", text)),
         "wayland-info sees xdg_wm_base")
    emit(bool(re.search(r"interface:\s+'wp_presentation'", text)),
         "wayland-info sees wp_presentation")
    has_syncobj_manager = bool(re.search(
        r"interface:\s+'wp_linux_drm_syncobj_manager_v1'", text))
    syncobj_status = "present" if has_syncobj_manager else "absent"
    emit(True, f"wayland-info linux drm syncobj manager is {syncobj_status}")
    emit(bool(re.search(r"presentation clock id:\s*[0-9]+", text)),
         "wayland-info reports presentation clock")
    emit(bool(re.search(r"interface:\s+'wp_drm_lease_device_v1'", text)) and
         bool(re.search(r"path:\s+/dev/dri/card0\b", text)),
         "wayland-info exposes DRM lease device card0")
    emit(bool(re.search(r"interface:\s+'zwlr_export_dmabuf_manager_v1'", text)),
         "wayland-info sees dmabuf export manager")
    emit(bool(re.search(r"interface:\s+'wl_output'.*?name:\s+HDMI-A-1",
                        text, re.S)),
         "wayland-info sees HDMI-A-1 output")
    emit(bool(re.search(r"mode:\s+width:\s+1920 px,\s+height:\s+1080 px",
                        text, re.S)),
         "wayland-info sees current 1920x1080 mode")
    emit(not has_error_text(text), "wayland-info output has no errors")


def report_wayland_egl(out_dir: pathlib.Path, emit) -> None:
    path = out_dir / "libdecor-egl.wayland"
    if not path.exists():
        return

    rc = command_return_code(path)
    emit(rc in (0, 124), f"libdecor-egl rc={rc}")
    text = captured_text(path)
    emit("WAYLAND_EGL_START" in text, "libdecor-egl started")
    emit(not re.search(r"Segmentation fault|signal 11|core dumped|wl_proxy_create_wrapper",
                       text, re.I),
         "libdecor-egl did not hit the Wayland EGL surface crash")
    emit(not has_error_text(text), "libdecor-egl output has no errors")

    logs = sorted(out_dir.glob("*sway*.log"))
    log_text = "\n".join(path.read_text(errors="replace") for path in logs)
    emit(bool(re.search(r"new xdg_surface", log_text, re.I)),
         "libdecor-egl created an xdg surface")
    emit(bool(re.search(r"New xdg_shell toplevel", log_text, re.I)),
         "libdecor-egl created an xdg toplevel")
    emit(bool(re.search(r"new xdg_toplevel_decoration", log_text, re.I)),
         "libdecor-egl created an xdg decoration")


def report_wayland_glmark(out_dir: pathlib.Path, emit) -> None:
    path = out_dir / "glmark2-wayland.wayland"
    if not path.exists():
        return

    rc = command_return_code(path)
    emit(rc in (0, 137, 143), f"glmark2-wayland controlled shutdown rc={rc}")
    text = captured_text(path)
    emit("GLMARK2_WAYLAND_START" in text, "glmark2-wayland started")
    emit(bool(re.search(r"GL_RENDERER:.*zink.*NVK|zink.*NVK", text, re.I)),
         "glmark2-wayland uses zink/NVK")
    emit(not re.search(r"Segmentation fault|signal 11|core dumped|DeviceLost",
                       text, re.I),
         "glmark2-wayland did not crash")
    emit(not re.search(
        r"QueuePresentKHR.*(-13|VK_ERROR_UNKNOWN)|"
        r"queue-present\s+ret=-13|"
        r"present\s+queue-present\s+ret=-13|"
        r"VK_ERROR_UNKNOWN",
        text, re.I),
         "glmark2-wayland does not fail QueuePresentKHR")
    explicit_sync = "wp_linux_drm_syncobj_manager_v1" in text
    emit(bool(re.search(r"wl_surface#[0-9]+\.attach", text)),
         "glmark2-wayland attaches a Wayland buffer")
    emit(bool(re.search(r"wl_surface#[0-9]+\.(damage|damage_buffer)", text)),
         "glmark2-wayland damages a Wayland surface")
    emit(bool(re.search(r"wl_surface#[0-9]+\.commit", text)),
         "glmark2-wayland commits a Wayland surface")
    if explicit_sync:
        emit("set_acquire_point" in text,
             "glmark2-wayland sets explicit acquire point")
        emit("set_release_point" in text,
             "glmark2-wayland sets explicit release point")
    else:
        emit(bool(re.search(r"wl_buffer@[0-9]+\.release", text)),
             "glmark2-wayland receives wl_buffer release")


def report_xwayland_glxinfo(out_dir: pathlib.Path, emit) -> None:
    path = out_dir / "glxinfo.xwayland"
    if not path.exists():
        return

    text = path.read_text(errors="replace")
    emit(bool(re.search(r"direct rendering:\s*Yes", text, re.I)),
         "Xwayland glxinfo has direct rendering")
    emit(bool(re.search(r"OpenGL renderer string:.*zink.*NVK|Device: zink .*NVK", text, re.I)),
         "Xwayland glxinfo uses zink/NVK renderer")
    emit(bool(re.search(r"Accelerated:\s*yes", text, re.I)),
         "Xwayland glxinfo is accelerated")
    emit(not is_software_renderer(text),
         "Xwayland glxinfo is not software renderer")
    emit(not has_error_text(text),
         "Xwayland glxinfo output has no errors")


def report_xwayland_glxgears(out_dir: pathlib.Path, emit) -> None:
    path = out_dir / "glxgears.xwayland"
    if not path.exists():
        return

    gears_rc = command_return_code(path)
    emit(gears_rc in (0, 124), f"Xwayland glxgears rc={gears_rc}")
    text = captured_text(path)
    emit(bool(re.search(r"GL_RENDERER|GL_VERSION|frames in|GLXGEARS_START",
                        text, re.I)),
         "Xwayland glxgears produced renderer/frame/marker output")
    emit(not has_error_text(text),
         "Xwayland glxgears output has no errors")


def report_hpd_inject(out_dir: pathlib.Path, label: str, emit) -> None:
    hpd_before = parse_state(out_dir / f"drm_state.{label}_hpd_before")
    hpd_after = parse_state(out_dir / f"drm_state.{label}_hpd_after")
    inject_path = out_dir / f"kms-hpd-inject.{label}"
    if not (hpd_before or hpd_after or inject_path.exists()):
        return

    inject_rc = command_return_code(inject_path)
    emit(inject_rc == 0, f"kms-hpd-inject.{label} rc={inject_rc}")
    inject_text = captured_text(inject_path)
    emit(bool(re.search(
        r"!system=DRM\s+subsystem=card[0-9]+\s+type=HOTPLUG\b"
        r".*\bHOTPLUG=1\b.*\bcard=[0-9]+\b.*\brender=-?[0-9]+\b",
        inject_text,
    )), f"{label} devd client received DRM HOTPLUG event")
    for key in (
        "hotplug_count",
        "hotplug_notify_only_count",
        "hotplug_nochange_count",
    ):
        if key in hpd_before and key in hpd_after:
            delta = hpd_after[key] - hpd_before[key]
            emit(delta > 0, f"{label} {key} HPD delta={delta}")
        else:
            emit(False, f"missing {label} {key} HPD state")
    if (
        "hotplug_auto_kms_count" in hpd_before and
        "hotplug_auto_kms_count" in hpd_after
    ):
        delta = (
            hpd_after["hotplug_auto_kms_count"] -
            hpd_before["hotplug_auto_kms_count"]
        )
        emit(delta == 0, f"{label} hotplug_auto_kms_count HPD delta={delta}")
    else:
        emit(False, f"missing {label} hotplug_auto_kms_count HPD state")
    if "scanout_user" in hpd_after:
        emit(hpd_after["scanout_user"] == 1,
             f"{label} HPD kept user scanout={hpd_after['scanout_user']}")
    else:
        emit(False, f"missing {label} scanout_user HPD state")
    if "hpd_last_plug_mask" in hpd_after:
        emit(hpd_after["hpd_last_plug_mask"] != 0,
             f"{label} hpd_last_plug_mask=0x{hpd_after['hpd_last_plug_mask']:08x}")
    else:
        emit(False, f"missing {label} hpd_last_plug_mask HPD state")


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
    after_state_text = captured_text(out_dir / "drm_state.after")
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
            "lastclose_restore_last_error",
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

    capacity_checks = (
        ("ready head index", nonzero_indices(after_state_text, "head", "ready"),
         "display_head_capacity"),
        ("present window index", nonzero_indices(after_state_text, "wndw", "present"),
         "display_window_capacity"),
        ("cursor channel index",
         nonzero_indices(after_state_text, "head", "cursor_channel_present"),
         "display_cursor_capacity"),
        ("output IOR id",
         [ior_id for _, ior_id in indexed_state_values(after_state_text, "outp", "ior_id")],
         "display_sor_capacity"),
    )
    for label, indices, capacity_key in capacity_checks:
        if capacity_key not in after:
            continue
        capacity = after[capacity_key]
        bad = [index for index in indices if index >= capacity]
        emit(not bad, f"{label} fits {capacity_key}={capacity}")
        for index in bad:
            print(f"INFO {label} {index} exceeds {capacity_key}={capacity}")

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

    tail_summary_pairs = (
        ("atomic_tail_last_legacy_cursor_update", "atomic_last_legacy_cursor_update"),
        ("atomic_tail_last_async_update", "atomic_last_async_update"),
        ("atomic_tail_last_lock_core", "atomic_last_lock_core"),
        ("atomic_tail_last_flush_disable", "atomic_last_flush_disable"),
        ("atomic_tail_last_old_active_heads", "atomic_last_old_active_heads"),
        ("atomic_tail_last_new_active_heads", "atomic_last_new_active_heads"),
        ("atomic_tail_last_modeset_heads", "atomic_last_modeset_heads"),
        ("atomic_tail_last_disable_heads", "atomic_last_disable_heads"),
        ("atomic_tail_last_enable_heads", "atomic_last_enable_heads"),
        ("atomic_tail_last_primary_update_heads", "atomic_last_primary_update_heads"),
        ("atomic_tail_last_primary_disable_heads", "atomic_last_primary_disable_heads"),
        ("atomic_tail_last_cursor_update_heads", "atomic_last_cursor_update_heads"),
        ("atomic_tail_last_cursor_disable_heads", "atomic_last_cursor_disable_heads"),
        ("atomic_tail_last_plane_update_mask", "atomic_last_plane_update_mask"),
        ("atomic_tail_last_plane_disable_mask", "atomic_last_plane_disable_mask"),
        ("atomic_tail_last_prepared_heads", "atomic_last_prepared_heads"),
        ("atomic_tail_last_prepared_displays", "atomic_last_prepared_displays"),
    )
    for tail_key, summary_key in tail_summary_pairs:
        if tail_key not in after or summary_key not in after:
            if tail_key not in after:
                emit(False, f"missing {tail_key}")
            if summary_key not in after:
                emit(False, f"missing {summary_key}")
            continue
        emit(after[tail_key] == after[summary_key],
             f"{tail_key} matches {summary_key}")

    if all(key in after for key in (
        "atomic_last_enable_heads",
        "atomic_last_prepared_heads",
        "atomic_last_prepared_displays",
    )):
        enable_heads = after["atomic_last_enable_heads"]
        prepared_heads = after["atomic_last_prepared_heads"]
        prepared_displays = after["atomic_last_prepared_displays"]
        if enable_heads != 0:
            emit((prepared_heads & enable_heads) == enable_heads,
                 "atomic display transaction prepared every enabled head")
            emit(prepared_displays != 0,
                 "atomic display transaction prepared a display route")
        else:
            print("INFO atomic display transaction had no enable heads")
    else:
        for key in (
            "atomic_last_enable_heads",
            "atomic_last_prepared_heads",
            "atomic_last_prepared_displays",
        ):
            if key not in after:
                emit(False, f"missing {key}")

    ps_after = (out_dir / "ps.after").read_text(errors="replace") if (out_dir / "ps.after").exists() else ""
    leftovers = leftover_graphics_commands(ps_after)
    emit(not leftovers, "no Xorg/glxgears/firefox leftovers")
    for command in leftovers:
        print(f"INFO leftover: {command}")

    x11_ran = (out_dir / "drm_state.x11").exists()
    wayland_ran = any(out_dir.glob("*sway*.log"))
    if x11_ran or wayland_ran:
        if all(key in before and key in after for key in (
            "lastclose_restore_count",
            "lastclose_restore_error_count",
        )):
            restore_delta = (
                after["lastclose_restore_count"] -
                before["lastclose_restore_count"]
            )
            restore_error_delta = (
                after["lastclose_restore_error_count"] -
                before["lastclose_restore_error_count"]
            )
            emit(restore_delta > 0,
                 f"display owner lastclose restore delta={restore_delta}")
            emit(restore_error_delta == 0,
                 f"display owner lastclose restore error delta={restore_error_delta}")
        else:
            emit(False, "missing lastclose restore counters")
        if "lastclose_restore_last_error" in after:
            emit(after["lastclose_restore_last_error"] == 0,
                 f"lastclose_restore_last_error={after['lastclose_restore_last_error']}")
        else:
            emit(False, "missing lastclose_restore_last_error")

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
        report_hpd_inject(out_dir, "x11", emit)
        if (out_dir / "xrandr-panning.x11").exists():
            panning_rc = command_return_code(out_dir / "xrandr-panning.x11")
            emit(panning_rc == 0, f"xrandr-panning.x11 rc={panning_rc}")
        glxinfo = out_dir / "glxinfo-B.x11"
        if glxinfo.exists():
            text = captured_text(glxinfo)
            emit(bool(re.search(r"zink|NVK|Vulkan", text, re.I)),
                 "glxinfo shows zink/NVK/Vulkan")
            emit(not is_software_renderer(text),
                 "glxinfo is not software rasterizer")

    report_wayland_log(out_dir, emit)
    report_wayland_info(out_dir, emit)
    report_wayland_egl(out_dir, emit)
    report_wayland_glmark(out_dir, emit)
    report_xwayland_glxinfo(out_dir, emit)
    report_xwayland_glxgears(out_dir, emit)
    report_hpd_inject(out_dir, "wayland", emit)

    dark_before = parse_state(out_dir / "drm_state.console_dark_before")
    dark_after = parse_state(out_dir / "drm_state.console_dark_after")
    dark_restore = parse_state(out_dir / "drm_state.console_dark_restore")
    if dark_before or dark_after or dark_restore:
        for name in (
            "kms-detect-force-disconnect.console",
            "kms-hpd-unplug.console",
            "drmtest-build.console_dark",
            "drmtest.console_dark",
            "kms-detect-force-disconnect-clear.console",
            "kms-hpd-plug-restore.console",
            "kms-lightup-restore.console",
        ):
            rc = command_return_code(out_dir / name)
            emit(rc == 0, f"{name} rc={rc}")
        for state_name, state in (
            ("dark-before", dark_before),
            ("dark-after", dark_after),
            ("dark-restore", dark_restore),
        ):
            emit(bool(state), f"{state_name} state captured")
        if dark_before and dark_after:
            if "dark_down_count" in dark_before and "dark_down_count" in dark_after:
                delta = dark_after["dark_down_count"] - dark_before["dark_down_count"]
                emit(delta > 0, f"dark_down_count delta={delta}")
            else:
                emit(False, "missing dark_down_count around console dark-down")
            if (
                "dark_down_error_count" in dark_before and
                "dark_down_error_count" in dark_after
            ):
                delta = (
                    dark_after["dark_down_error_count"] -
                    dark_before["dark_down_error_count"]
                )
                emit(delta == 0, f"dark_down_error_count delta={delta}")
            else:
                emit(False, "missing dark_down_error_count around console dark-down")
            if (
                "hotplug_auto_kms_count" in dark_before and
                "hotplug_auto_kms_count" in dark_after
            ):
                delta = (
                    dark_after["hotplug_auto_kms_count"] -
                    dark_before["hotplug_auto_kms_count"]
                )
                emit(delta > 0, f"console unplug auto-KMS delta={delta}")
            else:
                emit(False, "missing hotplug_auto_kms_count around console dark-down")
        if dark_after and dark_restore:
            if (
                "hotplug_auto_kms_count" in dark_after and
                "hotplug_auto_kms_count" in dark_restore
            ):
                delta = (
                    dark_restore["hotplug_auto_kms_count"] -
                    dark_after["hotplug_auto_kms_count"]
                )
                emit(delta > 0, f"console restore auto-KMS delta={delta}")
            else:
                emit(False, "missing hotplug_auto_kms_count around console restore")
        dark_drmtest = captured_text(out_dir / "drmtest.console_dark")
        for text in (
            "no connected connector exposed when requested",
            "disconnected connector exposes no modes",
            "disconnected connector EDID is 0",
            "disconnected connector CRTC_ID is 0",
            "SKIP active CRTC runtime probes by NVKM_DRMTEST_METADATA_ONLY",
        ):
            emit(text in dark_drmtest, f"drmtest.console_dark has {text}")
        if dark_restore:
            for key in (
                "commit_error_count",
                "dark_down_error_count",
                "hotplug_enqueue_error_count",
                "display_audit_pending_valid",
            ):
                if key in dark_restore:
                    emit(dark_restore[key] == 0, f"restore {key}=0")
                else:
                    emit(False, f"missing restore {key}")
            if "scanout_user" in dark_restore:
                emit(dark_restore["scanout_user"] == 0,
                     f"console restore scanout_user={dark_restore['scanout_user']}")
            else:
                emit(False, "missing restore scanout_user")

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
    report_all_object_suffixes(
        drmtest_after, "connector", CONNECTOR_DYNAMIC_SUFFIXES, emit)
    report_all_object_suffixes(
        drmtest_after, "crtc", CRTC_DYNAMIC_SUFFIXES, emit)
    report_all_object_suffixes(
        drmtest_after, "plane", PLANE_DYNAMIC_SUFFIXES, emit)
    report_any_object_suffixes(
        drmtest_after,
        "plane",
        ACTIVE_PRIMARY_PLANE_GEOMETRY_SUFFIXES,
        "active primary plane geometry",
        emit,
    )
    for text in (
        "DRM cap DUMB_PREFERRED_DEPTH is readable",
        "DRM cap DUMB_PREFERRED_DEPTH is 24",
        "DRM cap DUMB_PREFER_SHADOW is readable",
        "DRM cap DUMB_PREFER_SHADOW is 1",
        "DRM cap DUMB_BUFFER is readable",
        "DRM cap DUMB_BUFFER is 1",
        "DRM cap TIMESTAMP_MONOTONIC is readable",
        "DRM cap TIMESTAMP_MONOTONIC is 1",
        "DRM cap VBLANK_HIGH_CRTC is readable",
        "DRM cap VBLANK_HIGH_CRTC is 1",
        "DRM cap ASYNC_PAGE_FLIP is readable",
        "DRM cap ASYNC_PAGE_FLIP is 0",
        "DRM cap ATOMIC_ASYNC_PAGE_FLIP is readable",
        "DRM cap ATOMIC_ASYNC_PAGE_FLIP is 0",
        "DRM cap PAGE_FLIP_TARGET is readable",
        "DRM cap PAGE_FLIP_TARGET is 0",
        "DRM cap CURSOR_WIDTH is readable",
        "DRM cap CURSOR_WIDTH is 256",
        "DRM cap CURSOR_HEIGHT is readable",
        "DRM cap CURSOR_HEIGHT is 256",
        "DRM cap ADDFB2_MODIFIERS is readable",
        "DRM cap ADDFB2_MODIFIERS is 1",
        "DRM cap CRTC_IN_VBLANK_EVENT is readable",
        "DRM cap CRTC_IN_VBLANK_EVENT is 1",
        "DRM cap SYNCOBJ is readable",
        "DRM cap SYNCOBJ is 1",
        "DRM cap SYNCOBJ_TIMELINE is readable",
        "DRM cap SYNCOBJ_TIMELINE is 1",
        "DRM cap PRIME is readable",
        "DRM cap PRIME is 3",
        "DRM client cap STEREO_3D value 2 is rejected",
        "DRM client cap STEREO_3D value 2 fails with errno 22",
        "DRM client cap STEREO_3D is accepted",
        "DRM client cap UNIVERSAL_PLANES is accepted",
        "DRM client cap UNIVERSAL_PLANES value 2 is rejected",
        "DRM client cap UNIVERSAL_PLANES value 2 fails with errno 22",
        "DRM client cap ASPECT_RATIO is accepted",
        "DRM client cap ASPECT_RATIO value 2 is rejected",
        "DRM client cap ASPECT_RATIO value 2 fails with errno 22",
        "DRM client cap WRITEBACK_CONNECTORS before ATOMIC is rejected",
        "DRM client cap WRITEBACK_CONNECTORS before ATOMIC fails with errno 22",
        "DRM client cap ATOMIC value 2 is rejected",
        "DRM client cap ATOMIC value 2 fails with errno 22",
        "DRM client cap ATOMIC is accepted",
        "DRM client cap WRITEBACK_CONNECTORS value 2 is rejected",
        "DRM client cap WRITEBACK_CONNECTORS value 2 fails with errno 22",
        "DRM client cap WRITEBACK_CONNECTORS is accepted",
        "DRM client cap CURSOR_PLANE_HOTSPOT is rejected",
        "DRM client cap CURSOR_PLANE_HOTSPOT fails with errno 45",
        "DRM lease found active CRTC",
        "DRM lease found active connector",
        "DRM lease reads owner plane resources",
        "DRM lease found primary plane for active CRTC",
        "DRM lease has unleased connector for visibility probe",
        "DRM lease CREATE_LEASE succeeds",
        "DRM lease returns lessee id",
        "DRM lease returns lease fd",
        "DRM lease fd accepts UNIVERSAL_PLANES",
        "DRM lease fd accepts ATOMIC",
        "DRM lease fd resources are readable",
        "DRM lease fd exposes one connector",
        "DRM lease fd exposes one CRTC",
        "DRM lease fd exposes leased connector",
        "DRM lease fd exposes leased CRTC",
        "DRM lease fd plane resources are readable",
        "DRM lease fd exposes one plane",
        "DRM lease fd exposes leased primary plane",
        "DRM lease hides unleased connector",
        "DRM lease unleased connector lookup fails with ENOENT",
        "DRM empty lease CREATE_LEASE with O_NONBLOCK succeeds",
        "DRM empty lease returns lessee id",
        "DRM empty lease fd flags are readable",
        "DRM empty lease fd preserves O_NONBLOCK",
        "DRM empty lease fd resources are readable",
        "DRM empty lease fd exposes no connector",
        "DRM empty lease fd exposes no CRTC",
        "DRM empty lease fd plane resources are readable",
        "DRM empty lease fd exposes no plane",
        "DRM empty lease GET_LEASE succeeds",
        "DRM empty lease GET_LEASE returns no objects",
        "DRM empty lease CRTC_GET_SEQUENCE is rejected",
        "DRM empty lease CRTC_GET_SEQUENCE fails with ENOENT",
        "DRM empty lease CRTC_QUEUE_SEQUENCE is rejected",
        "DRM empty lease CRTC_QUEUE_SEQUENCE fails with ENOENT",
        "DRM empty lease WAIT_VBLANK builds CRTC index 0",
        "DRM empty lease WAIT_VBLANK is rejected",
        "DRM empty lease WAIT_VBLANK fails with EINVAL",
        "close empty DRM lease fd succeeds",
        "DRM non-universal lease owner disables ATOMIC client cap",
        "DRM non-universal lease owner disables UNIVERSAL_PLANES",
        "DRM non-universal lease CREATE_LEASE succeeds",
        "DRM non-universal lease fd accepts UNIVERSAL_PLANES",
        "DRM non-universal lease fd plane resources are readable",
        "DRM non-universal lease exposes implicit primary plane",
        "DRM non-universal lease GET_LEASE succeeds",
        "DRM non-universal lease GET_LEASE returns implicit primary plane",
        "close non-universal DRM lease fd succeeds",
        "DRM non-universal lease owner restores ATOMIC client cap",
        "DRM non-universal lease owner restores UNIVERSAL_PLANES",
        "DRM lease owner GET_LEASE succeeds",
        "DRM lease owner GET_LEASE fits probe buffer",
        "DRM lease owner GET_LEASE returns full mode object set",
        "DRM lease owner GET_LEASE returns connector",
        "DRM lease owner GET_LEASE returns CRTC",
        "DRM lease owner GET_LEASE returns primary plane",
        "DRM lease owner GET_LEASE returns encoder",
        "DRM lease lessee GET_LEASE succeeds",
        "DRM lease lessee GET_LEASE returns exact object count",
        "DRM lease lessee GET_LEASE returns connector",
        "DRM lease lessee GET_LEASE returns CRTC",
        "DRM lease lessee GET_LEASE returns primary plane",
        "DRM lease LIST_LESSEES succeeds",
        "DRM lease LIST_LESSEES returns lessee",
        "DRM lease lessee LIST_LESSEES succeeds",
        "DRM lease lessee LIST_LESSEES returns empty list",
        "DRM lease LIST_LESSEES rejects non-zero pad",
        "DRM lease GET_LEASE rejects non-zero pad",
        "DRM lease lessee cannot create sub-lease",
        "DRM lease connector is readable for encoder filter",
        "DRM lease connector exposes encoder list",
        "DRM lease connector has current encoder",
        "DRM lease current encoder is attached to connector",
        "DRM lease current encoder is readable",
        "DRM lease current encoder points at leased CRTC",
        "DRM lease current encoder possible_crtcs is non-empty",
        "DRM lease current encoder possible_crtcs is lease-relative",
        "DRM lease current encoder allows leased CRTC index",
        "DRM lease primary plane is readable for CRTC filter",
        "DRM lease primary plane GETPLANE points at leased CRTC",
        "DRM lease primary plane possible_crtcs is non-empty",
        "DRM lease primary plane possible_crtcs is lease-relative",
        "DRM lease primary plane allows leased CRTC index",
        "DRM lease active CRTC is readable for atomic TEST_ONLY",
        "DRM lease active CRTC has mode for atomic TEST_ONLY",
        "DRM lease primary plane has framebuffer",
        "DRM lease primary plane is attached to leased CRTC",
        "DRM lease creates MODE_ID blob for atomic TEST_ONLY",
        "DRM lease atomic TEST_ONLY allocates request",
        "DRM lease atomic TEST_ONLY request describes leased state",
        "DRM lease atomic TEST_ONLY on leased objects succeeds",
        "DRM lease destroys MODE_ID blob for atomic TEST_ONLY",
        "DRM lease vblank sequence accepts leased CRTC",
        "DRM lease atomic unleased connector allocates request",
        "DRM lease atomic unleased connector request is constructed",
        "DRM lease atomic TEST_ONLY with unleased connector is rejected",
        "DRM lease atomic unleased connector fails with ENOENT",
        "DRM lease duplicate live object is rejected with EBUSY",
        "DRM lease duplicate request object is rejected with EEXIST",
        "DRM lease bad object id is rejected with ENOENT",
        "DRM lease missing connector is rejected with EINVAL",
        "DRM lease missing plane is rejected with EINVAL",
        "DRM lease REVOKE_LEASE succeeds",
        "DRM lease revoked fd resources are readable",
        "DRM lease fd has no CRTC after revoke",
        "DRM lease fd has no connector after revoke",
        "DRM lease revoked fd plane resources are readable",
        "DRM lease fd has no plane after revoke",
        "DRM lease object can be re-leased after revoke",
        "DRM lease re-lease returns a fresh lessee id",
        "close second DRM lease fd succeeds",
        "close DRM lease fd succeeds",
        "SYNCOBJ_TRANSFER creates binary source syncobj",
        "SYNCOBJ_TRANSFER creates destination syncobj",
        "SYNCOBJ_TRANSFER binary source signal succeeds",
        "SYNCOBJ_TRANSFER binary source to timeline point succeeds",
        "SYNCOBJ_TRANSFER destination timeline point waits successfully",
        "SYNCOBJ_TRANSFER creates timeline source syncobj",
        "SYNCOBJ_TRANSFER source timeline signal succeeds",
        "SYNCOBJ_TRANSFER timeline source to timeline point succeeds",
        "SYNCOBJ_TRANSFER copied timeline point waits successfully",
        "SYNCOBJ_TRANSFER rejects missing source point",
        "SYNCOBJ_TRANSFER WAIT_FOR_SUBMIT waits for source point",
        "SYNCOBJ_TRANSFER WAIT_FOR_SUBMIT destination point waits successfully",
        "SYNCOBJ_TRANSFER creates binary destination syncobj",
        "SYNCOBJ_TRANSFER timeline source to binary succeeds",
        "SYNCOBJ_TRANSFER binary destination waits successfully",
        "SYNCOBJ_TRANSFER creates second binary source syncobj",
        "SYNCOBJ_TRANSFER second binary source signal succeeds",
        "SYNCOBJ_TRANSFER creates temporary timeline syncobj",
        "SYNCOBJ_TRANSFER creates chain destination syncobj",
        "SYNCOBJ_TRANSFER first wait into temporary timeline",
        "SYNCOBJ_TRANSFER second wait into temporary timeline",
        "SYNCOBJ_TRANSFER whole temporary chain to timeline succeeds",
        "SYNCOBJ_TRANSFER copied whole chain waits successfully",
        "SYNCOBJ_TRANSFER creates missing-source destination syncobj",
        "SYNCOBJ_TRANSFER rejects source with no fence",
        "at least one connector exposed",
        "at least one CRTC exposed",
        "at least one encoder exposed",
        "at least one KMS plane exposed",
        "mode_config min_width is 1",
        "mode_config min_height is 1",
        "mode_config max_width is 16384",
        "mode_config max_height is 16384",
        "CRTC count is valid for encoder masks",
        "CRTC count is valid for plane topology masks",
        "connector resource ids are unique",
        "CRTC resource ids are unique",
        "encoder resource ids are unique",
        "connector encoder ids are unique",
        "plane resource ids are unique",
        "plane resources available",
        "encoder type is not NONE",
        "encoder possible_crtcs is non-empty",
        "encoder possible_crtcs fits resources CRTC mask",
        "connector has at least one encoder",
        "connector encoder id is present in resources",
        "connector attached encoder is readable for type contract",
        "connector current encoder type matches connector type",
        "at least one connected connector exposed",
        "connected connector exposes at least one mode",
        "connected connector has current encoder",
        "connected connector current encoder is attached",
        "encoder current CRTC is present",
        "connected connector CRTC_ID is non-zero",
        "connected connector CRTC_ID is present in resources",
        "connected connector current encoder is readable",
        "connected connector current encoder has current CRTC",
        "connected connector CRTC_ID matches current encoder CRTC",
        "master legacy AttachMode no-op succeeds",
        "master legacy DetachMode no-op succeeds",
        "connected connector supports deprecated master mode no-op probe",
        "connected connector EDID property is blob",
        "connected connector EDID blob is non-zero",
        "connected connector EDID blob is readable",
        "connected connector EDID blob has data",
        "connected connector EDID blob has base block",
        "connected connector EDID blob length is block aligned",
        "connected connector EDID base header is valid",
        "connected connector EDID extension count matches blob length",
        "connected connector EDID block checksums are valid",
        "connected connector exposes TEST_ONLY-checkable modes",
        "connected connector mode list has sane timings",
        "connected connector encoder readable for mode TEST_ONLY probe",
        "connected connector CRTC present for mode TEST_ONLY probe",
        "connected connector primary plane available for mode TEST_ONLY probe",
        "plane resources readable for connected mode TEST_ONLY probe",
        "CREATE_DUMB succeeds for connected mode TEST_ONLY framebuffer",
        "ADDFB2 accepts connected mode TEST_ONLY framebuffer",
        "connected connector mode list passes atomic TEST_ONLY",
        "destroy MODE_ID blob for connected mode TEST_ONLY probe",
        "RMFB succeeds for connected mode TEST_ONLY framebuffer",
        "DESTROY_DUMB succeeds for connected mode TEST_ONLY framebuffer",
        "link-status defaults to Good",
        "scaling mode defaults to None",
        "dithering mode defaults to auto",
        "dithering depth defaults to auto",
        "max bpc value is 8",
        "underscan defaults to off",
        "underscan hborder value is 0",
        "underscan vborder value is 0",
        "encoder current CRTC index fits possible_crtcs mask width",
        "encoder current CRTC is allowed by possible_crtcs",
        "plane type is readable",
        "plane FB_ID and CRTC_ID enable state match",
        "plane current CRTC is present",
        "plane current CRTC index fits possible_crtcs mask width",
        "plane current CRTC is allowed by possible_crtcs",
        "has property OUT_FENCE_PTR",
        "OUT_FENCE_PTR is range",
        "OUT_FENCE_PTR min is 0",
        "OUT_FENCE_PTR max is 18446744073709551615",
        "OUT_FENCE_PTR value is 0",
        "CRTC state is readable",
        "CRTC ACTIVE and MODE_ID enable state match",
        "CRTC ACTIVE matches legacy mode_valid",
        "active CRTC has framebuffer",
        "active CRTC has non-zero size",
        "active CRTC MODE_ID fits property blob id",
        "active CRTC MODE_ID blob is readable",
        "active CRTC MODE_ID blob has data",
        "active CRTC MODE_ID blob has modeinfo size",
        "active CRTC MODE_ID blob matches legacy mode size",
        "active CRTC MODE_ID blob matches legacy mode timings",
        "active CRTC MODE_ID blob matches legacy mode refresh",
        "active CRTC MODE_ID blob matches legacy mode flags",
        "active CRTC MODE_ID blob matches legacy mode name",
        "inactive CRTC has no framebuffer",
        "every CRTC has a primary plane",
        "every CRTC has a cursor plane",
        "has property IN_FENCE_FD",
        "IN_FENCE_FD is signed range",
        "IN_FENCE_FD min is -1",
        "IN_FENCE_FD max is 2147483647",
        "IN_FENCE_FD value is -1",
        "plane has IN_FORMATS property",
        "IN_FORMATS property is a blob",
        "IN_FORMATS blob id is non-zero",
        "IN_FORMATS blob is readable",
        "IN_FORMATS blob header is present",
        "IN_FORMATS blob version is current",
        "IN_FORMATS blob is non-empty",
        "IN_FORMATS blob ranges are valid",
        "IN_FORMATS format count matches GETPLANE",
        "IN_FORMATS format order matches GETPLANE",
        "IN_FORMATS modifier references a format",
        "primary IN_FORMATS has XRGB8888 linear",
        "primary IN_FORMATS has ARGB8888 linear",
        "primary IN_FORMATS has RGB565 linear",
        "primary IN_FORMATS has XRGB8888 NVIDIA blocklinear",
        "primary IN_FORMATS has ARGB8888 NVIDIA blocklinear",
        "primary IN_FORMATS excludes RGB565 NVIDIA blocklinear",
        "primary IN_FORMATS has NVIDIA modifier for ADDFB2 negative probe",
        "CREATE_DUMB succeeds for ADDFB2 negative probe",
        "ADDFB2 rejects RGB565 NVIDIA blocklinear with EINVAL",
        "DESTROY_DUMB succeeds for ADDFB2 negative probe",
        "cursor GETPLANE exposes only ARGB8888",
        "cursor IN_FORMATS has ARGB8888 linear",
        "cursor IN_FORMATS excludes non-linear ARGB8888",
        "active CRTC available for atomic TEST_ONLY probe",
        "cursor plane supports active CRTC for atomic TEST_ONLY probe",
        "CREATE_DUMB succeeds for cursor atomic positive probe",
        "ADDFB2 accepts ARGB8888 linear cursor probe",
        "atomic TEST_ONLY accepts ARGB8888 linear cursor",
        "RMFB succeeds for cursor atomic positive probe",
        "DESTROY_DUMB succeeds for cursor atomic positive probe",
        "CREATE_DUMB succeeds for cursor atomic negative probe",
        "ADDFB2 accepts RGB565 linear atomic negative probe",
        "atomic TEST_ONLY rejects RGB565 linear cursor with EINVAL",
        "RMFB succeeds for cursor atomic negative probe",
        "DESTROY_DUMB succeeds for cursor atomic negative probe",
        "active CRTC mode size available for primary panning probe",
        "CREATE_DUMB succeeds for primary panning TEST_ONLY probe",
        "ADDFB2 accepts XRGB8888 linear primary panning probe",
        "atomic TEST_ONLY accepts integer primary source panning",
        "atomic TEST_ONLY rejects fractional primary source",
        "atomic TEST_ONLY rejects shifted primary destination with EINVAL",
        "RMFB succeeds for primary panning TEST_ONLY probe",
        "DESTROY_DUMB succeeds for primary panning TEST_ONLY probe",
        "primary plane supports active CRTC for panning TEST_ONLY probe",
        "DRM state is readable before legacy cursor probe",
        "cursor counters are present before legacy cursor probe",
        "CREATE_DUMB succeeds for legacy cursor runtime probe",
        "MAP_DUMB succeeds for transparent cursor probe",
        "legacy cursor SetCursor2 enables cursor image",
        "DRM state is readable before legacy cursor enable",
        "cursor counters are present before legacy cursor enable",
        "legacy cursor SetCursor2 increments cursor_update_count",
        "legacy cursor SetCursor2 does not increment cursor_error_count",
        "legacy cursor SetCursor2 marks head cursor enabled",
        "legacy cursor SetCursor2 publishes cursor framebuffer",
        "legacy cursor SetCursor2 publishes cursor BO",
        "legacy cursor MOVE succeeds",
        "DRM state is readable before legacy cursor move",
        "cursor counters are present before legacy cursor move",
        "legacy cursor MOVE increments cursor_async_update_count",
        "legacy cursor MOVE does not reprogram cursor image",
        "legacy cursor MOVE does not disable cursor",
        "legacy cursor MOVE does not increment cursor_error_count",
        "legacy cursor MOVE does not update primary plane",
        "legacy cursor MOVE sets legacy cursor atomic summary bit",
        "legacy cursor MOVE sets async atomic summary bit",
        "legacy cursor MOVE keeps head cursor enabled",
        "legacy cursor MOVE keeps cursor framebuffer",
        "legacy cursor MOVE keeps cursor BO",
        "legacy cursor SetCursor2 hides cursor image",
        "DRM state is readable before legacy cursor hide",
        "cursor counters are present before legacy cursor hide",
        "legacy cursor hide increments cursor_disable_count",
        "legacy cursor hide does not increment cursor_error_count",
        "legacy cursor pin counter is monotonic",
        "legacy cursor unpin counter is monotonic",
        "legacy cursor probe balances cursor pin/unpin",
        "legacy cursor async counter remains monotonic after hide",
        "legacy cursor hide marks head cursor disabled",
        "legacy cursor hide clears cursor framebuffer",
        "legacy cursor hide clears cursor BO",
        "DESTROY_DUMB succeeds for legacy cursor runtime probe",
        "does not expose unsupported connector property HDR_OUTPUT_METADATA",
        "does not expose unsupported connector property Colorspace",
        "does not expose unsupported connector property content type",
        "does not expose unsupported connector property vrr_capable",
        "does not expose unsupported CRTC property VRR_ENABLED",
        "has property DEGAMMA_LUT",
        "DEGAMMA_LUT is blob",
        "DEGAMMA_LUT defaults to 0",
        "has property DEGAMMA_LUT_SIZE",
        "DEGAMMA_LUT_SIZE is range",
        "DEGAMMA_LUT_SIZE value is 1024",
        "has property CTM",
        "CTM is blob",
        "CTM defaults to 0",
        "has property GAMMA_LUT",
        "GAMMA_LUT is blob",
        "GAMMA_LUT defaults to 0",
        "has property GAMMA_LUT_SIZE",
        "GAMMA_LUT_SIZE is range",
        "GAMMA_LUT_SIZE value is 1024",
        "allocate identity LUT buffer",
        "CREATE_BLOB succeeds for 1024-entry identity LUT",
        "CREATE_BLOB succeeds for legacy 256-entry identity LUT",
        "CREATE_BLOB succeeds for invalid-size identity LUT",
        "CREATE_BLOB succeeds for identity CTM",
        "atomic TEST_ONLY accepts 1024-entry CRTC color state",
        "atomic TEST_ONLY accepts legacy 256-entry CRTC color state",
        "atomic TEST_ONLY rejects invalid CRTC LUT size with EINVAL",
        "DESTROY_BLOB succeeds for identity CTM",
        "DESTROY_BLOB succeeds for invalid-size identity LUT",
        "DESTROY_BLOB succeeds for legacy 256-entry identity LUT",
        "DESTROY_BLOB succeeds for 1024-entry identity LUT",
        "active CRTC color runtime probe has property DEGAMMA_LUT",
        "active CRTC color runtime probe has property CTM",
        "active CRTC color runtime probe has property GAMMA_LUT",
        "active CRTC DEGAMMA_LUT blob id fits uint32_t",
        "active CRTC CTM blob id fits uint32_t",
        "active CRTC GAMMA_LUT blob id fits uint32_t",
        "DRM state is readable before CRTC color runtime probe",
        "color counters are present before CRTC color runtime probe",
        "CREATE_BLOB succeeds for runtime 1024-entry identity LUT",
        "CREATE_BLOB succeeds for runtime identity CTM",
        "atomic CRTC color runtime commit succeeds",
        "atomic CRTC color runtime DEGAMMA_LUT is applied",
        "atomic CRTC color runtime CTM is applied",
        "atomic CRTC color runtime GAMMA_LUT is applied",
        "DRM state is readable before CRTC color runtime commit",
        "color counters are present before CRTC color runtime commit",
        "atomic CRTC color runtime commit does not increment commit_error_count",
        "atomic CRTC color runtime commit programs degamma LUT",
        "atomic CRTC color runtime commit programs CTM",
        "atomic CRTC color runtime commit programs gamma LUT",
        "atomic CRTC color runtime commit leaves tail idle",
        "atomic CRTC color runtime commit leaves no pending display audit",
        "atomic CRTC color runtime restore commit succeeds",
        "atomic CRTC color runtime DEGAMMA_LUT is restored",
        "atomic CRTC color runtime CTM is restored",
        "atomic CRTC color runtime GAMMA_LUT is restored",
        "DRM state is readable before CRTC color runtime restore",
        "color counters are present before CRTC color runtime restore",
        "atomic CRTC color runtime restore does not increment commit_error_count",
        "atomic CRTC color runtime restore leaves tail idle",
        "atomic CRTC color runtime restore leaves no pending display audit",
        "DESTROY_BLOB succeeds for runtime identity CTM",
        "DESTROY_BLOB succeeds for runtime 1024-entry identity LUT",
        "active connector is available for scaler runtime probe",
        "active connector is attached to active CRTC for scaler runtime probe",
        "DRM state is readable before connector scaler runtime probe",
        "modeset counters are present before connector scaler runtime probe",
        "atomic connector scaling-only probe commit succeeds",
        "atomic connector scaling-only probe updates scaling mode",
        "atomic connector scaling-only probe preserves underscan",
        "DRM state is readable before connector scaling-only probe",
        "modeset counters are present before connector scaling-only probe",
        "atomic connector scaling-only probe does not increment commit_error_count",
        "atomic connector scaling-only probe leaves no active tail transaction",
        "atomic connector scaling-only probe leaves no pending display audit",
        "atomic connector scaling-only restore commit succeeds",
        "atomic connector scaling-only restore restores scaling mode",
        "DRM state is readable before connector scaling-only restore",
        "modeset counters are present before connector scaling-only restore",
        "atomic connector scaling-only restore does not increment commit_error_count",
        "atomic connector scaling-only restore leaves no active tail transaction",
        "atomic connector scaling-only restore leaves no pending display audit",
        "atomic connector underscan-only probe commit succeeds",
        "atomic connector underscan-only probe preserves scaling mode",
        "atomic connector underscan-only probe updates underscan",
        "atomic connector underscan-only probe updates underscan hborder",
        "atomic connector underscan-only probe updates underscan vborder",
        "DRM state is readable before connector underscan-only probe",
        "modeset counters are present before connector underscan-only probe",
        "atomic connector underscan-only probe does not increment commit_error_count",
        "atomic connector underscan-only probe leaves no active tail transaction",
        "atomic connector underscan-only probe leaves no pending display audit",
        "atomic connector scaler restore commit succeeds",
        "atomic connector scaler restore restores scaling mode",
        "atomic connector scaler restore restores underscan",
        "atomic connector scaler restore restores underscan hborder",
        "atomic connector scaler restore restores underscan vborder",
        "DRM state is readable before connector scaler restore completion",
        "modeset counters are present before connector scaler restore completion",
        "atomic connector scaler restore does not increment commit_error_count",
        "atomic connector scaler restore leaves no active tail transaction",
        "atomic connector scaler restore leaves no pending display audit",
        "active primary plane has framebuffer for OUT_FENCE_PTR probe",
        "active primary plane is attached to active CRTC for OUT_FENCE_PTR probe",
        "atomic commit with OUT_FENCE_PTR succeeds",
        "OUT_FENCE_PTR returns a sync_file fd",
        "OUT_FENCE_PTR sync_file becomes readable",
        "close OUT_FENCE_PTR sync_file fd",
        "active primary plane has framebuffer for IN_FENCE_FD probe",
        "active primary plane is attached to active CRTC for IN_FENCE_FD probe",
        "nouveau channel alloc succeeds for IN_FENCE_FD probe",
        "IN_FENCE_FD sync_file is pending before atomic commit",
        "atomic commit with IN_FENCE_FD succeeds",
        "IN_FENCE_FD sync_file is readable after commit",
        "close IN_FENCE_FD sync_file fd",
        "DRM state is readable before framebuffer UAPI probe",
        "pageflip counters are present before framebuffer UAPI probe",
        "CREATE_DUMB succeeds for framebuffer UAPI probe",
        "MAP_DUMB succeeds for framebuffer UAPI probe",
        "mmap succeeds for dumb buffer probe",
        "munmap succeeds for dumb buffer probe",
        "ADDFB2 accepts XRGB8888 linear framebuffer UAPI probe",
        "GETFB succeeds for owned framebuffer",
        "GETFB returns requested FB_ID",
        "GETFB reports framebuffer width",
        "GETFB reports framebuffer height",
        "GETFB reports framebuffer pitch",
        "GETFB reports framebuffer bpp",
        "GETFB reports XRGB8888 depth",
        "GETFB returns a GEM handle to master",
        "GEM_CLOSE succeeds for GETFB returned handle",
        "GETFB2 succeeds for owned framebuffer",
        "GETFB2 returns requested FB_ID",
        "GETFB2 reports framebuffer width",
        "GETFB2 reports framebuffer height",
        "GETFB2 reports XRGB8888 format",
        "GETFB2 reports modifier flag",
        "GETFB2 reports linear modifier",
        "GETFB2 returns a GEM handle to master",
        "GETFB2 reports framebuffer pitch",
        "GETFB2 reports zero plane offset",
        "GETFB2 has no extra plane handles for XRGB8888",
        "GEM_CLOSE succeeds for GETFB2 returned handle",
        "PRIME render node opens for framebuffer roundtrip",
        "CREATE_DUMB succeeds on render fd for PRIME framebuffer probe",
        "MAP_DUMB succeeds on render fd for PRIME framebuffer probe",
        "PRIME_HANDLE_TO_FD exports render dumb BO",
        "PRIME_FD_TO_HANDLE imports render BO on KMS fd",
        "ADDFB2 accepts PRIME-imported XRGB8888 framebuffer",
        "GETFB2 succeeds for PRIME-imported framebuffer",
        "PRIME-imported framebuffer reports XRGB8888 format",
        "PRIME-imported framebuffer reports modifier flag",
        "PRIME-imported framebuffer reports linear modifier",
        "PRIME-imported framebuffer reports exported pitch",
        "PRIME-imported framebuffer returns a GEM handle to master",
        "GEM_CLOSE succeeds for PRIME-imported GETFB2 handle",
        "RMFB succeeds for PRIME-imported framebuffer probe",
        "GEM_CLOSE succeeds for PRIME-imported KMS handle",
        "close succeeds for PRIME framebuffer dma-buf fd",
        "DESTROY_DUMB succeeds for PRIME framebuffer render BO",
        "close succeeds for PRIME framebuffer render fd",
        "GETFB non-master opens secondary card fd",
        "GETFB non-master reads framebuffer metadata",
        "GETFB non-master returns requested FB_ID",
        "GETFB non-master reports framebuffer width",
        "GETFB non-master reports framebuffer height",
        "GETFB non-master reports framebuffer pitch",
        "GETFB non-master returns no GEM handle",
        "GETFB2 non-master reads framebuffer metadata",
        "GETFB2 non-master returns requested FB_ID",
        "GETFB2 non-master reports framebuffer width",
        "GETFB2 non-master reports framebuffer height",
        "GETFB2 non-master reports framebuffer format",
        "GETFB2 non-master reports modifier flag",
        "GETFB2 non-master reports framebuffer modifier",
        "GETFB2 non-master reports framebuffer pitch",
        "GETFB2 non-master reports zero plane offset",
        "GETFB2 non-master returns no GEM handles",
        "GETFB non-master closes secondary card fd",
        "ADDFB2 accepts XRGB8888 linear CLOSEFB probe",
        "CLOSEFB rejects non-zero pad",
        "CLOSEFB non-zero pad fails with EINVAL",
        "CLOSEFB succeeds for framebuffer UAPI probe",
        "CLOSEFB second close fails",
        "CLOSEFB second close fails with ENOENT",
        "RMFB after CLOSEFB fails",
        "RMFB after CLOSEFB fails with ENOENT",
        "active connector is available for non-master display mutation probe",
        "active primary plane has framebuffer for non-master display mutation probe",
        "active primary plane is attached to active CRTC for non-master display mutation probe",
        "active CRTC is readable for non-master display mutation probe",
        "active CRTC has a mode for non-master display mutation probe",
        "active CRTC has gamma ramp for non-master display mutation probe",
        "non-master display mutation connector has property DPMS",
        "non-master display mutation opens secondary card fd",
        "non-master legacy SetCrtc is denied",
        "non-master legacy SetCrtc fails with EACCES",
        "non-master legacy pageflip is denied",
        "non-master legacy pageflip fails with EACCES",
        "non-master legacy SetPlane is denied",
        "non-master legacy SetPlane fails with EACCES",
        "non-master legacy AttachMode is denied",
        "non-master legacy AttachMode fails with EACCES",
        "non-master legacy DetachMode is denied",
        "non-master legacy DetachMode fails with EACCES",
        "non-master legacy cursor MOVE is denied",
        "non-master legacy cursor MOVE fails with EACCES",
        "non-master legacy cursor2 MOVE is denied",
        "non-master legacy cursor2 MOVE fails with EACCES",
        "non-master legacy SetGamma allocates identity ramp",
        "non-master legacy SetGamma is denied",
        "non-master legacy SetGamma fails with EACCES",
        "non-master connector SetProperty is denied",
        "non-master connector SetProperty fails with EACCES",
        "non-master object SetProperty is denied",
        "non-master object SetProperty fails with EACCES",
        "non-master atomic TEST_ONLY allocates request",
        "non-master atomic TEST_ONLY request describes current primary plane",
        "non-master atomic TEST_ONLY commit is denied",
        "non-master atomic TEST_ONLY commit fails with EACCES",
        "non-master display mutation closes secondary card fd",
        "legacy ADDFB accepts depth 24 XRGB8888 dumb framebuffer",
        "legacy ADDFB depth 24 GETFB succeeds",
        "legacy ADDFB depth 24 GETFB returns requested FB_ID",
        "legacy ADDFB depth 24 GETFB reports framebuffer width",
        "legacy ADDFB depth 24 GETFB reports framebuffer height",
        "legacy ADDFB depth 24 GETFB reports framebuffer pitch",
        "legacy ADDFB depth 24 GETFB reports framebuffer bpp",
        "legacy ADDFB depth 24 GETFB reports framebuffer depth",
        "GEM_CLOSE succeeds for legacy ADDFB depth 24 GETFB handle",
        "legacy ADDFB depth 24 GETFB2 succeeds",
        "legacy ADDFB depth 24 GETFB2 reports XRGB8888 format",
        "legacy ADDFB depth 24 GETFB2 returns a GEM handle",
        "GEM_CLOSE succeeds for legacy ADDFB depth 24 GETFB2 handle",
        "RMFB succeeds for legacy ADDFB depth 24 probe",
        "legacy ADDFB accepts depth 30 dumb framebuffer",
        "legacy ADDFB depth 30 GETFB succeeds",
        "legacy ADDFB depth 30 GETFB returns requested FB_ID",
        "legacy ADDFB depth 30 GETFB reports framebuffer width",
        "legacy ADDFB depth 30 GETFB reports framebuffer height",
        "legacy ADDFB depth 30 GETFB reports framebuffer pitch",
        "legacy ADDFB depth 30 GETFB reports framebuffer bpp",
        "legacy ADDFB depth 30 GETFB reports framebuffer depth",
        "GEM_CLOSE succeeds for legacy ADDFB depth 30 GETFB handle",
        "legacy ADDFB depth 30 GETFB2 succeeds",
        "legacy ADDFB depth 30 GETFB2 reports XBGR2101010 format",
        "legacy ADDFB depth 30 GETFB2 returns a GEM handle",
        "GEM_CLOSE succeeds for legacy ADDFB depth 30 GETFB2 handle",
        "RMFB succeeds for legacy ADDFB depth 30 probe",
        "legacy ADDFB rejects invalid depth 31",
        "legacy ADDFB invalid depth fails with EINVAL",
        "DIRTYFB accepts one framebuffer damage clip",
        "DIRTYFB accepts full-frame no-clip damage",
        "DIRTYFB non-master opens secondary card fd",
        "DIRTYFB non-master is denied",
        "DIRTYFB non-master fails with EACCES",
        "DIRTYFB non-master closes secondary card fd",
        "DIRTYFB rejects missing clip pointer",
        "DIRTYFB missing clip pointer fails with EINVAL",
        "DIRTYFB rejects odd COPY clip count",
        "DIRTYFB odd COPY clip count fails with EINVAL",
        "framebuffer UAPI probe does not increment commit_error_count",
        "framebuffer UAPI probe leaves no active tail transaction",
        "framebuffer UAPI probe leaves no pending display audit",
        "RMFB succeeds for framebuffer UAPI probe",
        "DESTROY_DUMB succeeds for framebuffer UAPI probe",
        "drmCrtcGetSequence succeeds on active CRTC",
        "active CRTC index fits legacy WAIT_VBLANK UAPI",
        "drmWaitVBlank relative wait succeeds on active CRTC",
        "drmCrtcGetSequence succeeds after WAIT_VBLANK",
        "WAIT_VBLANK advances active CRTC sequence",
        "drmWaitVBlank event queues active CRTC event",
        "drmWaitVBlank event arrives",
        "drmWaitVBlank event preserves user_data",
        "drmWaitVBlank event reports active CRTC id",
        "drmWaitVBlank event reaches queued sequence",
        "drmCrtcQueueSequence queues active CRTC event",
        "drmCrtcQueueSequence event arrives",
        "drmCrtcQueueSequence delivers exactly one event",
        "drmCrtcQueueSequence preserves user_data",
        "drmCrtcQueueSequence event reaches queued sequence",
        "connector encoder type matches connector type",
        "active connector is available for modeset disable probe",
        "active primary plane has framebuffer for modeset disable probe",
        "active primary plane is attached to active CRTC for modeset disable probe",
        "active CRTC index fits modeset head mask",
        "DRM state is readable before modeset disable probe",
        "modeset counters are present before modeset disable probe",
        "atomic modeset disable commit succeeds",
        "atomic modeset disable marks CRTC inactive",
        "atomic modeset disable increments tail disable op count",
        "atomic modeset disable records last disable op count",
        "atomic modeset disable records disabled head",
        "atomic modeset disable clears active head bit",
        "atomic modeset disable turns vblank off",
        "atomic modeset disable does not keep vblank active",
        "atomic modeset disable does not increment commit_error_count",
        "atomic modeset disable leaves no active tail transaction",
        "atomic modeset disable leaves no pending display audit",
        "active CRTC is readable for modeset restore probe",
        "active CRTC has a mode for restore probe",
        "CREATE_DUMB succeeds for modeset restore probe",
        "clear modeset restore probe framebuffer",
        "ADDFB2 accepts XRGB8888 linear modeset restore probe",
        "create MODE_ID blob for modeset restore probe",
        "DRM state is readable before modeset restore probe",
        "modeset counters are present before modeset restore probe",
        "atomic modeset restore commit succeeds",
        "atomic modeset restore marks CRTC active",
        "atomic modeset restore restores primary FB_ID",
        "atomic modeset restore restores primary CRTC_ID",
        "atomic modeset restore increments tail enable op count",
        "atomic modeset restore records last enable op count",
        "atomic modeset restore records enabled head",
        "atomic modeset restore does not increment commit_error_count",
        "atomic modeset restore leaves no active tail transaction",
        "atomic modeset restore leaves no pending display audit",
        "atomic modeset restore publishes atomic_enable audit",
        "DRM state is readable before modeset restore completion",
        "modeset counters are present before modeset restore completion",
        "destroy MODE_ID blob for modeset restore probe",
        "active primary plane has framebuffer for legacy pageflip probe",
        "active primary plane is attached to active CRTC for legacy pageflip probe",
        "DRM state is readable before legacy pageflip probe",
        "pageflip counters are present before legacy pageflip probe",
        "CREATE_DUMB succeeds for legacy pageflip probe",
        "MAP_DUMB succeeds for legacy pageflip probe",
        "ADDFB2 accepts XRGB8888 linear legacy pageflip probe",
        "CREATE_DUMB succeeds for legacy pageflip restore probe",
        "MAP_DUMB succeeds for legacy pageflip restore probe",
        "ADDFB2 accepts XRGB8888 linear legacy pageflip restore probe",
        "legacy pageflip ASYNC flag is rejected",
        "legacy pageflip ASYNC flag is rejected with EINVAL",
        "DRM state is readable before legacy pageflip ASYNC flag",
        "pageflip counters are present before legacy pageflip ASYNC flag",
        "legacy pageflip ASYNC flag keeps page_flip_reject_count monotonic",
        "legacy pageflip ASYNC flag does not increment page_flip_count",
        "legacy pageflip ASYNC flag does not increment page_flip_event_count",
        "legacy pageflip ASYNC flag does not increment page_flip_error_count",
        "legacy pageflip ASYNC flag does not increment commit_error_count",
        "legacy pageflip ASYNC flag leaves no active tail transaction",
        "legacy pageflip ASYNC flag leaves no pending display audit",
        "legacy pageflip TARGET flag is rejected",
        "legacy pageflip TARGET flag is rejected with EINVAL",
        "DRM state is readable before legacy pageflip TARGET flag",
        "pageflip counters are present before legacy pageflip TARGET flag",
        "legacy pageflip TARGET flag keeps page_flip_reject_count monotonic",
        "legacy pageflip TARGET flag does not increment page_flip_count",
        "legacy pageflip TARGET flag does not increment page_flip_event_count",
        "legacy pageflip TARGET flag does not increment page_flip_error_count",
        "legacy pageflip TARGET flag does not increment commit_error_count",
        "legacy pageflip TARGET flag leaves no active tail transaction",
        "legacy pageflip TARGET flag leaves no pending display audit",
        "legacy pageflip to temporary FB pageflip ioctl succeeds",
        "legacy pageflip to temporary FB pageflip event arrives",
        "DRM state is readable before legacy pageflip temporary FB",
        "pageflip counters are present before legacy pageflip temporary FB",
        "legacy pageflip increments page_flip_count",
        "legacy pageflip increments page_flip_event_count",
        "legacy pageflip does not increment page_flip_error_count",
        "legacy pageflip does not increment commit_error_count",
        "legacy pageflip leaves no active tail transaction",
        "legacy pageflip leaves no pending display audit",
        "atomic restore after legacy pageflip succeeds",
        "legacy pageflip restore installs restore FB_ID",
        "legacy pageflip restore keeps primary CRTC_ID",
        "DRM state is readable before legacy pageflip restore",
        "pageflip counters are present before legacy pageflip restore",
        "atomic pageflip restore does not increment page_flip_count",
        "atomic pageflip restore does not increment page_flip_event_count",
        "legacy pageflip restore does not increment page_flip_error_count",
        "legacy pageflip restore does not increment commit_error_count",
        "legacy pageflip restore leaves no active tail transaction",
        "legacy pageflip restore leaves no pending display audit",
        "RMFB succeeds for legacy pageflip probe",
        "DESTROY_DUMB succeeds for legacy pageflip probe",
        "active connector is available for legacy DPMS probe",
        "active connector is attached to active CRTC for legacy DPMS probe",
        "active connector DPMS starts On for legacy DPMS probe",
        "active CRTC index fits DPMS head mask",
        "DRM state is readable before legacy DPMS probe",
        "modeset counters are present before legacy DPMS probe",
        "legacy DPMS OFF commit succeeds",
        "legacy DPMS OFF updates connector DPMS property",
        "DPMS-off CRTC has property ACTIVE",
        "legacy DPMS OFF marks CRTC inactive",
        "DRM state is readable before legacy DPMS OFF",
        "modeset counters are present before legacy DPMS OFF",
        "legacy DPMS OFF increments tail disable op count",
        "legacy DPMS OFF records disabled head",
        "legacy DPMS OFF does not increment commit_error_count",
        "legacy DPMS OFF leaves no active tail transaction",
        "legacy DPMS OFF leaves no pending display audit",
        "legacy DPMS ON restore succeeds",
        "legacy DPMS ON restores connector DPMS property",
        "DPMS-restored CRTC has property ACTIVE",
        "legacy DPMS ON marks CRTC active",
        "DRM state is readable before legacy DPMS ON restore",
        "modeset counters are present before legacy DPMS ON restore",
        "legacy DPMS ON increments tail enable op count",
        "legacy DPMS ON records enabled head",
        "legacy DPMS ON does not increment commit_error_count",
        "legacy DPMS ON leaves no active tail transaction",
        "legacy DPMS ON leaves no pending display audit",
        "active CRTC index fits legacy SetCrtc head mask",
        "active connector is available for legacy SetCrtc probe",
        "active primary plane has framebuffer for legacy SetCrtc probe",
        "active primary plane is attached to active CRTC for legacy SetCrtc probe",
        "active CRTC is readable for legacy SetCrtc probe",
        "active CRTC has a mode for legacy SetCrtc probe",
        "DRM state is readable before legacy SetCrtc probe",
        "modeset counters are present before legacy SetCrtc probe",
        "CREATE_DUMB succeeds for legacy SetCrtc restore probe",
        "clear legacy SetCrtc restore framebuffer",
        "ADDFB2 accepts XRGB8888 linear legacy SetCrtc restore probe",
        "legacy SetCrtc disable succeeds",
        "legacy SetCrtc disabled CRTC has property ACTIVE",
        "legacy SetCrtc disable marks CRTC inactive",
        "legacy SetCrtc disabled connector has property CRTC_ID",
        "legacy SetCrtc disable detaches connector CRTC_ID",
        "DRM state is readable before legacy SetCrtc disable",
        "modeset counters are present before legacy SetCrtc disable",
        "legacy SetCrtc disable increments tail disable op count",
        "legacy SetCrtc disable records disabled head",
        "legacy SetCrtc disable does not increment commit_error_count",
        "legacy SetCrtc disable leaves no active tail transaction",
        "legacy SetCrtc disable leaves no pending display audit",
        "legacy SetCrtc restore succeeds",
        "legacy SetCrtc restored CRTC has property ACTIVE",
        "legacy SetCrtc restore marks CRTC active",
        "legacy SetCrtc restored connector has property CRTC_ID",
        "legacy SetCrtc restore attaches connector CRTC_ID",
        "legacy SetCrtc restore restores primary FB_ID",
        "legacy SetCrtc restore restores primary CRTC_ID",
        "DRM state is readable before legacy SetCrtc restore",
        "modeset counters are present before legacy SetCrtc restore",
        "legacy SetCrtc restore increments tail enable op count",
        "legacy SetCrtc restore records enabled head",
        "legacy SetCrtc restore does not increment commit_error_count",
        "legacy SetCrtc restore leaves no active tail transaction",
        "legacy SetCrtc restore leaves no pending display audit",
    ):
        emit(text in drmtest_after, f"drmtest.after has {text}")

    if "status=disconnected" in drmtest_after:
        for text in (
            "disconnected connector exposes no modes",
            "disconnected connector has no current encoder",
            "disconnected connector EDID is 0",
            "disconnected connector CRTC_ID is 0",
        ):
            emit(text in drmtest_after, f"drmtest.after has {text}")
    else:
        print("INFO no disconnected connector in drmtest.after")

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
    parser.add_argument("phase", choices=(
        "before",
        "x11",
        "wayland",
        "wayland_egl",
        "wayland_glmark",
        "wayland_info",
        "wayland_hpd",
        "wayland_hpd_smoke",
        "syncobj_transfer",
        "xwayland",
        "after",
        "report",
    ))
    parser.add_argument("--out-dir", default=None)
    parser.add_argument("--display", default=os.environ.get("DISPLAY", ":0"))
    parser.add_argument("--xauthority", default=os.environ.get("XAUTHORITY"))
    parser.add_argument("--gears-seconds", type=int, default=5)
    parser.add_argument("--egl-seconds", type=int, default=6)
    parser.add_argument("--glmark-seconds", type=int, default=6)
    parser.add_argument("--run-panning", action="store_true",
                        help="briefly set and clear xrandr panning on the "
                             "first connected output")
    parser.add_argument("--run-hpd-inject", action="store_true",
                        help="inject a debug HPD event while X11 owns DRM "
                             "master")
    parser.add_argument("--hpd-inject-value", type=lambda value: int(value, 0),
                        default=0x400,
                        help="packed HPD mask: low16 plug, high16 unplug")
    parser.add_argument("--run-console-dark-down", action="store_true",
                        help="after phase only: force one connector to look "
                             "disconnected, verify no-master dark-down, then "
                             "restore normal detect")
    parser.add_argument("--dark-down-display-id",
                        type=lambda value: int(value, 0), default=0x400,
                        help="displayId bit used by --run-console-dark-down")
    parser.add_argument("--allow-missing-x11", action="store_true",
                        help="allow report-only console/debug runs without an x11 phase")
    parser.add_argument("--kms-idle-timeout", type=float, default=8.0,
                        help="after phase only: wait this many seconds for "
                             "atomic KMS tail/console restore to become idle "
                             "before collecting the final snapshot")
    parser.add_argument("--sway-command", default="sway",
                        help="Wayland compositor executable used by wayland "
                             "and xwayland full-smoke phases")
    parser.add_argument("--wayland-timeout", type=int, default=40,
                        help="timeout in seconds for full Wayland/XWayland "
                             "smoke phases")
    parser.add_argument("--wayland-exit-delay", type=int, default=6,
                        help="seconds before a plain Wayland smoke exits")
    parser.add_argument("--wayland-hpd-delay", type=int, default=2,
                        help="seconds before Wayland HPD smoke injects HPD")
    args = parser.parse_args()

    out_dir = choose_out_dir(args.phase, args.out_dir)
    if args.phase == "report":
        print(out_dir)
        return report(out_dir, args.allow_missing_x11)
    if args.phase == "syncobj_transfer":
        rc = capture_syncobj_transfer_probe(out_dir)
        print(out_dir)
        return 1 if rc != 0 else 0
    if args.phase in FULL_WAYLAND_PHASES:
        print(out_dir)
        return run_wayland_smoke(out_dir, args.phase, args)

    if args.phase == "x11":
        probe_rc = probe_x11_display(out_dir, args)
        if probe_rc != 0:
            print(out_dir)
            return probe_rc

    capture_phase(out_dir, args.phase, args)
    print(out_dir)
    if args.phase == "after":
        return report(out_dir, args.allow_missing_x11)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
