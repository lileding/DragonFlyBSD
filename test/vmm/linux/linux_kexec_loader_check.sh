#!/bin/sh
#
# Verify the Linux loader's fd3 memory and fd2 cpustate protocol offline.
set -eu

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
LOADER="$REPO/sbin/vmmld_linux/vmmld_linux"
BASE=/var/tmp/vmmld-linux-check-$$
MEM=$BASE.mem
STATE=$BASE.state
KERNEL=$BASE.bzImage

cleanup()
{
	rm -f "$MEM" "$STATE" "$KERNEL"
}

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

hex_at()
{
	hexdump -v -e '1/1 "%02x"' -s "$2" -n "$3" "$1"
}

trap cleanup EXIT INT TERM
make -C "$REPO/sbin/vmmld_linux"
truncate -s 6656 "$KERNEL"
printf '\004' | dd of="$KERNEL" bs=1 seek=497 conv=notrunc >/dev/null 2>&1
printf '\110\144\162\123' | dd of="$KERNEL" bs=1 seek=514 conv=notrunc >/dev/null 2>&1
printf '\014\002' | dd of="$KERNEL" bs=1 seek=518 conv=notrunc >/dev/null 2>&1
printf '\001' | dd of="$KERNEL" bs=1 seek=529 conv=notrunc >/dev/null 2>&1
printf '\001\000' | dd of="$KERNEL" bs=1 seek=566 conv=notrunc >/dev/null 2>&1
printf '\000\020\000\000' | dd of="$KERNEL" bs=1 seek=568 conv=notrunc >/dev/null 2>&1
printf '\000\040\000\000' | dd of="$KERNEL" bs=1 seek=608 conv=notrunc >/dev/null 2>&1
truncate -s 256M "$MEM"
"$LOADER" "$KERNEL" console=ttyS0 3<>"$MEM" 2>"$STATE"
[ -s "$STATE" ] || fail "loader did not write fd2 cpustate"
[ "$(hex_at "$MEM" $((0x90000 + 0x70)) 8)" = "0000070000000000" ] ||
	fail "boot_params.acpi_rsdp_addr"
[ "$(hex_at "$MEM" $((0x90000 + 0x2d0 + 20)) 20)" = \
	"0000070000000000000001000000000002000000" ] ||
	fail "E820 platform reservation"
grep -aq 'vcpu=' "$MEM" && fail "vcpu argument leaked into command line"
echo "PASS: Linux loader fd3/fd2 platform ABI"
