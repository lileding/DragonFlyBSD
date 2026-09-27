#!/bin/sh
#
# cgroupfs regression test.  Runs as root inside a guest kernel that has
# cgroupfs compiled in or loadable.  The control group tree is global and
# outlives mounts, so every group created here is removed again.

set -eu

mountpoint=$(mktemp -d /tmp/cgroupfs.XXXXXX)
other=$(mktemp -d /tmp/cgroupfs.XXXXXX)
errfile=$(mktemp /tmp/cgroupfs-err.XXXXXX)
failures=0

is_mounted() {
	mount | grep -q " on $1 "
}

cleanup() {
	exec 3<&- 4<&-
	for dir in "${mountpoint}" "${other}"; do
		is_mounted "${dir}" && umount -f "${dir}" || true
	done
	if mount -t cgroupfs cgroupfs "${mountpoint}"; then
		printf '%s\n' "$$" >"${mountpoint}/cgroup.procs" || true
		for group in a/b a b c x y z; do
			[ -d "${mountpoint}/${group}" ] &&
			    rmdir "${mountpoint}/${group}" || true
		done
		umount "${mountpoint}" || true
	fi
	rmdir "${mountpoint}" "${other}" || true
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

# expect_member <group directory> <pid>
expect_member() {
	grep -qx "$2" "$1/cgroup.procs" || fail "pid $2 not in $1"
}

expect_not_member() {
	! grep -qx "$2" "$1/cgroup.procs" || fail "pid $2 unexpectedly in $1"
}

# dd reports the write(2) errno text; printf only says "write error".
write_file() {
	printf '%s\n' "$2" | dd of="$1" 2>"${errfile}.dd" ||
	    { cat "${errfile}.dd" >&2; rm -f "${errfile}.dd"; return 1; }
	rm -f "${errfile}.dd"
}

# Allocation count of a malloc type: "cgroup" for the kernel's group
# objects, "cgroupfs" for filesystem views.  A type is only listed after
# its first allocation.
malloc_count() {
	vmstat -m | awk -v type="$1" \
	    '$1 == type { count = $2 } END { print count + 0 }'
}

initial_groups=$(malloc_count cgroup)
unmounted_views=$(malloc_count cgroupfs)
mount -t cgroupfs cgroupfs "${mountpoint}"
root=${mountpoint}

echo "--- root layout"
[ -d "${root}" ] || fail "root is not a directory"
expect_eq "root listing" "$(listing "${root}")" \
    "cgroup.controllers cgroup.procs cgroup.subtree_control"
expect_eq "root cgroup.controllers" "$(cat "${root}/cgroup.controllers")" "pids"
expect_eq "root cgroup.subtree_control" \
    "$(cat "${root}/cgroup.subtree_control")" ""
expect_member "${root}" 1
expect_member "${root}" "$$"
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

echo "--- persistence across mounts"
mkdir "${root}/a" "${root}/a/b"
write_file "${root}/cgroup.subtree_control" "+pids"
umount "${mountpoint}" || fail "unmount with groups"
mount -t cgroupfs cgroupfs "${mountpoint}"
expect_eq "root listing after remount" "$(listing "${root}")" \
    "a cgroup.controllers cgroup.procs cgroup.subtree_control"
expect_eq "subtree_control after remount" \
    "$(cat "${root}/cgroup.subtree_control")" "pids"
expect_eq "a pids.max after remount" "$(cat "${root}/a/pids.max")" "max"
[ -d "${root}/a/b" ] || fail "a/b lost across remount"
umount -f "${mountpoint}" || fail "forced unmount with groups"
mount -t cgroupfs cgroupfs "${mountpoint}"
[ -d "${root}/a/b" ] || fail "a/b lost across forced unmount"
rmdir "${root}/a/b" "${root}/a"
write_file "${root}/cgroup.subtree_control" "-pids"

echo "--- multiple mounts"
mount -t cgroupfs cgroupfs "${other}"
expect_eq "second mount listing" "$(listing "${other}")" \
    "cgroup.controllers cgroup.procs cgroup.subtree_control"
# Negative entries in one mount must not hide groups created in another.
expect_absent "${other}/x"
mkdir "${root}/x"
[ -d "${other}/x" ] || fail "mkdir not visible in other mount"
expect_eq "inode across mounts" "$(stat -f %i "${other}/x")" \
    "$(stat -f %i "${root}/x")"
# Positive entries must not keep removed groups alive.
ls "${other}/x" >/dev/null
rmdir "${root}/x"
expect_absent "${other}/x"
mkdir "${other}/x" || fail "recreate in other mount after remote rmdir"
[ -d "${root}/x" ] || fail "recreated group not visible in first mount"
rmdir "${other}/x"
# Controller files flip visibility in every mount.
mkdir "${root}/y"
expect_absent "${other}/y/pids.max"
write_file "${root}/cgroup.subtree_control" "+pids"
expect_eq "pids.max via other mount" "$(cat "${other}/y/pids.max")" "max"
expect_eq "subtree_control via other mount" \
    "$(cat "${other}/cgroup.subtree_control")" "pids"
stat "${root}/y/pids.max" >/dev/null
write_file "${other}/cgroup.subtree_control" "-pids"
expect_absent "${root}/y/pids.max"
expect_absent "${other}/y/pids.max"
expect_eq "subtree_control via first mount" \
    "$(cat "${root}/cgroup.subtree_control")" ""
rmdir "${other}/y"
expect_absent "${root}/y"
# A working directory inside a group removed through another mount.
mkdir "${root}/z"
expect_error "lookup in cwd removed via other mount" "" \
    sh -c "cd '${other}/z' && rmdir '${root}/z' && cat cgroup.procs"
expect_absent "${root}/z"
expect_absent "${other}/z"
umount "${other}" || fail "unmount second mount"

echo "--- membership"
mkdir "${root}/a" "${root}/b"
write_file "${root}/a/cgroup.procs" "$$"
expect_member "${root}/a" "$$"
expect_not_member "${root}" "$$"
sleep 60 &
member=$!
expect_member "${root}/a" "${member}"
mount -t cgroupfs cgroupfs "${other}"
expect_member "${other}/a" "$$"
expect_member "${other}/a" "${member}"
umount "${other}"
# "0" moves the writer only: the subshell lists itself in b, while this
# shell stays in a.
joined=$(sh -c "echo 0 >'${root}/b/cgroup.procs' && echo \$\$ &&
    cat '${root}/b/cgroup.procs'") || fail "writing 0 to cgroup.procs"
self=$(printf '%s\n' "${joined}" | sed -n 1p)
printf '%s\n' "${joined}" | sed 1d | grep -qx "${self}" ||
    fail "writing 0 did not move the writer"
expect_not_member "${root}/b" "$$"
expect_member "${root}/a" "$$"
expect_error "rmdir group with members" "Device busy" rmdir "${root}/a"
expect_error "migrate malformed pid" "Invalid argument" \
    write_file "${root}/b/cgroup.procs" "abc"
expect_error "migrate two pids" "Invalid argument" \
    write_file "${root}/b/cgroup.procs" "1 2"
expect_error "migrate empty" "Invalid argument" \
    write_file "${root}/b/cgroup.procs" ""
expect_error "migrate missing pid" "No such process" \
    write_file "${root}/b/cgroup.procs" "999999"
mkdir "${root}/c"
exec 3>"${root}/c/cgroup.procs"
rmdir "${root}/c"
if printf '%s\n' "$$" >&3; then
	fail "migrate into removed group: unexpectedly succeeded"
fi
exec 3>&-
expect_member "${root}/a" "$$"
kill "${member}"
wait "${member}" || true
expect_not_member "${root}/a" "${member}"
write_file "${root}/cgroup.procs" "$$"
expect_member "${root}" "$$"
expect_not_member "${root}/a" "$$"
rmdir "${root}/a" || fail "rmdir after members left"
rmdir "${root}/b" || fail "rmdir after subshell exited"

echo "--- leak check"
group_baseline=$(malloc_count cgroup)
view_baseline=$(malloc_count cgroupfs)
mount -t cgroupfs cgroupfs "${other}"
iteration=0
while [ "${iteration}" -lt 50 ]; do
	mkdir "${root}/a" "${root}/a/b"
	write_file "${root}/cgroup.subtree_control" "+pids"
	cat "${root}/a/pids.max" "${other}/a/b/cgroup.controllers" >/dev/null
	write_file "${other}/cgroup.subtree_control" "-pids"
	write_file "${other}/a/b/cgroup.procs" "$$"
	sleep 30 &
	member=$!
	write_file "${root}/cgroup.procs" "$$"
	kill "${member}"
	wait "${member}" || true
	rmdir "${other}/a/b" "${root}/a"
	iteration=$((iteration + 1))
done
umount "${other}"
expect_eq "group count after churn" "$(malloc_count cgroup)" \
    "${group_baseline}"
expect_eq "view count after churn" "$(malloc_count cgroupfs)" \
    "${view_baseline}"

echo "--- unmount"
umount "${mountpoint}"
expect_eq "group count after unmount" "$(malloc_count cgroup)" \
    "${initial_groups}"
expect_eq "view count after unmount" "$(malloc_count cgroupfs)" \
    "${unmounted_views}"

if [ "${failures}" -ne 0 ]; then
	echo "cgroupfs: ${failures} failure(s)" >&2
	exit 1
fi
echo "cgroupfs: all checks passed"
