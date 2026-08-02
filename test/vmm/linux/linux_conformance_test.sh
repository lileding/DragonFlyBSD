#!/bin/sh
# pc64 true-hardware conformance gate for the dfvmm-modern-x86_64 contract.
#
# Each stage owns one module load, vmmfs mount, and guest.  The stage boundary
# requires vmm.ko to be unloaded so a leaked vnode, loader capability, or
# machine cannot be hidden by the next stage.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
WORK=${VMM_CONFORMANCE_WORK:-/var/tmp/dfvmm-linux-conformance-$$}

say()
{
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"
}

fail()
{
	say "FAIL: $*"
	exit 1
}

check_unloaded()
{
	kldstat -n vmm >/dev/null 2>&1 && fail "vmm.ko remains loaded after $1"
}

run_stage()
{
	name=$1
	shift
	stage="$WORK/$name"

	check_unloaded "previous stage"
	mkdir -p "$stage" || fail "mkdir $stage"
	say "BEGIN: $name"
	"$@" >"$stage/output.log" 2>&1 || {
		cat "$stage/output.log"
		fail "$name"
	}
	check_unloaded "$name"
	say "PASS: $name"
}

[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
[ "$(uname -m)" = x86_64 ] || fail "run on a pc64 host"
[ -f "$KERNEL" ] || fail "missing Linux kernel: $KERNEL"
[ -f "$INITRD" ] || fail "missing Linux initrd rootfs: $INITRD"
check_unloaded "entry"
mkdir -p "$WORK" || fail "mkdir $WORK"

run_stage offline "$REPO/test/vmm/offline_check.sh"

say "+ make -C $REPO/sys/vmm MACHINE_PLATFORM=pc64"
(
	cd "$REPO/sys/vmm" || exit 1
	make MACHINE_PLATFORM=pc64
) >"$WORK/build.log" 2>&1 || {
	cat "$WORK/build.log"
	fail "build vmm.ko"
}
readelf -SW "$VMM_KO" | grep -qi eh_frame && fail "$VMM_KO contains .eh_frame"

run_stage loader-abi env \
	LINUX_KERNEL="$KERNEL" \
	"$ROOT/linux_kexec_loader_check.sh"
run_stage guest-reset env \
	VMM_KO="$VMM_KO" \
	VMM_MOUNT="$WORK/guest-reset-vmm" \
	VMM_LOG="$WORK/guest-reset.log" \
	VMM_CONSOLE_LOG="$WORK/guest-reset.console" \
	VMM_LOADER_RUN_LOG="$WORK/guest-reset.loader-runs" \
	LINUX_KERNEL="$KERNEL" \
	LINUX_INITRD_ROOTFS="$INITRD" \
	"$ROOT/linux_guest_reset_test.sh"
run_stage console env \
	VMM_KO="$VMM_KO" \
	VMM_MOUNT="$WORK/console-vmm" \
	VMM_LOG="$WORK/console.log" \
	VMM_CONSOLE_LOG="$WORK/console.console" \
	LINUX_KERNEL="$KERNEL" \
	LINUX_INITRD_ROOTFS="$INITRD" \
	"$ROOT/linux_console_terminal_test.sh"
run_stage time-idle env \
	VMM_KO="$VMM_KO" \
	VMM_MOUNT="$WORK/time-vmm" \
	VMM_LOG="$WORK/time.log" \
	VMM_CONSOLE_LOG_DIR="$WORK/time-console" \
	VMM_LINUX_INSTANCES=2 \
	LINUX_KERNEL="$KERNEL" \
	LINUX_INITRD_ROOTFS="$INITRD" \
	"$ROOT/linux_time_idle_test.sh"
run_stage reset env \
	VMM_KO="$VMM_KO" \
	VMM_MOUNT="$WORK/reset-vmm" \
	VMM_LOG="$WORK/reset.log" \
	VMM_CONSOLE_LOG="$WORK/reset.console" \
	VMM_RESET_ROUNDS=2 \
	LINUX_KERNEL="$KERNEL" \
	LINUX_INITRD_ROOTFS="$INITRD" \
	"$ROOT/linux_reset_test.sh"
run_stage triplefault env \
	VMM_KO="$VMM_KO" \
	VMM_SMOKE_MODES=triplefault \
	"$REPO/test/vmm/smoke/smoke_exec_test.sh"

say "PASS: dfvmm-modern-x86_64 pc64 conformance"
