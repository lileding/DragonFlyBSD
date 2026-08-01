#!/bin/sh
# pc64 true-hardware Linux SVM trace gate.
#
# Always-on lifecycle and diagnostic records remain visible in the per-machine
# events log.  This gate verifies that Linux's TSC-deadline trace is absent
# when debug.vmm.svm_trace is disabled and present when it is enabled.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-trace-vmm}
VM_OFF=${VMM_TRACE_OFF_MACHINE:-linuxtraceoff0}
VM_ON=${VMM_TRACE_ON_MACHINE:-linuxtraceon0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-trace-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-linux-trace.console}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_trace}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-trace-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-40}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-30}

LOADED=0
MOUNTED=0
CONSOLE_READER_PID=
OLD_SVM_TRACE=

say()
{
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"
}

fail()
{
	say "FAIL: $*"
	dump_state
	exit 1
}

run()
{
	say "+ $*"
	"$@" >>"$LOG" 2>&1 || fail "$*"
}

machine_dir()
{
	printf '%s/%s\n' "$MNT" "$1"
}

console_path()
{
	printf '%s/console\n' "$(machine_dir "$1")"
}

events_path()
{
	printf '%s/events\n' "$(machine_dir "$1")"
}

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}

append_file()
{
	label=$1
	file=$2

	[ -e "$file" ] || return 0
	{
		printf -- '--- %s: %s ---\n' "$label" "$file"
		cat "$file" 2>&1
		printf -- '--- end %s ---\n' "$label"
	} >>"$LOG"
}

dump_state()
{
	{
		printf '%s\n' '--- kldstat ---'
		kldstat -n vmm 2>&1 || true
		printf '%s\n' '--- mount ---'
		mount 2>&1 || true
		printf '%s\n' '--- machines ---'
		[ -d "$MNT/machines" ] && ls -la "$MNT/machines" 2>&1 || true
		printf '%s\n' '--- end state ---'
	} >>"$LOG"
	for vm in "$VM_OFF" "$VM_ON"; do
		[ -d "$(machine_dir "$vm")" ] || continue
		append_file "$vm events" "$(events_path "$vm")"
	done
	append_file console "$CONSOLE_LOG"
}

start_console_reader()
{
	vm=$1

	: >"$CONSOLE_LOG" || fail "truncate $CONSOLE_LOG"
	cat "$(console_path "$vm")" >>"$CONSOLE_LOG" 2>>"$LOG" &
	CONSOLE_READER_PID=$!
}

stop_console_reader()
{
	[ -n "$CONSOLE_READER_PID" ] || return 0
	kill "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
	wait "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
	CONSOLE_READER_PID=
}

wait_console_pattern()
{
	pattern=$1
	label=$2
	wait_i=0

	while [ "$wait_i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$CONSOLE_LOG" && return 0
		sleep 1
		wait_i=$((wait_i + 1))
	done
	append_file "$label" "$CONSOLE_LOG"
	return 1
}

write_console()
{
	vm=$1
	line=$2

	printf '%s\n' "$line" >"$(console_path "$vm")" ||
	    fail "console write failed for $vm"
}

force_stop()
{
	vm=$1
	stop_i=0

	echo force >"$(machine_dir "$vm")/stopped" 2>>"$LOG" ||
	    fail "force stop request failed for $vm"
	while [ "$stop_i" -lt "$STOP_TIMEOUT" ]; do
		if [ -e "$(machine_dir "$vm")/stopped" ] &&
		    cat "$(events_path "$vm")" 2>>"$LOG" |
		    grep -q 'state stopped reason=force'; then
			return 0
		fi
		sleep 1
		stop_i=$((stop_i + 1))
	done
	fail "force stop did not complete for $vm"
}

remove_machine()
{
	vm=$1
	remove_i=0

	[ -d "$(machine_dir "$vm")" ] || return 0
	while [ "$remove_i" -lt "$STOP_TIMEOUT" ]; do
		rmdir "$(machine_dir "$vm")" >>"$LOG" 2>&1 && return 0
		sleep 1
		remove_i=$((remove_i + 1))
	done
	return 1
}

unmount_vmmfs()
{
	unmount_i=0

	[ "$MOUNTED" -eq 1 ] || return 0
	while [ "$unmount_i" -lt "$STOP_TIMEOUT" ]; do
		umount "$MNT" >>"$LOG" 2>&1 && {
			MOUNTED=0
			return 0
		}
		sleep 1
		unmount_i=$((unmount_i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	stop_console_reader
	if [ "$MOUNTED" -eq 1 ]; then
		for vm in "$VM_OFF" "$VM_ON"; do
			[ -d "$(machine_dir "$vm")" ] || continue
			if [ ! -e "$(machine_dir "$vm")/stopped" ]; then
				echo force >"$(machine_dir "$vm")/stopped" 2>>"$LOG"
			fi
			remove_machine "$vm" || say "machine cleanup did not finish: $vm"
		done
		unmount_vmmfs || say "vmmfs unmount did not finish"
	fi
	if [ "$LOADED" -eq 1 ] && [ -n "$OLD_SVM_TRACE" ]; then
		sysctl debug.vmm.svm_trace="$OLD_SVM_TRACE" >>"$LOG" 2>&1
		OLD_SVM_TRACE=
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1 || say "kldunload vmm failed"
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER"
}

preflight()
{
	: >"$LOG" || exit 1
	say "Linux SVM trace true-hardware test"
	say "repo=$REPO vmm_ko=$VMM_KO kernel=$KERNEL initrd=$INITRD"
	say "trace-off=$VM_OFF trace-on=$VM_ON mem=$MEM"
	[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
	case "$VMM_KO" in
	/*) ;;
	*) fail "VMM_KO must be an absolute path" ;;
	esac
	case "$MOUNT_HELPER" in
	*_vmm) ;;
	*) fail "VMM_MOUNT_HELPER path must end in _vmm for mount_std" ;;
	esac
	[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
	[ -f "$KERNEL" ] || fail "missing $KERNEL"
	[ -f "$INITRD" ] ||
	    fail "missing $INITRD; run linux_initrd_rootfs_build.sh first"
	check_module_image
	kldstat -n vmm >/dev/null 2>&1 &&
	    fail "vmm already loaded; unload it before running this harness"
}

build_loader_wrapper()
{
	run cc -Wall -Wextra -Werror -std=c11 -O2 \
		"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
	{
		printf '%s\n' '#!/bin/sh'
		printf 'exec "%s" "%s" "initramfs=%s" ' \
		    "$LOADER" "$KERNEL" "$INITRD"
		printf '%s\n' '"tsc_hz=host" "console=ttyS0,115200" "earlycon=uart,io,0x3f8,115200" "loglevel=7" "rdinit=/init"'
	} >"$WRAPPER" || fail "write $WRAPPER"
	chmod +x "$WRAPPER" || fail "chmod $WRAPPER"
}

start_machine()
{
	vm=$1

	run mkdir "$(machine_dir "$vm")"
	printf '%s\n' 1 >"$(machine_dir "$vm")/vcpu" || fail "vcpu config $vm"
	printf '%s\n' "$MEM" >"$(machine_dir "$vm")/mem" || fail "mem config $vm"
	printf '%s\n' "$WRAPPER" >"$(machine_dir "$vm")/loader" ||
	    fail "loader config $vm"
	start_console_reader "$vm"
	run rm "$(machine_dir "$vm")/stopped"
	wait_console_pattern 'DFVMM_LINUX_SERIAL_OK' "$vm console" ||
	    fail "$vm serial marker missing"
}

finish_machine()
{
	vm=$1

	force_stop "$vm"
	stop_console_reader
	remove_machine "$vm" || fail "rmdir $vm failed"
}

trap cleanup EXIT INT TERM
preflight
build_loader_wrapper
run kldload "$VMM_KO"
LOADED=1
OLD_SVM_TRACE=$(sysctl -n debug.vmm.svm_trace 2>>"$LOG") ||
	fail "read debug.vmm.svm_trace"
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1

run sysctl debug.vmm.svm_trace=0
start_machine "$VM_OFF"
sleep 1
write_console "$VM_OFF" 'echo DFVMM_TRACE_OFF_READY'
wait_console_pattern 'DFVMM_TRACE_OFF_READY' "$VM_OFF shell" ||
    fail "$VM_OFF shell marker missing"
if grep -q 'svm vcpu0 tsc deadline ' "$(events_path "$VM_OFF")"; then
	append_file trace-off-events "$(events_path "$VM_OFF")"
	fail "trace-off TSC deadline trace is present"
fi
finish_machine "$VM_OFF"

start_machine "$VM_ON"
run sysctl debug.vmm.svm_trace=1
sleep 1
if ! grep -q 'svm vcpu0 tsc deadline ' "$(events_path "$VM_ON")"; then
	append_file trace-on-events "$(events_path "$VM_ON")"
	fail "trace-on TSC deadline trace missing"
fi
run sysctl debug.vmm.svm_trace=0
write_console "$VM_ON" 'echo DFVMM_TRACE_ON_READY'
wait_console_pattern 'DFVMM_TRACE_ON_READY' "$VM_ON shell" ||
    fail "$VM_ON shell marker missing"
finish_machine "$VM_ON"

unmount_vmmfs || fail "umount $MNT"
run sysctl debug.vmm.svm_trace="$OLD_SVM_TRACE"
OLD_SVM_TRACE=
run kldunload vmm
LOADED=0
rm -f "$MOUNT_HELPER" "$WRAPPER"

say "PASS: Linux SVM trace test"
