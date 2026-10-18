#!/usr/bin/env bash
# Regenerates writeup/data/ and writeup/figures/gen/ from the current code and data.
# Usage: writeup/scripts/make_data.sh [D1 D2 ...]   (default: all items)
# Env: DATA (default <repo>/data), REF (default <repo>/ref), PY (default <repo>/.venv/bin/python),
#      THREADS (default 8). Reads DATA and REF only: scratch solves go to writeup/data/tmp/ (gitignored).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DATA=${DATA:-$ROOT/data}
REF=${REF:-$ROOT/ref}
PY=${PY:-$ROOT/.venv/bin/python}
THREADS=${THREADS:-8}
OUT=$ROOT/writeup/data
FIG=$ROOT/writeup/figures/gen
TMP=$OUT/tmp
B=$ROOT/build
GEN="$PY $ROOT/writeup/scripts/gen.py"
mkdir -p "$OUT" "$FIG" "$TMP"
cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
cmake --build "$B" -j8 --target solve validate dump_range pushfold_trace sample_evs >/dev/null

# Spots drawn as range grids: main-text pairs first, then the appendix (every open + a spread).
MAIN="open_lj open_btn vs_open_bb_vs_btn vs_open_btn_vs_co vs_3bet_co_vs_btn vs_4bet_bb_vs_btn"
APPX="open_hj open_co open_sb vs_open_bb_vs_sb vs_open_co_vs_lj vs_open_sb_vs_btn vs_3bet_sb_vs_bb
      vs_3bet_lj_vs_btn vs_4bet_sb_vs_btn vs_4bet_btn_vs_co vs_5bet_btn_vs_bb vs_5bet_co_vs_sb"

D1() {  # heads-up push/fold exploitability trace and final ranges
    "$B/pushfold_trace" --max 1e8 --seeds 3 --equity "$DATA/hu_equity.bin" --out "$OUT/pushfold_expl.dat" \
        --strategy "$TMP/pushfold.bin"
    "$B/dump_range" --strategy "$TMP/pushfold.bin" FFFF FFFFR > "$TMP/pushfold.dump"
    $GEN grids "$TMP/pushfold.dump" "$FIG" --rename FFFF=grid_pushfold_sb,FFFFR=grid_pushfold_bb
}

D2() {  # fresh 300M Scratch solve: strategy change and speed per 10M chunk
    rm -f "$TMP/scratch.ck"
    "$B/solve" --iters 300000000 --chunk 10000000 --threads 8 --seed 1 --checkpoint "$TMP/scratch.ck" \
        --out "$TMP/scratch.bin" | tee "$TMP/scratch.log"
    { echo "iters change_pct its"
      sed -nE 's/^iteration ([0-9]+): ([0-9]+) it\/s, strategy change ([0-9.]+)%$/\1 \3 \2/p' "$TMP/scratch.log"
    } > "$OUT/convergence.dat"
}

D3() {  # range grids (Scratch / GTO / PTO) and Scratch's per-spot distance from each chart
    local gto=() pto=()
    for s in $MAIN $APPX; do gto+=("$REF/gto_$s.txt"); pto+=("$REF/pto/pto_$s.txt"); done
    "$B/dump_range" --strategy "$DATA/strategy_6max.bin" "${gto[@]}" > "$TMP/scratch.dump"
    "$B/dump_range" --strategy "$DATA/strategy_anchored.bin" --charts "$REF" "${gto[@]}" > "$TMP/gto.dump"
    "$B/dump_range" --strategy "$DATA/strategy_anchored.bin" --charts "$REF/pto" "${pto[@]}" > "$TMP/pto.dump"
    $GEN grids "$TMP/scratch.dump" "$FIG" --prefix grid_scratch_
    $GEN grids "$TMP/gto.dump" "$FIG" --prefix grid_gto_
    $GEN grids "$TMP/pto.dump" "$FIG" --prefix grid_pto_
    "$B/validate" --strategy "$DATA/strategy_6max.bin" --ref "$REF" > "$TMP/validate_scratch_gto.txt" || true
    "$B/validate" --strategy "$DATA/strategy_6max.bin" --ref "$REF/pto" > "$TMP/validate_scratch_pto.txt" || true
    $GEN distance "$TMP/validate_scratch_gto.txt" "$TMP/validate_scratch_pto.txt" "$OUT/spot_distance.dat" \
        "$OUT/spot_distance.tex"
}

val() { "$B/validate" --strategy "$1" --ref "${2:-$REF}"; }

D4() {  # score tables: neutral model, previous and adopted defaults, Gate 2 candidates
    "$B/solve" --iters 300000000 --threads 8 --seed 1 --out "$TMP/neutral.bin" \
        --r flush=0 --r conn=0 --r set=0 --r dom=0 --r low=0 --r spr=0 --r init=0 --r pairLow=0 \
        --r oopCall=1 --r callerShrink=1 --r ipCall=1 --r domPot=0 > "$TMP/neutral.log"
    val "$TMP/neutral.bin" > "$TMP/val_neutral.txt"
    val "$DATA/strategy_6max.bin" > "$TMP/val_shipped.txt"
    val "$DATA/strategy_6max.bin" "$REF/pto" > "$TMP/val_shipped_pto.txt"
    local g=() c
    for c in base tau1 tau03 a25 a50 nospr; do
        local fs=()
        for f in "$DATA"/gate2/$c.*.bin; do
            val "$f" > "$TMP/val_gate2_$(basename "$f" .bin).txt"
            fs+=("$TMP/val_gate2_$(basename "$f" .bin).txt")
        done
        g+=("$c=$(IFS=,; echo "${fs[*]}")")
    done
    v() { echo "$TMP"/val_gate2_$1.[1-4].txt | tr ' ' ,; }
    $GEN scores "$OUT/scores.tex" "$OUT/scores.dat" "Position only (neutral)=$TMP/val_neutral.txt" \
        "Previous defaults=$(v base)" "Adopted defaults=$(v a50)" "Adopted, shipped solve=$TMP/val_shipped.txt"
    $GEN scores "$OUT/scores_pto.tex" "$OUT/scores_pto.dat" "Adopted, shipped solve=$TMP/val_shipped_pto.txt"
    $GEN scores "$OUT/gate2.tex" "$OUT/gate2.dat" "${g[@]}"
}

D5() {  # lock report of the Anchored recipe
    "$B/solve" --warm "$DATA/6max.ck" --reset-average --iters 300000000 --chunk 100000000 --threads 8 \
        --lock "$REF" --out "$TMP/anchored.bin" > "$TMP/anchored.log"
    $GEN lockreport "$TMP/anchored.log" "$REF" "$OUT/lockreport.txt" "$OUT/lockreport.tex"
}

D6() {  # sanity metrics of the shipped Scratch solve, from a copy of its checkpoint (no iterations run)
    cp "$DATA/6max.ck" "$TMP/6max_copy.ck"
    "$B/solve" --checkpoint "$TMP/6max_copy.ck" --iters 1 --report --out "$TMP/report.bin" > "$TMP/report.log"
    cmp -s "$TMP/report.bin" "$DATA/strategy_6max.bin" || echo "warning: 6max.ck's average differs from strategy_6max.bin" >&2
    local s charts=()  # the GTO charts' widths at the same spots, with the reach the charts themselves give
    for s in lj hj co btn sb; do charts+=("$REF/gto_open_$s.txt" "$REF/gto_vs_open_bb_vs_$s.txt" "$REF/gto_vs_3bet_${s}_vs_bb.txt"); done
    "$B/dump_range" --strategy "$DATA/strategy_anchored.bin" --charts "$REF" "${charts[@]}" > "$TMP/sanity_gto.dump"
    $GEN sanity "$TMP/report.log" "$TMP/sanity_gto.dump" "$OUT/sanity.tex"
}

D7() {  # fixed-strategy fit on the study's samples, and Gate 1 on the current Anchored
    for tau in 0.3 1 3; do
        $PY "$ROOT/src/tools/fit_realization.py" "$DATA/samples" --tau $tau > "$OUT/fit_tau$tau.txt"
    done
    $GEN fit "$OUT/fit_tau0.3.txt" "$OUT/fit_tau1.txt" "$OUT/fit_tau3.txt" "$OUT/fit_kl.tex" "$OUT/fit_theta.tex"
    "$B/sample_evs" --check --ess 20000 --strategy "$DATA/strategy_anchored.bin" --equity "$DATA/hu_equity.bin" \
        --ref "$REF" > "$OUT/gate1.txt"
    $GEN gate1 "$OUT/gate1.txt" "$OUT/gate1.tex"
}

D8() {  # realization factor heat grids for four roles, with the shipped solve's RealizationConfig
    $GEN heat "$DATA/strategy_6max.bin" "$DATA/samples" "$FIG" "$OUT"
}

ITEMS=${*:-D1 D2 D3 D4 D5 D6 D7 D8}
for item in $ITEMS; do
    echo "== $item" >&2
    "$item"
done
