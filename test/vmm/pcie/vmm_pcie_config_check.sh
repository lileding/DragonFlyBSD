#!/bin/sh
# Compile and run pure PCIe configuration-space checks.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
BASE_SYS="$ROOT/sys"
BIN=/var/tmp/vmm_pcie_config_test

cc -Wall -Wextra -Werror -std=c11 -O2 \
	-I "$ROOT/test/vmm/pcie/compat" -I "$ROOT/sys/dev/vmm" -I "$BASE_SYS" \
	"$ROOT/sys/dev/vmm/vmm_pcie_config.c" \
	"$ROOT/test/vmm/pcie/vmm_pcie_config_test.c" \
	-o "$BIN"
"$BIN"
printf '%s\n' 'PASS: vPCIe configuration space'
