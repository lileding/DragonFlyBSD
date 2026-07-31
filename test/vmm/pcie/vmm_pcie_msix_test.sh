#!/bin/sh
# pc64 Linux MSI-X and direct-doorbell harness.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-pcie-msix-vmm}
VM=${VMM_MACHINE:-pciemsix0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-pcie-msix-test.log}
CONSOLE_LOG=${VMM_CONSOLE_LOG:-/var/tmp/dfvmm-pcie-msix-console.log}
LOADER=${LINUX_LOADER:-/var/tmp/vmmld_linux_kexec}
WRAPPER=${LINUX_WRAPPER:-/var/tmp/vmmld_linux_pcie_msix}
PROVIDER=${VMM_PCIE_MSIX_PROVIDER:-/var/tmp/vmm_pcie_msix_provider}
PROVIDER_LOG=${VMM_PCIE_MSIX_PROVIDER_LOG:-/var/tmp/dfvmm-pcie-msix-provider.log}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-pcie-msix-$$-mount_vmm}
KERNEL=${LINUX_KERNEL:-/var/tmp/dfvmm-linux-virt-6.18.40/root/boot/vmlinuz-virt}
MODULE=${LINUX_PCIE_MSIX_MODULE:-/var/tmp/dfvmm_pcie_msix.ko}
INITRD=${LINUX_INITRD_ROOTFS:-/var/tmp/dfvmm-linux-msix-initrd-rootfs.gz}
MEM=${LINUX_MEM:-256M}
TIMEOUT=${VMM_TIMEOUT:-45}
STOP_TIMEOUT=${VMM_STOP_TIMEOUT:-20}

LOADED=0
MOUNTED=0
CONSOLE_READER_PID=
PROVIDER_PID=
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
	printf '%s/machines/%s\n' "$MNT" "$VM"
}

device()
{
	printf '%s/devices/msix0\n' "$(mach)"
}

host_device()
{
	printf '%s/machines/host/devices/msix0\n' "$MNT"
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
	if [ -n "$OLD_SVM_TRACE" ]; then
		sysctl debug.vmm.svm_trace="$OLD_SVM_TRACE" >>"$LOG" 2>&1
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
[ -f "$MODULE" ] || fail "missing $MODULE"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
readelf -SW "$VMM_KO" 2>>"$LOG" | grep -qi eh_frame &&
	fail "$VMM_KO contains .eh_frame"

say "+ build Linux initrd with MSI-X test module"
LINUX_PCIE_MSIX_MODULE="$MODULE" LINUX_INITRD_ROOTFS="$INITRD" \
	"$REPO/test/vmm/linux/linux_initrd_rootfs_build.sh" >>"$LOG" 2>&1 ||
	fail "build Linux initrd with MSI-X test module"
[ -f "$INITRD" ] || fail "missing $INITRD"
run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/linux/linux_kexec_loader.c" -o "$LOADER"
run cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys/vmm" \
	"$REPO/sys/vmm/vmm_pcie_abi.c" \
	"$REPO/test/vmm/pcie/vmm_pcie_msix_provider.c" -o "$PROVIDER"
printf '%s\n' '#!/bin/sh' >"$WRAPPER" || fail "create $WRAPPER"
printf '%s\n' "exec \"$LOADER\" \"$KERNEL\" \"initramfs=$INITRD\" \"console=ttyS0,115200\" \"loglevel=7\" \"rdinit=/init\"" >>"$WRAPPER" ||
	fail "write $WRAPPER"
chmod +x "$WRAPPER" || fail "chmod $WRAPPER"

run kldload "$VMM_KO"
LOADED=1
OLD_SVM_TRACE=$(sysctl -n debug.vmm.svm_trace 2>>"$LOG") ||
	fail "read debug.vmm.svm_trace"
run sysctl debug.vmm.svm_trace=0
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
wait_device_state || fail "provider did not register"
wait_pattern "$PROVIDER_LOG" 'DFVMM_PCIE_MSIX_PROVIDER_READY' provider ||
	fail "provider did not map BAR"

start_console_reader
run rm "$(mach)/stopped"
wait_pattern "$CONSOLE_LOG" 'DFVMM_LINUX_CONSOLE_READY' console ||
	fail "Linux console was not ready"
run sysctl debug.vmm.svm_trace=1
printf '%s\n' true >"$(mach)/console" || fail "trigger MSI-X module load"
wait_pattern "$PROVIDER_LOG" 'DFVMM_PCIE_MSIX_PROVIDER_KICK' provider_kick ||
	fail "provider did not observe direct BAR doorbell"
wait_pattern "$PROVIDER_LOG" 'DFVMM_PCIE_MSIX_PROVIDER_SENT' provider_msix ||
	fail "provider did not send MSI-X"
wait_pattern "$CONSOLE_LOG" 'DFVMM_PCIE_MSIX_IRQ_OK' guest_irq ||
	fail "Linux did not receive MSI-X"
wait_pattern "$CONSOLE_LOG" 'DFVMM_PCIE_MSIX_PROBE_OK' guest_probe ||
	fail "Linux PCI MSI-X probe did not complete"
cat "$(mach)/events" >>"$LOG" 2>&1 || fail "read machine events"
say 'PASS: Linux PCIe direct BAR doorbell and MSI-X'
