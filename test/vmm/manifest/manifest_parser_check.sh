#!/bin/sh
#
# Offline x86 manifest parser verifier.  It compiles the kernel parser with
# small userland compatibility headers, then exercises boundary cases without
# loading vmm.ko.
set -eu

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BIN=${VMM_MANIFEST_TEST_BIN:-/var/tmp/vmm_manifest_parser_test}

cleanup()
{
	rm -f "$BIN"
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 \
    -I "$ROOT/compat" -I "$REPO/sys/vmm" \
    "$REPO/sys/vmm/vmm_loader_x86.c" "$ROOT/manifest_parser_test.c" \
    -o "$BIN"
"$BIN"
