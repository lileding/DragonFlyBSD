#!/bin/sh
#
# Offline Linux kexec loader verifier.  It uses ordinary files as fd3/fd4
# stand-ins and validates the resulting manifest with the kernel parser.  The
# default case uses a small synthetic bzImage header so the gate does not depend
# on a downloaded Linux kernel; setting LINUX_KERNEL runs one real-image case
# as well.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
KERNEL=${LINUX_KERNEL:-/var/tmp/bzImage}
INITRAMFS=${LINUX_INITRAMFS:-}
MEM_SIZE=${LINUX_MEM:-256M}
MANIFEST_SIZE=${LINUX_MANIFEST_SIZE:-4096}
LOADER=${LINUX_KEXEC_LOADER_CHECK_BIN:-/var/tmp/vmmld_linux_kexec_check}
PARSER=${VMM_MANIFEST_FILE_CHECK_BIN:-/var/tmp/vmm_manifest_file_check}
MEM_FILE=${LINUX_CHECK_MEM_FILE:-/var/tmp/dfvmm-linux-loader-$$.mem}
MANIFEST_FILE=${LINUX_CHECK_MANIFEST_FILE:-/var/tmp/dfvmm-linux-loader-$$.manifest}
SYNTH_KERNEL_FILE=${LINUX_SYNTH_KERNEL_FILE:-/var/tmp/dfvmm-linux-loader-$$.bzImage}
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

hex_at()
{
	file=$1
	offset=$2
	count=$3

	hexdump -v -e '1/1 "%02x"' -s "$offset" -n "$count" "$file"
}

check_zero_sum()
{
	file=$1
	offset=$2
	count=$3
	sum_label=$4

	hexdump -v -e '1/1 "%u\n"' -s "$offset" -n "$count" "$file" |
	    awk '
	    {
		    for (i = 1; i <= NF; i++)
			    sum += $i;
	    }
	    END {
		    exit (sum % 256 == 0) ? 0 : 1;
	    }' || fail "$sum_label checksum"
}

cleanup()
{
	set +e
	if [ "$KEEP_ARTIFACTS" -eq 0 ]; then
		rm -f "$MEM_FILE" "$MANIFEST_FILE" "$SYNTH_KERNEL_FILE" \
		    "$LOADER" "$PARSER"
	fi
}

trap cleanup EXIT INT TERM

write_synth_bytes()
{
	offset=$1
	bytes=$2

	printf "$bytes" | dd of="$SYNTH_KERNEL_FILE" bs=1 seek="$offset" \
	    conv=notrunc >/dev/null 2>&1 || fail "write synthetic kernel"
}

make_synth_kernel()
{
	rm -f "$SYNTH_KERNEL_FILE" || fail "remove synthetic kernel"
	run truncate -s 6656 "$SYNTH_KERNEL_FILE"
	write_synth_bytes 497 '\004'			# setup_sects
	write_synth_bytes 514 '\110\144\162\123'	# HdrS
	write_synth_bytes 518 '\014\002'		# boot protocol 2.12
	write_synth_bytes 529 '\001'			# LOADED_HIGH
	write_synth_bytes 566 '\001\000'		# XLF_KERNEL_64
	write_synth_bytes 568 '\000\020\000\000'	# cmdline_size 4096
	write_synth_bytes 608 '\000\040\000\000'	# init_size 8192
}

check_linux_boot_data()
{
	case_label=$1

	[ "$(hex_at "$MEM_FILE" $((0x70000)) 8)" = "5253442050545220" ] ||
	    fail "missing RSDP signature in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70100)) 4)" = "58534454" ] ||
	    fail "missing XSDT signature in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70200)) 4)" = "46414350" ] ||
	    fail "missing FADT signature in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70400)) 4)" = "41504943" ] ||
	    fail "missing MADT signature in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70500)) 4)" = "48504554" ] ||
	    fail "missing HPET signature in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600)) 4)" = "44534454" ] ||
	    fail "missing DSDT signature in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x90000 + 0x70)) 8)" = \
	    "0000070000000000" ] ||
	    fail "boot_params.acpi_rsdp_addr missing in $case_label case"
	check_zero_sum "$MEM_FILE" $((0x70000)) 20 "$case_label RSDP"
	check_zero_sum "$MEM_FILE" $((0x70000)) 36 "$case_label extended RSDP"
	check_zero_sum "$MEM_FILE" $((0x70100)) 60 "$case_label XSDT"
	check_zero_sum "$MEM_FILE" $((0x70200)) 276 "$case_label FADT"
	check_zero_sum "$MEM_FILE" $((0x70400)) 52 "$case_label MADT"
	check_zero_sum "$MEM_FILE" $((0x70500)) 56 "$case_label HPET"
	check_zero_sum "$MEM_FILE" $((0x70600)) 36 "$case_label DSDT"
}

run_loader_case()
{
	kernel=$1
	label=$2

	rm -f "$MEM_FILE" "$MANIFEST_FILE" || fail "remove old output files"
	run truncate -s "$MEM_SIZE" "$MEM_FILE"
	run truncate -s "$MANIFEST_SIZE" "$MANIFEST_FILE"
	if [ -n "$INITRAMFS" ]; then
		[ -f "$INITRAMFS" ] || fail "missing LINUX_INITRAMFS=$INITRAMFS"
		run "$LOADER" "$kernel" "initramfs=$INITRAMFS" \
		    "console=ttyS0" "earlyprintk=serial,ttyS0,115200" \
		    3<>"$MEM_FILE" 4<>"$MANIFEST_FILE"
	else
		run "$LOADER" "$kernel" "console=ttyS0" \
		    "earlyprintk=serial,ttyS0,115200" 3<>"$MEM_FILE" \
		    4<>"$MANIFEST_FILE"
	fi
	run "$PARSER" "$MEM_FILE" "$MANIFEST_FILE"
	check_linux_boot_data "$label"
	say "PASS: Linux kexec loader $label case"
}

run cc -Wall -Wextra -Werror -std=c11 -O2 \
    "$ROOT/linux_kexec_loader.c" -o "$LOADER"
run cc -Wall -Wextra -Werror -std=c11 -O2 \
    -I "$REPO/test/vmm/manifest/compat" -I "$REPO/sys/vmm" \
    "$REPO/sys/vmm/vmm_loader_x86.c" \
    "$REPO/test/vmm/manifest/manifest_file_check.c" -o "$PARSER"

make_synth_kernel
run_loader_case "$SYNTH_KERNEL_FILE" "synthetic"

if [ -f "$KERNEL" ]; then
	run_loader_case "$KERNEL" "real-image"
else
	say "SKIP: missing LINUX_KERNEL=$KERNEL; real-image case not run"
fi
say "PASS: Linux kexec loader offline check"
