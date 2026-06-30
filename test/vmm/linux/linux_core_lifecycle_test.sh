#!/bin/sh
# pc64 true-hardware Linux core lifecycle harness.
#
# This test intentionally uses the in-memory initrd rootfs.  It validates the
# VMM core before vPCIe exists: multiple one-vCPU Linux guests can boot at the
# same time, run shell commands through the serial console, idle long enough for
# guest timers to advance, stop cleanly, and start again on the same machine.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-core-vmm}
VM_PREFIX=${VMM_MACHINE_PREFIX:-linuxcore}
VM_COUNT=${VMM_LINUX_INSTANCES:-2}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-core-lifecycle-test.log}
CONSOLE_LOG_DIR=${VMM_CONSOLE_LOG_DIR:-/var/tmp/dfvmm-linux-core-console}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_core_lifecycle}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-core-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-40}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-30}
IDLE_SECONDS=${VMM_LINUX_IDLE_SECONDS:-5}

LOADED=0
MOUNTED=0
CREATED_MACHINES=
CONSOLE_READER_PIDS=

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

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}

mach()
{
	printf '%s/machines/%s\n' "$MNT" "$1"
}

machine_name()
{
	printf '%s%d\n' "$VM_PREFIX" "$1"
}

console_log()
{
	printf '%s/%s.console\n' "$CONSOLE_LOG_DIR" "$1"
}

append_file()
{
	label=$1
	file=$2

	if [ -e "$file" ]; then
		{
			printf -- '--- %s: %s ---\n' "$label" "$file"
			cat "$file" 2>&1
			printf -- '--- end %s ---\n' "$label"
		} >>"$LOG"
	else
		printf -- '--- %s: %s missing ---\n' "$label" "$file" >>"$LOG"
	fi
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
	if [ "$MOUNTED" -eq 1 ]; then
		for vm in $CREATED_MACHINES; do
			if [ -d "$(mach "$vm")" ]; then
				append_file "$vm-events" "$(mach "$vm")/events"
				append_file "$vm-console" "$(console_log "$vm")"
			fi
		done
	fi
}

start_console_reader()
{
	vm=$1

	mkdir -p "$CONSOLE_LOG_DIR" || fail "mkdir $CONSOLE_LOG_DIR"
	: >"$(console_log "$vm")" || fail "$vm console log"
	cat "$(mach "$vm")/console" >>"$(console_log "$vm")" 2>>"$LOG" &
	CONSOLE_READER_PIDS="$CONSOLE_READER_PIDS $!"
}

stop_console_readers()
{
	for pid in $CONSOLE_READER_PIDS; do
		kill "$pid" >/dev/null 2>&1 || true
	done
	for pid in $CONSOLE_READER_PIDS; do
		wait "$pid" >/dev/null 2>&1 || true
	done
	CONSOLE_READER_PIDS=
}

wait_file_pattern()
{
	file=$1
	pattern=$2
	label=$3
	wait_i=0
	wait_out=

	while [ "$wait_i" -lt "$TIMEOUT" ]; do
		wait_out=$(cat "$file" 2>>"$LOG")
		if printf '%s\n' "$wait_out" | grep -q "$pattern"; then
			return 0
		fi
		sleep 1
		wait_i=$((wait_i + 1))
	done
	{
		printf -- '--- final %s: %s ---\n' "$label" "$file"
		printf '%s\n' "$wait_out"
		printf -- '--- end final %s ---\n' "$label"
	} >>"$LOG"
	return 1
}

wait_console_pattern()
{
	vm=$1
	pattern=$2
	label=$3
	wait_i=0

	while [ "$wait_i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$(console_log "$vm")" && return 0
		sleep 1
		wait_i=$((wait_i + 1))
	done
	{
		printf -- '--- final %s: %s ---\n' "$label" "$(console_log "$vm")"
		cat "$(console_log "$vm")" 2>&1
		printf -- '--- end final %s ---\n' "$label"
	} >>"$LOG"
	return 1
}

write_console()
{
	vm=$1
	line=$2

	printf '\033[1;1R%s\n' "$line" >"$(mach "$vm")/console" ||
	    fail "$vm console write failed"
}

create_machine()
{
	vm=$1

	run mkdir "$(mach "$vm")"
	CREATED_MACHINES="$CREATED_MACHINES $vm"
	printf '1\n' >"$(mach "$vm")/vcpu" || fail "$vm vcpu"
	printf '%s\n' "$MEM" >"$(mach "$vm")/mem" || fail "$vm mem"
	printf '%s\n' "$WRAPPER" >"$(mach "$vm")/loader" || fail "$vm loader"
	append_file "$vm-created-events" "$(mach "$vm")/events"
	start_console_reader "$vm"
}

run_guest_smoke()
{
	vm=$1
	round=$2
	base="DFVMM_${vm}_ROUND_${round}"
	marker="${base}_SMOKE_END"

	write_console "$vm" \
	    "a=$base; echo \${a}_SMOKE_BEGIN; dfvmm-core-smoke; echo \${a}_SMOKE_END"
	wait_console_pattern "$vm" "$marker" "$vm console" ||
	    fail "$vm smoke marker missing in round $round"
}

wait_guest_shell()
{
	vm=$1
	round=$2
	base="DFVMM_${vm}_ROUND_${round}"
	marker="${base}_READY"
	probe_i=0

	while [ "$probe_i" -lt "$TIMEOUT" ]; do
		write_console "$vm" "a=$base; echo \${a}_READY"
		sleep 1
		if grep -q "$marker" "$(console_log "$vm")"; then
			return 0
		fi
		probe_i=$((probe_i + 1))
	done
	fail "$vm shell readiness probe missing in round $round"
}

run_guest_idle()
{
	vm=$1
	round=$2
	base="DFVMM_${vm}_ROUND_${round}"
	marker="${base}_IDLE_END"

	write_console "$vm" \
	    "a=$base; echo \${a}_IDLE_BEGIN; sleep $IDLE_SECONDS; cat /proc/uptime; echo \${a}_IDLE_END"
	wait_console_pattern "$vm" "$marker" "$vm console" ||
	    fail "$vm idle marker missing in round $round"
}

stop_machine()
{
	vm=$1
	reason=$2
	stop_i=0

	say "force stop $vm reason=$reason"
	echo force >"$(mach "$vm")/stopped" 2>>"$LOG" ||
	    fail "$vm force stop request failed"
	wait_file_pattern "$(mach "$vm")/events" 'state stopped reason=force' \
	    "$vm events" || fail "$vm force stopped event missing"
	while [ "$stop_i" -lt "$STOP_TIMEOUT" ]; do
		[ -e "$(mach "$vm")/stopped" ] && return 0
		sleep 1
		stop_i=$((stop_i + 1))
	done
	fail "$vm stopped control file did not reappear after force stop"
}

remove_machine()
{
	vm=$1
	remove_i=0

	[ -d "$(mach "$vm")" ] || return 0
	while [ "$remove_i" -lt "$STOP_TIMEOUT" ]; do
		rmdir "$(mach "$vm")" >>"$LOG" 2>&1 && return 0
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
	stop_console_readers
	if [ "$MOUNTED" -eq 1 ]; then
		for vm in $CREATED_MACHINES; do
			if [ -d "$(mach "$vm")" ]; then
				echo force >"$(mach "$vm")/stopped" 2>>"$LOG"
				remove_machine "$vm" || say "$vm cleanup did not finish"
			fi
		done
		unmount_vmmfs || say "vmmfs unmount did not finish"
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1 || say "kldunload vmm failed"
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER"
	rm -rf "$CONSOLE_LOG_DIR"
}

preflight()
{
	: >"$LOG" || exit 1
	say "Linux core lifecycle true-hardware test"
	say "repo=$REPO vmm_ko=$VMM_KO"
	say "kernel=$KERNEL initrd=$INITRD mem=$MEM instances=$VM_COUNT"
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
	[ "$VM_COUNT" -gt 0 ] || fail "VMM_LINUX_INSTANCES must be positive"
	check_module_image
	kldstat -n vmm >/dev/null 2>&1 &&
	    fail "vmm already loaded; unload it before running this harness"
}

build_loader_wrapper()
{
	run cc -Wall -Wextra -Werror -std=c11 -O2 \
		"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
	cat >"$WRAPPER" <<EOF_WRAP
#!/bin/sh
exec "$LOADER" "$KERNEL" "initramfs=$INITRD" "console=ttyS0,115200" "earlycon=uart,io,0x3f8,115200" "loglevel=7" "rdinit=/init"
EOF_WRAP
	chmod +x "$WRAPPER" || fail "chmod $WRAPPER"
}

start_round()
{
	round=$1
	start_i=0

	while [ "$start_i" -lt "$VM_COUNT" ]; do
		vm=$(machine_name "$start_i")
		say "start $vm round=$round"
		run rm "$(mach "$vm")/stopped"
		start_i=$((start_i + 1))
	done
	start_i=0
	while [ "$start_i" -lt "$VM_COUNT" ]; do
		vm=$(machine_name "$start_i")
		wait_console_pattern "$vm" 'DFVMM_LINUX_SERIAL_OK' \
		    "$vm console" || fail "$vm serial console marker missing in round $round"
		wait_guest_shell "$vm" "$round"
		start_i=$((start_i + 1))
	done
	start_i=0
	while [ "$start_i" -lt "$VM_COUNT" ]; do
		run_guest_smoke "$(machine_name "$start_i")" "$round"
		start_i=$((start_i + 1))
	done
	start_i=0
	while [ "$start_i" -lt "$VM_COUNT" ]; do
		run_guest_idle "$(machine_name "$start_i")" "$round"
		start_i=$((start_i + 1))
	done
}

stop_round()
{
	round=$1
	stop_round_i=0

	while [ "$stop_round_i" -lt "$VM_COUNT" ]; do
		stop_machine "$(machine_name "$stop_round_i")" "round-$round"
		stop_round_i=$((stop_round_i + 1))
	done
}

remove_all_machines()
{
	remove_all_i=0

	while [ "$remove_all_i" -lt "$VM_COUNT" ]; do
		vm=$(machine_name "$remove_all_i")
		remove_machine "$vm" || fail "$vm rmdir failed"
		remove_all_i=$((remove_all_i + 1))
	done
}

trap cleanup EXIT INT TERM
preflight
build_loader_wrapper
run kldload "$VMM_KO"
LOADED=1
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1

main_i=0
while [ "$main_i" -lt "$VM_COUNT" ]; do
	create_machine "$(machine_name "$main_i")"
	main_i=$((main_i + 1))
done

start_round 1
stop_round 1
start_round 2
stop_round 2
remove_all_machines
unmount_vmmfs || fail "umount $MNT"
run kldunload vmm
LOADED=0
rm -f "$MOUNT_HELPER" "$WRAPPER"

say "PASS: Linux core lifecycle test"
