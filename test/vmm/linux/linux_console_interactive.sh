#!/bin/sh
# pc64 manual Linux serial-console harness.
#
# This starts one in-memory Linux guest and attaches cu to
# machines/<name>/console.  Type '~.' at the beginning of a line to exit; the
# script then stops and removes the guest.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)

VMM_KO=${VMM_KO:-$REPO/sys/dev/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-linux-console-live-vmm}
VM=${VMM_MACHINE:-linuxlive0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-linux-console-interactive.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-linux-console-interactive-boot.log}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_console_interactive}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-linux-console-live-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-45}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}
CONNECTOR=${VMM_CONNECTOR:-cu}

LOADED=0
MOUNTED=0
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

mach()
{
	printf '%s/%s\n' "$MNT" "$VM"
}

console_path()
{
	printf '%s/console\n' "$(mach)"
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
			cat "$(mach)/events" 2>&1 || true
		fi
		printf '%s\n' '--- boot console ---'
		cat "$CONSOLE_LOG" 2>&1 || true
		printf '%s\n' '--- end state ---'
	} >>"$LOG"
}

start_console_reader()
{
	: >"$CONSOLE_LOG" || fail "create console log"
	cat "$(console_path)" >>"$CONSOLE_LOG" 2>>"$LOG" &
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

ensure_module_image()
{
	if [ -f "$VMM_KO" ]; then
		return
	fi
	if [ "$VMM_KO" != "$REPO/sys/dev/vmm/vmm.ko" ]; then
		fail "missing $VMM_KO"
	fi
	say "+ make -C $REPO/sys/dev/vmm MACHINE_PLATFORM=pc64"
	( cd "$REPO/sys/dev/vmm" && make MACHINE_PLATFORM=pc64 ) >>"$LOG" 2>&1 ||
	    fail "build $VMM_KO"
	[ -f "$VMM_KO" ] || fail "missing $VMM_KO after build"
}

check_module_image()
{
	sections=$(readelf -SW "$VMM_KO" 2>>"$LOG") ||
	    fail "readelf failed for $VMM_KO"
	printf '%s\n' "$sections" | grep -qi eh_frame &&
	    fail "$VMM_KO contains .eh_frame"
}

attach_console()
{
	console=$(console_path)

	say "Linux guest is running."
	say "console: $console"
	say "log: $LOG"
	say "cu escape: type '~.' at the beginning of a line to exit."
	case "$CONNECTOR" in
	cu)
		cu -s 115200 -l "$console"
		;;
	stdio)
		"$REPO/test/vmm/linux/linux_console_attach.py" "$console"
		;;
	none)
		printf '%s\n' "Press Enter to stop $VM and clean up."
		read _answer
		;;
	*)
		"$CONNECTOR" "$console"
		;;
	esac
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
ensure_module_image
[ -f "$KERNEL" ] || fail "missing $KERNEL"
[ -f "$INITRD" ] || fail "missing $INITRD"
case "$CONNECTOR" in
cu|stdio)
	[ -t 0 ] || fail "interactive terminal required"
	;;
esac
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
check_module_image

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
cat >"$WRAPPER" <<EOF_WRAP
#!/bin/sh
exec "$LOADER" "$KERNEL" "initramfs=$INITRD" "console=ttyS0,115200" "loglevel=3" "rdinit=/init"
EOF_WRAP
chmod +x "$WRAPPER" || fail "chmod $WRAPPER"

run kldload "$VMM_KO"
LOADED=1
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1

run mkdir "$(mach)"
printf '1\n' >"$(mach)/vcpu" || fail "write vcpu"
printf '%s\n' "$MEM" >"$(mach)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(mach)/loader" || fail "write loader"
[ -c "$(console_path)" ] || fail "$(console_path) is not a character device"

start_console_reader
run rm "$(mach)/stopped"

wait_console_pattern 'DFVMM_LINUX_INITRD_ROOTFS_OK' initrd ||
    fail "initrd rootfs marker not observed"
wait_console_pattern 'DFVMM_LINUX_SERIAL_OK' serial ||
    fail "serial marker not observed"
stop_console_reader

run stty -f "$(console_path)" raw -echo cs8 -parenb -cstopb 115200
attach_console
say "console session ended"

exit 0
