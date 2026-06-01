#!/usr/local/bin/python3
import argparse
import datetime as dt
import json
import os
import pathlib
import shlex
import subprocess
import sys


ROOT = pathlib.Path("/home/lileding/src/llama.cpp")
M6_DIR = pathlib.Path("/home/lileding/src/llama-m6")
ICD = pathlib.Path("/home/lileding/src/nvk-test/nouveau_icd.json")
BIN_DIR = ROOT / "build-vulkan" / "bin"
BENCH = BIN_DIR / "llama-bench"

MODELS = {
    "3b": ROOT / "models/real/Qwen2.5-3B-Instruct-Q4_K_M.gguf",
    "7b": ROOT / "models/real/Qwen2.5-7B-Instruct-Q4_K_M-00001-of-00002.gguf",
    "13b": ROOT / "models/real/Llama-2-13B-Chat-Q4_K_M.gguf",
}

BENCH_ARGS = [
    "-p", "128",
    "-n", "128",
    "-b", "128",
    "-ub", "128",
    "-ngl", "99",
    "-r", "3",
    "-o", "jsonl",
]

FAULT_PATTERN = (
    "panic|BADFREE|DeviceLost|EXEC timeout|fault|status=0x19|"
    "NV01_DEVICE alloc failed|VM_BIND.*failed|BO wait failed|submit_test|RC_TRIGGERED"
)


def run(cmd, out_path=None, env=None, cwd=None, check=False):
    text = " ".join(shlex.quote(str(part)) for part in cmd)
    if out_path is None:
        print(f"### {text}")
        return subprocess.run(cmd, cwd=cwd, env=env, check=check)
    with out_path.open("w") as out:
        out.write(f"### {text}\n")
        out.flush()
        return subprocess.run(
            cmd,
            cwd=cwd,
            env=env,
            stdout=out,
            stderr=subprocess.STDOUT,
            check=check,
        )


def capture_shell(command, out_path):
    return run(["/bin/sh", "-c", command], out_path=out_path)


def git_value(path, args):
    try:
        return subprocess.check_output(["git", "-C", str(path)] + args, text=True).strip()
    except subprocess.CalledProcessError:
        return "unknown"


def main():
    parser = argparse.ArgumentParser(description="nouveau performance measurement contract")
    parser.add_argument("--run-bench", action="store_true",
                        help="run the pinned llama-bench commands after writing the contract")
    parser.add_argument("--models", default="3b,7b,13b",
                        help="comma-separated model keys to include")
    args = parser.parse_args()

    selected = [item.strip() for item in args.models.split(",") if item.strip()]
    unknown = [item for item in selected if item not in MODELS]
    if unknown:
        raise SystemExit(f"unknown model key(s): {', '.join(unknown)}")

    run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = M6_DIR / "logs" / f"perf_contract_{run_id}"
    out_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["VK_ICD_FILENAMES"] = str(ICD)
    env["LD_LIBRARY_PATH"] = f"{BIN_DIR}:/usr/local/lib/gcc13:/usr/local/lib"
    env.pop("MESA_DEBUG", None)
    env.pop("VK_LOADER_DEBUG", None)
    env.setdefault("MESA_SHADER_CACHE_DISABLE", "false")

    manifest = {
        "step": "performance measurement contract",
        "run_id": run_id,
        "host": subprocess.check_output(["hostname"], text=True).strip(),
        "date": subprocess.check_output(["date"], text=True).strip(),
        "llama_root": str(ROOT),
        "llama_commit": git_value(ROOT, ["rev-parse", "--short=12", "HEAD"]),
        "llama_status": git_value(ROOT, ["status", "--short"]),
        "nvkm_root": "/home/lileding/src/nvkm",
        "nvkm_commit": git_value("/home/lileding/src/nvkm", ["rev-parse", "--short=12", "HEAD"]),
        "nvkm_status": git_value("/home/lileding/src/nvkm", ["status", "--short", "--untracked-files=no"]),
        "icd": str(ICD),
        "binary": str(BENCH),
        "ld_library_path": env["LD_LIBRARY_PATH"],
        "mesa_shader_cache_disable": env["MESA_SHADER_CACHE_DISABLE"],
        "driver_debug_sysctl": "dev.drm.0.debug must be 0 for performance runs",
        "benchmark_args": BENCH_ARGS,
        "models": {key: str(MODELS[key]) for key in selected},
        "fault_scan_pattern": FAULT_PATTERN,
        "run_bench": args.run_bench,
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    bench_plan = out_dir / "bench_plan.sh"
    with bench_plan.open("w") as plan:
        plan.write("#!/bin/sh\nset -eu\n")
        plan.write(f"export VK_ICD_FILENAMES={shlex.quote(str(ICD))}\n")
        plan.write(f"export LD_LIBRARY_PATH={shlex.quote(env['LD_LIBRARY_PATH'])}\n")
        plan.write("unset MESA_DEBUG\nunset VK_LOADER_DEBUG\n")
        plan.write("export MESA_SHADER_CACHE_DISABLE=false\n")
        plan.write("doas sysctl dev.drm.0.debug=0\n")
        for key in selected:
            cmd = [str(BENCH), "-m", str(MODELS[key])] + BENCH_ARGS
            plan.write(" ".join(shlex.quote(part) for part in cmd) + "\n")
    bench_plan.chmod(0o755)

    capture_shell("uname -a; sysctl hw.model hw.ncpu kern.boottime; "
                  "kldstat | egrep 'drm|nvkm' || true",
                  out_dir / "host.txt")
    capture_shell("sysctl dev.drm.0.debug; sysctl -n dev.drm.0.state; "
                  "sysctl -n dev.drm.0.vram_state",
                  out_dir / "state_before.txt")
    capture_shell(f"dmesg | egrep -i {shlex.quote(FAULT_PATTERN)} | tail -80 || true",
                  out_dir / "dmesg_fault_scan_before.txt")

    list_rc = run([str(BENCH), "--list-devices"], out_path=out_dir / "list_devices.log",
                  env=env, cwd=ROOT)

    if args.run_bench:
        run(["doas", "sysctl", "dev.drm.0.debug=0"], out_path=out_dir / "set_debug_0.log")
        for key in selected:
            cmd = [str(BENCH), "-m", str(MODELS[key])] + BENCH_ARGS
            rc = run(cmd, out_path=out_dir / f"bench_{key}.jsonl", env=env, cwd=ROOT)
            if rc.returncode != 0:
                print(f"bench_{key}_rc={rc.returncode}", file=sys.stderr)
        capture_shell("sysctl -n dev.drm.0.state; sysctl -n dev.drm.0.vram_state",
                      out_dir / "state_after.txt")
        capture_shell(f"dmesg | egrep -i {shlex.quote(FAULT_PATTERN)} | tail -80 || true",
                      out_dir / "dmesg_fault_scan_after.txt")

    print(f"perf_contract_dir={out_dir}")
    print(f"perf_contract_list_devices_rc={list_rc.returncode}")
    print(f"perf_contract_bench_plan={bench_plan}")
    return list_rc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
