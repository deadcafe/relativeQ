#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BENCH="$SCRIPT_DIR/../../tests/fcache2/fc2_bench"
BENCH_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../tests/fcache2" && pwd)
VARIANT="${1:-flow4}"

if [ ! -x "$BENCH" ]; then
    make -C "$BENCH_DIR" fc2_bench
fi

usage() {
    cat <<EOF
usage: $0 [variant]

variants:
  flow4   current fc2 flow4 matrix
  flow6   reserved for future fc2 flow6 matrix
EOF
}

run_bench() {
    echo
    echo ">>> $*"
    "$BENCH" "$@"
}

# Accepted batch-maint policy:
#   thresholds: 70/73/75/77
#   kicks:      0/0/1/2
TRACE_POLICY_FILL0=70
TRACE_POLICY_FILL1=73
TRACE_POLICY_FILL2=75
TRACE_POLICY_FILL3=77
TRACE_POLICY_KICK0=0
TRACE_POLICY_KICK1=0
TRACE_POLICY_KICK2=1
TRACE_POLICY_KICK3=2
TRACE_POLICY_SCALE=1

run_trace_case() {
    desired_entries="$1"
    start_fill_pct="$2"
    hit_pct="$3"
    pps="$4"

    run_bench rate_trace_custom \
        "$desired_entries" "$start_fill_pct" "$hit_pct" "$pps" \
        8000 3 2000 \
        "$TRACE_POLICY_FILL0" "$TRACE_POLICY_FILL1" \
        "$TRACE_POLICY_FILL2" "$TRACE_POLICY_FILL3" \
        "$TRACE_POLICY_KICK0" "$TRACE_POLICY_KICK1" \
        "$TRACE_POLICY_KICK2" "$TRACE_POLICY_KICK3" \
        "$TRACE_POLICY_SCALE"
}

run_flow4_matrix() {
    echo "== Typical Compare =="
    run_bench rate_compare 1000000 60 100 500000
    run_bench rate_compare 1000000 75 100 500000
    run_bench rate_compare 1000000 90 100 500000
    run_bench rate_fc2_only 1000000 50 100 500000
    run_bench rate_fc2_only 1000000 60 100 500000
    run_bench rate_fc2_only 1000000 90 100 500000

    echo
    echo "== Trace Policy: 1M / 500kpps =="
    run_trace_case 1000000 60 95 500000
    run_trace_case 1000000 60 90 500000
    run_trace_case 1000000 60 80 500000
    run_trace_case 1000000 60 70 500000
    run_trace_case 1000000 75 95 500000
    run_trace_case 1000000 75 90 500000
    run_trace_case 1000000 85 95 500000
    run_trace_case 1000000 85 90 500000

    echo
    echo "== Trace Policy: 2M / 500kpps =="
    run_trace_case 2000000 75 95 500000
    run_trace_case 2000000 85 90 500000

    echo
    echo "== Trace Policy: 2M / 1Mpps =="
    run_trace_case 2000000 75 95 1000000
    run_trace_case 2000000 75 90 1000000
    run_trace_case 2000000 75 80 1000000
}

if [ "$VARIANT" = "-h" ] || [ "$VARIANT" = "--help" ]; then
    usage
    exit 0
fi

# Accepted batch-maint policy:
#   thresholds: 70/73/75/77
#   kicks:      0/0/1/2
case "$VARIANT" in
    flow4)
        run_flow4_matrix
        ;;
    flow6)
        echo "flow6 matrix is not implemented yet" >&2
        exit 3
        ;;
    *)
        echo "unknown variant: $VARIANT" >&2
        usage >&2
        exit 2
        ;;
esac
