#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BENCH="$SCRIPT_DIR/../../tests/fcache/fc_bench"
BENCH_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../tests/fcache" && pwd)
VARIANT="${1:-flow4}"

if [ ! -x "$BENCH" ]; then
    make -C "$BENCH_DIR" fc_bench
fi

usage() {
    cat <<EOF
usage: $0 [variant]

variants:
  flow4   fc flow4 matrix
  flow6   fc flow6 matrix
  flowu   fc flowu matrix
  all     run all 3 variants
EOF
}

run_bench() {
    echo
    echo ">>> $VARIANT $*"
    "$BENCH" "$VARIANT" "$@"
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

run_variant_matrix() {
    echo
    echo "====== $VARIANT matrix ======"

    echo
    echo "== FC2-only: 1M / 500kpps =="
    run_bench rate_fc_only 1000000 50 100 500000
    run_bench rate_fc_only 1000000 60 100 500000
    run_bench rate_fc_only 1000000 90 100 500000

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

case "$VARIANT" in
    flow4|flow6|flowu)
        run_variant_matrix
        ;;
    all)
        for v in flow4 flow6 flowu; do
            VARIANT="$v"
            run_variant_matrix
        done
        ;;
    *)
        echo "unknown variant: $VARIANT" >&2
        usage >&2
        exit 2
        ;;
esac
