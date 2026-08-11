#!/bin/sh
# pc64 Linux direct-BAR shared-mapping harness.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BASE_SYS="$REPO/sys"
VMM_KO=${VMM_KO:-$REPO/sys/dev/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-pcie-bar-vmm}
VM=${VMM_MACHINE:-pciebar0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-pcie-bar-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-pcie-bar-console.log}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_pcie_bar}
PROVIDER=${VMM_PCIE_BAR_PROVIDER:-/var/tmp/vmm_pcie_bar_provider}
PROVIDER_LOG=${VMM_PCIE_BAR_PROVIDER_LOG:-/var/tmp/dfvmm-pcie-bar-provider.log}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-pcie-bar-$$-mount_vmmfs}
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
	printf '%s/%s\n' "$MNT" "$VM"
}

device()
{
	printf '%s/devices/bar0\n' "$(mach)"
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

wait_pattern()
{
	file=$1
	pattern=$2
	label=$3
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$file" && return 0
		sleep 1
		i=$((i + 1))
	done
	say "missing marker: $label"
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
		touch "$(mach)/stopped" 2>>"$LOG"
	fi
	# Provider close is independent PCIe hot-unplug and may race this stop.
	stop_provider
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(device)" ]; then
		remove_path "$(device)"
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
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
readelf -SW "$VMM_KO" 2>>"$LOG" | grep -qi eh_frame &&
	fail "$VMM_KO contains .eh_frame"

run "$REPO/test/vmm/linux/linux_initrd_rootfs_build.sh"
[ -f "$INITRD" ] || fail "missing $INITRD"
run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
run cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys/dev/vmm" -I "$BASE_SYS" \
	"$REPO/sys/dev/vmm/vmm_pcie_abi.c" \
	"$REPO/test/vmm/pcie/vmm_pcie_bar_provider.c" -o "$PROVIDER"
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
run mkdir "$(device)"
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
	wait_pattern "$PROVIDER_LOG" 'DFVMM_PCIE_BAR_PROVIDER_READY' provider ||
	fail "provider did not map BAR"
wait_pattern "$CONSOLE_LOG" 'DFVMM_LINUX_CONSOLE_READY' console ||
	fail "Linux console was not ready"
printf '%s\n' 'dfvmm-pcie-bar-probe' >"$(mach)/console" ||
	fail "write BAR probe command"
wait_pattern "$CONSOLE_LOG" 'DFVMM_PCIE_CONFIG_MEMORY_OK' config_memory ||
	fail "guest did not enable PCI memory decoding"
wait_pattern "$CONSOLE_LOG" 'DFVMM_PCIE_BAR_GUEST_READ_OK' guest_read ||
	fail "guest BAR read failed"
wait_pattern "$CONSOLE_LOG" 'DFVMM_PCIE_BAR_GUEST_WRITE_OK' guest_write ||
	fail "guest BAR write failed"
wait_pattern "$PROVIDER_LOG" 'DFVMM_PCIE_BAR_PROVIDER_OK' provider_write ||
	fail "provider did not observe guest BAR write"
cat "$(mach)/events" >>"$LOG" 2>&1 || fail "read machine events"
grep -q 'pcie bar map bdf=0008 bar=0 gpa=0xc0000000 size=0x1000' "$LOG" ||
	fail "missing direct BAR NPT map event"
say 'PASS: Linux direct PCIe BAR shared mapping'
