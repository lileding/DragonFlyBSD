#!/usr/local/bin/python3
import argparse
import datetime as dt
import glob
import json
import pathlib
import re
import subprocess


M6_DIR = pathlib.Path("/home/lileding/src/llama-m6")

RTX_2080_TI = {
    "cuda_cores": 4352,
    "reference_boost_clock_hz": 1.545e9,
    "founders_boost_clock_hz": 1.635e9,
    "fp32_fma_flops_per_core_cycle": 2,
    "memory_bandwidth_bytes_s": 616e9,
    "source": "https://www.nvidia.com/pt-br/geforce/graphics-cards/rtx-2080-ti/",
    "notes": [
        "NVIDIA lists 4352 CUDA cores, 1545 MHz reference boost clock, "
        "1635 MHz Founders Edition boost clock, and 616 GB/s memory bandwidth.",
        "M6.1 uses the reference 1545 MHz clock for the conservative FP32 roof.",
    ],
}

MODEL_PARAM_COUNTS = {
    "3b": 3.09e9,
    "7b": 7.61e9,
    "13b": 13.02e9,
}


def git_value(path, args):
    try:
        return subprocess.check_output(["git", "-C", str(path)] + args, text=True).strip()
    except subprocess.CalledProcessError:
        return "unknown"


def latest_contract():
    paths = sorted(pathlib.Path(path) for path in glob.glob(str(M6_DIR / "logs" / "m6_0_contract_*")))
    if not paths:
        raise SystemExit("no M6.0 contract directories found")
    for path in reversed(paths):
        try:
            manifest = json.loads((path / "manifest.json").read_text())
        except (OSError, json.JSONDecodeError):
            continue
        if {"3b", "7b", "13b"}.issubset(set(manifest.get("models", {}).keys())):
            return path
    return paths[-1]


def expand_gguf_parts(path):
    path = pathlib.Path(path)
    match = re.match(r"(.+)-00001-of-(\d+)(\.gguf)$", str(path))
    if match is None:
        return [path]
    prefix, count_text, suffix = match.groups()
    count = int(count_text)
    width = len(count_text)
    return [pathlib.Path(f"{prefix}-{idx:05d}-of-{count:0{width}d}{suffix}")
            for idx in range(1, count + 1)]


def human_gb(value):
    return value / 1e9


def human_gib(value):
    return value / (1024 ** 3)


def main():
    parser = argparse.ArgumentParser(description="M6.1 RTX 2080 Ti llama.cpp roofline")
    parser.add_argument("--contract", default=None,
                        help="M6.0 contract directory; defaults to latest")
    args = parser.parse_args()

    contract_dir = pathlib.Path(args.contract) if args.contract else latest_contract()
    manifest_path = contract_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text())

    run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = M6_DIR / "logs" / f"m6_1_roofline_{run_id}"
    out_dir.mkdir(parents=True, exist_ok=True)

    fp32_peak_ref = (
        RTX_2080_TI["cuda_cores"] *
        RTX_2080_TI["fp32_fma_flops_per_core_cycle"] *
        RTX_2080_TI["reference_boost_clock_hz"]
    )
    fp32_peak_fe = (
        RTX_2080_TI["cuda_cores"] *
        RTX_2080_TI["fp32_fma_flops_per_core_cycle"] *
        RTX_2080_TI["founders_boost_clock_hz"]
    )

    rows = []
    for key, model_path in manifest["models"].items():
        parts = expand_gguf_parts(model_path)
        missing = [str(part) for part in parts if not part.exists()]
        if missing:
            raise SystemExit(f"missing model part(s) for {key}: {', '.join(missing)}")

        model_bytes = sum(part.stat().st_size for part in parts)
        params = MODEL_PARAM_COUNTS.get(key)
        if params is None:
            raise SystemExit(f"missing nominal parameter count for model key {key}")

        min_weight_stream_tps = RTX_2080_TI["memory_bandwidth_bytes_s"] / model_bytes
        fp32_dense_tps = fp32_peak_ref / (2.0 * params)
        roof_tps = min(min_weight_stream_tps, fp32_dense_tps)
        dominant = "memory_bandwidth" if min_weight_stream_tps <= fp32_dense_tps else "fp32_dense_matvec"
        rows.append({
            "model": key,
            "paths": [str(part) for part in parts],
            "model_bytes": model_bytes,
            "model_GB_decimal": human_gb(model_bytes),
            "model_GiB": human_gib(model_bytes),
            "nominal_params": params,
            "minimum_weight_stream_tps": min_weight_stream_tps,
            "fp32_dense_matvec_tps": fp32_dense_tps,
            "dominant_limit": dominant,
            "roofline_tps": roof_tps,
            "target_70pct_tps": roof_tps * 0.70,
        })

    result = {
        "m6_step": "M6.1 theoretical roofline",
        "run_id": run_id,
        "date": subprocess.check_output(["date"], text=True).strip(),
        "contract_dir": str(contract_dir),
        "contract_manifest": str(manifest_path),
        "llama_commit": manifest.get("llama_commit"),
        "nvkm_commit": git_value("/home/lileding/src/nvkm", ["rev-parse", "--short=12", "HEAD"]),
        "benchmark_args": manifest.get("benchmark_args"),
        "gpu": {
            **RTX_2080_TI,
            "fp32_peak_reference_flops_s": fp32_peak_ref,
            "fp32_peak_reference_tflops": fp32_peak_ref / 1e12,
            "fp32_peak_founders_tflops": fp32_peak_fe / 1e12,
        },
        "formula": {
            "memory_roof_tps": "616e9 bytes/s / sum(GGUF shard file sizes in bytes)",
            "fp32_dense_roof_tps": "fp32_peak_flops/s / (2 * nominal_dense_parameter_count)",
            "selected_roof_tps": "min(memory_roof_tps, fp32_dense_roof_tps)",
            "target_70pct_tps": "0.70 * selected_roof_tps",
        },
        "caveats": [
            "This is a first-order decode roofline, not a final kernel model.",
            "It assumes each generated token streams each model weight at least once.",
            "KV cache traffic, activations, dequant overhead, launch overhead, and "
            "sync/CPU overhead can only lower the practical roof.",
            "The ggml-vulkan NVK TU102 path currently reports int dot: 0, so the "
            "compute-side comparison uses FP32 dense matvec throughput rather than "
            "tensor-core or integer-dot peak.",
        ],
        "models": rows,
    }

    json_path = out_dir / "roofline.json"
    json_path.write_text(json.dumps(result, indent=2) + "\n")

    md_path = out_dir / "roofline.md"
    with md_path.open("w") as md:
        md.write("# M6.1 Theoretical Roofline\n\n")
        md.write(f"- contract: `{contract_dir}`\n")
        md.write(f"- llama.cpp: `{manifest.get('llama_commit')}`\n")
        md.write(f"- nvkm: `{result['nvkm_commit']}`\n")
        md.write(f"- RTX 2080 Ti reference FP32: `{fp32_peak_ref / 1e12:.2f} TFLOP/s`\n")
        md.write(f"- RTX 2080 Ti memory bandwidth: `616 GB/s`\n\n")
        md.write("| Model | GGUF GB | Mem roof tok/s | FP32 roof tok/s | Dominant | 70% target tok/s |\n")
        md.write("| --- | ---: | ---: | ---: | --- | ---: |\n")
        for row in rows:
            md.write(
                f"| {row['model']} | {row['model_GB_decimal']:.3f} | "
                f"{row['minimum_weight_stream_tps']:.2f} | "
                f"{row['fp32_dense_matvec_tps']:.2f} | "
                f"{row['dominant_limit']} | {row['target_70pct_tps']:.2f} |\n"
            )
        md.write("\nThe selected M6.1 roofline is memory-bandwidth-bound for all pinned Q4_K_M gates.\n")

    print(f"m6_1_roofline_dir={out_dir}")
    print(f"m6_1_roofline_json={json_path}")
    print(f"m6_1_roofline_md={md_path}")
    for row in rows:
        print(f"m6_1_target_{row['model']}_tps={row['target_70pct_tps']:.2f}")


if __name__ == "__main__":
    main()
