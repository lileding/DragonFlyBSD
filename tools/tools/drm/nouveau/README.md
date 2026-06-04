# bwbench — NVK/Vulkan memory-read-bandwidth microbenchmark

A standalone Vulkan compute benchmark that measures the DRAM **read** bandwidth a
Vulkan application actually gets through NVK on this GPU. It exists to separate a
*kernel-driver / memory-clock* limit from a *caller-side* (NVK-Mesa codegen,
llama.cpp Vulkan backend) inefficiency when a memory-bound workload (e.g. LLM
token generation) runs slower than expected.

The kernel is a grid-stride read-accumulate over a device-local buffer that is
much larger than L2, with four independent accumulators to expose enough
memory-level parallelism to saturate DRAM. This mirrors what LLM decode is bound
by (streaming the weights once = GEMV), with none of llama.cpp's per-token,
attention, or synchronization overhead.

## Build & run

```sh
./build.sh   # glslc bw.comp -> bw.spv ; cc bwbench.c -> bwbench
VK_ICD_FILENAMES=/path/to/nouveau_icd.json LD_LIBRARY_PATH=/usr/local/lib \
    ./bwbench [buf_MB] [iters] [reps] [groups]
# defaults: 256 MB, iters=8, reps=30, groups=4096
```

## Result on TU102 / RTX 2080 Ti (NVK, GSP r570), 2026-06-04

```
device: NVIDIA GeForce RTX 2080 Ti (NVK TU102)
buffer: 512 MB, iters=8, groups=4096, reps=30
=> 478 GB/s  (77.7% of the 616 GB/s peak)
```

A streaming kernel reaching ~78% of peak is only possible if DRAM runs at
(near-)full clock, so **the memory clock is fine — there is no clock problem.**
For comparison, `llama-bench` decode at `tg≈18` for a 3B Q4_K_M model moves only
~35 GB/s (≈7% of the 478 GB/s the GPU delivers here), i.e. the decode shortfall
is ~13× of caller-side headroom in the NVK/Mesa compute path and llama.cpp's
Vulkan backend, **not** the DragonFly kernel driver.
