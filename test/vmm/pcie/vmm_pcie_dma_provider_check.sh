#!/bin/sh
#
# Offline compiler gate for the P7 revocable DMA provider.
set -eu

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BIN=${VMM_PCIE_DMA_PROVIDER_BIN:-/var/tmp/vmm_pcie_dma_provider}

cleanup()
{
	rm -f "$BIN"
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 \
	-I "$REPO/sys/vmm" \
	"$REPO/sys/vmm/vmm_pcie_abi.c" \
	"$ROOT/vmm_pcie_dma_provider.c" -o "$BIN"
