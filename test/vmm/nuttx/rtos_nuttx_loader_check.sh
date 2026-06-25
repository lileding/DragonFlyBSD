#!/bin/sh
#
# Offline NuttX loader verifier.  It uses ordinary files as fd3/fd4 stand-ins
# and never loads vmm.ko, mounts vmmfs, or enters a guest.
set -u

ROOT=$(dirname "$0")
ELF=${NUTTX_ELF:-/var/tmp/nuttx.elf}
MEM_SIZE=${NUTTX_MEM:-64M}
LOADER=${NUTTX_LOADER_CHECK_BIN:-/var/tmp/vmmld_nuttx_check}
CHECKER=${NUTTX_MANIFEST_CHECK_BIN:-/var/tmp/vmmld_nuttx_manifest_check}
MEM_FILE=${NUTTX_CHECK_MEM_FILE:-/var/tmp/dfvmm-nuttx-loader-$$.mem}
MANIFEST_FILE=${NUTTX_CHECK_MANIFEST_FILE:-/var/tmp/dfvmm-nuttx-loader-$$.manifest}
KEEP_ARTIFACTS=${VMM_KEEP_ARTIFACTS:-0}

say()
{
	printf '%s\n' "$*"
}

fail()
{
	say "FAIL: $*"
	exit 1
}

run()
{
	say "+ $*"
	"$@" || fail "$*"
}

cleanup()
{
	set +e
	if [ "$KEEP_ARTIFACTS" -eq 0 ]; then
		rm -f "$MEM_FILE" "$MANIFEST_FILE" "$LOADER" "$CHECKER"
	fi
}

trap cleanup EXIT INT TERM

[ -f "$ELF" ] || fail "missing NUTTX_ELF=$ELF"
run cc -Wall -Wextra -Werror -std=c11 -O2 \
    "$ROOT/rtos_nuttx_loader.c" -o "$LOADER"
run cc -Wall -Wextra -Werror -std=c11 -O2 \
    "$ROOT/rtos_nuttx_manifest_check.c" -o "$CHECKER"
rm -f "$MEM_FILE" "$MANIFEST_FILE" || fail "remove old output files"
run truncate -s "$MEM_SIZE" "$MEM_FILE"
run truncate -s 65536 "$MANIFEST_FILE"
run "$LOADER" "$ELF" 3<>"$MEM_FILE" 4<>"$MANIFEST_FILE"
run "$CHECKER" "$MEM_FILE" "$MANIFEST_FILE"
say "PASS: NuttX loader offline check"
