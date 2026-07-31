#!/bin/sh
# Compile and run the pure vPCIe root BDF allocator checks.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
BASE_SYS=$(cd "$ROOT/../nvkm/sys" && pwd)
BIN=/var/tmp/vmm_pcie_relation_test

cc -Wall -Wextra -Werror -std=c11 -O2 \
	-I "$ROOT/test/vmm/pcie/compat" -I "$ROOT/sys/vmm" -I "$BASE_SYS" \
	"$ROOT/sys/vmm/vmm_pcie_root.c" \
	"$ROOT/test/vmm/pcie/vmm_pcie_relation_test.c" \
	-o "$BIN"
"$BIN"
printf '%s\n' 'PASS: vPCIe root BDF allocation'
