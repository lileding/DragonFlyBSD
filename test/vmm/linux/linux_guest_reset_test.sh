#!/bin/sh
#
# pc64 true-hardware guest-reset test.
#
# Linux reboots through the FADT Reset Register.  The reset must discard the
# current COW runtime, restart from the loader-complete boot snapshot, retain
# the console endpoint, and never execute the loader again.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-guest-reset-vmm}
VM=${VMM_MACHINE:-linuxguestreset0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-guest-reset-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-linux-guest-reset.console}
LOADER_RUN_LOG=${VMM_LOADER_RUN_LOG:-/var/tmp/dfvmm-linux-guest-reset.loader-runs}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_guest_reset}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-guest-reset-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-45}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-30}
VCPU_COUNT=${VMM_VCPU_COUNT:-1}

LOADED=0
MOUNTED=0
GUEST_REBOOT_COMMAND=${VMM_GUEST_REBOOT_COMMAND:-/bin/busybox reboot -f}
GUEST_SHUTDOWN_COMMAND=${VMM_GUEST_SHUTDOWN_COMMAND:-/bin/busybox poweroff -f}
CREATED=0
CONSOLE_READER_PID=

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
	printf '%s/%s\n' "$MNT" "$VM"
}

console_path()
{
	printf '%s/console\n' "$(machine_dir)"
}

events_path()
{
	printf '%s/events\n' "$(machine_dir)"
}

loader_runs()
{
	[ -f "$LOADER_RUN_LOG" ] || {
		printf '0\n'
		return
	}
	wc -l <"$LOADER_RUN_LOG" | tr -d ' '
}

console_matches()
{
	grep -c "$1" "$CONSOLE_LOG" 2>/dev/null || true
}

dump_state()
{
	{
		printf '%s\n' '--- kldstat ---'
		kldstat -n vmm 2>&1 || true
		printf '%s\n' '--- mount ---'
		mount 2>&1 || true
		if [ -d "$(machine_dir)" ]; then
			printf '%s\n' '--- events ---'
			cat "$(events_path)" 2>&1 || true
		fi
		printf '%s\n' '--- loader runs ---'
		cat "$LOADER_RUN_LOG" 2>&1 || true
		printf '%s\n' '--- console ---'
		cat "$CONSOLE_LOG" 2>&1 || true
		printf '%s\n' '--- end state ---'
	} >>"$LOG"
}

start_console_reader()
{
	: >"$CONSOLE_LOG" || fail "truncate console log"
	cat "$(console_path)" >>"$CONSOLE_LOG" 2>>"$LOG" &
	CONSOLE_READER_PID=$!
}

stop_console_reader()
{
	[ -n "$CONSOLE_READER_PID" ] || return 0
	kill "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
	wait "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
	CONSOLE_READER_PID=
}

wait_console_next()
{
	pattern=$1
	before=$2
	label=$3
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		count=$(console_matches "$pattern")
		[ "$count" -gt "$before" ] && return 0
		sleep 1
		i=$((i + 1))
	done
	say "console marker missing label=$label pattern=$pattern before=$before"
	return 1
}

wait_smp_online()
{
	marker=$1
	expected="0-$((VCPU_COUNT - 1))"
	i=0

	printf '%s\n' "echo $marker \$(cat /sys/devices/system/cpu/online)" \
	    >"$(console_path)" || return 1
	while [ "$i" -lt "$TIMEOUT" ]; do
		if grep -q "$marker $expected" "$CONSOLE_LOG" 2>/dev/null; then
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	return 1
}

wait_guest_reset()
{
	events=
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		out=$(cat "$(events_path)" 2>>"$LOG")
		[ -n "$out" ] && events="$events
$out"
		if printf '%s\n' "$events" | awk '
			/guest reset source=acpi_fadt/ { reset = 1; next }
			reset && /state draining reason=guest_reset/ { draining = 1; next }
			draining && /state starting reason=guest_reset/ { starting = 1; next }
			starting && /state running reason=guest_reset/ { running = 1 }
			END { exit running ? 0 : 1 }
		'; then
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	printf '%s\n' "$events" >>"$LOG"
	return 1
}

force_stop_remove()
{
	i=0

	[ "$MOUNTED" -eq 1 ] || return 0
	if [ -d "$(machine_dir)" ]; then
		touch "$(machine_dir)/stopped" || true
		while [ "$i" -lt "$STOP_TIMEOUT" ]; do
			rmdir "$(machine_dir)" >>"$LOG" 2>&1 && break
			sleep 1
			i=$((i + 1))
		done
	fi
}

cleanup()
{
	set +e
	stop_console_reader
	force_stop_remove
	if [ "$MOUNTED" -eq 1 ]; then
		i=0
		while [ "$i" -lt "$STOP_TIMEOUT" ]; do
			umount "$MNT" >>"$LOG" 2>&1 && {
				MOUNTED=0
				break
			}
			sleep 1
			i=$((i + 1))
		done
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		kldunload vmm >>"$LOG" 2>&1 || true
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER" "$LOADER_RUN_LOG"
}

ensure_module_image()
{
	[ -f "$VMM_KO" ] && return 0
	[ "$VMM_KO" = "$REPO/sys/vmm/vmm.ko" ] || fail "missing $VMM_KO"
	say "+ make -C $REPO/sys/vmm MACHINE_PLATFORM=pc64"
	( cd "$REPO/sys/vmm" && make MACHINE_PLATFORM=pc64 ) >>"$LOG" 2>&1 ||
		fail "build $VMM_KO"
}

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") || fail "readelf $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
		fail "$VMM_KO contains .eh_frame"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VCPU_COUNT" in
1|2|4) ;;
*) fail "VMM_VCPU_COUNT must be 1, 2, or 4" ;;
esac
ensure_module_image
check_module_image
[ -f "$KERNEL" ] || fail "missing $KERNEL"
[ -f "$INITRD" ] || fail "missing $INITRD; run linux_initrd_rootfs_build.sh"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
: >"$LOADER_RUN_LOG" || fail "create loader run log"
cat >"$WRAPPER" <<EOF_WRAP
#!/bin/sh
printf '%s\\n' loader >>'$LOADER_RUN_LOG'
exec '$LOADER' '$KERNEL' 'initramfs=$INITRD' 'vcpu=$VCPU_COUNT' 'reboot=a' 'console=ttyS0,115200' 'earlycon=uart,io,0x3f8,115200' 'loglevel=7' 'rdinit=/init'
EOF_WRAP
chmod +x "$WRAPPER" || fail "chmod $WRAPPER"

run kldload "$VMM_KO"
LOADED=1
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1
run mkdir "$(machine_dir)"
CREATED=1
printf '%s\n' "$VCPU_COUNT" >"$(machine_dir)/vcpu" || fail "write vcpu"
printf '%s\n' "$MEM" >"$(machine_dir)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(machine_dir)/loader" || fail "write loader"
start_console_reader
run rm "$(machine_dir)/stopped"

boot_before=$(console_matches 'DFVMM_LINUX_SERIAL_OK')
wait_console_next 'DFVMM_LINUX_SERIAL_OK' "$boot_before" initial-boot ||
	fail "initial Linux serial marker missing"
[ "$(loader_runs)" -eq 1 ] || fail "loader executed unexpected count before reset"
wait_smp_online DFVMM_GUEST_RESET_SMP_INITIAL ||
	fail "Linux did not bring all vCPUs online before reset"

reset_before=$(console_matches 'DFVMM_LINUX_SERIAL_OK')
printf '%s\n' "/bin/busybox touch /run/dfvmm-reset-cow; echo DFVMM_GUEST_RESET_PREPARED; $GUEST_REBOOT_COMMAND" >"$(console_path)" ||
	fail "request guest reset"
wait_guest_reset || fail "guest reset event sequence missing"
wait_console_next 'DFVMM_LINUX_SERIAL_OK' "$reset_before" guest-reset-boot ||
	fail "guest did not boot after reset"
kill -0 "$CONSOLE_READER_PID" >/dev/null 2>&1 ||
	fail "console reader exited across guest reset"
[ "$(loader_runs)" -eq 1 ] || fail "guest reset executed loader again"
wait_smp_online DFVMM_GUEST_RESET_SMP_AFTER_RESET ||
	fail "Linux did not bring all vCPUs online after reset"

cow_before=$(console_matches 'DFVMM_GUEST_RESET_COW_OK')
printf '%s\n' '[ ! -e /run/dfvmm-reset-cow ] && echo DFVMM_GUEST_RESET_COW_OK || echo DFVMM_GUEST_RESET_COW_BAD' >"$(console_path)" ||
	fail "probe reset COW state"
wait_console_next 'DFVMM_GUEST_RESET_COW_OK' "$cow_before" guest-reset-cow ||
	fail "guest reset did not restore boot memory"
[ ! -e "$(machine_dir)/stopped" ] || fail "guest reset recreated stopped"

printf '%s\n' "$GUEST_SHUTDOWN_COMMAND" >"$(console_path)" || fail "request guest shutdown"
i=0
shutdown_events=
while [ "$i" -lt "$STOP_TIMEOUT" ]; do
	out=$(cat "$(events_path)" 2>>"$LOG")
	[ -n "$out" ] && shutdown_events="$shutdown_events
$out"
	if printf '%s\n' "$shutdown_events" | awk '
		/guest shutdown source=acpi_s5/ { shutdown = 1; next }
		shutdown && /state stopped reason=guest_shutdown/ { stopped = 1 }
		END { exit stopped ? 0 : 1 }
	'; then
		break
	fi
	sleep 1
	i=$((i + 1))
done
[ "$i" -lt "$STOP_TIMEOUT" ] || fail "guest S5 shutdown missing after reset"
[ ! -e "$(machine_dir)/stopped" ] || fail "guest S5 recreated stopped"

say "PASS: Linux ${VCPU_COUNT}-vCPU guest reset COW test"
