#!/bin/sh
#
# Offline vPCIe provider/consumer ABI verifier.
set -eu

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BASE_SYS="$REPO/sys"
BIN=${VMM_PCIE_ABI_TEST_BIN:-/var/tmp/vmm_pcie_abi_test}

cleanup()
{
	rm -f "$BIN"
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 \
	-I "$REPO/sys/dev/vmm" \
	-I "$BASE_SYS" \
	"$REPO/sys/dev/vmm/vmm_pcie_abi.c" \
	"$ROOT/vmm_pcie_abi_test.c" -o "$BIN"
"$BIN"
