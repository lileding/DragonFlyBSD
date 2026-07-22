#!/bin/sh
#
# pc64 true-hardware gate for HLT park and hardware PAUSE filtering.
set -eu

ROOT=$(dirname "$0")

exec env \
	VMM_SMOKE_MODES='pausefilter hlt serialirq' \
	VMM_SMOKE_SELF_EXIT_MODES='pausefilter serialirq' \
	sh "$ROOT/smoke_exec_test.sh"
