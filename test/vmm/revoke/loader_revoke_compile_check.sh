#!/bin/sh
#
# Offline loader revoke helper verifier.  It compiles the userland probe and
# runs it once against ordinary files so result-field drift is caught.  Ordinary
# files cannot emulate kernel revoke, so the expected result is pass=0.
set -eu

ROOT=$(dirname "$0")
BIN=${VMM_REVOKE_COMPILE_BIN:-/var/tmp/vmm_revoke_loader_compile_check}
MEM=${VMM_REVOKE_COMPILE_MEM:-/var/tmp/vmm_revoke_loader_compile_check.mem}
MANIFEST=${VMM_REVOKE_COMPILE_MANIFEST:-/var/tmp/vmm_revoke_loader_compile_check.manifest}
RESULT=${VMM_REVOKE_COMPILE_RESULT:-/var/tmp/vmm_revoke_loader_compile_check.result}

cleanup()
{
	rm -f "$BIN" "$MEM" "$MANIFEST" "$RESULT" "$RESULT.ready"
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 "$ROOT/loader_revoke.c" -o "$BIN"
truncate -s 4096 "$MEM"
truncate -s 4096 "$MANIFEST"
"$BIN" exit "$RESULT" 1 3<>"$MEM" 4<>"$MANIFEST"

i=0
while [ "$i" -lt 5 ] && [ ! -f "$RESULT" ]; do
	sleep 1
	i=$((i + 1))
done
[ -f "$RESULT" ] || {
	echo "FAIL: revoke helper did not write result" >&2
	exit 1
}

for key in \
    mode pid new_mmap3_failed new_mmap4_failed old_mmap3_faulted \
    old_mmap4_faulted private_mmap3_failed private_mmap4_failed \
    exec_mmap3_failed exec_mmap4_failed exec_mprotect3_failed \
    exec_mprotect4_failed pass
do
	grep -q "^$key=" "$RESULT" || {
		echo "FAIL: revoke helper missing $key" >&2
		cat "$RESULT" >&2
		exit 1
	}
done
grep -q '^pass=0$' "$RESULT" || {
	echo "FAIL: ordinary-file smoke unexpectedly passed revoke contract" >&2
	cat "$RESULT" >&2
	exit 1
}

echo "PASS: loader revoke helper compile and smoke check"
