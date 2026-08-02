#!/bin/sh
# vkernel-only control-plane test for per-machine textual events.
set -u

MNT=/vmm
M=$MNT/machines
MOUNT_HELPER=/tmp/dfvmm-events-$$-mount_vmm
PASS=0
FAIL=0

say()
{
	echo "$@"
}

pass()
{
	say "PASS $*"
	PASS=$((PASS + 1))
}

fail()
{
	say "FAIL $*"
	FAIL=$((FAIL + 1))
}

ck()
{
	name=$1
	shift
	if "$@"; then
		pass "$name"
	else
		fail "$name"
	fi
}

contains()
{
	file=$1
	pattern=$2

	cat "$file" 2>/dev/null | grep -q "$pattern"
}

wait_contains()
{
	file=$1
	pattern=$2
	i=0

	while [ "$i" -lt 10 ]; do
		contains "$file" "$pattern" && return 0
		sleep 1
		i=$((i + 1))
	done
	say "--- final $file ---"
	cat "$file" 2>/dev/null || true
	say "--- end $file ---"
	return 1
}

cleanup_machine()
{
	vm=$1
	dir=$M/$vm
	i=0

	[ -d "$dir" ] || return 0
	touch "$dir/stopped" 2>/dev/null || true
	while [ "$i" -lt 10 ] && [ -d "$dir" ]; do
		rmdir "$dir" 2>/dev/null && return 0
		sleep 1
		i=$((i + 1))
	done
	return 0
}

cleanup()
{
	set +e
	cleanup_machine exit7
	cleanup_machine empty
	umount "$MNT" 2>/dev/null || true
	kldunload vmm 2>/dev/null || true
	rm -f "$MOUNT_HELPER" /tmp/vmmld_exit7 /tmp/vmmld_empty
}

setup_machine()
{
	vm=$1
	loader=$2
	dir=$M/$vm

	mkdir "$dir" || return 1
	echo 1 >"$dir/vcpu" || return 1
	echo 2M >"$dir/mem" || return 1
	echo "$loader" >"$dir/loader" || return 1
}

test_retained_events()
{
	first=$(cat "$M/exit7/events") || return 1
	second=$(cat "$M/exit7/events") || return 1
	[ "$first" = "$second" ] || return 1
	echo "$first" | grep -q "machine created" || return 1
	echo "$first" | grep -q "state stopped reason=create"
}

test_exit7_loader()
{
	dir=$M/exit7

	rm "$dir/stopped" || return 1
	wait_contains "$dir/events" "loader wait failed" || return 1
	wait_contains "$dir/events" "reason=exit" || return 1
	wait_contains "$dir/events" "exit_status=" || return 1
	wait_contains "$dir/events" "start failed" || return 1
	wait_contains "$dir/events" "state stopped reason=start_failed"
}

test_empty_manifest()
{
	dir=$M/empty

	rm "$dir/stopped" || return 1
	wait_contains "$dir/events" "loader exited ok" || return 1
	wait_contains "$dir/events" "manifest rejected" || return 1
	wait_contains "$dir/events" "start failed" || return 1
	wait_contains "$dir/events" "state stopped reason=start_failed"
}

trap cleanup EXIT INT TERM

ck "kldload" kldload /xchg/vmm.ko
ck "mount helper" ln -sf /sbin/mount_std "$MOUNT_HELPER"
ck "mkdir mountpoint" mkdir -p "$MNT"
ck "mount" "$MOUNT_HELPER" vmm "$MNT"
sysctl debug.vmm.allow_vcpu_start=0 >/dev/null 2>&1 || true

cat >/tmp/vmmld_exit7 <<'EOF_EXIT7'
#!/bin/sh
exit 7
EOF_EXIT7
chmod +x /tmp/vmmld_exit7

cat >/tmp/vmmld_empty <<'EOF_EMPTY'
#!/bin/sh
exit 0
EOF_EMPTY
chmod +x /tmp/vmmld_empty

ck "setup exit7" setup_machine exit7 /tmp/vmmld_exit7
ck "setup empty" setup_machine empty /tmp/vmmld_empty
ck "events retained" test_retained_events
ck "exit7 loader failure log" test_exit7_loader
ck "empty manifest failure log" test_empty_manifest

say "EVENTS-TESTS: $PASS passed $FAIL failed"
[ "$FAIL" -eq 0 ]
