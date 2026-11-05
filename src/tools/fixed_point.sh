#!/usr/bin/env bash
# Phase 2 fixed-point iteration (docs/architecture.md, "Version 2"): the realization table depends on the
# ranges and the ranges on the table, so alternate until the table stops moving. Orchestration only;
# every computation is a C++ tool. Round k, from strategy S_k (S_0 = --strategy):
#   1. realization_table --strategy S_k             -> round_k/measured.bin (+ .records, resumable)
#   2. realization_table --fill                     -> round_k/filled.bin
#   3. solve --realization-table round_k/filled.bin -> round_k/strategy.bin = S_{k+1}
#      fresh, or with --warm from the previous round's checkpoint (--reset-average: regrets kept)
#   4. validate S_{k+1} against the charts          -> round_k/score.txt
#   5. realization_table --diff filled_{k-1} filled_k -> round_k/diff.txt (k >= 1)
# Every file stays under --out. A rerun skips the steps whose output exists, and solve resumes its
# checkpoint, so an interrupted run continues where it stopped. Stops after --rounds rounds or once
# mean |dR| < --tol. One summary line per round, also in <out>/summary.txt.
#
# Usage: fixed_point.sh --out DIR [--rounds N] [--iters N] [--threads N] [--flops N] [--tol X]
#                       [--strategy S0] [--checkpoint CK0] [--warm] [--bin DIR] [--table-tool NAME]
#                       [--table-args "..."] [--solve-args "..."] [--ref DIR]
#   --iters       solve iterations per round (with --warm: new iterations after the restart)
#   --threads     solve threads
#   --flops       canonical flops sampled per spot (realization_table --flops)
#   --warm        warm-start each solve from the previous round's checkpoint (round 0: --checkpoint, if given)
#   --table-args  extra realization_table arguments, measuring and filling (e.g. "--charts ref --jobs 2")
#   --solve-args  extra solve arguments (e.g. "--lock ref", "--seats 3")
#   --table-tool  realization_table (default) or realization_table_stub (tests)
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
out="" rounds=5 iters=100000000 threads=4 flops=0 tol=0 warm=0 ck0=""
strategy="$root/data/strategy_anchored.bin" bin="$root/build" tool=realization_table
table_args="" solve_args="" ref=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) out=$2; shift 2 ;;
        --rounds) rounds=$2; shift 2 ;;
        --iters) iters=$2; shift 2 ;;
        --threads) threads=$2; shift 2 ;;
        --flops) flops=$2; shift 2 ;;
        --tol) tol=$2; shift 2 ;;
        --strategy) strategy=$2; shift 2 ;;
        --checkpoint) ck0=$2; shift 2 ;;
        --warm) warm=1; shift ;;
        --bin) bin=$2; shift 2 ;;
        --table-tool) tool=$2; shift 2 ;;
        --table-args) table_args=$2; shift 2 ;;
        --solve-args) solve_args=$2; shift 2 ;;
        --ref) ref=$2; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
[[ -n "$out" ]] || { echo "--out DIR is required" >&2; exit 2; }
mkdir -p "$out"
: > "$out/summary.txt"

# Runs "$@" unless $1 (the step's output) exists; the log goes next to it.
step() {
    local target=$1 name=$2; shift 2
    if [[ -f "$target" ]]; then
        echo "  $name: kept $(basename "$target")"
    else
        echo "  $name"
        echo "$*" > "$target.log"
        "$@" >> "$target.log" 2>&1 || { echo "$name failed, see $target.log" >&2; tail -5 "$target.log" >&2; exit 1; }
    fi
}

s_k=$strategy prev_ck=$ck0 prev_filled=""
for ((k = 0; k < rounds; k++)); do
    d="$out/round_$k"
    mkdir -p "$d"
    echo "round $k (strategy $s_k)"
    # shellcheck disable=SC2086  # the *_args strings are split into words on purpose
    step "$d/measured.bin" measure "$bin/$tool" --strategy "$s_k" --flops "$flops" --out "$d/measured.bin" $table_args
    # shellcheck disable=SC2086
    step "$d/filled.bin" fill "$bin/$tool" --fill "$d/measured.bin" --strategy "$s_k" --out "$d/filled.bin" $table_args

    solve=("$bin/solve" --iters "$iters" --threads "$threads" --realization-table "$d/filled.bin"
           --checkpoint "$d/solve.ck" --out "$d/strategy.bin")
    # A checkpoint of this round means an interrupted solve: resume it rather than warm-start again.
    if [[ $warm == 1 && ! -f "$d/solve.ck" && -n "$prev_ck" && -f "$prev_ck" ]]; then
        solve+=(--warm "$prev_ck" --reset-average)
    fi
    # shellcheck disable=SC2086
    step "$d/strategy.bin" solve "${solve[@]}" $solve_args

    validate=("$bin/validate" --strategy "$d/strategy.bin")
    [[ -n "$ref" ]] && validate+=(--ref "$ref")
    step "$d/score.txt" validate sh -c '"$@" > "$0.tmp" && mv "$0.tmp" "$0"' "$d/score.txt" "${validate[@]}"
    score=$(sed -n 's/^score \([0-9.]*\).*/\1/p' "$d/score.txt")

    dr="-"
    if [[ -n "$prev_filled" ]]; then
        step "$d/diff.txt" diff sh -c '"$@" > "$0.tmp" && mv "$0.tmp" "$0"' "$d/diff.txt" \
            "$bin/$tool" --diff "$prev_filled" "$d/filled.bin"
        dr=$(sed -n 's/^mean |dR| \([0-9.e+-]*\)$/\1/p' "$d/diff.txt")
    fi
    line="round $k score ${score:-?} mean|dR| $dr"
    echo "$line" | tee -a "$out/summary.txt"

    s_k="$d/strategy.bin" prev_ck="$d/solve.ck" prev_filled="$d/filled.bin"
    if [[ $dr != "-" ]] && awk -v a="$dr" -v t="$tol" 'BEGIN { exit !(a < t) }'; then
        echo "converged: mean |dR| $dr < $tol"
        break
    fi
done
