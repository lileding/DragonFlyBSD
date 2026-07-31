#!/bin/sh
# pc64 Linux MCFG/ECAM provider-enumeration harness.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-pcie-ecam-vmm}
VM=${VMM_MACHINE:-pcieecam0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-pcie-ecam-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-pcie-ecam-console.log}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_pcie_ecam}
PROVIDER=${VMM_PCIE_ECAM_PROVIDER:-/var/tmp/vmm_pcie_ecam_provider}
PROVIDER_LOG=${VMM_PCIE_ECAM_PROVIDER_LOG:-/var/tmp/dfvmm-pcie-ecam-provider.log}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-pcie-ecam-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/alpine-vmlinuz-virt}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-45}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}

LOADED=0
MOUNTED=0
CONSOLE_READER_PID=
PROVIDER_PID=

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
	printf '%s/machines/%s\n' "$MNT" "$VM"
}

device()
{
	printf '%s/devices/ecam0\n' "$(mach)"
}

host_device()
{
	printf '%s/machines/host/devices/ecam0\n' "$MNT"
}

dump_state()
{
	{
		printf '%s\n' '--- kldstat ---'
		kldstat -n vmm 2>&1 || true
		printf '%s\n' '--- events ---'
		[ -d "$(mach)" ] && cat "$(mach)/events" 2>&1 || true
		printf '%s\n' '--- console ---'
		cat "$CONSOLE_LOG" 2>&1 || true
		printf '%s\n' '--- provider ---'
		cat "$PROVIDER_LOG" 2>&1 || true
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

stop_provider()
{
	if [ -n "$PROVIDER_PID" ]; then
		kill "$PROVIDER_PID" >/dev/null 2>&1 || true
		wait "$PROVIDER_PID" >/dev/null 2>&1 || true
		PROVIDER_PID=
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
	say "missing console marker: $label"
	return 1
}

wait_device_state()
{
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		state=$(cat "$(device)/state" 2>/dev/null) || state=
		case "$state" in
		'provider=registered
consumer=root')
			return 0
			;;
		esac
		sleep 1
		i=$((i + 1))
	done
	return 1
}

remove_path()
{
	path=$1
	i=0

	while [ "$i" -lt "$STOP_TIMEOUT" ]; do
		rmdir "$path" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	stop_console_reader
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach)" ]; then
		echo force >"$(mach)/stopped" 2>>"$LOG"
	fi
	stop_provider
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(device)" ]; then
		remove_path "$(device)"
	fi
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(host_device)" ]; then
		remove_path "$(host_device)"
	fi
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(mach)" ]; then
		remove_path "$(mach)"
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
		kldunload vmm >>"$LOG" 2>&1
	fi
	rm -f "$MOUNT_HELPER" "$WRAPPER" "$LOADER" "$PROVIDER"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be absolute" ;; esac
[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
[ -f "$KERNEL" ] || fail "missing $KERNEL"
[ -f "$INITRD" ] || fail "missing $INITRD"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
readelf -SW "$VMM_KO" 2>>"$LOG" | grep -qi eh_frame &&
	fail "$VMM_KO contains .eh_frame"

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
run cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys/vmm" \
	"$REPO/sys/vmm/vmm_pcie_abi.c" \
	"$REPO/test/vmm/pcie/vmm_pcie_ecam_provider.c" -o "$PROVIDER"
cat >"$WRAPPER" <<EOF_WRAP
#!/bin/sh
exec "$LOADER" "$KERNEL" "initramfs=$INITRD" "console=ttyS0,115200" "loglevel=7" "rdinit=/init"
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
run mkdir "$(host_device)"
run mv "$(host_device)" "$(device)"
: >"$PROVIDER_LOG" || fail "create provider log"
	"$PROVIDER" "$(device)" >>"$PROVIDER_LOG" 2>>"$LOG" &
	PROVIDER_PID=$!
	i=0
	while [ "$i" -lt "$TIMEOUT" ]; do
		state=$(cat "$(device)/state" 2>/dev/null) || state=
		[ "$state" = 'provider=pending
consumer=root' ] && break
		sleep 1
		i=$((i + 1))
	done
[ "${state-}" = 'provider=pending
consumer=root' ] || fail "provider was not pending before START"

	start_console_reader
	run rm "$(mach)/stopped"
	wait_device_state || fail "provider did not register after START"
wait_console_pattern 'DFVMM_LINUX_CONSOLE_READY' console ||
	fail "Linux console was not ready"
printf '%s\n' 'if [ "$(cat /sys/bus/pci/devices/0000:00:01.0/vendor)" = 0x1b36 ] && [ "$(cat /sys/bus/pci/devices/0000:00:01.0/device)" = 0xdf01 ]; then echo DFVMM_PCIE_ECAM_OK; else echo DFVMM_PCIE_ECAM_BAD; fi' >"$(mach)/console" ||
	fail "write PCI ECAM check"
wait_console_pattern 'DFVMM_PCIE_ECAM_OK' ecam ||
	fail "Linux did not enumerate the ECAM provider"
say 'PASS: Linux MCFG/ECAM provider enumeration'
