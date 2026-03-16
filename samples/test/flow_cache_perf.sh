#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
TOPDIR=$(cd "$SCRIPT_DIR/../.." && pwd)
FCACHEDIR="$TOPDIR/samples/fcache"
TESTDIR="$TOPDIR/samples/test"

CC_BIN=${CC:-cc}
OPTLEVEL=${OPTLEVEL:-3}
SIMD=${SIMD:-avx2}
CASE=${PERF_CASE:-flow4:pkt_std}
BACKEND=${PERF_BACKEND:-avx2}
EVENTS_PRESET=${PERF_EVENTS:-core}
REPEAT=${PERF_REPEAT:-1000}
ENTRIES=${PERF_ENTRIES:-1048576}
WARMUP=${PERF_WARMUP:-50}
SEED=${PERF_SEED:-}
CPU=${PERF_CPU:-}
STAT_REPEAT=${PERF_STAT_REPEAT:-5}

usage() {
    cat <<EOF
Usage: $(basename "$0")

Environment overrides:
  CC                 compiler (default: cc)
  OPTLEVEL           optimisation level (default: 3)
  SIMD               gen | avx2 | avx512 (default: avx2)
  PERF_CASE          benchmark case (default: flow4:pkt_std)
  PERF_BACKEND       auto | gen | sse | avx2 | avx512 (default: avx2)
  PERF_EVENTS        core | mem | frontend | raw event list (default: core)
  PERF_REPEAT        benchmark repeat count passed to flow_cache_test
  PERF_ENTRIES       entry count passed with -n
  PERF_WARMUP        warmup iterations passed with --warmup
  PERF_SEED          optional PRNG seed
  PERF_CPU           optional CPU id for taskset pinning
  PERF_STAT_REPEAT   perf stat -r count (default: 5)
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if ! command -v perf >/dev/null 2>&1; then
    echo "error: perf not found in PATH" >&2
    exit 1
fi

case "$EVENTS_PRESET" in
    core)
        EVENTS="cycles,instructions,branches,branch-misses"
        ;;
    mem)
        EVENTS="cache-references,cache-misses,LLC-loads,LLC-load-misses,dTLB-load-misses"
        ;;
    frontend)
        EVENTS="stalled-cycles-frontend,stalled-cycles-backend,branches,branch-misses"
        ;;
    *)
        EVENTS="$EVENTS_PRESET"
        ;;
esac

make -C "$FCACHEDIR" static CC="$CC_BIN" OPTLEVEL="$OPTLEVEL" SIMD="$SIMD"
make -C "$TESTDIR" all CC="$CC_BIN" OPTLEVEL="$OPTLEVEL" SIMD="$SIMD"

cmd=("$TESTDIR/flow_cache_test"
     "--bench-case" "$CASE"
     "--backend" "$BACKEND"
     "--json"
     "--warmup" "$WARMUP"
     "-n" "$ENTRIES"
     "-r" "$REPEAT")

if [[ -n "$SEED" ]]; then
    cmd+=("--seed" "$SEED")
fi

run_prefix=()
if [[ -n "$CPU" ]]; then
    if ! command -v taskset >/dev/null 2>&1; then
        echo "error: taskset not found in PATH" >&2
        exit 1
    fi
    run_prefix=(taskset -c "$CPU")
fi

json_tmp=$(mktemp)
perf_tmp=$(mktemp)
trap 'rm -f "$json_tmp" "$perf_tmp"' EXIT

"${run_prefix[@]}" "${cmd[@]}" >/dev/null

perf stat -x, -r "$STAT_REPEAT" -e "$EVENTS" -- \
    "${run_prefix[@]}" "${cmd[@]}" >"$json_tmp" 2>"$perf_tmp"

cat "$json_tmp"
cat "$perf_tmp" >&2
