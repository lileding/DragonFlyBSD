#!/bin/sh
# pc64 P7 DMA generation reset harness.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BASE_SYS=$(cd "$REPO/../nvkm/sys" && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-pcie-dma-reset-vmm}
VM=${VMM_MACHINE:-pciedmares0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-pcie-dma-reset-test.log}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_pcie_dma_reset_loader}
WRAPPER=${VMM_LOADER_WRAPPER:-/var/tmp/vmm_pcie_dma_reset_loader_wrapper}
PROVIDER=${VMM_PCIE_DMA_RESET_PROVIDER:-/var/tmp/vmm_pcie_dma_reset_provider}
PROVIDER_LOG=${VMM_PCIE_DMA_RESET_PROVIDER_LOG:-/var/tmp/dfvmm-pcie-dma-reset-provider.log}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-pcie-dma-reset-$$-mount_vmm}
TIMEOUT=${VMM_TIMEOUT:-25}

LOADED=0
MOUNTED=0
PROVIDER_PID=

say()
{
	printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"
}

fail()
{
	say "FAIL: $*"
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(machine)" ]; then
		cat "$(machine)/events" >>"$LOG" 2>&1 || true
	fi
	[ -f "$PROVIDER_LOG" ] && cat "$PROVIDER_LOG" >>"$LOG" 2>&1 || true
	exit 1
}

run()
{
	say "+ $*"
	"$@" >>"$LOG" 2>&1 || fail "$*"
}

machine()
{
	printf '%s/machines/%s\n' "$MNT" "$VM"
}

device()
{
	printf '%s/devices/dmares0\n' "$(machine)"
}

host_device()
{
	printf '%s/machines/host/devices/dmares0\n' "$MNT"
}

wait_pattern()
{
	file=$1
	pattern=$2
	label=$3
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		grep -q "$pattern" "$file" 2>/dev/null && return 0
		sleep 1
		i=$((i + 1))
	done
	fail "missing $label"
}

remove_path()
{
	path=$1
	i=0

	while [ "$i" -lt "$TIMEOUT" ]; do
		rmdir "$path" >>"$LOG" 2>&1 && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}

cleanup()
{
	set +e
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(machine)" ]; then
		echo force >"$(machine)/stopped" 2>>"$LOG"
	fi
	if [ -n "$PROVIDER_PID" ]; then
		kill "$PROVIDER_PID" >/dev/null 2>&1
		wait "$PROVIDER_PID" >/dev/null 2>&1
	fi
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(device)" ]; then
		remove_path "$(device)"
	fi
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(host_device)" ]; then
		remove_path "$(host_device)"
	fi
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(machine)" ]; then
		remove_path "$(machine)"
	fi
	if [ "$MOUNTED" -eq 1 ]; then
		i=0
		while [ "$i" -lt "$TIMEOUT" ]; do
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
	rm -f "$MOUNT_HELPER" "$LOADER" "$WRAPPER" "$PROVIDER"
}

: >"$LOG" || exit 1
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root on the pc64 host"
case "$VMM_KO" in /*) ;; *) fail "VMM_KO must be absolute" ;; esac
[ -f "$VMM_KO" ] || fail "missing $VMM_KO"
kldstat -n vmm >/dev/null 2>&1 && fail "vmm already loaded"
readelf -SW "$VMM_KO" 2>>"$LOG" | grep -qi eh_frame &&
	fail "$VMM_KO contains .eh_frame"

run cc -Wall -Wextra -Werror -std=c11 -O2 \
	"$REPO/test/vmm/smoke/smoke_loader.c" -o "$LOADER"
run cc -Wall -Wextra -Werror -std=c11 -O2 -I "$REPO/sys/vmm" -I "$BASE_SYS" \
	"$REPO/sys/vmm/vmm_pcie_abi.c" \
	"$REPO/test/vmm/pcie/vmm_pcie_dma_reset_provider.c" -o "$PROVIDER"
printf '%s\n' '#!/bin/sh' >"$WRAPPER" || fail "create $WRAPPER"
printf '%s\n' "exec \"$LOADER\" loop" >>"$WRAPPER" ||
	fail "write $WRAPPER"
chmod 755 "$WRAPPER" || fail "chmod $WRAPPER"

run kldload "$VMM_KO"
LOADED=1
run mkdir -p "$MNT"
run rm -f "$MOUNT_HELPER"
run ln -s /sbin/mount_std "$MOUNT_HELPER"
run "$MOUNT_HELPER" vmm "$MNT"
MOUNTED=1
run mkdir "$(machine)"
printf '1\n' >"$(machine)/vcpu" || fail "write vcpu"
printf '2M\n' >"$(machine)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(machine)/loader" || fail "write loader"
run mkdir "$(host_device)"
run mv "$(host_device)" "$(device)"
: >"$PROVIDER_LOG" || fail "create provider log"
"$PROVIDER" "$(device)" >>"$PROVIDER_LOG" 2>>"$LOG" &
PROVIDER_PID=$!
wait_pattern "$(device)/state" 'provider=pending' provider_pending

run rm "$(machine)/stopped"
wait_pattern "$PROVIDER_LOG" 'DMA_RESET_PROVIDER_READY run=1' first_start
printf '%s\n' 'reset force' >"$(machine)/events" || fail "force reset"
wait_pattern "$PROVIDER_LOG" 'DMA_RESET_PROVIDER_REVOKED run=1' first_revoke
wait_pattern "$PROVIDER_LOG" 'DMA_RESET_PROVIDER_READY run=2' second_start
printf '%s\n' force >"$(machine)/stopped" || fail "force stop"
wait_pattern "$PROVIDER_LOG" 'DMA_RESET_PROVIDER_REVOKED run=2' second_revoke
wait "$PROVIDER_PID" || fail "provider did not exit after second stop"
PROVIDER_PID=
remove_path "$(device)" || fail "remove device"
remove_path "$(machine)" || fail "remove machine"
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
	umount "$MNT" >>"$LOG" 2>&1 && {
		MOUNTED=0
		break
	}
	sleep 1
	i=$((i + 1))
done
[ "$MOUNTED" -eq 0 ] || fail "umount $MNT"
run kldunload vmm
LOADED=0
say 'PASS: vPCIe reset revokes old DMA generation and restarts provider'
