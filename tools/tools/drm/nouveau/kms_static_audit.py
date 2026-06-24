#!/usr/local/bin/python3
# SPDX-License-Identifier: BSD-2-Clause
"""Audit static nvkm KMS boundary constraints.

The audit is intentionally narrow and read-only.  It checks the tracked
display/KMS source files that belong to the current nvkm display goal rather
than recursively scanning build symlinks or unrelated DragonFly DRM drivers.
"""

import argparse
import datetime as dt
import json
import pathlib
import re
import subprocess


TARGET_PATTERNS = (
    "sys/dev/drm/nouveau/nvkm_drm_kms.c",
    "sys/dev/drm/nouveau/nvkm_gsp_disp.c",
    "sys/dev/drm/nouveau/nvkm_gsp_disp.h",
    "sys/dev/drm/nouveau/nvkm_disp_tu102.c",
    "sys/dev/drm/nouveau/dispnv50/*",
    "sys/dev/drm/nouveau/engine/disp.h",
    "sys/dev/drm/nouveau/engine/disp/*",
    "sys/dev/drm/drm_lease.c",
    "sys/dev/drm/include/drm/drm_lease.h",
    "tools/tools/drm/nouveau/kms_smoke.py",
    "tools/tools/drm/nouveau/kms_completion_audit.py",
    "tools/tools/drm/nouveau/kms_static_audit.py",
)

SOURCE_SUFFIXES = (".c", ".h", ".py")
STATIC_AUDIT_PATH = "tools/tools/drm/nouveau/kms_static_audit.py"

ALLOWED_LINUX_INCLUDES = {
    "sys/dev/drm/drm_lease.c": {
        "linux/bitops.h",
        "linux/file.h",
        "linux/slab.h",
        "linux/uaccess.h",
    },
    "sys/dev/drm/nouveau/dispnv50/nvkm_dispnv50_bridge.c": {
        "linux/math64.h",
    },
    "sys/dev/drm/nouveau/nvkm_drm_kms.c": {
        "linux/slab.h",
        "linux/workqueue.h",
    },
}

FORBIDDEN_PATTERNS = (
    re.compile(r"SPDX-License-Identifier:\s*.*GPL", re.I),
    re.compile(r"EXPORT_SYMBOL_GPL\s*\("),
    re.compile(r"MODULE_LICENSE\s*\(\s*\"GPL", re.I),
    re.compile(r"GPL-only", re.I),
    re.compile(r"GNU General Public License", re.I),
)

LINUX_INCLUDE_RE = re.compile(r"^\s*#\s*include\s*<linux/([^>]+)>", re.M)
SPDX_RE = re.compile(r"SPDX-License-Identifier:\s*([^\n*]+)")


def source_tree_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[4]


def run_git_ls_files(root: pathlib.Path) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", *TARGET_PATTERNS],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    paths = {
        line.strip()
        for line in result.stdout.splitlines()
        if line.strip().endswith(SOURCE_SUFFIXES)
    }
    return sorted(paths)


def has_allowed_license(text: str) -> bool:
    spdx = SPDX_RE.search(text)
    if spdx is not None:
        expression = spdx.group(1)
        return "GPL" not in expression and (
            "BSD" in expression or "MIT" in expression or "ISC" in expression
        )

    return (
        "Permission is hereby granted, free of charge" in text
        and "THE SOFTWARE IS PROVIDED" in text
        and "MERCHANTABILITY" in text
    )


def linux_includes(text: str) -> list[str]:
    return [f"linux/{match.group(1)}" for match in LINUX_INCLUDE_RE.finditer(text)]


def is_self_audit_pattern_line(relative_path: str, line: str) -> bool:
    return relative_path == STATIC_AUDIT_PATH and (
        "re.compile(" in line
        or '"GPL" not in expression' in line
        or "has no GPL-only markers" in line
    )


def forbidden_matches(relative_path: str, text: str) -> list[str]:
    matches: list[str] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        if is_self_audit_pattern_line(relative_path, line):
            continue
        for pattern in FORBIDDEN_PATTERNS:
            match = pattern.search(line)
            if match is not None:
                matches.append(f"{line_number}:{match.group(0)}")
    return matches


def check(ok: bool, text: str, checks: list[dict]) -> None:
    checks.append({"ok": ok, "text": text})
    print(("PASS " if ok else "FAIL ") + text)


def audit_file(root: pathlib.Path, relative_path: str,
               checks: list[dict]) -> dict:
    path = root / relative_path
    result = {
        "path": relative_path,
        "allowed_linux_includes": sorted(
            ALLOWED_LINUX_INCLUDES.get(relative_path, set())
        ),
        "linux_includes": [],
        "forbidden_matches": [],
        "has_allowed_license": False,
    }

    check(path.exists(), f"{relative_path} exists", checks)
    check(not path.is_symlink(), f"{relative_path} is not a symlink", checks)
    if not path.exists() or path.is_symlink():
        return result

    text = path.read_text(errors="replace")
    result["has_allowed_license"] = has_allowed_license(text)
    check(result["has_allowed_license"],
          f"{relative_path} has BSD/MIT-compatible license text", checks)

    result["forbidden_matches"] = forbidden_matches(relative_path, text)
    check(not result["forbidden_matches"],
          f"{relative_path} has no GPL-only markers", checks)

    includes = linux_includes(text)
    result["linux_includes"] = includes
    allowed = ALLOWED_LINUX_INCLUDES.get(relative_path, set())
    unexpected = [include for include in includes if include not in allowed]
    check(not unexpected,
          f"{relative_path} has no unexpected linux includes", checks)

    return result


def write_summary(path: pathlib.Path, summary: dict) -> None:
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    tmp_path.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Audit static nvkm KMS license and LinuxKPI boundaries",
    )
    parser.add_argument("--output", default=None,
                        help="Optional JSON summary path")
    args = parser.parse_args()

    root = source_tree_root()
    checks: list[dict] = []
    files = run_git_ls_files(root)
    check(bool(files), "tracked KMS static audit file set is non-empty", checks)

    file_results = [audit_file(root, path, checks) for path in files]
    passed = all(bool(item["ok"]) for item in checks)
    summary = {
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source_tree": str(root),
        "passed": passed,
        "pass_count": sum(1 for item in checks if item["ok"]),
        "fail_count": sum(1 for item in checks if not item["ok"]),
        "failures": [item["text"] for item in checks if not item["ok"]],
        "checks": checks,
        "files": file_results,
    }

    if args.output is not None:
        try:
            write_summary(pathlib.Path(args.output), summary)
        except OSError as err:
            print(f"FAIL write static audit summary {args.output}: {err}")
            return 1
        print(f"INFO wrote {args.output}")

    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
