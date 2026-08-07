#!/bin/sh
#
# Offline mem verifier.  It compiles the kernel mem object with small userland
# compatibility headers, then exercises config and backing boundaries without
# loading vmm.ko.
set -eu

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BIN=${VMM_MEM_CONFIG_TEST_BIN:-/var/tmp/vmm_mem_config_test}

cleanup()
{
	rm -f "$BIN"
}

trap cleanup EXIT INT TERM

cc -Wall -Wextra -Werror -std=c11 -O2 \
    -I "$ROOT/compat" -I "$REPO/sys/dev/vmm" \
    "$REPO/sys/dev/vmm/vmm_parse.c" "$REPO/sys/dev/vmm/vmm_mem.c" \
    "$ROOT/mem_config_test.c" -o "$BIN"
"$BIN"
