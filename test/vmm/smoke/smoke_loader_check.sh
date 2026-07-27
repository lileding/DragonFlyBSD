#!/bin/sh
#
# Offline smoke-loader verifier.  It uses ordinary files as fd3/fd4 stand-ins
# and never loads vmm.ko, mounts vmmfs, or enters a guest.
set -eu

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
LOADER=${VMM_SMOKE_LOADER_CHECK_BIN:-/var/tmp/vmm_smoke_loader_check}
CHECKER=${VMM_MANIFEST_FILE_CHECK_BIN:-/var/tmp/vmm_manifest_file_check}
MEM_SIZE=${VMM_SMOKE_MEM:-2M}
MANIFEST_SIZE=${VMM_SMOKE_MANIFEST_SIZE:-4096}
MODES=${VMM_SMOKE_LOADER_MODES:-"vmmcall cpuid fpu msrpatch msrsyscfg mtrrcap msrhwcr pcicfg pitfallback pit0 rtccmos rtc_settime iodelay elcr serial serialin serialirq time xsetbv apicmsr timerint lapictimer lapictimer_masked ud mwaitxud pic ioapic ioapicirq x2apic cachetlb pm64 avicirq avicipi aviclvt avictimercfg aviclint aviclvtpc avicesr avicsvr avicnoaccel avicread hlt loop cliloop triplefault acpi_s5"}
PREFIX=/var/tmp/dfvmm-smoke-loader-check-$$

cleanup()
{
	rm -f "$LOADER" "$CHECKER" "$PREFIX"-*
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 "$ROOT/smoke_loader.c" -o "$LOADER"
cc -Wall -Wextra -Werror -std=c11 -O2 \
	-I "$REPO/test/vmm/manifest/compat" -I "$REPO/sys/vmm" \
	"$REPO/sys/vmm/vmm_loader_x86.c" \
	"$REPO/test/vmm/manifest/manifest_file_check.c" -o "$CHECKER"
for mode in $MODES; do
	mem=$PREFIX-$mode.mem
	manifest=$PREFIX-$mode.manifest
	truncate -s "$MEM_SIZE" "$mem"
	truncate -s "$MANIFEST_SIZE" "$manifest"
	mem_bytes=$(stat -f %z "$mem")
	sentinel_offset=$((mem_bytes - 1))
	printf '\245' | dd of="$mem" bs=1 seek="$sentinel_offset" \
	    count=1 conv=notrunc >/dev/null 2>&1
	"$LOADER" "$mode" 3<>"$mem" 4<>"$manifest"
	byte=$(od -An -tx1 -j "$sentinel_offset" -N 1 "$mem" |
	    tr -d ' \n')
	[ "$byte" = "a5" ] || {
		echo "FAIL: fd3 sentinel changed for $mode: $byte"
		exit 1
	}
	"$CHECKER" "$mem" "$manifest"
	rm -f "$mem" "$manifest"
done
echo "PASS: smoke loader offline check"
