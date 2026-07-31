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
TSC_RECORD_OFFSET=520
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
	mem_size=$2

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
	[ "$(hex_at "$MEM_FILE" $((0x70800)) 4)" = "4d434647" ] ||
	    fail "missing MCFG signature in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600)) 4)" = "44534454" ] ||
	    fail "missing DSDT signature in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70200 + 244)) 12)" = \
	    "010800010404000000000000" ] ||
	    fail "FADT sleep-control GAS missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70200 + 256)) 12)" = \
	    "010800010504000000000000" ] ||
	    fail "FADT sleep-status GAS missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70200 + 112)) 4)" = "01041000" ] ||
	    fail "FADT reset flag missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70200 + 116)) 12)" = \
	    "010800010c04000000000000" ] ||
	    fail "FADT reset GAS missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70200 + 128)) 1)" = "01" ] ||
	    fail "FADT reset value missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70100 + 60)) 8)" = \
	    "0008070000000000" ] ||
	    fail "XSDT MCFG entry missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70800 + 44)) 8)" = \
	    "000000e000000000" ] ||
	    fail "MCFG ECAM base missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70800 + 52)) 8)" = \
	    "000000ff00000000" ] ||
	    fail "MCFG segment/bus range missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 4)) 4)" = "09010000" ] ||
		fail "DSDT PCI root table length missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 36)) 12)" = \
	    "085f53355f1206020a050a05" ] ||
	    fail "DSDT S5 package missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 58)) 4)" = "434f4d31" ] ||
	    fail "DSDT COM1 device missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 113)) 4)" = "52544330" ] ||
	    fail "DSDT RTC device missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 123)) 4)" = "000bd041" ] ||
	    fail "DSDT RTC PNP0B00 HID missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 152)) 8)" = "4701700070000102" ] ||
	    fail "DSDT RTC io resource missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 160)) 3)" = "220001" ] ||
	    fail "DSDT RTC IRQ8 resource missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 165)) 15)" = \
	    "1043065f53425f5b824b0550434930" ] ||
		fail "DSDT PCI0 AML scope missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 180)) 12)" = \
	    "085f4849440c41d00a08085f" ] ||
		fail "DSDT PCI0 PNP0A08 HID missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 221 + 10)) 2)" = "ff00" ] ||
		fail "DSDT PCI0 bus maximum missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 221 + 14)) 2)" = "0001" ] ||
		fail "DSDT PCI0 bus length missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 237)) 19)" = \
	    "871700000c0100000000000000c0ffffffdf00" ] ||
		fail "DSDT PCI0 MMIO resource missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70600 + 165 + 96)) 4)" = \
	    "00207900" ] ||
		fail "DSDT PCI0 resource end tag missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x90000 + 0x70)) 8)" = \
	    "0000070000000000" ] ||
	    fail "boot_params.acpi_rsdp_addr missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70400 + 52)) 12)" = \
	    "010c01000000c0fe00000000" ] ||
	    fail "MADT IOAPIC entry missing in $case_label case"
	[ "$(hex_at "$MEM_FILE" $((0x70400 + 64)) 10)" = \
	    "020a0004040000000500" ] ||
	    fail "MADT COM1 interrupt override missing in $case_label case"
	check_zero_sum "$MEM_FILE" $((0x70000)) 20 "$case_label RSDP"
	check_zero_sum "$MEM_FILE" $((0x70000)) 36 "$case_label extended RSDP"
	check_zero_sum "$MEM_FILE" $((0x70100)) 68 "$case_label XSDT"
	check_zero_sum "$MEM_FILE" $((0x70200)) 276 "$case_label FADT"
	check_zero_sum "$MEM_FILE" $((0x70400)) 74 "$case_label MADT"
	check_zero_sum "$MEM_FILE" $((0x70500)) 56 "$case_label HPET"
	check_zero_sum "$MEM_FILE" $((0x70600)) 265 "$case_label DSDT"
	check_zero_sum "$MEM_FILE" $((0x70800)) 60 "$case_label MCFG"
	if [ "$mem_size" = 4G ]; then
		[ "$(hex_at "$MEM_FILE" $((0x90000 + 0x1e8)) 1)" = "0a" ] ||
			fail "e820 PCIe aperture split count missing in $case_label case"
		[ "$(hex_at "$MEM_FILE" $((0x90000 + 0x2d0 + 4 * 20)) 20)" = \
		    "00001000000000000000f0bf0000000001000000" ] ||
			fail "e820 pre-MMIO RAM missing in $case_label case"
		[ "$(hex_at "$MEM_FILE" $((0x90000 + 0x2d0 + 5 * 20)) 20)" = \
		    "000000c000000000000000200000000002000000" ] ||
			fail "e820 MMIO reservation missing in $case_label case"
		[ "$(hex_at "$MEM_FILE" $((0x90000 + 0x2d0 + 6 * 20)) 20)" = \
		    "000000e000000000000000100000000002000000" ] ||
			fail "e820 ECAM reservation missing in $case_label case"
		[ "$(hex_at "$MEM_FILE" $((0x90000 + 0x2d0 + 7 * 20)) 20)" = \
		    "000000f0000000000000e00e0000000001000000" ] ||
			fail "e820 pre-LAPIC RAM missing in $case_label case"
		[ "$(hex_at "$MEM_FILE" $((0x90000 + 0x2d0 + 8 * 20)) 20)" = \
		    "0000e0fe00000000001000000000000002000000" ] ||
			fail "e820 LAPIC reservation missing in $case_label case"
		[ "$(hex_at "$MEM_FILE" $((0x90000 + 0x2d0 + 9 * 20)) 20)" = \
		    "0010e0fe0000000000f01f010000000001000000" ] ||
			fail "e820 post-LAPIC RAM missing in $case_label case"
	fi
}

check_tsc_manifest()
{
	time_arg=$1

	case "$time_arg" in
	tsc_hz=host)
		expected=0000000000000000
		;;
	tsc_hz=1000000000)
		expected=00ca9a3b00000000
		;;
	*)
		fail "unsupported test TSC argument: $time_arg"
		;;
	esac
	[ "$(hex_at "$MANIFEST_FILE" "$TSC_RECORD_OFFSET" 8)" = "$expected" ] ||
	    fail "bad TSC manifest record for $time_arg"
	grep -aq 'tsc_hz=' "$MEM_FILE" &&
	    fail "TSC loader argument leaked into guest command line"
}

run_loader_case()
{
	kernel=$1
	label=$2
	time_arg=$3
	mem_size=$4

	rm -f "$MEM_FILE" "$MANIFEST_FILE" || fail "remove old output files"
	run truncate -s "$mem_size" "$MEM_FILE"
	run truncate -s "$MANIFEST_SIZE" "$MANIFEST_FILE"
	if [ -n "$INITRAMFS" ]; then
		[ -f "$INITRAMFS" ] || fail "missing LINUX_INITRAMFS=$INITRAMFS"
		run "$LOADER" "$kernel" "initramfs=$INITRAMFS" "$time_arg" \
		    "console=ttyS0" "earlyprintk=serial,ttyS0,115200" \
		    3<>"$MEM_FILE" 4<>"$MANIFEST_FILE"
	else
		run "$LOADER" "$kernel" "$time_arg" "console=ttyS0" \
		    "earlyprintk=serial,ttyS0,115200" 3<>"$MEM_FILE" \
		    4<>"$MANIFEST_FILE"
	fi
	run "$PARSER" "$MEM_FILE" "$MANIFEST_FILE"
	check_linux_boot_data "$label" "$mem_size"
	check_tsc_manifest "$time_arg"
	say "PASS: Linux kexec loader $label case"
}

run cc -Wall -Wextra -Werror -std=c11 -O2 \
    "$ROOT/linux_kexec_loader.c" -o "$LOADER"
run cc -Wall -Wextra -Werror -std=c11 -O2 \
    -I "$REPO/test/vmm/manifest/compat" -I "$REPO/sys/vmm" \
    "$REPO/sys/vmm/vmm_loader_x86.c" \
    "$REPO/test/vmm/manifest/manifest_file_check.c" -o "$PARSER"

make_synth_kernel
run_loader_case "$SYNTH_KERNEL_FILE" "synthetic-host" "tsc_hz=host" \
	"$MEM_SIZE"
run_loader_case "$SYNTH_KERNEL_FILE" "synthetic-scaled" "tsc_hz=1000000000" \
	"$MEM_SIZE"
run_loader_case "$SYNTH_KERNEL_FILE" "synthetic-ecam-hole" "tsc_hz=host" 4G

if [ -f "$KERNEL" ]; then
	run_loader_case "$KERNEL" "real-image" "tsc_hz=host" "$MEM_SIZE"
else
	say "SKIP: missing LINUX_KERNEL=$KERNEL; real-image case not run"
fi
say "PASS: Linux kexec loader offline check"
