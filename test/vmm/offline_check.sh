#!/bin/sh
#
# Non-destructive vmm loader/memory ABI checks.
#
# This script compiles userland helpers and uses ordinary files as fd3/fd4
# stand-ins.  It never loads vmm.ko, mounts vmmfs, or enters a guest.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)

run()
{
	printf '+ %s\n' "$*"
	"$@"
}

run sh "$ROOT/test/vmm/manifest/manifest_parser_check.sh"
run sh "$ROOT/test/vmm/mem/mem_config_check.sh"
run sh "$ROOT/test/vmm/revoke/loader_revoke_compile_check.sh"
run sh "$ROOT/test/vmm/smoke/smoke_loader_check.sh"
run sh "$ROOT/test/vmm/nuttx/rtos_nuttx_loader_check.sh"
run sh "$ROOT/test/vmm/linux/linux_kexec_loader_check.sh"
run sh "$ROOT/test/vmm/pcie/vmm_pcie_abi_check.sh"
run sh "$ROOT/test/vmm/pcie/vmm_pcie_dma_provider_check.sh"
run sh "$ROOT/test/vmm/pcie/vmm_pcie_ecam_provider_check.sh"
run sh "$ROOT/test/vmm/pcie/vmm_pcie_bar_provider_check.sh"
run sh "$ROOT/test/vmm/pcie/vmm_pcie_msix_provider_check.sh"
run sh "$ROOT/test/vmm/pcie/vmm_pcie_config_check.sh"
run sh "$ROOT/test/vmm/pcie/vmm_pcie_relation_check.sh"
printf '%s\n' 'PASS: vmm offline checks'
