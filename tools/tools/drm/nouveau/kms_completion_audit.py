#!/usr/local/bin/python3
# SPDX-License-Identifier: BSD-2-Clause
"""Aggregate nvkm KMS completion evidence.

This script is intentionally read-only.  It consumes evidence already collected
by kms_smoke.py and writes one machine-readable completion summary.
"""

import argparse
import datetime as dt
import json
import pathlib
import re


MODULES = ("drm", "nvgsp_570", "nvkm")
WAYLAND_REPORTS = (
    ("wayland_info", "wayland-info"),
    ("wayland_hpd_smoke", "Wayland HPD smoke"),
    ("xwayland", "XWayland"),
)
WAYLAND_REQUIRED_CHECKS = {
    "wayland_info": (
        ("nouveau DRM backend", r"^Wayland log uses nouveau DRM backend$"),
        ("atomic DRM", r"^Wayland log uses atomic DRM interface$"),
        ("zink/NVK renderer", r"^Wayland log uses zink/NVK renderer$"),
        ("wayland-info command", r"^wayland-info rc=0$"),
        ("wl_compositor global", r"^wayland-info sees wl_compositor$"),
        ("xdg_wm_base global", r"^wayland-info sees xdg_wm_base$"),
        ("presentation clock", r"^wayland-info reports presentation clock$"),
        ("DRM lease device", r"^wayland-info exposes DRM lease device card0$"),
        ("HDMI output", r"^wayland-info sees HDMI-A-1 output$"),
        ("wayland-info clean output", r"^wayland-info output has no errors$"),
    ),
    "wayland_hpd_smoke": (
        ("nouveau DRM backend", r"^Wayland log uses nouveau DRM backend$"),
        ("atomic DRM", r"^Wayland log uses atomic DRM interface$"),
        ("Wayland compositor clean exit", r"^Wayland compositor rc=0$"),
        ("HPD inject command", r"^kms-hpd-inject\.wayland rc=0$"),
        ("devd hotplug event", r"^wayland devd client received DRM HOTPLUG event$"),
        ("notify-only HPD", r"^wayland hotplug_notify_only_count HPD delta=[1-9][0-9]*$"),
        ("no auto-KMS while Wayland owns master",
         r"^wayland hotplug_auto_kms_count HPD delta=0$"),
        ("user scanout preserved", r"^wayland HPD kept user scanout=1$"),
    ),
    "xwayland": (
        ("nouveau DRM backend", r"^Wayland log uses nouveau DRM backend$"),
        ("atomic DRM", r"^Wayland log uses atomic DRM interface$"),
        ("Wayland compositor clean exit", r"^Wayland compositor rc=0$"),
        ("Xwayland server started", r"^Xwayland server started$"),
        ("Xwayland server ready", r"^Xwayland server became ready$"),
        ("X11 surface", r"^Xwayland created an X11 surface$"),
        ("GLX direct rendering", r"^Xwayland glxinfo has direct rendering$"),
        ("GLX zink/NVK renderer", r"^Xwayland glxinfo uses zink/NVK renderer$"),
        ("GLX accelerated", r"^Xwayland glxinfo is accelerated$"),
        ("glxgears output",
         r"^Xwayland glxgears produced renderer/frame/marker output$"),
    ),
}
STATIC_AUDIT_REQUIRED_FILES = (
    "sys/dev/drm/drm_lease.c",
    "sys/dev/drm/include/drm/drm_lease.h",
    "sys/dev/drm/nouveau/nvkm_drm_kms.c",
    "sys/dev/drm/nouveau/nvkm_gsp_disp.c",
    "sys/dev/drm/nouveau/core/object.c",
    "sys/dev/drm/nouveau/dispnv50/nvkm_dispnv50_bridge.c",
    "sys/dev/drm/nouveau/nvhw/drf.h",
    "sys/dev/drm/nouveau/nvhw/class/clc57d.h",
    "sys/dev/drm/nouveau/nvif/class.h",
    "sys/dev/drm/nouveau/rm/rm.h",
    "sys/dev/drm/nouveau/subdev/gsp.h",
    "tools/tools/drm/nouveau/kms_smoke.py",
    "tools/tools/drm/nouveau/kms_completion_audit.py",
    "tools/tools/drm/nouveau/kms_static_audit.py",
)


def read_text(path: pathlib.Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(errors="replace")


def command_return_code(path: pathlib.Path) -> int | None:
    match = re.search(r"^### rc=([0-9]+)$", read_text(path), re.M)
    if match is None:
        return None
    return int(match.group(1))


def load_json(path: pathlib.Path) -> dict:
    with path.open() as json_file:
        return json.load(json_file)


def module_entries(out_dir: pathlib.Path, phase: str) -> list[dict]:
    path = out_dir / f"module_files.{phase}"
    text = read_text(path)
    pattern = re.compile(
        r"^(?P<name>\S+)\s+(?P<path>\S+)\s+size=(?P<size>[0-9]+)\s+"
        r"mtime_utc=(?P<mtime>\S+)\s+sha256=(?P<sha256>[0-9a-f]{64})$",
        re.M,
    )
    entries: list[dict] = []
    for match in pattern.finditer(text):
        entries.append({
            "name": match.group("name"),
            "path": match.group("path"),
            "size": int(match.group("size")),
            "mtime_utc": match.group("mtime"),
            "sha256": match.group("sha256"),
        })
    return entries


def module_git(out_dir: pathlib.Path, phase: str) -> dict:
    text = read_text(out_dir / f"module_files.{phase}")
    git: dict[str, object] = {}
    head = re.search(r"^### git_head=([0-9a-f]{40})$", text, re.M)
    if head is not None:
        git["head"] = head.group(1)
        git["head_short"] = head.group(1)[:10]
    dirty = re.search(r"^### git_tracked_dirty=([01])$", text, re.M)
    if dirty is not None:
        git["tracked_dirty"] = dirty.group(1) == "1"
    git["status_short"] = re.findall(r"^### git_status_short=(.*)$", text, re.M)
    return git


def module_entry_map(entries: list[dict]) -> dict[str, dict]:
    return {
        entry["name"]: entry
        for entry in entries
        if isinstance(entry.get("name"), str)
    }


def module_identity(entry: dict) -> dict:
    return {
        "path": entry.get("path"),
        "size": entry.get("size"),
        "sha256": entry.get("sha256"),
    }


def module_identity_map(entries: list[dict]) -> dict[str, dict]:
    by_name = module_entry_map(entries)
    return {
        name: module_identity(by_name[name])
        for name in MODULES
        if name in by_name
    }


def check(ok: bool, text: str, checks: list[dict]) -> None:
    checks.append({"ok": ok, "text": text})
    print(("PASS " if ok else "FAIL ") + text)


def check_module_set(out_dir: pathlib.Path, phase: str,
                     checks: list[dict]) -> list[dict]:
    entries = module_entries(out_dir, phase)
    by_name = module_entry_map(entries)
    check(bool(entries), f"{phase} module snapshot exists", checks)
    for name in MODULES:
        entry = by_name.get(name)
        check(entry is not None, f"{phase} records {name} module", checks)
        if entry is not None:
            check(entry["size"] > 0, f"{phase} {name} module has size", checks)
            check(bool(re.fullmatch(r"[0-9a-f]{64}", entry["sha256"])),
                  f"{phase} {name} module has sha256", checks)
    return entries


def check_full_report(out_dir: pathlib.Path, checks: list[dict]) -> dict:
    summary_path = out_dir / "report_summary.json"
    check(summary_path.exists(), "full report_summary.json exists", checks)
    if not summary_path.exists():
        return {}

    try:
        summary = load_json(summary_path)
    except (OSError, json.JSONDecodeError) as err:
        check(False, f"full report_summary.json is readable: {err}", checks)
        return {}

    check(summary.get("passed") is True, "full KMS report passed", checks)
    check(summary.get("fail_count") == 0, "full KMS report fail_count is zero",
          checks)
    check(summary.get("allow_missing_x11") is False,
          "full KMS report required X11 phase", checks)

    modules = summary.get("modules")
    check(isinstance(modules, dict), "full report has module summary", checks)
    if isinstance(modules, dict):
        for phase in ("before", "x11", "after"):
            entries = modules.get(phase)
            check(isinstance(entries, list),
                  f"full report summary has {phase} modules", checks)
            if isinstance(entries, list):
                by_name = module_entry_map(entries)
                for name in MODULES:
                    check(name in by_name,
                          f"full report summary {phase} has {name}", checks)

    return summary


def check_standalone_report(out_dir: pathlib.Path, label: str,
                            description: str, checks: list[dict]) -> dict:
    check(out_dir.exists(), f"{label} evidence directory exists", checks)
    summary_path = out_dir / "report_summary.json"
    check(summary_path.exists(), f"{label} report_summary.json exists", checks)
    if not summary_path.exists():
        return {}

    try:
        summary = load_json(summary_path)
    except (OSError, json.JSONDecodeError) as err:
        check(False, f"{label} report_summary.json is readable: {err}", checks)
        return {}

    check(summary.get("passed") is True, f"{description} report passed", checks)
    check(summary.get("fail_count") == 0,
          f"{description} report fail_count is zero", checks)
    check(summary.get("allow_missing_x11") is True,
          f"{description} report is standalone", checks)

    modules = summary.get("modules")
    check(isinstance(modules, dict), f"{label} report has module summary", checks)
    if isinstance(modules, dict):
        for phase in ("before", "after"):
            entries = modules.get(phase)
            check(isinstance(entries, list),
                  f"{label} report summary has {phase} modules", checks)
            if isinstance(entries, list):
                by_name = module_entry_map(entries)
                for name in MODULES:
                    check(name in by_name,
                          f"{label} report summary {phase} has {name}", checks)

    report_checks = summary.get("checks")
    check(isinstance(report_checks, list), f"{label} report has check list", checks)
    if isinstance(report_checks, list):
        for check_name, pattern in WAYLAND_REQUIRED_CHECKS.get(label, ()):
            found = any(
                isinstance(item, dict) and
                item.get("ok") is True and
                isinstance(item.get("text"), str) and
                re.search(pattern, item["text"])
                for item in report_checks
            )
            check(found,
                  f"{label} report proves {check_name}",
                  checks)

    return summary


def check_sync_gate(out_dir: pathlib.Path, phase: str,
                    checks: list[dict]) -> dict:
    check(out_dir.exists(), f"{phase} evidence directory exists", checks)
    check_module_set(out_dir, phase, checks)

    drmtest_path = out_dir / f"drmtest.{phase}"
    check(drmtest_path.exists(), f"{phase} drmtest output exists", checks)
    rc = command_return_code(drmtest_path)
    check(rc == 0, f"{phase} drmtest rc={rc}", checks)

    return {
        "out_dir": str(out_dir),
        "phase": phase,
        "drmtest_rc": rc,
        "modules": module_entries(out_dir, phase),
        "module_git": module_git(out_dir, phase),
    }


def check_static_audit(path: pathlib.Path, checks: list[dict]) -> dict:
    check(path.exists(), "static audit summary exists", checks)
    if not path.exists():
        return {"path": str(path)}

    try:
        summary = load_json(path)
    except (OSError, json.JSONDecodeError) as err:
        check(False, f"static audit summary is readable: {err}", checks)
        return {"path": str(path)}

    check(summary.get("passed") is True, "static audit passed", checks)
    check(summary.get("fail_count") == 0,
          "static audit fail_count is zero", checks)
    git = summary.get("git")
    check(isinstance(git, dict), "static audit has git identity", checks)
    if isinstance(git, dict):
        head = git.get("head")
        check(isinstance(head, str) and bool(re.fullmatch(r"[0-9a-f]{40}", head)),
              "static audit git head is a full SHA1", checks)
        check(git.get("tracked_dirty") is False,
              "static audit git tracked worktree is clean", checks)

    files = summary.get("files")
    check(isinstance(files, list) and bool(files),
          "static audit file summary is non-empty", checks)
    if isinstance(files, list):
        by_path = {
            item.get("path"): item
            for item in files
            if isinstance(item, dict)
        }
        for required_path in STATIC_AUDIT_REQUIRED_FILES:
            check(required_path in by_path,
                  f"static audit covers {required_path}", checks)

    return {
        "path": str(path),
        "summary": summary,
    }


def collect_module_identity_sets(full_summary: dict, transfer_summary: dict,
                                 pending_summary: dict,
                                 wayland_summaries: dict[str, dict]
                                 ) -> dict[str, dict]:
    identity_sets: dict[str, dict] = {}
    full_modules = full_summary.get("modules")
    if isinstance(full_modules, dict):
        for phase in ("before", "x11", "after"):
            entries = full_modules.get(phase)
            if isinstance(entries, list):
                identity_sets[f"full:{phase}"] = module_identity_map(entries)

    for label, summary in (
        ("syncobj_transfer", transfer_summary),
        ("syncobj_pending_exec", pending_summary),
    ):
        entries = summary.get("modules")
        if isinstance(entries, list):
            identity_sets[label] = module_identity_map(entries)

    for label, summary in wayland_summaries.items():
        modules = summary.get("modules")
        if not isinstance(modules, dict):
            continue
        for phase in ("before", "after"):
            entries = modules.get(phase)
            if isinstance(entries, list):
                identity_sets[f"{label}:{phase}"] = module_identity_map(entries)

    return identity_sets


def check_module_identity_consistency(identity_sets: dict[str, dict],
                                      checks: list[dict]) -> None:
    baseline = identity_sets.get("full:before")
    check(isinstance(baseline, dict) and len(baseline) == len(MODULES),
          "module identity baseline full:before exists", checks)
    if not isinstance(baseline, dict):
        return

    required_labels = (
        "full:x11",
        "full:after",
        "syncobj_transfer",
        "syncobj_pending_exec",
    ) + tuple(
        f"{label}:{phase}"
        for label, _description in WAYLAND_REPORTS
        for phase in ("before", "after")
    )

    for label in required_labels:
        current = identity_sets.get(label)
        check(isinstance(current, dict) and len(current) == len(MODULES),
              f"module identity set {label} exists", checks)
        if not isinstance(current, dict):
            continue
        for name in MODULES:
            check(current.get(name) == baseline.get(name),
                  f"{label} {name} module identity matches full:before",
                  checks)


def collect_module_git_sets(full_summary: dict, transfer_summary: dict,
                            pending_summary: dict,
                            wayland_summaries: dict[str, dict]
                            ) -> dict[str, dict]:
    git_sets: dict[str, dict] = {}
    full_git = full_summary.get("module_git")
    if isinstance(full_git, dict):
        for phase in ("before", "x11", "after"):
            git = full_git.get(phase)
            if isinstance(git, dict) and git:
                git_sets[f"full:{phase}"] = git

    for label, summary in (
        ("syncobj_transfer", transfer_summary),
        ("syncobj_pending_exec", pending_summary),
    ):
        git = summary.get("module_git")
        if isinstance(git, dict) and git:
            git_sets[label] = git

    for label, summary in wayland_summaries.items():
        module_git_sets = summary.get("module_git")
        if not isinstance(module_git_sets, dict):
            continue
        for phase in ("before", "after"):
            git = module_git_sets.get(phase)
            if isinstance(git, dict) and git:
                git_sets[f"{label}:{phase}"] = git

    return git_sets


def check_module_git_consistency(static_summary: dict, git_sets: dict[str, dict],
                                 checks: list[dict]) -> None:
    static_git = static_summary.get("summary", {}).get("git")
    static_head = None
    if isinstance(static_git, dict):
        static_head = static_git.get("head")
    check(isinstance(static_head, str) and bool(re.fullmatch(r"[0-9a-f]{40}", static_head)),
          "static audit git head is available for runtime comparison", checks)

    required_labels = (
        "full:before",
        "full:x11",
        "full:after",
        "syncobj_transfer",
        "syncobj_pending_exec",
    ) + tuple(
        f"{label}:{phase}"
        for label, _description in WAYLAND_REPORTS
        for phase in ("before", "after")
    )

    for label in required_labels:
        git = git_sets.get(label)
        check(isinstance(git, dict) and bool(git),
              f"module git set {label} exists", checks)
        if not isinstance(git, dict) or not git:
            continue
        head = git.get("head")
        check(isinstance(head, str) and bool(re.fullmatch(r"[0-9a-f]{40}", head)),
              f"{label} module git head is a full SHA1", checks)
        check(git.get("tracked_dirty") is False,
              f"{label} module git tracked tree is clean", checks)
        if isinstance(static_head, str):
            check(head == static_head,
                  f"{label} module git head matches static audit", checks)


def write_summary(path: pathlib.Path, summary: dict) -> None:
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    tmp_path.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Aggregate nvkm KMS completion evidence",
    )
    parser.add_argument("--full-report", required=True,
                        help="Directory containing kms_smoke.py report output")
    parser.add_argument("--syncobj-transfer", required=True,
                        help="Directory from kms_smoke.py syncobj_transfer")
    parser.add_argument("--syncobj-pending-exec", required=True,
                        help="Directory from kms_smoke.py syncobj_pending_exec")
    parser.add_argument("--wayland-info", required=True,
                        help="Directory from kms_smoke.py wayland_info")
    parser.add_argument("--wayland-hpd-smoke", required=True,
                        help="Directory from kms_smoke.py wayland_hpd_smoke")
    parser.add_argument("--xwayland", required=True,
                        help="Directory from kms_smoke.py xwayland")
    parser.add_argument("--static-audit", required=True,
                        help="JSON summary from kms_static_audit.py --output")
    parser.add_argument("--output", default=None,
                        help="Output JSON path; defaults to completion_summary.json in the full report directory")
    args = parser.parse_args()

    full_dir = pathlib.Path(args.full_report)
    transfer_dir = pathlib.Path(args.syncobj_transfer)
    pending_dir = pathlib.Path(args.syncobj_pending_exec)
    wayland_dirs = {
        "wayland_info": pathlib.Path(args.wayland_info),
        "wayland_hpd_smoke": pathlib.Path(args.wayland_hpd_smoke),
        "xwayland": pathlib.Path(args.xwayland),
    }
    static_audit_path = pathlib.Path(args.static_audit)
    output = (
        pathlib.Path(args.output)
        if args.output
        else full_dir / "completion_summary.json"
    )

    checks: list[dict] = []
    static_audit_summary = check_static_audit(static_audit_path, checks)
    full_summary = check_full_report(full_dir, checks)
    transfer_summary = check_sync_gate(transfer_dir, "syncobj_transfer", checks)
    pending_summary = check_sync_gate(pending_dir, "syncobj_pending_exec", checks)
    wayland_summaries = {
        label: check_standalone_report(
            wayland_dirs[label],
            label,
            description,
            checks,
        )
        for label, description in WAYLAND_REPORTS
    }
    identity_sets = collect_module_identity_sets(
        full_summary,
        transfer_summary,
        pending_summary,
        wayland_summaries,
    )
    check_module_identity_consistency(identity_sets, checks)
    git_sets = collect_module_git_sets(
        full_summary,
        transfer_summary,
        pending_summary,
        wayland_summaries,
    )
    check_module_git_consistency(static_audit_summary, git_sets, checks)

    passed = all(bool(item["ok"]) for item in checks)
    summary = {
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "passed": passed,
        "pass_count": sum(1 for item in checks if item["ok"]),
        "fail_count": sum(1 for item in checks if not item["ok"]),
        "failures": [item["text"] for item in checks if not item["ok"]],
        "checks": checks,
        "static_audit": static_audit_summary,
        "full_report": {
            "out_dir": str(full_dir),
            "report_summary": full_summary,
        },
        "syncobj_transfer": transfer_summary,
        "syncobj_pending_exec": pending_summary,
        "wayland_reports": {
            label: {
                "out_dir": str(wayland_dirs[label]),
                "report_summary": wayland_summaries[label],
            }
            for label, _description in WAYLAND_REPORTS
        },
        "module_identity_sets": identity_sets,
        "module_git_sets": git_sets,
    }

    try:
        write_summary(output, summary)
    except OSError as err:
        print(f"FAIL write completion summary {output}: {err}")
        return 1

    print(f"INFO wrote {output}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
