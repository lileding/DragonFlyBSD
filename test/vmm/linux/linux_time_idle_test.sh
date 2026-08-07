#!/bin/sh
# pc64 true-hardware Linux time/idle correctness harness.
#
# This test keeps the rootfs in initrd memory and focuses on vmm core time and
# idle behavior: guest sleep must advance on the TSC-deadline clockevent, an
# idle guest must wake on serial input, and multiple Linux guests must clean up
# through stop, rmdir, umount, and kldunload.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/dev/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-time-vmm}
VM_PREFIX=${VMM_MACHINE_PREFIX:-linuxtime}
VM_COUNT=${VMM_LINUX_INSTANCES:-2}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-time-idle-test.log}
CONSOLE_LOG_DIR=${VMM_CONSOLE_LOG_DIR:-/var/tmp/dfvmm-linux-time-console}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_time_idle}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-time-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-45}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-30}

LOADED=0
MOUNTED=0
CREATED_MACHINES=
CONSOLE_READER_PIDS=
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

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}

mach()
{
	printf '%s/%s\n' "$MNT" "$1"
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

	printf '%s\n' "$line" >"$(mach "$vm")/console" ||
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
}

wait_guest_shell()
{
	vm=$1
	base="DFVMM_${vm}_TIME"
	marker="${base}_READY"
	probe_i=0

	while [ "$probe_i" -lt "$TIMEOUT" ]; do
		write_console "$vm" "echo $marker"
		sleep 1
		if grep -q "$marker" "$(console_log "$vm")"; then
			return 0
		fi
		probe_i=$((probe_i + 1))
	done
	fail "$vm shell readiness probe missing"
}

marker_value()
{
	file=$1
	marker=$2

	awk -v marker="$marker" '$1 == marker { value = $2; sub(/\r$/, "", value) } END { if (value != "") print value }' "$file"
}

check_uptime_progress()
{
	vm=$1
	base="DFVMM_${vm}_TIME"
	file=$(console_log "$vm")
	up0=$(marker_value "$file" "${base}_UP0")
	up1=$(marker_value "$file" "${base}_UP1")
	up2=$(marker_value "$file" "${base}_UP2")
	clockevent=$(marker_value "$file" "${base}_CLOCKEVENT")
	clocksource=$(marker_value "$file" "${base}_CLOCKSOURCE_FINAL")
	tsc_deadline=$(marker_value "$file" "${base}_TSC_DEADLINE")

	case "$tsc_deadline" in
	1)
		[ "$clockevent" = "lapic-deadline" ] ||
		    fail "$vm exposes tsc_deadline_timer but uses ${clockevent:-missing}"
		;;
	0)
		[ "$clockevent" = "lapic" ] ||
		    fail "$vm lacks tsc_deadline_timer but uses ${clockevent:-missing}"
		;;
	*)
		fail "$vm missing ${base}_TSC_DEADLINE"
		;;
	esac
	[ "$clocksource" = "tsc" ] ||
	    fail "$vm expected stable tsc clocksource, got ${clocksource:-missing}"
	grep -q "TSC doesn't count with P0 frequency" "$file" &&
	    fail "$vm Linux still reports TSC/P0 frequency mismatch"
	[ -n "$up0" ] || fail "$vm missing ${base}_UP0"
	[ -n "$up1" ] || fail "$vm missing ${base}_UP1"
	[ -n "$up2" ] || fail "$vm missing ${base}_UP2"
	awk -v a="$up0" -v b="$up1" -v c="$up2" '
	    BEGIN { if (b > a && c > b && c - a >= 2.0) exit 0; exit 1 }' ||
	    fail "$vm uptime did not advance across guest sleep: $up0 $up1 $up2"
}

run_guest_time_idle()
{
	vm=$1
	base="DFVMM_${vm}_TIME"

	write_console "$vm" \
	    "a=$base; td=0; grep -qw tsc_deadline_timer /proc/cpuinfo && td=1; read ce < /sys/devices/system/clockevents/clockevent0/current_device 2>/dev/null || ce=missing; read cs < /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || cs=missing; echo \${a}_TSC_DEADLINE \$td; echo \${a}_CLOCKEVENT \$ce; echo \${a}_CLOCKSOURCE \$cs; echo \${a}_BEGIN; read u _ < /proc/uptime; echo \${a}_UP0 \$u; sleep 1; read u _ < /proc/uptime; echo \${a}_UP1 \$u; sleep 3; read u _ < /proc/uptime; echo \${a}_UP2 \$u; read cs < /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || cs=missing; echo \${a}_CLOCKSOURCE_FINAL \$cs; echo \${a}_INTERRUPTS_BEGIN; cat /proc/interrupts; echo \${a}_INTERRUPTS_END; echo \${a}_END"
	wait_console_pattern "$vm" "${base}_END" "$vm console" ||
	    fail "$vm time marker missing"
	check_uptime_progress "$vm"
	grep -q 'ttyS0' "$(console_log "$vm")" ||
	    fail "$vm /proc/interrupts does not mention ttyS0"

	sleep 3
	write_console "$vm" "echo ${base}_INPUT_WAKE_OK"
	wait_console_pattern "$vm" "${base}_INPUT_WAKE_OK" "$vm console" ||
	    fail "$vm console input did not wake idle shell"
	append_file "$vm-events-after-time" "$(mach "$vm")/events"
}

stop_machine()
{
	vm=$1
	stop_i=0

	say "stop $vm"
	touch "$(mach "$vm")/stopped" 2>>"$LOG" ||
	    fail "$vm stop request failed"
	wait_file_pattern "$(mach "$vm")/events" 'state stopped reason=stop' \
	    "$vm events" || fail "$vm stopped event missing"
	while [ "$stop_i" -lt "$STOP_TIMEOUT" ]; do
		[ -e "$(mach "$vm")/stopped" ] && return 0
		sleep 1
		stop_i=$((stop_i + 1))
	done
	fail "$vm stopped control file did not reappear after stop"
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
	if [ -n "$OLD_SVM_TRACE" ]; then
		sysctl debug.vmm.svm_trace="$OLD_SVM_TRACE" >>"$LOG" 2>&1
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		for vm in $CREATED_MACHINES; do
			if [ -d "$(mach "$vm")" ]; then
				touch "$(mach "$vm")/stopped" 2>>"$LOG"
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
	say "Linux time/idle true-hardware test"
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

trap cleanup EXIT INT TERM
preflight
build_loader_wrapper
run kldload "$VMM_KO"
LOADED=1
OLD_SVM_TRACE=$(sysctl -n debug.vmm.svm_trace 2>>"$LOG" || true)
run sysctl debug.vmm.svm_trace=1
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

main_i=0
while [ "$main_i" -lt "$VM_COUNT" ]; do
	vm=$(machine_name "$main_i")
	start_console_reader "$vm"
	say "start $vm"
	run rm "$(mach "$vm")/stopped"
	main_i=$((main_i + 1))
done

main_i=0
while [ "$main_i" -lt "$VM_COUNT" ]; do
	vm=$(machine_name "$main_i")
	wait_console_pattern "$vm" 'DFVMM_LINUX_SERIAL_OK' "$vm console" ||
	    fail "$vm serial console marker missing"
	run stty -f "$(mach "$vm")/console" raw -echo cs8 -parenb -cstopb 115200
	wait_guest_shell "$vm"
	main_i=$((main_i + 1))
done

main_i=0
while [ "$main_i" -lt "$VM_COUNT" ]; do
	run_guest_time_idle "$(machine_name "$main_i")"
	main_i=$((main_i + 1))
done

main_i=0
while [ "$main_i" -lt "$VM_COUNT" ]; do
	stop_machine "$(machine_name "$main_i")"
	main_i=$((main_i + 1))
done

stop_console_readers

main_i=0
while [ "$main_i" -lt "$VM_COUNT" ]; do
	vm=$(machine_name "$main_i")
	remove_machine "$vm" || fail "$vm rmdir failed"
	main_i=$((main_i + 1))
done

unmount_vmmfs || fail "umount $MNT"
[ -n "$OLD_SVM_TRACE" ] && run sysctl debug.vmm.svm_trace="$OLD_SVM_TRACE"
OLD_SVM_TRACE=
run kldunload vmm
LOADED=0
rm -f "$MOUNT_HELPER" "$WRAPPER"

say "PASS: Linux time/idle test"
