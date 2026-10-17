#!/usr/bin/env python3
"""Fits the realization factors (utility.h RealizationConfig) to the reference charts in ref/
(docs/validation.md "Tuning knobs") by coordinate search.

Each candidate R warm-starts a 6-max solve from a checkpoint (regrets kept, averaging restarted),
runs --iters iterations, and is scored with `validate` (mean distance over ref/, lower is better).
Every round scores the current R plus +/- one step on each parameter in parallel, moves to the best
improvement, and halves the steps when nothing improves.

Usage: src/tools/tune.py [--checkpoint data/6max.ck] [--iters 20000000] [--jobs 8] [--rounds 6]
"""
import argparse
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "build")
# Start point = current defaults in utility.h; step = initial search step per parameter.
START = {"oop": 0.85, "pair": 1.0, "suited": 1.075, "offsuit": 0.8, "gap": 0.08, "low": 0.03, "spr": 2.0, "init": 0.0}
STEP = {"oop": 0.05, "pair": 0.05, "suited": 0.05, "offsuit": 0.05, "gap": 0.02, "low": 0.01, "spr": 1.0, "init": 0.05}


def evaluate(r, args, tmp):
    tag = "_".join(f"{k}{v:.4f}" for k, v in sorted(r.items()))
    ck, out = os.path.join(tmp, tag + ".ck"), os.path.join(tmp, tag + ".bin")
    shutil.copy(args.checkpoint, ck)
    cmd = [os.path.join(BUILD, "solve"), "--seats", "6", "--iters", str(args.iters), "--chunk", str(args.iters),
           "--checkpoint", ck, "--out", out, "--report", "--reset-average", "--boards", "25"]  # K=25: within noise of 100
    for k, v in r.items():
        cmd += ["--r", f"{k}={v}"]
    report = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    val = subprocess.run([os.path.join(BUILD, "validate"), "--strategy", out], capture_output=True, text=True).stdout
    m = re.search(r"^score ([0-9.]+)", val, re.M)
    if not m:
        raise RuntimeError("validate failed:\n" + val)
    btn = re.search(r"^  BTN\s+[0-9.]+% \|\s+([0-9.]+)%", report, re.M)  # BB fold vs BTN open, for context
    os.remove(ck), os.remove(out)
    return float(m.group(1)), btn.group(1) if btn else "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", default=os.path.join(ROOT, "data", "6max.ck"))
    ap.add_argument("--iters", type=int, default=20_000_000)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--rounds", type=int, default=6)
    args = ap.parse_args()

    cur, step = dict(START), dict(STEP)
    with tempfile.TemporaryDirectory() as tmp, cf.ThreadPoolExecutor(args.jobs) as pool:
        for rnd in range(1, args.rounds + 1):
            cands = [dict(cur)]
            for k in cur:
                for sign in (1, -1):
                    c = dict(cur)
                    c[k] = round(max(0.0, c[k] + sign * step[k]), 4)
                    if c != cur and c not in cands:
                        cands.append(c)
            scores = list(pool.map(lambda c: evaluate(c, args, tmp), cands))
            best = min(range(len(cands)), key=lambda i: scores[i][0])
            print(f"round {rnd}: current {scores[0][0]:.4f} (BB folds {scores[0][1]}% vs BTN)  {cur}")
            if best == 0:
                step = {k: v / 2 for k, v in step.items()}
                print(f"  no improvement, halving steps -> {step}")
            else:
                changed = {k: cands[best][k] for k in cur if cands[best][k] != cur[k]}
                print(f"  best {scores[best][0]:.4f} (BB folds {scores[best][1]}% vs BTN) by {changed}")
                cur = cands[best]
            print(flush=True)
    print("final R:", " ".join(f"--r {k}={v}" for k, v in cur.items()))


if __name__ == "__main__":
    main()
