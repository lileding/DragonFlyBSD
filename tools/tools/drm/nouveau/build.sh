#!/bin/sh
# Build bwbench. Needs glslc (shaderc), a C compiler, and Vulkan headers+loader.
set -e
PREFIX="${VULKAN_PREFIX:-/usr/local}"
glslc -fshader-stage=comp bw.comp -o bw.spv
cc -O2 -I"$PREFIX/include" -o bwbench bwbench.c -L"$PREFIX/lib" -lvulkan
echo "built bwbench + bw.spv"
