#!/usr/local/bin/python3
# SPDX-License-Identifier: BSD-2-Clause
import argparse
import datetime as dt
import glob
import json
import os
import pathlib
import re
import shlex
import subprocess


M6_DIR = pathlib.Path("/home/lileding/src/llama-m6")


def latest_dir(patterns, required_models=False):
    paths = sorted(
        pathlib.Path(path)
        for pattern in patterns
        for path in glob.glob(str(M6_DIR / "logs" / pattern))
    )
    if not paths:
        raise SystemExit(f"no directories found for patterns: {', '.join(patterns)}")
    if not required_models:
        return paths[-1]
    for path in reversed(paths):
        try:
            manifest = json.loads((path / "manifest.json").read_text())
        except (OSError, json.JSONDecodeError):
            continue
        if {"3b", "7b", "13b"}.issubset(set(manifest.get("models", {}).keys())):
            return path
    return paths[-1]


def latest_roofline():
    paths = sorted(
        pathlib.Path(path)
        for pattern in ("roofline_*", "m6_1_roofline_*")
        for path in glob.glob(str(M6_DIR / "logs" / pattern))
    )
    if not paths:
        return None
    return paths[-1]


def git_value(path, args):
    try:
        return subprocess.check_output(["git", "-C", str(path)] + args, text=True).strip()
    except subprocess.CalledProcessError:
        return "unknown"


def run(cmd, out_path, env=None, cwd=None):
    with out_path.open("w") as out:
        out.write("### " + " ".join(shlex.quote(str(part)) for part in cmd) + "\n")
        out.flush()
        return subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, env=env, cwd=cwd)


def capture(command, out_path):
    return run(["/bin/sh", "-c", command], out_path)


def parse_state(path):
    values = {}
    pattern = re.compile(r"^([A-Za-z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)")
    for line in path.read_text(errors="replace").splitlines():
        match = pattern.match(line.strip())
        if not match:
            continue
        key, text = match.groups()
        values[key] = int(text, 16 if text.startswith("0x") else 10)
    return values


def parse_bench_jsonl(path):
    rows = []
    for line in path.read_text(errors="replace").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows


def parse_time(path):
    text = path.read_text(errors="replace")
    result = {}
    match = re.search(r"^\s*([0-9.]+)\s+real\s+([0-9.]+)\s+user\s+([0-9.]+)\s+sys\s*$",
                      text, re.MULTILINE)
    if match:
        result["real_s"] = float(match.group(1))
        result["user_s"] = float(match.group(2))
        result["sys_s"] = float(match.group(3))
    match = re.search(r"^\s*([0-9]+)\s+maximum resident set size\s*$", text, re.MULTILINE)
    if match:
        result["max_rss_kb"] = int(match.group(1))
    match = re.search(r"^\s*([0-9]+)\s+voluntary context switches\s*$", text, re.MULTILINE)
    if match:
        result["voluntary_ctx_switches"] = int(match.group(1))
    match = re.search(r"^\s*([0-9]+)\s+involuntary context switches\s*$", text, re.MULTILINE)
    if match:
        result["involuntary_ctx_switches"] = int(match.group(1))
    return result


def bench_tps(rows):
    values = {}
    for row in rows:
        test = str(row.get("test", ""))
        if test == "":
            n_prompt = int(row.get("n_prompt", 0) or 0)
            n_gen = int(row.get("n_gen", 0) or 0)
            if n_prompt > 0:
                test = f"pp{n_prompt}"
            elif n_gen > 0:
                test = f"tg{n_gen}"
        value = row.get("avg_ts") or row.get("t/s") or row.get("ts")
        if value is None:
            continue
        try:
            value = float(value)
        except (TypeError, ValueError):
            continue
        values[test] = value
    return values


def load_roofline_targets(path):
    if path is None:
        return {}
    data = json.loads((path / "roofline.json").read_text())
    return {
        row["model"]: row["target_70pct_tps"]
        for row in data.get("models", [])
    }


def main():
    parser = argparse.ArgumentParser(description="nouveau llama.cpp baseline profiler")
    parser.add_argument("--contract", default=None,
                        help="performance contract directory; defaults to latest complete contract")
    parser.add_argument("--roofline", default=None,
                        help="roofline directory; defaults to latest")
    parser.add_argument("--models", default="3b,7b,13b",
                        help="comma-separated model keys to run")
    args = parser.parse_args()

    contract_dir = pathlib.Path(args.contract) if args.contract else latest_dir(
        ("perf_contract_*", "m6_0_contract_*"), required_models=True)
    roofline_dir = pathlib.Path(args.roofline) if args.roofline else latest_roofline()
    manifest = json.loads((contract_dir / "manifest.json").read_text())
    targets = load_roofline_targets(roofline_dir)

    selected = [item.strip() for item in args.models.split(",") if item.strip()]
    missing = [key for key in selected if key not in manifest["models"]]
    if missing:
        raise SystemExit(f"model key(s) absent from contract: {', '.join(missing)}")

    run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = M6_DIR / "logs" / f"baseline_profile_{run_id}"
    out_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["VK_ICD_FILENAMES"] = manifest["icd"]
    env["LD_LIBRARY_PATH"] = manifest["ld_library_path"]
    env["MESA_SHADER_CACHE_DISABLE"] = manifest.get("mesa_shader_cache_disable", "false")
    env.pop("MESA_DEBUG", None)
    env.pop("VK_LOADER_DEBUG", None)

    root = pathlib.Path(manifest["llama_root"])
    bench = pathlib.Path(manifest["binary"])
    bench_args = list(manifest["benchmark_args"])
    fault_pattern = manifest["fault_scan_pattern"]

    meta = {
        "step": "baseline profile",
        "run_id": run_id,
        "date": subprocess.check_output(["date"], text=True).strip(),
        "contract_dir": str(contract_dir),
        "roofline_dir": str(roofline_dir) if roofline_dir else None,
        "llama_commit": manifest.get("llama_commit"),
        "nvkm_commit": git_value("/home/lileding/src/nvkm", ["rev-parse", "--short=12", "HEAD"]),
        "models": selected,
        "benchmark_args": bench_args,
        "env": {
            "VK_ICD_FILENAMES": env["VK_ICD_FILENAMES"],
            "LD_LIBRARY_PATH": env["LD_LIBRARY_PATH"],
            "MESA_SHADER_CACHE_DISABLE": env["MESA_SHADER_CACHE_DISABLE"],
        },
    }
    (out_dir / "manifest.json").write_text(json.dumps(meta, indent=2) + "\n")

    capture("doas sysctl dev.drm.0.debug=0", out_dir / "set_debug_0.log")
    capture("uname -a; sysctl hw.model hw.ncpu kern.boottime; "
            "sysctl dev.drm.0.debug; kldstat | egrep 'drm|nvkm' || true",
            out_dir / "host.txt")

    summary = {"models": []}
    for key in selected:
        prefix = out_dir / key
        capture("sysctl -n dev.drm.0.state; sysctl -n dev.drm.0.vram_state",
                prefix.with_name(f"{key}_state_before.txt"))
        capture(f"dmesg | egrep -i {shlex.quote(fault_pattern)} | tail -80 || true",
                prefix.with_name(f"{key}_dmesg_fault_scan_before.txt"))

        cmd = ["/usr/bin/time", "-l", str(bench), "-m", manifest["models"][key]] + bench_args
        log_path = prefix.with_name(f"{key}_bench.jsonl")
        rc = run(cmd, log_path, env=env, cwd=root)

        capture("sysctl -n dev.drm.0.state; sysctl -n dev.drm.0.vram_state",
                prefix.with_name(f"{key}_state_after.txt"))
        capture(f"dmesg | egrep -i {shlex.quote(fault_pattern)} | tail -80 || true",
                prefix.with_name(f"{key}_dmesg_fault_scan_after.txt"))

        before = parse_state(prefix.with_name(f"{key}_state_before.txt"))
        after = parse_state(prefix.with_name(f"{key}_state_after.txt"))
        counters = [
            "submit_count", "signal_only_count", "timeout_count",
            "internal_fence_count", "signal_fence_count",
            "resv_attach_calls", "resv_attach_bos",
            "token_wait_us", "wait_sync_us", "push_build_us",
            "prepare_signal_us", "attach_resv_us", "flush_cpu_us",
            "cache_flush_us", "doorbell_us", "poll_us", "cleanup_us",
            "poll_iters", "pushes", "cpu_bind_scanned",
            "cpu_bind_flushed",
            "wait_count", "wait_error_count", "signal_count",
            "signal_error_count", "bo_wait_count", "bo_wait_error_count",
            "vm_bind_wait_count", "vm_bind_wait_error_count",
            "cpu_prep_wait_count", "cpu_prep_wait_error_count",
            "gva_used", "user_pd0_count", "user_pt_count", "valid_pte_count",
            "active_bytes",
        ]
        delta = {
            name: after[name] - before[name]
            for name in counters
            if name in before and name in after
        }
        rows = parse_bench_jsonl(log_path)
        tps = bench_tps(rows)
        target = targets.get(key)
        tg_value = next((value for test, value in tps.items() if test.startswith("tg")), None)
        summary["models"].append({
            "model": key,
            "returncode": rc.returncode,
            "bench_log": str(log_path),
            "time": parse_time(log_path),
            "bench_tps": tps,
            "target_70pct_tps": target,
            "tg_target_ratio": (tg_value / target) if (tg_value is not None and target) else None,
            "counter_delta": delta,
        })
        (out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(f"baseline_{key}_rc={rc.returncode}")

    md_path = out_dir / "summary.md"
    with md_path.open("w") as md:
        md.write("# Baseline Profile\n\n")
        md.write(f"- contract: `{contract_dir}`\n")
        md.write(f"- roofline: `{roofline_dir}`\n\n")
        md.write("| Model | rc | pp tok/s | tg tok/s | 70% target | tg/target | real s | submit delta | timeout delta | poll s | flush CPU s |\n")
        md.write("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n")
        for row in summary["models"]:
            pp = next((value for test, value in row["bench_tps"].items() if test.startswith("pp")), None)
            tg = next((value for test, value in row["bench_tps"].items() if test.startswith("tg")), None)
            target = row["target_70pct_tps"]
            ratio = row["tg_target_ratio"]
            real_s = row["time"].get("real_s")
            delta = row["counter_delta"]
            md.write(
                f"| {row['model']} | {row['returncode']} | "
                f"{pp if pp is not None else 0:.2f} | "
                f"{tg if tg is not None else 0:.2f} | "
                f"{target if target is not None else 0:.2f} | "
                f"{ratio if ratio is not None else 0:.4f} | "
                f"{real_s if real_s is not None else 0:.2f} | "
                f"{delta.get('submit_count', 0)} | {delta.get('timeout_count', 0)} | "
                f"{delta.get('poll_us', 0) / 1000000.0:.2f} | "
                f"{delta.get('flush_cpu_us', 0) / 1000000.0:.2f} |\n"
            )

    print(f"baseline_profile_dir={out_dir}")
    print(f"baseline_profile_summary={out_dir / 'summary.json'}")
    print(f"baseline_profile_report={md_path}")


if __name__ == "__main__":
    main()
