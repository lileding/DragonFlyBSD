#!/bin/sh
# pc64 P7 provider-loss DMA capability harness.
set -u

ROOT=$(dirname "$0")
REPO=$(cd "$ROOT/../../.." && pwd)
BASE_SYS=$(cd "$REPO/../nvkm/sys" && pwd)
VMM_KO=${VMM_KO:-$REPO/sys/vmm/vmm.ko}
MNT=${VMM_MOUNT:-/var/tmp/dfvmm-pcie-dma-loss-vmm}
VM=${VMM_MACHINE:-pciedmaloss0}
LOG=${VMM_LOG:-/var/tmp/dfvmm-pcie-dma-loss-test.log}
LOADER=${VMM_SMOKE_LOADER:-/var/tmp/vmm_pcie_dma_loss_loader}
WRAPPER=${VMM_LOADER_WRAPPER:-/var/tmp/vmm_pcie_dma_loss_loader_wrapper}
PROVIDER=${VMM_PCIE_DMA_LOSS_PROVIDER:-/var/tmp/vmm_pcie_dma_loss_provider}
PROVIDER_LOG=${VMM_PCIE_DMA_LOSS_PROVIDER_LOG:-/var/tmp/dfvmm-pcie-dma-loss-provider.log}
UNLOAD_LOG=${VMM_PCIE_DMA_LOSS_UNLOAD_LOG:-/var/tmp/dfvmm-pcie-dma-loss-unload.log}
MOUNT_HELPER=${VMM_MOUNT_HELPER:-/var/tmp/dfvmm-pcie-dma-loss-$$-mount_vmm}
TIMEOUT=${VMM_TIMEOUT:-25}
VCPU=${VMM_VCPU:-1}
SMOKE_MODE=${VMM_SMOKE_MODE:-loop}

LOADED=0
MOUNTED=0
PROVIDER_PID=
HOLDER_PID=

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
	printf '%s/%s\n' "$MNT" "$VM"
}

device()
{
	printf '%s/devices/dmaloss0\n' "$(machine)"
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
		touch "$(machine)/stopped" 2>>"$LOG"
	fi
	if [ -n "$PROVIDER_PID" ]; then
		kill "$PROVIDER_PID" >/dev/null 2>&1
		wait "$PROVIDER_PID" >/dev/null 2>&1
	fi
	if [ -n "$HOLDER_PID" ]; then
		kill "$HOLDER_PID" >/dev/null 2>&1
	fi
	if [ "$MOUNTED" -eq 1 ] && [ -d "$(device)" ]; then
		remove_path "$(device)"
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
	rm -f "$MOUNT_HELPER" "$LOADER" "$WRAPPER" "$PROVIDER" "$UNLOAD_LOG"
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
	"$REPO/test/vmm/pcie/vmm_pcie_dma_loss_provider.c" -o "$PROVIDER"
printf '%s\n' '#!/bin/sh' >"$WRAPPER" || fail "create $WRAPPER"
printf '%s\n' "exec \"$LOADER\" \"$SMOKE_MODE\"" >>"$WRAPPER" ||
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
printf '%s\n' "$VCPU" >"$(machine)/vcpu" || fail "write vcpu"
printf '2M\n' >"$(machine)/mem" || fail "write mem"
printf '%s\n' "$WRAPPER" >"$(machine)/loader" || fail "write loader"
run mkdir "$(device)"
: >"$PROVIDER_LOG" || fail "create provider log"
"$PROVIDER" "$(device)" >>"$PROVIDER_LOG" 2>>"$LOG" &
PROVIDER_PID=$!
wait_pattern "$(device)/state" 'provider=pending' provider_pending

run rm "$(machine)/stopped"
wait_pattern "$PROVIDER_LOG" 'DMA_LOSS_PROVIDER_CLOSED' provider_close
wait "$PROVIDER_PID" || fail "provider close process failed"
PROVIDER_PID=
HOLDER_PID=$(sed -n 's/.*holder=\([0-9][0-9]*\).*/\1/p' "$PROVIDER_LOG" | tail -n 1)
case "$HOLDER_PID" in
''|*[!0-9]*) fail "invalid mapping holder pid" ;;
esac
wait_pattern "$PROVIDER_LOG" 'DMA_LOSS_HOLDER_REVOKED' holder_revoke
kill -0 "$HOLDER_PID" >/dev/null 2>&1 || fail "mapping holder exited early"

touch "$(machine)/stopped" || fail "stop"
remove_path "$(device)" || fail "remove detached device"
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

: >"$UNLOAD_LOG" || fail "create unload log"
say '+ kldunload vmm (expect EBUSY while surprise-revoked mappings remain)'
if kldunload vmm >"$UNLOAD_LOG" 2>&1; then
	cat "$UNLOAD_LOG" >>"$LOG"
	fail "kldunload succeeded with surprise-revoked mappings"
fi
cat "$UNLOAD_LOG" >>"$LOG"
grep -qi 'device busy' "$UNLOAD_LOG" ||
	fail "kldunload did not report EBUSY for surprise-revoked mappings"
kldstat -n vmm >/dev/null 2>&1 ||
	fail "vmm unloaded despite surprise-revoked mappings"
kill "$HOLDER_PID" || fail "terminate mapping holder"
HOLDER_PID=
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
	if kldunload vmm >>"$LOG" 2>&1; then
		LOADED=0
		break
	fi
	sleep 1
	i=$((i + 1))
done
[ "$LOADED" -eq 0 ] || fail "vmm stayed loaded after holder exit"
say 'PASS: vPCIe provider loss revokes retained BAR and DMA mappings'
