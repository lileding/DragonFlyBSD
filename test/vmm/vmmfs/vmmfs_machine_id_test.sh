#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause

set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 mountpoint" >&2
	exit 2
fi

mountpoint=$1
first="$mountpoint/machine-id-a-$$"
second="$mountpoint/machine-id-b-$$"

mkdir "$first"
mkdir "$second"
first_id=$(cat "$first/id")
second_id=$(cat "$second/id")

case $first_id in
	[0-9][0-9][0-9][0-9][0-9][0-9]) ;;
	*) echo "invalid first machine ID: $first_id" >&2; exit 1 ;;
esac
case $second_id in
	[0-9][0-9][0-9][0-9][0-9][0-9]) ;;
	*) echo "invalid second machine ID: $second_id" >&2; exit 1 ;;
esac
if [ "$first_id" = "$second_id" ]; then
	echo "machine IDs are not unique" >&2
	exit 1
fi
if rm "$first/id" >/dev/null 2>&1; then
	echo "machine ID node was removable" >&2
	exit 1
fi

rmdir "$first"
rmdir "$second"
echo "vmmfs machine ID test passed: $first_id $second_id"
