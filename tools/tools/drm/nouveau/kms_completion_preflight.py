#!/usr/local/bin/python3
# SPDX-License-Identifier: BSD-2-Clause
"""Emit a read-only nvkm KMS completion preflight manifest."""

import argparse
import datetime as dt
import hashlib
import json
import pathlib
import shutil
import subprocess


MODULES = (
    ("drm", "sys/dev/drm/drm/drm.ko"),
    ("nvgsp_570", "sys/dev/drm/nouveau/fw/nvgsp_570.ko"),
    ("nvkm", "sys/dev/drm/nouveau/nvkm.ko"),
)

COMMANDS = (
    "cc",
    "doas",
    "glxgears",
    "glxinfo",
    "glmark2-wayland",
    "pkg-config",
    "startx",
    "sway",
    "wayland-info",
    "xdotool",
    "xrandr",
)


def source_tree_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[4]


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_text(argv: list[str]) -> tuple[int | None, str]:
    try:
        result = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except FileNotFoundError as err:
        return None, f"missing executable: {err.filename}"
    return result.returncode, result.stdout


def git_identity(root: pathlib.Path) -> dict:
    status_rc, status = run_text(
        ["git", "-C", str(root), "status", "--short", "--untracked-files=no"],
    )
    head_rc, head = run_text(["git", "-C", str(root), "rev-parse", "HEAD"])
    status_lines = status.splitlines() if status_rc == 0 else []
    head_text = head.strip() if head_rc == 0 else ""
    return {
        "head": head_text,
        "head_short": head_text[:10],
        "tracked_dirty": bool(status_lines) if status_rc == 0 else None,
        "status_short": status_lines,
        "status_rc": status_rc,
        "head_rc": head_rc,
    }


def project_modules(root: pathlib.Path) -> list[dict]:
    modules = []
    for name, relative_path in MODULES:
        path = root / relative_path
        entry = {
            "name": name,
            "path": str(path),
            "exists": path.exists(),
        }
        if path.exists():
            stat = path.stat()
            entry.update({
                "size": stat.st_size,
                "mtime_utc": dt.datetime.fromtimestamp(
                    stat.st_mtime,
                    dt.timezone.utc,
                ).isoformat(),
                "sha256": file_sha256(path),
            })
        modules.append(entry)
    return modules


def loaded_modules() -> dict[str, dict]:
    rc, text = run_text(["kldstat"])
    entries: dict[str, dict] = {}
    if rc != 0:
        return entries
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 5:
            continue
        module = fields[4]
        if not module.endswith(".ko"):
            continue
        name = module.removesuffix(".ko")
        if name not in {item[0] for item in MODULES}:
            continue
        entries[name] = {
            "id": fields[0],
            "refs": fields[1],
            "address": fields[2],
            "size_hex": fields[3],
            "size_bytes": int(fields[3], 16),
            "name": module,
        }
    return entries


def command_paths() -> dict[str, dict]:
    return {
        command: {
            "path": shutil.which(command),
            "exists": shutil.which(command) is not None,
        }
        for command in COMMANDS
    }


def shell_commands(root: pathlib.Path) -> dict:
    smoke = root / "tools/tools/drm/nouveau/kms_smoke.py"
    preflight = root / "tools/tools/drm/nouveau/kms_completion_preflight.py"
    static_audit = root / "tools/tools/drm/nouveau/kms_static_audit.py"
    completion = root / "tools/tools/drm/nouveau/kms_completion_audit.py"
    return {
        "load_modules_after_reboot": [
            f"doas kldload {root / MODULES[0][1]}",
            f"doas kldload {root / MODULES[1][1]}",
            f"doas kldload {root / MODULES[2][1]}",
        ],
        "full_x11_report": [
            f"{smoke} before",
            "startx",
            f"DISPLAY=:0 {smoke} x11 --run-panning --run-hpd-inject",
            "logout from X11",
            f"{smoke} after --run-console-dark-down --dark-down-display-id 0x400",
            f"{smoke} report",
        ],
        "standalone_reports": [
            f"{preflight} --output /var/tmp/nvkm-kms-completion-preflight.json",
            f"{static_audit} --output /var/tmp/nvkm-kms-static-audit.json",
            f"{smoke} syncobj_transfer",
            f"{smoke} syncobj_pending_exec",
            f"{smoke} wayland_info",
            f"{smoke} wayland_hpd_smoke",
            f"{smoke} wayland_glmark",
            f"{smoke} xwayland",
        ],
        "completion_template": (
            f"{completion} "
            "--static-audit /var/tmp/nvkm-kms-static-audit.json "
            "--preflight /var/tmp/nvkm-kms-completion-preflight.json "
            "--full-report <full-report-dir> "
            "--syncobj-transfer <syncobj-transfer-dir> "
            "--syncobj-pending-exec <syncobj-pending-exec-dir> "
            "--wayland-info <wayland-info-dir> "
            "--wayland-hpd-smoke <wayland-hpd-smoke-dir> "
            "--wayland-glmark <wayland-glmark-dir> "
            "--xwayland <xwayland-dir>"
        ),
    }


def add_check(checks: list[dict], ok: bool, text: str) -> None:
    checks.append({"ok": ok, "text": text})


def build_manifest() -> dict:
    root = source_tree_root()
    git = git_identity(root)
    modules = project_modules(root)
    loaded = loaded_modules()
    commands = command_paths()
    checks: list[dict] = []

    add_check(checks, git["head_rc"] == 0, "source git HEAD is readable")
    add_check(checks, git["status_rc"] == 0, "source git status is readable")
    add_check(checks, git["tracked_dirty"] is False, "source tracked tree is clean")
    for module in modules:
        add_check(checks, module["exists"], f"project module exists: {module['name']}")
        add_check(
            checks,
            bool(module.get("sha256")),
            f"project module has sha256: {module['name']}",
        )
    for command, entry in commands.items():
        add_check(checks, entry["exists"], f"required command exists: {command}")
    for name, _relative_path in MODULES:
        add_check(
            checks,
            name in loaded,
            f"loaded module is present by name: {name}",
        )

    return {
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source_tree": str(root),
        "git": git,
        "project_modules": modules,
        "loaded_modules": loaded,
        "command_paths": commands,
        "checks": checks,
        "passed": all(check["ok"] for check in checks),
        "fail_count": sum(1 for check in checks if not check["ok"]),
        "commands": shell_commands(root),
        "notes": [
            "kldstat proves loaded module names and loaded image sizes, not the source path.",
            "Final completion still requires kms_completion_audit.py passed=true on real runtime evidence.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Emit a read-only nvkm KMS completion preflight manifest",
    )
    parser.add_argument(
        "--output",
        default="/var/tmp/nvkm-kms-completion-preflight.json",
        help="JSON manifest path",
    )
    args = parser.parse_args()

    manifest = build_manifest()
    output = pathlib.Path(args.output)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(output)
    return 0 if manifest["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
