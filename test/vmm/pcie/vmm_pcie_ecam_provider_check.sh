#!/bin/sh
#
# Offline compiler gate for the START-driven P3 ECAM provider.
set -eu

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BIN=${VMM_PCIE_ECAM_PROVIDER_BIN:-/var/tmp/vmm_pcie_ecam_provider}

cleanup()
{
	rm -f "$BIN"
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 \
	-I "$REPO/sys/vmm" \
	"$REPO/sys/vmm/vmm_pcie_abi.c" \
	"$ROOT/vmm_pcie_ecam_provider.c" -o "$BIN"
