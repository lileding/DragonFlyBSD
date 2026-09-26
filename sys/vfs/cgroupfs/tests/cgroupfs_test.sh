#!/bin/sh
#
# cgroupfs skeleton regression test.  Runs as root inside a guest kernel
# that has cgroupfs compiled in or loadable.

set -eu

mountpoint=$(mktemp -d /tmp/cgroupfs.XXXXXX)
errfile=$(mktemp /tmp/cgroupfs-err.XXXXXX)
failures=0

cleanup() {
	exec 3<&- 4<&-
	for group in a/b a c; do
		[ -d "${mountpoint}/${group}" ] && rmdir "${mountpoint}/${group}" || true
	done
	if mount | grep -q " on ${mountpoint} "; then
		umount "${mountpoint}" || true
	fi
	rmdir "${mountpoint}" || true
	rm -f "${errfile}"
}
trap cleanup EXIT INT TERM

fail() {
	echo "FAIL: $*" >&2
	failures=$((failures + 1))
}

# expect_eq <description> <actual> <expected>
expect_eq() {
	if [ "$2" != "$3" ]; then
		fail "$1: got '$2', expected '$3'"
	fi
}

# expect_error <description> <error message> <command...>
# The command must fail and its stderr must contain the strerror() text.
expect_error() {
	description=$1
	message=$2
	shift 2
	if "$@" 2>"${errfile}"; then
		fail "${description}: unexpectedly succeeded"
	elif ! grep -q "${message}" "${errfile}"; then
		fail "${description}: expected '${message}', got '$(cat "${errfile}")'"
	fi
}

expect_exists() {
	[ -e "$1" ] || fail "$1 should exist"
}

expect_absent() {
	[ ! -e "$1" ] || fail "$1 should not exist"
}

listing() {
	ls "$1" | tr '\n' ' ' | sed 's/ $//'
}

# dd reports the write(2) errno text; printf only says "write error".
write_file() {
	printf '%s\n' "$2" | dd of="$1" 2>"${errfile}.dd" ||
	    { cat "${errfile}.dd" >&2; rm -f "${errfile}.dd"; return 1; }
	rm -f "${errfile}.dd"
}

# The malloc type is only listed after its first allocation.
malloc_inuse() {
	vmstat -m | awk '$1 == "cgroupfs" { count = $2 } END { print count + 0 }'
}

unmounted_inuse=$(malloc_inuse)
mount -t cgroupfs cgroupfs "${mountpoint}"
root=${mountpoint}

echo "--- root layout"
[ -d "${root}" ] || fail "root is not a directory"
expect_eq "root listing" "$(listing "${root}")" \
    "cgroup.controllers cgroup.procs cgroup.subtree_control"
expect_eq "root cgroup.controllers" "$(cat "${root}/cgroup.controllers")" "pids"
expect_eq "root cgroup.subtree_control" \
    "$(cat "${root}/cgroup.subtree_control")" ""
expect_eq "root cgroup.procs" "$(cat "${root}/cgroup.procs")" ""
expect_absent "${root}/pids.max"
expect_absent "${root}/pids.current"

echo "--- read-only files and unsupported namespace operations"
expect_error "write cgroup.controllers" "" \
    write_file "${root}/cgroup.controllers" "pids"
expect_error "create regular file" "" touch "${root}/regular"
expect_absent "${root}/regular"
expect_error "remove cgroup.procs" "" rm -f "${root}/cgroup.procs"
expect_exists "${root}/cgroup.procs"
expect_error "rmdir on a file" "Not a directory" rmdir "${root}/cgroup.procs"
expect_error "mkdir over a file" "File exists" mkdir "${root}/cgroup.procs"

echo "--- mkdir"
mkdir "${root}/a"
mkdir "${root}/a/b"
mkdir "${root}/c"
expect_error "mkdir existing group" "File exists" mkdir "${root}/a"
expect_eq "root listing with groups" "$(listing "${root}")" \
    "a c cgroup.controllers cgroup.procs cgroup.subtree_control"
expect_eq "a listing, pids disabled" "$(listing "${root}/a")" \
    "b cgroup.controllers cgroup.procs cgroup.subtree_control"
expect_eq "a cgroup.controllers, pids disabled" \
    "$(cat "${root}/a/cgroup.controllers")" ""
expect_eq "a cgroup.subtree_control" \
    "$(cat "${root}/a/cgroup.subtree_control")" ""
expect_eq "a cgroup.procs" "$(cat "${root}/a/cgroup.procs")" ""
expect_absent "${root}/a/pids.max"
expect_absent "${root}/a/pids.current"
expect_eq "b cgroup.controllers" "$(cat "${root}/a/b/cgroup.controllers")" ""
[ -d "${root}/a/b/../b" ] || fail "dotdot lookup from a/b"

echo "--- subtree_control"
expect_error "enable unknown controller" "Invalid argument" \
    write_file "${root}/cgroup.subtree_control" "+nosuch"
expect_error "enable without leading sign" "Invalid argument" \
    write_file "${root}/cgroup.subtree_control" "pids"
expect_error "enable pids in a before root" "No such file or directory" \
    write_file "${root}/a/cgroup.subtree_control" "+pids"

write_file "${root}/cgroup.subtree_control" "+pids"
expect_eq "root subtree_control after enable" \
    "$(cat "${root}/cgroup.subtree_control")" "pids"
expect_absent "${root}/pids.max"
expect_eq "a cgroup.controllers, pids enabled" \
    "$(cat "${root}/a/cgroup.controllers")" "pids"
expect_eq "a listing, pids enabled" "$(listing "${root}/a")" \
    "b cgroup.controllers cgroup.procs cgroup.subtree_control pids.current pids.max"
expect_eq "a pids.max default" "$(cat "${root}/a/pids.max")" "max"
expect_eq "a pids.current default" "$(cat "${root}/a/pids.current")" "0"
expect_eq "c pids.max default" "$(cat "${root}/c/pids.max")" "max"
expect_absent "${root}/a/b/pids.max"

write_file "${root}/a/cgroup.subtree_control" "+pids"
expect_eq "b pids.max default" "$(cat "${root}/a/b/pids.max")" "max"
expect_error "disable pids in root while a uses it" "Device busy" \
    write_file "${root}/cgroup.subtree_control" "-pids"
expect_eq "root subtree_control after refused disable" \
    "$(cat "${root}/cgroup.subtree_control")" "pids"

write_file "${root}/a/cgroup.subtree_control" "-pids"
expect_absent "${root}/a/b/pids.max"
write_file "${root}/cgroup.subtree_control" "-pids"
expect_absent "${root}/a/pids.max"
expect_absent "${root}/c/pids.max"
expect_eq "root subtree_control after disable" \
    "$(cat "${root}/cgroup.subtree_control")" ""

echo "--- rmdir"
expect_error "rmdir non-empty group" "Device busy" rmdir "${root}/a"
expect_error "unmount with groups" "Device busy" umount "${mountpoint}"
rmdir "${root}/a/b"
rmdir "${root}/a"
rmdir "${root}/c"
expect_absent "${root}/a"
expect_eq "root listing after rmdir" "$(listing "${root}")" \
    "cgroup.controllers cgroup.procs cgroup.subtree_control"
expect_error "rmdir missing group" "No such file or directory" \
    rmdir "${root}/a"

echo "--- lifecycle"
read_fd3() {
	cat <&3
}
mkdir "${root}/a"
write_file "${root}/cgroup.subtree_control" "+pids"
exec 3<"${root}/a/pids.max"
rmdir "${root}/a"
expect_error "read open file of removed group" "No such file or directory" \
    read_fd3
exec 3<&-
write_file "${root}/cgroup.subtree_control" "-pids"

mkdir "${root}/a"
# ls(1) does not report a failing readdir, so check that it lists nothing
# and that a lookup inside the removed working directory fails.
if removed_listing=$(sh -c "cd '${root}/a' && rmdir '${root}/a' && ls . &&
    cat cgroup.procs" 2>"${errfile}"); then
	fail "lookup in removed cwd: unexpectedly succeeded"
elif ! grep -q "No such file or directory" "${errfile}"; then
	fail "lookup in removed cwd: got '$(cat "${errfile}")'"
fi
expect_eq "listing of removed cwd" "${removed_listing}" ""
expect_absent "${root}/a"

exec 4<"${root}/cgroup.procs"
expect_error "unmount with open file" "Device busy" umount "${mountpoint}"
exec 4<&-

mkdir "${root}/a"
expect_error "forced unmount with groups" "Device busy" \
    umount -f "${mountpoint}"
rmdir "${root}/a"

echo "--- leak check"
baseline=$(malloc_inuse)
iteration=0
while [ "${iteration}" -lt 50 ]; do
	mkdir "${root}/a" "${root}/a/b"
	write_file "${root}/cgroup.subtree_control" "+pids"
	cat "${root}/a/pids.max" >/dev/null
	write_file "${root}/cgroup.subtree_control" "-pids"
	rmdir "${root}/a/b" "${root}/a"
	iteration=$((iteration + 1))
done
expect_eq "cgroupfs malloc InUse after churn" "$(malloc_inuse)" "${baseline}"

echo "--- unmount"
umount "${mountpoint}"
expect_eq "cgroupfs malloc InUse after unmount" "$(malloc_inuse)" \
    "${unmounted_inuse}"

if [ "${failures}" -ne 0 ]; then
	echo "cgroupfs: ${failures} failure(s)" >&2
	exit 1
fi
echo "cgroupfs: all checks passed"
