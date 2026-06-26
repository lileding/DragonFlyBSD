#!/bin/sh
#
# Offline loader revoke helper verifier.  It only compiles the userland probe;
# it does not load vmm.ko, mount vmmfs, or require root.
set -eu

ROOT=$(dirname "$0")
BIN=${VMM_REVOKE_COMPILE_BIN:-/var/tmp/vmm_revoke_loader_compile_check}

cleanup()
{
	rm -f "$BIN"
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 "$ROOT/loader_revoke.c" -o "$BIN"
echo "PASS: loader revoke helper compile check"
