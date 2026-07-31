#!/bin/sh
# Offline compiler gate for the P7 surprise-removal provider.
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$ROOT/../../.." && pwd)
BASE_SYS=$(cd "$REPO/../nvkm/sys" && pwd)
BIN=${VMM_PCIE_DMA_LOSS_PROVIDER_BIN:-/var/tmp/vmm_pcie_dma_loss_provider}

cleanup()
{
	rm -f "$BIN"
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys/vmm" -I "$BASE_SYS" \
	"$REPO/sys/vmm/vmm_pcie_abi.c" \
	"$ROOT/vmm_pcie_dma_loss_provider.c" -o "$BIN"
