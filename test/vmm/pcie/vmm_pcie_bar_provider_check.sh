#!/bin/sh
#
# Offline compiler gate for the START-driven P4 direct-BAR provider.
set -eu

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BASE_SYS="$REPO/sys"
BIN=${VMM_PCIE_BAR_PROVIDER_BIN:-/var/tmp/vmm_pcie_bar_provider}

cleanup()
{
	rm -f "$BIN"
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 \
	-I "$REPO/sys/vmm" \
	-I "$BASE_SYS" \
	"$REPO/sys/vmm/vmm_pcie_abi.c" \
	"$ROOT/vmm_pcie_bar_provider.c" -o "$BIN"
