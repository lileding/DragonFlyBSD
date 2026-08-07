#!/bin/sh
set -u

REPO=${REPO:-/home/lileding/projects/dfly-vmm/DragonFlyBSD}
VMM_KO=${VMM_KO:-$REPO/sys/dev/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-smp-vmm}
VM=${VMM_MACHINE:-linuxsmp0}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_smp}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-smp-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-smp-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-linux-smp-console.log}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-30}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}
SVM_TRACE=${VMM_SVM_TRACE:-0}
GUEST_SHUTDOWN=${VMM_GUEST_SHUTDOWN:-1}

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

mach()
{
	printf '%s/%s\n' "$MNT" "$VM"
}

dump_state()
{
	{
		printf '%s\n' '--- kldstat ---'
		kldstat -n vmm 2>&1 || true
		printf '%s\n' '--- mount ---'
		mount 2>&1 || true
		if [ -d "$(mach)" ]; then
			printf '%s\n' '--- events ---'
			cat "$(mach)/events" 2>&1
			printf '%s\n' '--- console ---'
			cat "$CONSOLE_LOG" 2>&1
		fi
		printf '%s\n' '--- end state ---'
	} >>"$LOG"
}

start_console_reader()
{
	: >"$CONSOLE_LOG" || fail "create console log"
	cat "$(mach)/console" >>"$CONSOLE_LOG" 2>>"$LOG" &
	CONSOLE_READER_PID=$!
}

stop_console_reader()
{
	if [ -n "$CONSOLE_READER_PID" ]; then
		kill "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
		wait "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
		CONSOLE_READER_PID=
	fi
}

wait_console_pattern()
{
	pattern=$1
	label=$2
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$CONSOLE_LOG" && return 0
		sleep 1
		i=$((i + 1))
	done
	{
		printf '%s\n' "--- final console $label ---"
		cat "$CONSOLE_LOG" 2>&1
		printf '%s\n' "--- end final console $label ---"
	} >>"$LOG"
	return 1
}

event_seq()
{
	cat "$(mach)/events" 2>>"$LOG" | awk 'END { print $1 }'
}

wait_reset_sequence()
{
	after=$1
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		if cat "$(mach)/events" 2>>"$LOG" |
		    awk -v after="$after" '
			$1 > after {
				if ($0 ~ /reset requested/)
					requested = 1
				if (requested && $0 ~ /state draining reason=stop/)
					draining = 1
				if (draining && $0 ~ /state stopped reason=stop/)
					stopped = 1
				if (requested && (!draining || stopped) &&
				    $0 ~ /state starting/)
					starting = 1
				if (starting && $0 ~ /state running/)
					running = 1
			}
			END { exit running ? 0 : 1 }
		'; then
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	return 1
}

wait_smp_online()
{
	marker=$1
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		printf 'echo %s $(cat /sys/devices/system/cpu/online)\n' "$marker" \
		    >"$(mach)/console" || fail "write SMP online probe"
		sleep 1
		grep -q "$marker 0-1" "$CONSOLE_LOG" && return 0
		i=$((i + 1))
	done
	fail "Linux did not bring CPU1 online"
}

reset_machine()
{
	marker=$1
	before=$(event_seq)

	[ -n "$before" ] || fail "cannot read event sequence before reset"
	printf '%s\n' reset >"$(mach)/events" || fail "write reset command"
	wait_reset_sequence "$before" || fail "reset event sequence incomplete"
	wait_smp_online "$marker"
}

force_stop()
{
	i=0

	touch "$(mach)/stopped" || fail "write stop command"
	while [ "$i" -lt "$STOP_TIMEOUT" ]; do
		if [ -e "$(mach)/stopped" ] && cat "$(mach)/events" 2>>"$LOG" |
		    grep -q 'state stopped reason=stop'; then
			return 0
		fi
		sleep 1
		i=$((i + 1))
	done
	fail "external force stop did not complete"
}

cleanup()
{
	set +e
	stop_console_reader
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach)" ]; then
		say "cleanup: stop $VM"
		touch "$(mach)/stopped" 2>>"$LOG"
		i=0
		while [ "$i" -lt "$STOP_TIMEOUT" ] && [ -d "$(mach)" ]; do
			rmdir "$(mach)" >>"$LOG" 2>&1 && break
			sleep 1
			i=$((i + 1))
		done
	fi
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
	if [ "$LOADED" -eq 1 ] && [ -n "$OLD_SVM_TRACE" ]; then
		sysctl debug.vmm.svm_trace="$OLD_SVM_TRACE" >>"$LOG" 2>&1
		OLD_SVM_TRACE=
	fi
	if [ "$LOADED" -eq 1 ] && [ "$MOUNTED" -eq 0 ]; then
		i=0
		while [ "$i" -lt "$STOP_TIMEOUT" ]; do
			kldunload vmm >>"$LOG" 2>&1 && break
			sleep 1
			i=$((i + 1))
		done
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
[ -f "$KERNEL" ] || fail "missing $KERNEL"
[ -f "$INITRD" ] || fail "missing $INITRD"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
case "$SVM_TRACE" in
0|1)
	;;
*)
	fail "VMM_SVM_TRACE must be 0 or 1"
	;;
esac

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
cat >"$WRAPPER" <<EOF_WRAP
#!/bin/sh
exec "$LOADER" "$KERNEL" "initramfs=$INITRD" "vcpu=2" "console=ttyS0,115200" "earlycon=uart,io,0x3f8,115200" "loglevel=7" "rdinit=/init"
EOF_WRAP
chmod +x "$WRAPPER" || fail "chmod $WRAPPER"

run kldload "$VMM_KO"
LOADED=1
if [ "$SVM_TRACE" -eq 1 ]; then
	OLD_SVM_TRACE=$(sysctl -n debug.vmm.svm_trace 2>>"$LOG") ||
		fail "read debug.vmm.svm_trace"
	run sysctl debug.vmm.svm_trace=1
fi
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1

run mkdir "$(mach)"
printf '2\n' >"$(mach)/vcpu" || fail "write vcpu"
printf '%s\n' "$MEM" >"$(mach)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(mach)/loader" || fail "write loader"
start_console_reader
run rm "$(mach)/stopped"

wait_console_pattern 'DFVMM_LINUX_INITRD_ROOTFS_OK' initrd ||
	fail "initrd rootfs marker not observed"
wait_console_pattern 'DFVMM_LINUX_SERIAL_OK' serial ||
	fail "serial marker not observed"

wait_smp_online DFVMM_SMP_INITIAL

if grep -q 'Performance Events: Fam17h+ core perfctr' "$CONSOLE_LOG" ||
	grep -q 'NMI watchdog: Enabled' "$CONSOLE_LOG" ||
	grep -q 'invalid IBS interrupt offset' "$CONSOLE_LOG"; then
	fail "guest CPU template exposed host PMU capability"
fi

printf 'dfvmm-core-smoke\n' >"$(mach)/console" ||
	fail "write core smoke command"
wait_console_pattern 'DFVMM_CORE_SMOKE_END' core-smoke ||
	fail "core smoke marker not observed"

reset_machine DFVMM_SMP_RESET_RUNNING

if [ "$GUEST_SHUTDOWN" -eq 0 ]; then
	say "PASS: Linux two-vCPU SMP boot test"
	exit 0
fi

printf 'poweroff -f\n' >"$(mach)/console" ||
	fail "write guest poweroff command"
shutdown_events=
i=0
while [ "$i" -lt "$STOP_TIMEOUT" ]; do
	out=$(cat "$(mach)/events" 2>>"$LOG")
	if [ -n "$out" ]; then
		shutdown_events="$shutdown_events
$out"
		{
			printf '%s\n' '--- poll guest shutdown events ---'
			printf '%s\n' "$out"
			printf '%s\n' '--- end poll guest shutdown events ---'
		} >>"$LOG"
	fi
	if printf '%s\n' "$shutdown_events" | awk '
		/guest shutdown source=acpi_s5/ { shutdown = 1; next }
		shutdown && /state stopped reason=guest_shutdown/ { stopped = 1 }
		END { exit !stopped }
	'; then
		break
	fi
	sleep 1
	i=$((i + 1))
done
[ "$i" -lt "$STOP_TIMEOUT" ] || fail "guest S5 shutdown events not observed"
[ ! -e "$(mach)/stopped" ] || fail "guest S5 shutdown recreated stopped"

reset_machine DFVMM_SMP_RESET_STOPPED
force_stop

say "PASS: Linux two-vCPU SMP test"
