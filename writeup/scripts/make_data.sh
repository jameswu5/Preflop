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

ITEMS=${*:-D1 D2 D3}
for item in $ITEMS; do
    echo "== $item" >&2
    "$item"
done
