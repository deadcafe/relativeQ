#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BENCH="$SCRIPT_DIR/fc2_bench"

if [ ! -x "$BENCH" ]; then
    make -C "$SCRIPT_DIR" fc2_bench
fi

run() {
    echo
    echo ">>> $*"
    "$BENCH" "$@"
}

# Accepted batch-maint policy:
#   thresholds: 70/73/75/77
#   kicks:      0/0/1/2
TRACE_POLICY_ARGS="70 73 75 77 0 0 1 2 1"

echo "== Typical Compare =="
run rate_compare 1000000 60 100 500000
run rate_compare 1000000 75 100 500000
run rate_compare 1000000 90 100 500000
run rate_fc2_only 1000000 50 100 500000
run rate_fc2_only 1000000 60 100 500000
run rate_fc2_only 1000000 90 100 500000

echo
echo "== Trace Policy: 1M / 500kpps =="
run rate_trace_custom 1000000 60 95 500000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 1000000 60 90 500000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 1000000 60 80 500000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 1000000 60 70 500000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 1000000 75 95 500000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 1000000 75 90 500000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 1000000 85 95 500000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 1000000 85 90 500000 8000 3 2000 $TRACE_POLICY_ARGS

echo
echo "== Trace Policy: 2M / 500kpps =="
run rate_trace_custom 2000000 75 95 500000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 2000000 85 90 500000 8000 3 2000 $TRACE_POLICY_ARGS

echo
echo "== Trace Policy: 2M / 1Mpps =="
run rate_trace_custom 2000000 75 95 1000000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 2000000 75 90 1000000 8000 3 2000 $TRACE_POLICY_ARGS
run rate_trace_custom 2000000 75 80 1000000 8000 3 2000 $TRACE_POLICY_ARGS
