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
                                 pending_summary: dict) -> dict[str, dict]:
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

    return identity_sets


def check_module_identity_consistency(identity_sets: dict[str, dict],
                                      checks: list[dict]) -> None:
    baseline = identity_sets.get("full:before")
    check(isinstance(baseline, dict) and len(baseline) == len(MODULES),
          "module identity baseline full:before exists", checks)
    if not isinstance(baseline, dict):
        return

    for label in (
        "full:x11",
        "full:after",
        "syncobj_transfer",
        "syncobj_pending_exec",
    ):
        current = identity_sets.get(label)
        check(isinstance(current, dict) and len(current) == len(MODULES),
              f"module identity set {label} exists", checks)
        if not isinstance(current, dict):
            continue
        for name in MODULES:
            check(current.get(name) == baseline.get(name),
                  f"{label} {name} module identity matches full:before",
                  checks)


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
    parser.add_argument("--static-audit", required=True,
                        help="JSON summary from kms_static_audit.py --output")
    parser.add_argument("--output", default=None,
                        help="Output JSON path; defaults to completion_summary.json in the full report directory")
    args = parser.parse_args()

    full_dir = pathlib.Path(args.full_report)
    transfer_dir = pathlib.Path(args.syncobj_transfer)
    pending_dir = pathlib.Path(args.syncobj_pending_exec)
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
    identity_sets = collect_module_identity_sets(
        full_summary,
        transfer_summary,
        pending_summary,
    )
    check_module_identity_consistency(identity_sets, checks)

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
        "module_identity_sets": identity_sets,
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
